#include <linux/rtnetlink.h>  /* rtnl_[un]lock() */
#include "bsd_glue.h"
#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>


/* ====================== STUFF DEFINED in netmap.c ===================== */
// XXX Why don't we use another header?
struct netmap_priv_d {
	struct netmap_if * volatile np_nifp;	/* netmap if descriptor. */

	struct ifnet	*np_ifp;	/* device for which we hold a ref. */
	int		np_ringid;	/* from the ioctl */
	u_int		np_qfirst, np_qlast;	/* range of rings to scan */
	uint16_t	np_txpoll;

	struct netmap_mem_d *np_mref;	/* use with NMG_LOCK held */
#ifdef __FreeBSD__
	int		np_refcount;	/* use with NMG_LOCK held */
#endif /* __FreeBSD__ */
};

int netmap_get_memory(struct netmap_priv_d* p);
void netmap_dtor(void *data);
int netmap_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag, struct thread *td);
int netmap_poll(struct cdev *dev, int events, struct thread *td);
int netmap_init(void);
void netmap_fini(void);


/* ===================== GENERIC NETMAP ADAPTER SUPPORT ================== */
//#define GNA_RAW_XMIT   /* Call ndo_start_xmit() directly (UNSAFE). */
#ifdef GNA_RAW_XMIT
#define GNA_TX_OK       NETDEV_TX_OK
#define GNA_TX_FAIL     NETDEV_TX_BUSY
#else   /* ! GNA_RAW_XMIT */
#define GNA_TX_OK       NET_XMIT_SUCCESS
#define GNA_TX_FAIL     NET_XMIT_DROP
#endif /* ! GNA_RAW_XMIT */

/* This handler is registered within the attached net_device in the Linux RX subsystem,
   so that every sk_buff passed up by the driver can be stolen to the network stack.
   Stolen packets are put in a queue where the generic_netmap_rxsync() callback can
   extract them. */
rx_handler_result_t generic_netmap_rx_handler(struct sk_buff **pskb)
{
    struct netmap_adapter *na = NA((*pskb)->dev);
    u_int work_done;

    if (unlikely(skb_queue_len(&na->rx_rings[0].rx_queue) > 1024)) {
        kfree_skb(*pskb);
    } else {
        skb_queue_tail(&na->rx_rings[0].rx_queue, *pskb);
        netmap_rx_irq(na->ifp, 0, &work_done);
    }

    return RX_HANDLER_CONSUMED;
}

/* Enable/disable netmap mode for a generic network interface. */
int
generic_netmap_register(struct ifnet *ifp, int enable)
{
    struct netmap_adapter *na = NA(ifp);
    int error = 0;

    if (!na)
        return EINVAL;

#ifdef GNA_RAW_XMIT
    if ((error = ifp->netdev_ops->ndo_stop(ifp)))
        return error;
#endif  /* GNA_RAW_XMIT */

    rtnl_lock();

    if (enable) { /* Enable netmap mode. */
        ifp->if_capenable |= IFCAP_NETMAP;
#ifdef GNA_RAW_XMIT
        na->if_transmit = (void *)ifp->netdev_ops;
        ifp->netdev_ops = &na->nm_ndo;
#endif  /* GNA_RAW_XMIT */
        /* Initialize the queue structure, since the generic_netmap_rx_handler() callback can
           be called as soon after netdev_rx_handler_register() returns. */
        skb_queue_head_init(&na->rx_rings[0].rx_queue);
        na->rx_rings[0].nr_ntc = 0;
        NM_ATOMIC_SET(&na->tx_rings[0].tx_completed, 0);
        if ((error = netdev_rx_handler_register(ifp, &generic_netmap_rx_handler, na))) {
            D("netdev_rx_handler_register() failed\n");
            rtnl_unlock();
            return error;
        }
    } else { /* Disable netmap mode. */
        ifp->if_capenable &= ~IFCAP_NETMAP;
#ifdef GNA_RAW_XMIT
        ifp->netdev_ops = (void *)na->if_transmit;
#endif  /* GNA_RAW_XMIT */
        netdev_rx_handler_unregister(ifp);
        skb_queue_purge(&na->rx_rings[0].rx_queue);
    }

    rtnl_unlock();

#ifdef GNA_RAW_XMIT
    error = ifp->netdev_ops->ndo_open(ifp);
#endif  /* GNA_RAW_XMIT */

    return error;
}

/* Invoked when the driver of the attached interface frees a socket buffer used by netmap for
   transmitting a packet. This usually happens when the NIC notifies the driver that the
   transmission is completed. */
static void
generic_mbuf_destructor(struct sk_buff *skb)
{
    struct netmap_adapter *na = (struct netmap_adapter *)(skb_shinfo(skb)->destructor_arg);

    NM_ATOMIC_INC(&na->tx_rings[0].tx_completed);
    netmap_tx_irq(na->ifp, 0);
}

/* The generic txsync method transforms netmap buffers in sk_buffs and the invokes the
   driver ndo_start_xmit() method. This is not done directly, but using dev_queue_xmit(),
   since it implements the TX flow control (and takes some locks). */
static int
generic_netmap_txsync(struct ifnet *ifp, u_int ring_nr, int flags)
{
    struct netmap_adapter *na = NA(ifp);
#ifdef GNA_RAW_XMIT
    struct net_device_ops * ops = (struct net_device_ops *)na->if_transmit;
#endif
    struct netmap_kring *kring = &na->tx_rings[ring_nr];
    struct netmap_ring *ring = kring->ring;
    u_int j, k, n = 0, lim = kring->nkr_num_slots - 1;

    if (!netif_carrier_ok(ifp)) {
        return 0;
    }

    /* Take a copy of ring->cur now, and never read it again. */
    k = ring->cur;
    if (k > lim)
        return netmap_ring_reinit(kring);

    rmb();
    j = kring->nr_hwcur;
    if (j != k) {
        /* Process new packets to send: j is the current index in the netmap ring. */
        while (j != k) {
            struct netmap_slot *slot = &ring->slot[j]; /* Current slot in the netmap ring */
            void *addr = NMB(slot);
            u_int len = slot->len;
            struct sk_buff *skb;
            netdev_tx_t tx_ret;

            if (addr == netmap_buffer_base || len > NETMAP_BUF_SIZE) {
                return netmap_ring_reinit(kring);
            }
            /* Allocate a new mbuf for transmission and copy in the user packet. */
            skb = alloc_skb(len, GFP_ATOMIC);
            if (unlikely(!skb)) {
                D("mbuf allocation failed\n");
                return netmap_ring_reinit(kring);
            }
            /* TODO Support the slot flags (NS_FRAG, NS_INDIRECT). */
            skb_copy_to_linear_data(skb, addr, len); // skb_store_bits(skb, 0, addr, len);
            skb_put(skb, len);
            skb->destructor = &generic_mbuf_destructor;
            skb_shinfo(skb)->destructor_arg = na;
#ifdef GNA_RAW_XMIT
            tx_ret = ops->ndo_start_xmit(skb, ifp);
#else   /* GNA_RAW_XMIT */
            skb->dev = ifp;
            skb->priority = 100;
            tx_ret = dev_queue_xmit(skb);
#endif  /* GNA_RAW_XMIT */
            if (unlikely(tx_ret != GNA_TX_OK)) {
                ND("start_xmit failed: err %d [%d,%d,%d]\n", tx_ret, j, k, kring->nr_hwavail);
                if (likely(tx_ret == GNA_TX_FAIL)) {
                    skb->destructor = NULL;
                    kfree_skb(skb);
                    break;
                }
                D("start_xmit failed: HARD ERROR\n");
                return netmap_ring_reinit(kring);
            }
            slot->flags &= ~(NS_REPORT | NS_BUF_CHANGED);
            if (unlikely(j++ == lim))
                j = 0;
            n++;
        }
        kring->nr_hwcur = j;
        kring->nr_hwavail -= n;
        ND("tx #%d, hwavail = %d\n", n, kring->nr_hwavail);
    }

    /* Record completed transmissions using tx_completed and update hwavail/avail. */
    n = NM_ATOMIC_READ_AND_CLEAR(&kring->tx_completed);
    if (n) {
        kring->nr_hwavail += n;
        ring->avail += n;
        ND("tx completed [%d] -> hwavail %d\n", n, kring->nr_hwavail);
    }

    return 0;
}

/* The generic rxsync() method extracts sk_buffs from the queue filled by
   generic_netmap_rx_handler() and puts their content in the netmap receive ring. */
static int
generic_netmap_rxsync(struct ifnet *ifp, u_int ring_nr, int flags)
{
    struct netmap_adapter *na = NA(ifp);
    struct netmap_kring *kring = &na->rx_rings[ring_nr];
    struct netmap_ring *ring = kring->ring;
    u_int j, n, lim = kring->nkr_num_slots - 1;
    int force_update = (flags & NAF_FORCE_READ) || kring->nr_kflags & NKR_PENDINTR;
    u_int k, resvd = ring->reserved;

    if (ring->cur > lim)
        return netmap_ring_reinit(kring);

    /* Import newly received packets into the netmap ring. */
    if (netmap_no_pendintr || force_update) {
        uint16_t slot_flags = kring->nkr_slot_flags;
        struct sk_buff *skb;

        n = 0;
        j = kring->nr_ntc;
        /* The k index in the netmap ring prevents ntc from bumping into hwcur. */
        k = (kring->nr_hwcur) ? kring->nr_hwcur-1 : lim;
        while (j != k) {
            void *addr = NMB(&ring->slot[j]);

            if (addr == netmap_buffer_base) { /* Bad buffer */
                return netmap_ring_reinit(kring);
            }
            skb = skb_dequeue(&kring->rx_queue);
            if (!skb)
                break;
            skb_copy_from_linear_data(skb, addr, skb->len);
            ring->slot[j].len = skb->len;
            ring->slot[j].flags = slot_flags;
            kfree_skb(skb);
            if (unlikely(j++ == lim))
                j = 0;
            n++;
        }
        if (n) {
            kring->nr_ntc = j;
            kring->nr_hwavail += n;
        }
        kring->nr_kflags &= ~NKR_PENDINTR;
    }

    /* Skip past packets that userspace has released */
    j = kring->nr_hwcur;
    k = ring->cur;
    if (resvd > 0) {
        if (resvd + ring->avail >= lim + 1) {
            D("XXX invalid reserve/avail %d %d", resvd, ring->avail);
            ring->reserved = resvd = 0; // XXX panic...
        }
        k = (k >= resvd) ? k - resvd : k + lim + 1 - resvd;
    }
    if (j != k) {
        /* Userspace has released some packets. */
        for (n = 0; j != k; n++) {
            struct netmap_slot *slot = &ring->slot[j];

            slot->flags &= ~NS_BUF_CHANGED;
            if (unlikely(j++ == lim))
                j = 0;
        }
        kring->nr_hwavail -= n;
        kring->nr_hwcur = k;
    }
    /* Tell userspace that there are new packets. */
    ring->avail = kring->nr_hwavail - resvd;

    return 0;
}

/* The generic netmap attach method makes it possible to attach netmap to a network
   interface that doesn't have explicit netmap support. The netmap ring size has no
   relationship to the NIC ring size: 256 is a good compromise.
   Since this function cannot be called by the driver, it is called by get_ifp(). */
int
generic_netmap_attach(struct ifnet *ifp)
{
    struct netmap_adapter na;

    bzero(&na, sizeof(na));
    na.ifp = ifp;
    na.num_tx_desc = 256;
    na.num_rx_desc = 256;
    na.nm_register = &generic_netmap_register;
    na.nm_txsync = &generic_netmap_txsync;
    na.nm_rxsync = &generic_netmap_rxsync;

    ND("[GNA] num_tx_queues(%d), real_num_tx_queues(%d), len(%lu)\n", ifp->num_tx_queues,
                                        ifp->real_num_tx_queues, ifp->tx_queue_len);
    ND("[GNA] num_rx_queues(%d), real_num_rx_queues(%d)\n", ifp->num_rx_queues,
                                                            ifp->real_num_rx_queues);

    return netmap_attach(&na, 1);
}


/* ========================== LINUX-SPECIFIC ROUTINES ================== */

static struct device_driver*
linux_netmap_find_driver(struct device *dev)
{
	struct device_driver *dd;

	while ( (dd = dev->driver) == NULL ) {
		if ( (dev = dev->parent) == NULL )
			return NULL;
	}
	return dd;
}

struct net_device*
ifunit_ref(const char *name)
{
	struct net_device *ifp = dev_get_by_name(&init_net, name);
	struct device_driver *dd;

	if (ifp == NULL)
		return NULL;

	if ( (dd = linux_netmap_find_driver(&ifp->dev)) == NULL )
		goto error;

	if (!try_module_get(dd->owner))
		goto error;

	return ifp;
error:
	dev_put(ifp);
	return NULL;
}

void if_rele(struct net_device *ifp)
{
	struct device_driver *dd;
	dd = linux_netmap_find_driver(&ifp->dev);
	dev_put(ifp);
	if (dd)
		module_put(dd->owner);
}



/*
 * Remap linux arguments into the FreeBSD call.
 * - pwait is the poll table, passed as 'dev';
 *   If pwait == NULL someone else already woke up before. We can report
 *   events but they are filtered upstream.
 *   If pwait != NULL, then pwait->key contains the list of events.
 * - events is computed from pwait as above.
 * - file is passed as 'td';
 */
static u_int
linux_netmap_poll(struct file * file, struct poll_table_struct *pwait)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
	int events = POLLIN | POLLOUT; /* XXX maybe... */
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
	int events = pwait ? pwait->key : POLLIN | POLLOUT;
#else /* in 3.4.0 field 'key' was renamed to '_key' */
	int events = pwait ? pwait->_key : POLLIN | POLLOUT;
#endif
	return netmap_poll((void *)pwait, events, (void *)file);
}


static int
linux_netmap_mmap(struct file *f, struct vm_area_struct *vma)
{
	int error = 0;
	unsigned long off, va;
	vm_ooffset_t pa;
	struct netmap_priv_d *priv = f->private_data;
	/*
	 * vma->vm_start: start of mapping user address space
	 * vma->vm_end: end of the mapping user address space
	 * vma->vm_pfoff: offset of first page in the device
	 */

	// XXX security checks

	error = netmap_get_memory(priv);
	ND("get_memory returned %d", error);
	if (error)
	    return -error;

	if ((vma->vm_start & ~PAGE_MASK) || (vma->vm_end & ~PAGE_MASK)) {
		ND("vm_start = %lx vm_end = %lx", vma->vm_start, vma->vm_end);
		return -EINVAL;
	}

	for (va = vma->vm_start, off = vma->vm_pgoff;
	     va < vma->vm_end;
	     va += PAGE_SIZE, off++)
	{
		pa = netmap_mem_ofstophys(priv->np_mref, off << PAGE_SHIFT);
		if (pa == 0) 
			return -EINVAL;
	
		ND("va %lx pa %p", va, pa);	
		error = remap_pfn_range(vma, va, pa >> PAGE_SHIFT, PAGE_SIZE, vma->vm_page_prot);
		if (error) 
			return error;
	}
	return 0;
}


/*
 * This one is probably already protected by the netif lock XXX
 */
netdev_tx_t
linux_netmap_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netmap_transmit(dev, skb);
	return (NETDEV_TX_OK);
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)	// XXX was 38
#define LIN_IOCTL_NAME	.ioctl
int
linux_netmap_ioctl(struct inode *inode, struct file *file, u_int cmd, u_long data /* arg */)
#else
#define LIN_IOCTL_NAME	.unlocked_ioctl
long
linux_netmap_ioctl(struct file *file, u_int cmd, u_long data /* arg */)
#endif
{
	int ret;
	struct nmreq nmr;
	bzero(&nmr, sizeof(nmr));

	if (data && copy_from_user(&nmr, (void *)data, sizeof(nmr) ) != 0)
		return -EFAULT;
	ret = netmap_ioctl(NULL, cmd, (caddr_t)&nmr, 0, (void *)file);
	if (data && copy_to_user((void*)data, &nmr, sizeof(nmr) ) != 0)
		return -EFAULT;
	return -ret;
}


static int
netmap_release(struct inode *inode, struct file *file)
{
	(void)inode;	/* UNUSED */
	if (file->private_data)
		netmap_dtor(file->private_data);
	return (0);
}


static int
linux_netmap_open(struct inode *inode, struct file *file)
{
	struct netmap_priv_d *priv;
	(void)inode;	/* UNUSED */

	priv = malloc(sizeof(struct netmap_priv_d), M_DEVBUF,
			      M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return -ENOMEM;

	file->private_data = priv;

	return (0);
}


static struct file_operations netmap_fops = {
    .owner = THIS_MODULE,
    .open = linux_netmap_open,
    .mmap = linux_netmap_mmap,
    LIN_IOCTL_NAME = linux_netmap_ioctl,
    .poll = linux_netmap_poll,
    .release = netmap_release,
};


struct miscdevice netmap_cdevsw = { /* same name as FreeBSD */
	MISC_DYNAMIC_MINOR,
	"netmap",
	&netmap_fops,
};


static int linux_netmap_init(void)
{
        /* Errors have negative values on linux. */
	return -netmap_init();
}


static void linux_netmap_fini(void)
{
        netmap_fini();
}


module_init(linux_netmap_init);
module_exit(linux_netmap_fini);
/* export certain symbols to other modules */
EXPORT_SYMBOL(netmap_attach);		/* driver attach routines */
EXPORT_SYMBOL(netmap_detach);		/* driver detach routines */
EXPORT_SYMBOL(netmap_ring_reinit);	/* ring init on error */
EXPORT_SYMBOL(netmap_buffer_lut);
EXPORT_SYMBOL(netmap_total_buffers);	/* index check */
EXPORT_SYMBOL(netmap_buffer_base);
EXPORT_SYMBOL(netmap_reset);		/* ring init routines */
EXPORT_SYMBOL(netmap_buf_size);
EXPORT_SYMBOL(netmap_rx_irq);		/* default irq handler */
EXPORT_SYMBOL(netmap_no_pendintr);	/* XXX mitigation - should go away */
EXPORT_SYMBOL(netmap_bdg_ctl);		/* bridge configuration routine */
EXPORT_SYMBOL(netmap_bdg_learning);	/* the default lookup function */
EXPORT_SYMBOL(netmap_disable_all_rings);
EXPORT_SYMBOL(netmap_enable_all_rings);


MODULE_AUTHOR("http://info.iet.unipi.it/~luigi/netmap/");
MODULE_DESCRIPTION("The netmap packet I/O framework");
MODULE_LICENSE("Dual BSD/GPL"); /* the code here is all BSD. */

