/*
 * Dongle BUS interface
 * USB Linux Implementation
 *
 * Copyright (C) 2022 Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 *
 * <<Broadcom-WL-IPTag/Open:>>
 *
 * $Id: dbus_usb_linux.c 564663 2015-06-18 02:34:42Z $
 */

/**
 * @file @brief
 * This file contains DBUS code that is USB *and* OS (Linux) specific. DBUS is a Broadcom
 * proprietary host specific abstraction layer.
 */

#include <typedefs.h>
#include <osl.h>

/**
 * DBUS_LINUX_RXDPC is created for router platform performance tuning. A separate thread is created
 * to handle USB RX and avoid the call chain getting too long and enhance cache hit rate.
 *
 * DBUS_LINUX_RXDPC setting is in wlconfig file.
 */

/*
 * If DBUS_LINUX_RXDPC is off, spin_lock_bh() for CTFPOOL in
 * linux_osl.c has to be changed to spin_lock_irqsave() because
 * PKTGET/PKTFREE are no longer in bottom half.
 *
 * Right now we have another queue rpcq in wl_linux.c. Maybe we
 * can eliminate that one to reduce the overhead.
 *
 * Enabling 2nd EP and DBUS_LINUX_RXDPC causing traffic from
 * both EP's to be queued in the same rx queue. If we want
 * RXDPC to work with 2nd EP. The EP for RPC call return
 * should bypass the dpc and go directly up.
 */

/* #define DBUS_LINUX_RXDPC */

/* Dbus histogram for ntxq, nrxq, dpc parameter tuning */
/* #define DBUS_LINUX_HIST */

#include <usbrdl.h>
#include <bcmendian.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <asm/unaligned.h>
#include <dbus.h>
#include <bcmutils.h>
#include <bcmdevs.h>
#include <linux/usb.h>
#include <usbrdl.h>
#include <linux/firmware.h>
#include <dngl_stats.h>
#include <dhd.h>

#if defined(USBOS_THREAD) || defined(USBOS_TX_THREAD)

/**
 * The usb-thread is designed to provide currency on multiprocessors and SMP linux kernels. On the
 * dual cores platform, the WLAN driver, without threads, executed only on CPU0. The driver consumed
 * almost of 100% on CPU0, while CPU1 remained idle. The behavior was observed on Broadcom's STB.
 *
 * The WLAN driver consumed most of CPU0 and not CPU1 because tasklets/queues, software irq, and
 * hardware irq are executing from CPU0, only. CPU0 became the system's bottle-neck. TPUT is lower
 * and system's responsiveness is slower.
 *
 * To improve system responsiveness and TPUT usb-thread was implemented. The system's threads could
 * be scheduled to run on any core. One core could be processing data in the usb-layer and the other
 * core could be processing data in the wl-layer.
 *
 * For further info see [WlThreadAndUsbThread] Twiki.
 */

#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <asm/hardirq.h>
#include <linux/list.h>
#include <linux_osl.h>
#endif /* USBOS_THREAD || USBOS_TX_THREAD */



#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0))
#define KERNEL26
#endif

/**
 * Starting with the 3.10 kernel release, dynamic PM support for USB is present whenever
 * the kernel was built with CONFIG_PM_RUNTIME enabled. The CONFIG_USB_SUSPEND option has
 * been eliminated.
 */
#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 21)) && defined(CONFIG_USB_SUSPEND)) \
	|| ((LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)) && defined(CONFIG_PM_RUNTIME)) \
	|| (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0))
/* For USB power management support, see Linux kernel: Documentation/usb/power-management.txt */
#define USB_SUSPEND_AVAILABLE
#endif

/* Define alternate fw/nvram paths used in Android */
#ifdef OEM_ANDROID
#define CONFIG_ANDROID_BCMDHD_FW_PATH "broadcom/dhd/firmware/fw.bin.trx"
#define CONFIG_ANDROID_BCMDHD_NVRAM_PATH "broadcom/dhd/nvrams/nvm.txt"
#endif /* OEM_ANDROID */

static inline int usb_submit_urb_linux(struct urb *urb)
{

#ifdef BCM_MAX_URB_LEN
	if (urb && (urb->transfer_buffer_length > BCM_MAX_URB_LEN)) {
		DBUSERR(("URB transfer length=%d exceeded %d ra=%p\n", urb->transfer_buffer_length,
		BCM_MAX_URB_LEN, __builtin_return_address(0)));
		return DBUS_ERR;
	}
#endif

#ifdef KERNEL26
	return usb_submit_urb(urb, GFP_ATOMIC);
#else
	return usb_submit_urb(urb);
#endif

}

#define USB_SUBMIT_URB(urb) usb_submit_urb_linux(urb)

#ifdef KERNEL26

#define USB_ALLOC_URB()				usb_alloc_urb(0, GFP_ATOMIC)
#define USB_UNLINK_URB(urb)			(usb_kill_urb(urb))
#define USB_FREE_URB(urb)			(usb_free_urb(urb))
#define USB_REGISTER()				usb_register(&dbus_usbdev)
#define USB_DEREGISTER()			usb_deregister(&dbus_usbdev)

#ifdef USB_SUSPEND_AVAILABLE

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33))
#define USB_AUTOPM_SET_INTERFACE(intf)		usb_autopm_set_interface(intf)
#else
#define USB_ENABLE_AUTOSUSPEND(udev)		usb_enable_autosuspend(udev)
#define USB_DISABLE_AUTOSUSPEND(udev)       usb_disable_autosuspend(udev)
#endif  /* LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33))  */

#define USB_AUTOPM_GET_INTERFACE(intf)		usb_autopm_get_interface(intf)
#define USB_AUTOPM_PUT_INTERFACE(intf)		usb_autopm_put_interface(intf)
#define USB_AUTOPM_GET_INTERFACE_ASYNC(intf)	usb_autopm_get_interface_async(intf)
#define USB_AUTOPM_PUT_INTERFACE_ASYNC(intf)	usb_autopm_put_interface_async(intf)
#define USB_MARK_LAST_BUSY(dev)			usb_mark_last_busy(dev)

#else /* USB_SUSPEND_AVAILABLE */

#define USB_AUTOPM_GET_INTERFACE(intf)		do {} while (0)
#define USB_AUTOPM_PUT_INTERFACE(intf)		do {} while (0)
#define USB_AUTOPM_GET_INTERFACE_ASYNC(intf)	do {} while (0)
#define USB_AUTOPM_PUT_INTERFACE_ASYNC(intf)	do {} while (0)
#define USB_MARK_LAST_BUSY(dev)			do {} while (0)
#endif /* USB_SUSPEND_AVAILABLE */

#define USB_CONTROL_MSG(dev, pipe, request, requesttype, value, index, data, size, timeout) \
	usb_control_msg((dev), (pipe), (request), (requesttype), (value), (index), \
	(data), (size), (timeout))
#define USB_BULK_MSG(dev, pipe, data, len, actual_length, timeout) \
	usb_bulk_msg((dev), (pipe), (data), (len), (actual_length), (timeout))
#define USB_BUFFER_ALLOC(dev, size, mem, dma)	usb_buffer_alloc(dev, size, mem, dma)
#define USB_BUFFER_FREE(dev, size, data, dma)	usb_buffer_free(dev, size, data, dma)

#ifdef WL_URB_ZPKT
#define URB_QUEUE_BULK   URB_ZERO_PACKET
#else
#define URB_QUEUE_BULK   0
#endif /* WL_URB_ZPKT */

#define CALLBACK_ARGS		struct urb *urb, struct pt_regs *regs
#define CALLBACK_ARGS_DATA	urb, regs
#define CONFIGDESC(usb)		(&((usb)->actconfig)->desc)
#define IFPTR(usb, idx)		((usb)->actconfig->interface[idx])
#define IFALTS(usb, idx)	(IFPTR((usb), (idx))->altsetting[0])
#define IFDESC(usb, idx)	IFALTS((usb), (idx)).desc
#define IFEPDESC(usb, idx, ep)	(IFALTS((usb), (idx)).endpoint[ep]).desc

#else /* KERNEL26 */

#define USB_ALLOC_URB()				usb_alloc_urb(0)
#define USB_UNLINK_URB(urb)			usb_unlink_urb(urb)
#define USB_FREE_URB(urb)			(usb_free_urb(urb))
#define USB_REGISTER()				usb_register(&dbus_usbdev)
#define USB_DEREGISTER()			usb_deregister(&dbus_usbdev)
#define USB_AUTOPM_GET_INTERFACE(intf)		do {} while (0)
#define USB_AUTOPM_GET_INTERFACE_ASYNC(intf)	do {} while (0)
#define USB_AUTOPM_PUT_INTERFACE_ASYNC(intf)	do {} while (0)
#define USB_MARK_LAST_BUSY(dev)			do {} while (0)

#define USB_CONTROL_MSG(dev, pipe, request, requesttype, value, index, data, size, timeout) \
	usb_control_msg((dev), (pipe), (request), (requesttype), (value), (index), \
	(data), (size), (timeout))
#define USB_BUFFER_ALLOC(dev, size, mem, dma)  kmalloc(size, mem)
#define USB_BUFFER_FREE(dev, size, data, dma)  kfree(data)

#ifdef WL_URB_ZPKT
#define URB_QUEUE_BULK   USB_QUEUE_BULK|URB_ZERO_PACKET
#else
#define URB_QUEUE_BULK   0
#endif /*  WL_URB_ZPKT */

#define CALLBACK_ARGS		struct urb *urb
#define CALLBACK_ARGS_DATA	urb
#define CONFIGDESC(usb)		((usb)->actconfig)
#define IFPTR(usb, idx)		(&(usb)->actconfig->interface[idx])
#define IFALTS(usb, idx)	((usb)->actconfig->interface[idx].altsetting[0])
#define IFDESC(usb, idx)	IFALTS((usb), (idx))
#define IFEPDESC(usb, idx, ep)	(IFALTS((usb), (idx)).endpoint[ep])


#endif /* KERNEL26 */

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31))
#define USB_SPEED_SUPER		5
#endif  /* #if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31)) */

#define CONTROL_IF   0
#define BULK_IF      0

#ifdef BCMUSBDEV_COMPOSITE
#define USB_COMPIF_MAX       4

#define USB_CLASS_WIRELESS	0xe0
#define USB_CLASS_MISC		0xef
#define USB_SUBCLASS_COMMON	0x02
#define USB_PROTO_IAD		0x01
#define USB_PROTO_VENDOR	0xff

#define USB_QUIRK_NO_SET_INTF   0x04 /* device does not support set_interface */
#endif /* BCMUSBDEV_COMPOSITE */

#define USB_SYNC_WAIT_TIMEOUT  300  /* ms */

/* Private data kept in skb */
#define SKB_PRIV(skb, idx)  (&((void **)skb->cb)[idx])
#define SKB_PRIV_URB(skb)   (*(struct urb **)SKB_PRIV(skb, 0))

#ifndef DBUS_USB_RXQUEUE_BATCH_ADD
/* items to add each time within limit */
#define DBUS_USB_RXQUEUE_BATCH_ADD            8
#endif

#ifndef DBUS_USB_RXQUEUE_LOWER_WATERMARK
/* add a new batch req to rx queue when waiting item count reduce to this number */
#define DBUS_USB_RXQUEUE_LOWER_WATERMARK      4
#endif

enum usbos_suspend_state {
	USBOS_SUSPEND_STATE_DEVICE_ACTIVE = 0, /* Device is busy, won't allow suspend */
	USBOS_SUSPEND_STATE_SUSPEND_PENDING,   /* Device is idle, can be suspended */
	                                       /* Wating PM to suspend */
	USBOS_SUSPEND_STATE_SUSPENDED          /* Device suspended */
};

enum usbos_request_state {
	USBOS_REQUEST_STATE_UNSCHEDULED = 0,	/* USB TX request not scheduled */
	USBOS_REQUEST_STATE_SCHEDULED,		/* USB TX request given to TX thread */
	USBOS_REQUEST_STATE_SUBMITTED		/* USB TX request submitted */
};

typedef struct {
	uint32 notification;
	uint32 reserved;
} intr_t;

typedef struct {
	dbus_pub_t *pub;

	void *cbarg;
	dbus_intf_callbacks_t *cbs;

	/* Imported */
	struct usb_device *usb;	/* USB device pointer from OS */
	struct urb *intr_urb; /* URB for interrupt endpoint */
	struct list_head req_rxfreeq;
	struct list_head req_txfreeq;
	struct list_head req_rxpostedq;	/* Posted down to USB driver for RX */
	struct list_head req_txpostedq;	/* Posted down to USB driver for TX */
	spinlock_t rxfree_lock; /* Lock for rx free list */
	spinlock_t txfree_lock; /* Lock for tx free list */
	spinlock_t rxposted_lock; /* Lock for rx posted list */
	spinlock_t txposted_lock; /* Lock for tx posted list */
	uint rx_pipe, tx_pipe, intr_pipe, rx_pipe2; /* Pipe numbers for USB I/O */
	uint rxbuf_len;

	struct list_head req_rxpendingq; /* RXDPC: Pending for dpc to send up */
	spinlock_t rxpending_lock;	/* RXDPC: Lock for rx pending list */
	long dpc_pid;
	struct semaphore dpc_sem;
	struct completion dpc_exited;
	int rxpending;

	struct urb               *ctl_urb;
	int                      ctl_in_pipe, ctl_out_pipe;
	struct usb_ctrlrequest   ctl_write;
	struct usb_ctrlrequest   ctl_read;
	struct semaphore         ctl_lock;     /* Lock for CTRL transfers via tx_thread */
#ifdef USBOS_TX_THREAD
	enum usbos_request_state ctl_state;
#endif /* USBOS_TX_THREAD */

	spinlock_t rxlock;      /* Lock for rxq management */
	spinlock_t txlock;      /* Lock for txq management */

	int intr_size;          /* Size of interrupt message */
	int interval;           /* Interrupt polling interval */
	intr_t intr;            /* Data buffer for interrupt endpoint */

	int maxps;
	atomic_t txposted;
	atomic_t rxposted;
	atomic_t txallocated;
	atomic_t rxallocated;
	bool rxctl_deferrespok;	/* Get a response for setup from dongle */

	wait_queue_head_t wait;
	bool waitdone;
	int sync_urb_status;

	struct urb *blk_urb; /* Used for downloading embedded image */

#ifdef USBOS_THREAD
	spinlock_t              ctrl_lock;
	spinlock_t              usbos_list_lock;
	struct list_head        usbos_list;
	struct list_head        usbos_free_list;
	atomic_t                usbos_list_cnt;
	wait_queue_head_t       usbos_queue_head;
	struct task_struct      *usbos_kt;
#endif /* USBOS_THREAD */

#ifdef USBOS_TX_THREAD
	spinlock_t              usbos_tx_list_lock;
	struct list_head	usbos_tx_list;
	wait_queue_head_t	usbos_tx_queue_head;
	struct task_struct      *usbos_tx_kt;
#endif /* USBOS_TX_THREAD */

	struct dma_pool *qtd_pool; /* QTD pool for USB optimization only */
	int tx_ep, rx_ep, rx2_ep;  /* EPs for USB optimization */
	struct usb_device *usb_device; /* USB device for optimization */
} usbos_info_t;

typedef struct urb_req {
	void         *pkt;
	int          buf_len;
	struct urb   *urb;
	void         *arg;
	usbos_info_t *usbinfo;
	struct list_head urb_list;
} urb_req_t;

#ifdef USBOS_THREAD
typedef struct usbos_list_entry {
	struct list_head    list;   /* must be first */
	void               *urb_context;
	int                 urb_length;
	int                 urb_status;
} usbos_list_entry_t;

static void* dbus_usbos_thread_init(usbos_info_t *usbos_info);
static void  dbus_usbos_thread_deinit(usbos_info_t *usbos_info);
static void  dbus_usbos_dispatch_schedule(CALLBACK_ARGS);
static int   dbus_usbos_thread_func(void *data);
#endif /* USBOS_THREAD */

#ifdef USBOS_TX_THREAD
void* dbus_usbos_tx_thread_init(usbos_info_t *usbos_info);
void  dbus_usbos_tx_thread_deinit(usbos_info_t *usbos_info);
int   dbus_usbos_tx_thread_func(void *data);
#endif /* USBOS_TX_THREAD */

/* Shared Function prototypes */
bool dbus_usbos_dl_cmd(usbos_info_t *usbinfo, uint8 cmd, void *buffer, int buflen);
int dbus_usbos_wait(usbos_info_t *usbinfo, uint16 ms);
bool dbus_usbos_dl_send_bulk(usbos_info_t *usbinfo, void *buffer, int len);
int dbus_write_membytes(usbos_info_t *usbinfo, bool set, uint32 address, uint8 *data, uint size);

/* Local function prototypes */
static void dbus_usbos_send_complete(CALLBACK_ARGS);
static void dbus_usbos_recv_complete(CALLBACK_ARGS);
static int  dbus_usbos_errhandler(void *bus, int err);
static int  dbus_usbos_state_change(void *bus, int state);
static void dbusos_stop(usbos_info_t *usbos_info);

#ifdef KERNEL26
static int dbus_usbos_probe(struct usb_interface *intf, const struct usb_device_id *id);
static void dbus_usbos_disconnect(struct usb_interface *intf);
#if defined(USB_SUSPEND_AVAILABLE)
static int dbus_usbos_resume(struct usb_interface *intf);
static int dbus_usbos_suspend(struct usb_interface *intf, pm_message_t message);
/* at the moment, used for full dongle host driver only */
static int dbus_usbos_reset_resume(struct usb_interface *intf);
#endif /* USB_SUSPEND_AVAILABLE */
#else /* KERNEL26 */
static void *dbus_usbos_probe(struct usb_device *usb, unsigned int ifnum,
	const struct usb_device_id *id);
static void dbus_usbos_disconnect(struct usb_device *usb, void *ptr);
#endif /* KERNEL26 */


/**
 * have to disable missing-field-initializers warning as last element {} triggers it
 * and different versions of kernel have different number of members so it is impossible
 * to specify the initializer. BTW issuing the warning here is bug og GCC as  universal
 * zero {0} specified in C99 standard as correct way of initialization of struct to all zeros
 */
#if defined(STRICT_GCC_WARNINGS) && defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == \
	4 && __GNUC_MINOR__ >= 6))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

static struct usb_device_id devid_table[] = {
	{ USB_DEVICE(BCM_DNGL_VID, 0x0000) }, /* Configurable via register() */
#if defined(BCM_REQUEST_FW)
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_4328) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_4322) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_4319) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_43236) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_43143) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_43242) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_4360) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_4350) },
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BL_PID_43569) },
#endif
#ifdef EXTENDED_VID_PID
	EXTENDED_VID_PID,
#endif /* EXTENDED_VID_PID */
	{ USB_DEVICE(BCM_DNGL_VID, BCM_DNGL_BDC_PID) }, /* Default BDC */
	{ }
};

#if defined(STRICT_GCC_WARNINGS) && defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == \
	4 && __GNUC_MINOR__ >= 6))
#pragma GCC diagnostic pop
#endif

MODULE_DEVICE_TABLE(usb, devid_table);

/** functions called by the Linux kernel USB subsystem */
static struct usb_driver dbus_usbdev = {
	name:           "dbus_usbdev",
	probe:          dbus_usbos_probe,
	disconnect:     dbus_usbos_disconnect,
	id_table:       devid_table,
#if defined(USB_SUSPEND_AVAILABLE)
	suspend:        dbus_usbos_suspend,
	resume:         dbus_usbos_resume,
	reset_resume:	dbus_usbos_reset_resume,
	/* Linux USB core will allow autosuspend for devices bound to this driver */
	supports_autosuspend: 1
#endif /* USB_SUSPEND_AVAILABLE */
};

/**
 * This stores USB info during Linux probe callback since attach() is not called yet at this point
 */
typedef struct {
	void    *usbos_info;
	struct usb_device *usb; /* USB device pointer from OS */
	uint    rx_pipe;   /* Pipe numbers for USB I/O */
	uint    tx_pipe;   /* Pipe numbers for USB I/O */
	uint    intr_pipe; /* Pipe numbers for USB I/O */
	uint    rx_pipe2;  /* Pipe numbers for USB I/O */
	int     intr_size; /* Size of interrupt message */
	int     interval;  /* Interrupt polling interval */
	bool    dldone;
	int     vid;
	int     pid;
	bool    dereged;
	bool    disc_cb_done;
	DEVICE_SPEED    device_speed;
	enum usbos_suspend_state suspend_state;
	struct usb_interface     *intf;
} probe_info_t;

/*
 * USB Linux dbus_intf_t
 */
static void *dbus_usbos_intf_attach(dbus_pub_t *pub, void *cbarg, dbus_intf_callbacks_t *cbs);
static void dbus_usbos_intf_detach(dbus_pub_t *pub, void *info);
static int  dbus_usbos_intf_send_irb(void *bus, dbus_irb_tx_t *txirb);
static int  dbus_usbos_intf_recv_irb(void *bus, dbus_irb_rx_t *rxirb);
static int  dbus_usbos_intf_recv_irb_from_ep(void *bus, dbus_irb_rx_t *rxirb, uint32 ep_idx);
static int  dbus_usbos_intf_cancel_irb(void *bus, dbus_irb_tx_t *txirb);
static int  dbus_usbos_intf_send_ctl(void *bus, uint8 *buf, int len);
static int  dbus_usbos_intf_recv_ctl(void *bus, uint8 *buf, int len);
static int  dbus_usbos_intf_get_attrib(void *bus, dbus_attrib_t *attrib);
static int  dbus_usbos_intf_up(void *bus);
static int  dbus_usbos_intf_down(void *bus);
static int  dbus_usbos_intf_stop(void *bus);
static int  dbus_usbos_readreg(void *bus, uint32 regaddr, int datalen, uint32 *value);
extern int dbus_usbos_loopback_tx(void *usbos_info_ptr, int cnt, int size);
int dbus_usbos_writereg(void *bus, uint32 regaddr, int datalen, uint32 data);
static int  dbus_usbos_intf_set_config(void *bus, dbus_config_t *config);
static bool dbus_usbos_intf_recv_needed(void *bus);
static void *dbus_usbos_intf_exec_rxlock(void *bus, exec_cb_t cb, struct exec_parms *args);
static void *dbus_usbos_intf_exec_txlock(void *bus, exec_cb_t cb, struct exec_parms *args);
#ifdef BCMUSBDEV_COMPOSITE
static int dbus_usbos_intf_wlan(struct usb_device *usb);
#endif /* BCMUSBDEV_COMPOSITE */

/** functions called by dbus_usb.c */
static dbus_intf_t dbus_usbos_intf = {
	.attach = dbus_usbos_intf_attach,
	.detach = dbus_usbos_intf_detach,
	.up = dbus_usbos_intf_up,
	.down = dbus_usbos_intf_down,
	.send_irb = dbus_usbos_intf_send_irb,
	.recv_irb = dbus_usbos_intf_recv_irb,
	.cancel_irb = dbus_usbos_intf_cancel_irb,
	.send_ctl = dbus_usbos_intf_send_ctl,
	.recv_ctl = dbus_usbos_intf_recv_ctl,
	.get_stats = NULL,
	.get_attrib = dbus_usbos_intf_get_attrib,
	.remove = NULL,
	.resume = NULL,
	.suspend = NULL,
	.stop = dbus_usbos_intf_stop,
	.reset = NULL,
	.pktget = NULL,
	.pktfree = NULL,
	.iovar_op = NULL,
	.dump = NULL,
	.set_config = dbus_usbos_intf_set_config,
	.get_config = NULL,
	.device_exists = NULL,
	.dlneeded = NULL,
	.dlstart = NULL,
	.dlrun = NULL,
	.recv_needed = dbus_usbos_intf_recv_needed,
	.exec_rxlock = dbus_usbos_intf_exec_rxlock,
	.exec_txlock = dbus_usbos_intf_exec_txlock,

	.tx_timer_init = NULL,
	.tx_timer_start = NULL,
	.tx_timer_stop = NULL,

	.sched_dpc = NULL,
	.lock = NULL,
	.unlock = NULL,
	.sched_probe_cb = NULL,

	.shutdown = NULL,

	.recv_stop = NULL,
	.recv_resume = NULL,

	.recv_irb_from_ep = dbus_usbos_intf_recv_irb_from_ep,
	.readreg = dbus_usbos_readreg
};

static probe_info_t    g_probe_info;
static probe_cb_t      probe_cb = NULL;
static disconnect_cb_t disconnect_cb = NULL;
static void            *probe_arg = NULL;
static void            *disc_arg = NULL;



static volatile int loopback_rx_cnt, loopback_tx_cnt;
int loopback_size;
bool is_loopback_pkt(void *buf);
int matches_loopback_pkt(void *buf);

/**
 * multiple code paths in this file dequeue a URB request, this function makes sure that it happens
 * in a concurrency save manner. Don't call this from a sleepable process context.
 */
static urb_req_t * BCMFASTPATH
dbus_usbos_qdeq(struct list_head *urbreq_q, spinlock_t *lock)
{
	unsigned long flags;
	urb_req_t *req;

	ASSERT(urbreq_q != NULL);

	spin_lock_irqsave(lock, flags);

	if (list_empty(urbreq_q)) {
		req = NULL;
	} else {
		ASSERT(urbreq_q->next != NULL);
		ASSERT(urbreq_q->next != urbreq_q);
#if defined(STRICT_GCC_WARNINGS) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
		req = list_entry(urbreq_q->next, urb_req_t, urb_list);
#if defined(STRICT_GCC_WARNINGS) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
		list_del_init(&req->urb_list);
	}

	spin_unlock_irqrestore(lock, flags);

	return req;
}

static void BCMFASTPATH
dbus_usbos_qenq(struct list_head *urbreq_q, urb_req_t *req, spinlock_t *lock)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);

	list_add_tail(&req->urb_list, urbreq_q);

	spin_unlock_irqrestore(lock, flags);
}

/**
 * multiple code paths in this file remove a URB request from a list, this function makes sure that
 * it happens in a concurrency save manner. Don't call this from a sleepable process context.
 * Is quite similar to dbus_usbos_qdeq(), I wonder why this function is needed.
 */
static void
dbus_usbos_req_del(urb_req_t *req, spinlock_t *lock)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);

	list_del_init(&req->urb_list);

	spin_unlock_irqrestore(lock, flags);
}


/**
 * Driver requires a pool of URBs to operate. This function is called during
 * initialization (attach phase), allocates a number of URBs, and puts them
 * on the free (req_rxfreeq and req_txfreeq) queue
 */
static int
dbus_usbos_urbreqs_alloc(usbos_info_t *usbos_info, uint32 count, bool is_rx)
{
	int i;
	int allocated = 0;
	int err = DBUS_OK;

	for (i = 0; i < count; i++) {
		urb_req_t *req;

		req = MALLOC(usbos_info->pub->osh, sizeof(urb_req_t));
		if (req == NULL) {
			DBUSERR(("%s: MALLOC req failed\n", __FUNCTION__));
			err = DBUS_ERR_NOMEM;
			goto fail;
		}
		bzero(req, sizeof(urb_req_t));

		req->urb = USB_ALLOC_URB();
		if (req->urb == NULL) {
			DBUSERR(("%s: USB_ALLOC_URB req->urb failed\n", __FUNCTION__));
			err = DBUS_ERR_NOMEM;
			goto fail;
		}

		INIT_LIST_HEAD(&req->urb_list);

		if (is_rx) {
#if defined(BCM_RPC_NOCOPY) || defined(BCM_RPC_RXNOCOPY)
			/* don't allocate now. Do it on demand */
			req->pkt = NULL;
#else
			/* pre-allocate  buffers never to be released */
			req->pkt = MALLOC(usbos_info->pub->osh, usbos_info->rxbuf_len);
			if (req->pkt == NULL) {
				DBUSERR(("%s: MALLOC req->pkt failed\n", __FUNCTION__));
				err = DBUS_ERR_NOMEM;
				goto fail;
			}
#endif
			req->buf_len = usbos_info->rxbuf_len;
			dbus_usbos_qenq(&usbos_info->req_rxfreeq, req, &usbos_info->rxfree_lock);
		} else {
			req->buf_len = 0;
			dbus_usbos_qenq(&usbos_info->req_txfreeq, req, &usbos_info->txfree_lock);
		}
		allocated++;
		continue;

fail:
		if (req) {
			if (is_rx && req->pkt) {
#if defined(BCM_RPC_NOCOPY) || defined(BCM_RPC_RXNOCOPY)
				/* req->pkt is NULL in "NOCOPY" mode */
#else
				MFREE(usbos_info->pub->osh, req->pkt, req->buf_len);
#endif
			}
			if (req->urb) {
				USB_FREE_URB(req->urb);
			}
			MFREE(usbos_info->pub->osh, req, sizeof(urb_req_t));
		}
		break;
	}

	atomic_add(allocated, is_rx ? &usbos_info->rxallocated : &usbos_info->txallocated);

	if (is_rx) {
		DBUSTRACE(("%s: add %d (total %d) rx buf, each has %d bytes\n", __FUNCTION__,
			allocated, atomic_read(&usbos_info->rxallocated), usbos_info->rxbuf_len));
	} else {
		DBUSTRACE(("%s: add %d (total %d) tx req\n", __FUNCTION__,
			allocated, atomic_read(&usbos_info->txallocated)));
	}

	return err;
} /* dbus_usbos_urbreqs_alloc */

/** Typically called during detach or when attach failed. Don't call until all URBs unlinked */
static int
dbus_usbos_urbreqs_free(usbos_info_t *usbos_info, bool is_rx)
{
	int rtn = 0;
	urb_req_t *req;
	struct list_head *req_q;
	spinlock_t *lock;

	if (is_rx) {
		req_q = &usbos_info->req_rxfreeq;
		lock = &usbos_info->rxfree_lock;
	} else {
		req_q = &usbos_info->req_txfreeq;
		lock = &usbos_info->txfree_lock;
	}
	while ((req = dbus_usbos_qdeq(req_q, lock)) != NULL) {

		if (is_rx) {
			if (req->pkt) {
				/* We do MFREE instead of PKTFREE because the pkt has been
				 * converted to native already
				 */
				MFREE(usbos_info->pub->osh, req->pkt, req->buf_len);
				req->pkt = NULL;
				req->buf_len = 0;
			}
		} else {
			/* sending req should not be assigned pkt buffer */
			ASSERT(req->pkt == NULL);
		}

		if (req->urb) {
			USB_FREE_URB(req->urb);
			req->urb = NULL;
		}
		MFREE(usbos_info->pub->osh, req, sizeof(urb_req_t));

		rtn++;
	}
	return rtn;
} /* dbus_usbos_urbreqs_free */

/**
 * called by Linux kernel on URB completion. Upper DBUS layer (dbus_usb.c) has to be notified of
 * send completion.
 */
void
dbus_usbos_send_complete(CALLBACK_ARGS)
{
	urb_req_t *req = urb->context;
	dbus_irb_tx_t *txirb = req->arg;
	usbos_info_t *usbos_info = req->usbinfo;
	unsigned long flags;
	int status = DBUS_OK;
	int txposted;

	USB_AUTOPM_PUT_INTERFACE_ASYNC(g_probe_info.intf);

	spin_lock_irqsave(&usbos_info->txlock, flags);

	dbus_usbos_req_del(req, &usbos_info->txposted_lock);
	txposted = atomic_dec_return(&usbos_info->txposted);
	if (unlikely (txposted < 0)) {
		DBUSERR(("%s ERROR: txposted is negative (%d)!!\n", __FUNCTION__, txposted));
	}
	spin_unlock_irqrestore(&usbos_info->txlock, flags);

	if (unlikely (urb->status)) {
		status = DBUS_ERR_TXFAIL;
		DBUSTRACE(("txfail status %d\n", urb->status));
	}

#if defined(BCM_RPC_NOCOPY) || defined(BCM_RPC_RXNOCOPY)
	/* sending req should not be assigned pkt buffer */
	ASSERT(req->pkt == NULL);
#endif
	/*  txirb should always be set, except for ZLP. ZLP is reusing this callback function. */
	if (txirb != NULL) {
		if (txirb->send_buf != NULL) {
			MFREE(usbos_info->pub->osh, txirb->send_buf, req->buf_len);
			txirb->send_buf = NULL;
			req->buf_len = 0;
		}
		if (likely (usbos_info->cbarg && usbos_info->cbs)) {
			if (likely (usbos_info->cbs->send_irb_complete != NULL))
			    usbos_info->cbs->send_irb_complete(usbos_info->cbarg, txirb, status);
		}
	}

	dbus_usbos_qenq(&usbos_info->req_txfreeq, req, &usbos_info->txfree_lock);
} /* dbus_usbos_send_complete */

/**
 * In order to receive USB traffic from the dongle, we need to supply the Linux kernel with a free
 * URB that is going to contain received data.
 */
static int BCMFASTPATH
dbus_usbos_recv_urb_submit(usbos_info_t *usbos_info, dbus_irb_rx_t *rxirb, uint32 ep_idx)
{
	urb_req_t *req;
	int ret = DBUS_OK;
	unsigned long flags;
	void *p;
	uint rx_pipe;
	int rxposted;

	BCM_REFERENCE(rxposted);

	if (!(req = dbus_usbos_qdeq(&usbos_info->req_rxfreeq, &usbos_info->rxfree_lock))) {
		DBUSTRACE(("%s No free URB!\n", __FUNCTION__));
		return DBUS_ERR_RXDROP;
	}

	spin_lock_irqsave(&usbos_info->rxlock, flags);

#if defined(BCM_RPC_NOCOPY) || defined(BCM_RPC_RXNOCOPY)
	req->pkt = rxirb->pkt = PKTGET(usbos_info->pub->osh, req->buf_len, FALSE);
	if (!rxirb->pkt) {
		DBUSERR(("%s: PKTGET failed\n", __FUNCTION__));
		dbus_usbos_qenq(&usbos_info->req_rxfreeq, req, &usbos_info->rxfree_lock);
		ret = DBUS_ERR_RXDROP;
		goto fail;
	}
	/* consider the packet "native" so we don't count it as MALLOCED in the osl */
	PKTTONATIVE(usbos_info->pub->osh, req->pkt);
	rxirb->buf = NULL;
	p = PKTDATA(usbos_info->pub->osh, req->pkt);
#else
	if (req->buf_len != usbos_info->rxbuf_len) {
		ASSERT(req->pkt);
		MFREE(usbos_info->pub->osh, req->pkt, req->buf_len);
		DBUSTRACE(("%s: replace rx buff: old len %d, new len %d\n", __FUNCTION__,
			req->buf_len, usbos_info->rxbuf_len));
		req->buf_len = 0;
		req->pkt = MALLOC(usbos_info->pub->osh, usbos_info->rxbuf_len);
		if (req->pkt == NULL) {
			DBUSERR(("%s: MALLOC req->pkt failed\n", __FUNCTION__));
			ret = DBUS_ERR_NOMEM;
			goto fail;
		}
		req->buf_len = usbos_info->rxbuf_len;
	}
	rxirb->buf = req->pkt;
	p = rxirb->buf;
#endif /* defined(BCM_RPC_NOCOPY) */
	rxirb->buf_len = req->buf_len;
	req->usbinfo = usbos_info;
	req->arg = rxirb;
	if (ep_idx == 0) {
		rx_pipe = usbos_info->rx_pipe;
	} else {
		rx_pipe = usbos_info->rx_pipe2;
		ASSERT(usbos_info->rx_pipe2);
	}
	/* Prepare the URB */
	usb_fill_bulk_urb(req->urb, usbos_info->usb, rx_pipe,
		p,
		rxirb->buf_len,
		(usb_complete_t)dbus_usbos_recv_complete, req);
		req->urb->transfer_flags |= URB_QUEUE_BULK;

	if ((ret = USB_SUBMIT_URB(req->urb))) {
		DBUSERR(("%s USB_SUBMIT_URB failed. status %d\n", __FUNCTION__, ret));
		dbus_usbos_qenq(&usbos_info->req_rxfreeq, req, &usbos_info->rxfree_lock);
		ret = DBUS_ERR_RXFAIL;
		goto fail;
	}
	rxposted = atomic_inc_return(&usbos_info->rxposted);

	dbus_usbos_qenq(&usbos_info->req_rxpostedq, req, &usbos_info->rxposted_lock);
fail:
	spin_unlock_irqrestore(&usbos_info->rxlock, flags);
	return ret;
} /* dbus_usbos_recv_urb_submit */


/**
 * Called by worked thread when a 'receive URB' completed or Linux kernel when it returns a URB to
 * this driver.
 */
static void BCMFASTPATH
dbus_usbos_recv_complete_handle(urb_req_t *req, int len, int status)
{
	dbus_irb_rx_t *rxirb = req->arg;
	usbos_info_t *usbos_info = req->usbinfo;
	unsigned long flags;
	int rxallocated, rxposted;
	int dbus_status = DBUS_OK;
	bool killed = (g_probe_info.suspend_state == USBOS_SUSPEND_STATE_SUSPEND_PENDING) ? 1 : 0;

	spin_lock_irqsave(&usbos_info->rxlock, flags);
	dbus_usbos_req_del(req, &usbos_info->rxposted_lock);
	rxposted = atomic_dec_return(&usbos_info->rxposted);
	rxallocated = atomic_read(&usbos_info->rxallocated);
	spin_unlock_irqrestore(&usbos_info->rxlock, flags);

	if ((rxallocated < usbos_info->pub->nrxq) && (!status) &&
		(rxposted == DBUS_USB_RXQUEUE_LOWER_WATERMARK)) {
			DBUSTRACE(("%s: need more rx buf: rxallocated %d rxposted %d!\n",
				__FUNCTION__, rxallocated, rxposted));
			dbus_usbos_urbreqs_alloc(usbos_info,
				MIN(DBUS_USB_RXQUEUE_BATCH_ADD,
				usbos_info->pub->nrxq - rxallocated), TRUE);
	}

	/* Handle errors */
	if (status) {
		/*
		 * Linux 2.4 disconnect: -ENOENT or -EILSEQ for CRC error; rmmod: -ENOENT
		 * Linux 2.6 disconnect: -EPROTO, rmmod: -ESHUTDOWN
		 */
		if ((status == -ENOENT && (!killed))|| status == -ESHUTDOWN) {
			/* NOTE: unlink() can not be called from URB callback().
			 * Do not call dbusos_stop() here.
			 */
			DBUSTRACE(("%s rx error %d\n", __FUNCTION__, status));
			dbus_usbos_state_change(usbos_info, DBUS_STATE_DOWN);
		} else if (status == -EPROTO) {
			DBUSTRACE(("%s rx error %d\n", __FUNCTION__, status));
		} else if (killed && (status == -EHOSTUNREACH || status == -ENOENT)) {
			/* Device is suspended */
		} else {
			DBUSTRACE(("%s rx error %d\n", __FUNCTION__, status));
			dbus_usbos_errhandler(usbos_info, DBUS_ERR_RXFAIL);
		}

		/* On error, don't submit more URBs yet */
		rxirb->buf = NULL;
		rxirb->actual_len = 0;
		dbus_status = DBUS_ERR_RXFAIL;
		goto fail;
	}

	/* Make the skb represent the received urb */
	rxirb->actual_len = len;

	if (rxirb->actual_len < sizeof(uint32)) {
		DBUSTRACE(("small pkt len %d, process as ZLP\n", rxirb->actual_len));
		dbus_status = DBUS_ERR_RXZLP;
	}

fail:
#if defined(BCM_RPC_NOCOPY) || defined(BCM_RPC_RXNOCOPY)
	/* detach the packet from the queue */
	req->pkt = NULL;
#endif /* BCM_RPC_NOCOPY || BCM_RPC_RXNOCOPY */

	if (usbos_info->cbarg && usbos_info->cbs) {
		if (usbos_info->cbs->recv_irb_complete) {
			usbos_info->cbs->recv_irb_complete(usbos_info->cbarg, rxirb, dbus_status);
		}
	}

	dbus_usbos_qenq(&usbos_info->req_rxfreeq, req, &usbos_info->rxfree_lock);

	/* Mark the interface as busy to reset USB autosuspend timer */
	USB_MARK_LAST_BUSY(usbos_info->usb);
} /* dbus_usbos_recv_complete_handle */

/** called by Linux kernel when it returns a URB to this driver */
static void
dbus_usbos_recv_complete(CALLBACK_ARGS)
{
#ifdef USBOS_THREAD
	dbus_usbos_dispatch_schedule(CALLBACK_ARGS_DATA);
#else /*  !USBOS_THREAD */
	dbus_usbos_recv_complete_handle(urb->context, urb->actual_length, urb->status);
#endif /*  USBOS_THREAD */
}


/**
 * If Linux notifies our driver that a control read or write URB has completed, we should notify
 * the DBUS layer above us (dbus_usb.c in this case).
 */
static void
dbus_usbos_ctl_complete(usbos_info_t *usbos_info, int type, int urbstatus)
{
	int status = DBUS_ERR;

	if (usbos_info == NULL)
		return;

	switch (urbstatus) {
		case 0:
			status = DBUS_OK;
		break;
		case -EINPROGRESS:
		case -ENOENT:
		default:
#ifdef INTR_EP_ENABLE
			DBUSERR(("%s:%d fail status %d bus:%d susp:%d intr:%d ctli:%d ctlo:%d\n",
				__FUNCTION__, type, urbstatus,
				usbos_info->pub->busstate, g_probe_info.suspend_state,
				usbos_info->intr_urb_submitted, usbos_info->ctlin_urb_submitted,
				usbos_info->ctlout_urb_submitted));
#else
			DBUSERR(("%s: failed with status %d\n", __FUNCTION__, urbstatus));
			status = DBUS_ERR;
		break;
#endif /* INTR_EP_ENABLE */
	}

	if (usbos_info->cbarg && usbos_info->cbs) {
		if (usbos_info->cbs->ctl_complete)
			usbos_info->cbs->ctl_complete(usbos_info->cbarg, type, status);
	}
}

/** called by Linux */
static void
dbus_usbos_ctlread_complete(CALLBACK_ARGS)
{
	usbos_info_t *usbos_info = (usbos_info_t *)urb->context;

	ASSERT(urb);
	usbos_info = (usbos_info_t *)urb->context;

	dbus_usbos_ctl_complete(usbos_info, DBUS_CBCTL_READ, urb->status);

#ifdef USBOS_THREAD
	if (usbos_info->rxctl_deferrespok) {
		usbos_info->ctl_read.bRequestType = USB_DIR_IN | USB_TYPE_CLASS |
		USB_RECIP_INTERFACE;
		usbos_info->ctl_read.bRequest = 1;
	}
#endif

	up(&usbos_info->ctl_lock);

	USB_AUTOPM_PUT_INTERFACE_ASYNC(g_probe_info.intf);
}

/** called by Linux */
static void
dbus_usbos_ctlwrite_complete(CALLBACK_ARGS)
{
	usbos_info_t *usbos_info = (usbos_info_t *)urb->context;

	ASSERT(urb);
	usbos_info = (usbos_info_t *)urb->context;

	dbus_usbos_ctl_complete(usbos_info, DBUS_CBCTL_WRITE, urb->status);

#ifdef USBOS_TX_THREAD
	usbos_info->ctl_state = USBOS_REQUEST_STATE_UNSCHEDULED;
#endif /* USBOS_TX_THREAD */

	up(&usbos_info->ctl_lock);

	USB_AUTOPM_PUT_INTERFACE_ASYNC(g_probe_info.intf);
}

#ifdef INTR_EP_ENABLE
/** called by Linux */
static void
dbus_usbos_intr_complete(CALLBACK_ARGS)
{
	usbos_info_t *usbos_info = (usbos_info_t *)urb->context;
	bool killed = (g_probe_info.suspend_state == USBOS_SUSPEND_STATE_SUSPEND_PENDING) ? 1 : 0;

	if (usbos_info == NULL || usbos_info->pub == NULL)
		return;
	if ((urb->status == -ENOENT && (!killed)) || urb->status == -ESHUTDOWN ||
		urb->status == -ENODEV) {
		dbus_usbos_state_change(usbos_info, DBUS_STATE_DOWN);
	}

	if (usbos_info->pub->busstate == DBUS_STATE_DOWN) {
		DBUSERR(("%s: intr cb when DBUS down, ignoring\n", __FUNCTION__));
		return;
	}
	dbus_usbos_ctl_complete(usbos_info, DBUS_CBINTR_POLL, urb->status);
}
#endif	/* INTR_EP_ENABLE */

/**
 * when the bus is going to sleep or halt, the Linux kernel requires us to take ownership of our
 * URBs again. Multiple code paths in this file require a list of URBs to be cancelled in a
 * concurrency save manner.
 */
static void
dbus_usbos_unlink(struct list_head *urbreq_q, spinlock_t *lock)
{
	urb_req_t *req;

	/* dbus_usbos_recv_complete() adds req back to req_freeq */
	while ((req = dbus_usbos_qdeq(urbreq_q, lock)) != NULL) {
		ASSERT(req->urb != NULL);
		USB_UNLINK_URB(req->urb);
	}
}

/** multiple code paths in this file require the bus to stop */
static void
dbus_usbos_cancel_all_urbs(usbos_info_t *usbos_info)
{
	int rxposted, txposted;

	DBUSTRACE(("%s: unlink all URBs\n", __FUNCTION__));

#ifdef USBOS_TX_THREAD
	usbos_info->ctl_state = USBOS_REQUEST_STATE_UNSCHEDULED;

	/* Yield the CPU to TX thread so all pending requests are submitted */
	while (!list_empty(&usbos_info->usbos_tx_list)) {
		wake_up_interruptible(&usbos_info->usbos_tx_queue_head);
		OSL_SLEEP(10);
	}
#endif /* USBOS_TX_THREAD */

	/* tell Linux kernel to cancel a single intr, ctl and blk URB */
	if (usbos_info->intr_urb)
		USB_UNLINK_URB(usbos_info->intr_urb);
	if (usbos_info->ctl_urb)
		USB_UNLINK_URB(usbos_info->ctl_urb);
	if (usbos_info->blk_urb)
		USB_UNLINK_URB(usbos_info->blk_urb);

	dbus_usbos_unlink(&usbos_info->req_txpostedq, &usbos_info->txposted_lock);
	dbus_usbos_unlink(&usbos_info->req_rxpostedq, &usbos_info->rxposted_lock);

	/* Wait until the callbacks for all submitted URBs have been called, because the
	 * handler needs to know is an USB suspend is in progress.
	 */
	SPINWAIT((atomic_read(&usbos_info->txposted) != 0 ||
		atomic_read(&usbos_info->rxposted) != 0), 10000);

	txposted = atomic_read(&usbos_info->txposted);
	rxposted = atomic_read(&usbos_info->rxposted);
	if (txposted != 0 || rxposted != 0) {
		DBUSERR(("%s ERROR: REQs posted, rx=%d tx=%d!\n",
			__FUNCTION__, rxposted, txposted));
	}
} /* dbus_usbos_cancel_all_urbs */

/** multiple code paths require the bus to stop */
static void
dbusos_stop(usbos_info_t *usbos_info)
{
	urb_req_t *req;
	int rxposted;
	req = NULL;
	BCM_REFERENCE(req);

	ASSERT(usbos_info);

	dbus_usbos_state_change(usbos_info, DBUS_STATE_DOWN);

	dbus_usbos_cancel_all_urbs(usbos_info);

#ifdef USBOS_THREAD
	/* yield the CPU to rx packet thread */
	while (1) {
		if (atomic_read(&usbos_info->usbos_list_cnt) <= 0)	break;
		wake_up_interruptible(&usbos_info->usbos_queue_head);
		OSL_SLEEP(3);
	}
#endif /* USBOS_THREAD */

	rxposted = atomic_read(&usbos_info->rxposted);
	if (rxposted > 0) {
		DBUSERR(("%s ERROR: rx REQs posted=%d in stop!\n", __FUNCTION__,
			rxposted));
	}

	ASSERT(atomic_read(&usbos_info->txposted) == 0 && rxposted == 0);

} /* dbusos_stop */

#if defined(USB_SUSPEND_AVAILABLE)

/**
 * Linux kernel sports a 'USB auto suspend' feature. See: http://lwn.net/Articles/373550/
 * The suspend method is called by the Linux kernel to warn the driver that the device is going to
 * be suspended.  If the driver returns a negative error code, the suspend will be aborted. If the
 * driver returns 0, it must cancel all outstanding URBs (usb_kill_urb()) and not submit any more.
 */
static int
dbus_usbos_suspend(struct usb_interface *intf,
            pm_message_t message)
{
	DBUSERR(("%s suspend state: %d\n", __FUNCTION__, g_probe_info.suspend_state));
	/* DHD for full dongle model */
	g_probe_info.suspend_state = USBOS_SUSPEND_STATE_SUSPEND_PENDING;
	dbus_usbos_state_change((usbos_info_t*)g_probe_info.usbos_info, DBUS_STATE_SLEEP);
	dbus_usbos_cancel_all_urbs((usbos_info_t*)g_probe_info.usbos_info);
	g_probe_info.suspend_state = USBOS_SUSPEND_STATE_SUSPENDED;

	return 0;
}

/**
 * The resume method is called to tell the driver that the device has been resumed and the driver
 * can return to normal operation.  URBs may once more be submitted.
 */
static int dbus_usbos_resume(struct usb_interface *intf)
{
	DBUSERR(("%s Device resumed\n", __FUNCTION__));

	dbus_usbos_state_change((usbos_info_t*)g_probe_info.usbos_info, DBUS_STATE_UP);
	g_probe_info.suspend_state = USBOS_SUSPEND_STATE_DEVICE_ACTIVE;
	return 0;
}

/**
* This function is directly called by the Linux kernel, when the suspended device has been reset
* instead of being resumed
*/
static int dbus_usbos_reset_resume(struct usb_interface *intf)
{
	DBUSERR(("%s Device reset resumed\n", __FUNCTION__));

	/* The device may have lost power, so a firmware download may be required */
	dbus_usbos_state_change((usbos_info_t*)g_probe_info.usbos_info, DBUS_STATE_DL_NEEDED);
	g_probe_info.suspend_state = USBOS_SUSPEND_STATE_DEVICE_ACTIVE;
	return 0;
}

#endif /* USB_SUSPEND_AVAILABLE */

/**
 * Called by Linux kernel at initialization time, kernel wants to know if our driver will accept the
 * caller supplied USB interface. Note that USB drivers are bound to interfaces, and not to USB
 * devices.
 */
#ifdef KERNEL26
#define DBUS_USBOS_PROBE() static int dbus_usbos_probe(struct usb_interface *intf, const struct usb_device_id *id)
#define DBUS_USBOS_DISCONNECT() static void dbus_usbos_disconnect(struct usb_interface *intf)
#else
#define DBUS_USBOS_PROBE() static void * dbus_usbos_probe(struct usb_device *usb, unsigned int ifnum, const struct usb_device_id *id)
#define DBUS_USBOS_DISCONNECT() static void dbus_usbos_disconnect(struct usb_device *usb, void *ptr)
#endif /* KERNEL26 */

DBUS_USBOS_PROBE()
{
	int ep;
	struct usb_endpoint_descriptor *endpoint;
	int ret = 0;
#ifdef KERNEL26
	struct usb_device *usb = interface_to_usbdev(intf);
#else
	int claimed = 0;
#endif
	int num_of_eps;
#ifdef BCMUSBDEV_COMPOSITE
	int wlan_if = -1;
	bool intr_ep = FALSE;
#endif /* BCMUSBDEV_COMPOSITE */
	wifi_adapter_info_t *adapter;

	DHD_MUTEX_LOCK();

	DBUSERR(("%s: bus num(busnum)=%d, slot num (portnum)=%d\n", __FUNCTION__,
		usb->bus->busnum, usb->portnum));
	adapter = dhd_wifi_platform_attach_adapter(USB_BUS, usb->bus->busnum,
		usb->portnum, WIFI_STATUS_POWER_ON);
	if (adapter == NULL) {
		DBUSERR(("%s: can't find adapter info for this chip\n", __FUNCTION__));
		goto fail;
	}

#ifdef BCMUSBDEV_COMPOSITE
	wlan_if = dbus_usbos_intf_wlan(usb);
#ifdef KERNEL26
	if ((wlan_if >= 0) && (IFPTR(usb, wlan_if) == intf))
#else
	if (wlan_if == ifnum)
#endif /* KERNEL26 */
	{
#endif /* BCMUSBDEV_COMPOSITE */
		g_probe_info.usb = usb;
		g_probe_info.dldone = TRUE;
#ifdef BCMUSBDEV_COMPOSITE
	} else {
		DBUSTRACE(("dbus_usbos_probe: skip probe for non WLAN interface\n"));
		ret = BCME_UNSUPPORTED;
		goto fail;
	}
#endif /* BCMUSBDEV_COMPOSITE */

#ifdef KERNEL26
	g_probe_info.intf = intf;
#endif /* KERNEL26 */

#ifdef BCMUSBDEV_COMPOSITE
	if (IFDESC(usb, wlan_if).bInterfaceNumber > USB_COMPIF_MAX)
#else
	if (IFDESC(usb, CONTROL_IF).bInterfaceNumber)
#endif /* BCMUSBDEV_COMPOSITE */
	{
		ret = -1;
		goto fail;
	}
	if (id != NULL) {
		g_probe_info.vid = id->idVendor;
		g_probe_info.pid = id->idProduct;
	}

#ifdef KERNEL26
	usb_set_intfdata(intf, &g_probe_info);
#endif

	/* Check that the device supports only one configuration */
	if (usb->descriptor.bNumConfigurations != 1) {
		ret = -1;
		goto fail;
	}

	if (usb->descriptor.bDeviceClass != USB_CLASS_VENDOR_SPEC) {
#ifdef BCMUSBDEV_COMPOSITE
		if ((usb->descriptor.bDeviceClass != USB_CLASS_MISC) &&
			(usb->descriptor.bDeviceClass != USB_CLASS_WIRELESS)) {
#endif /* BCMUSBDEV_COMPOSITE */
			ret = -1;
			goto fail;
#ifdef BCMUSBDEV_COMPOSITE
		}
#endif /* BCMUSBDEV_COMPOSITE */
	}

	/*
	 * Only the BDC interface configuration is supported:
	 *	Device class: USB_CLASS_VENDOR_SPEC
	 *	if0 class: USB_CLASS_VENDOR_SPEC
	 *	if0/ep0: control
	 *	if0/ep1: bulk in
	 *	if0/ep2: bulk out (ok if swapped with bulk in)
	 */
	if (CONFIGDESC(usb)->bNumInterfaces != 1) {
#ifdef BCMUSBDEV_COMPOSITE
		if (CONFIGDESC(usb)->bNumInterfaces > USB_COMPIF_MAX) {
#endif /* BCMUSBDEV_COMPOSITE */
			ret = -1;
			goto fail;
#ifdef BCMUSBDEV_COMPOSITE
		}
#endif /* BCMUSBDEV_COMPOSITE */
	}

	/* Check interface */
#ifndef KERNEL26
#ifdef BCMUSBDEV_COMPOSITE
	if (usb_interface_claimed(IFPTR(usb, wlan_if)))
#else
	if (usb_interface_claimed(IFPTR(usb, CONTROL_IF)))
#endif /* BCMUSBDEV_COMPOSITE */
	{
		ret = -1;
		goto fail;
	}
#endif /* !KERNEL26 */

#ifdef BCMUSBDEV_COMPOSITE
	if ((IFDESC(usb, wlan_if).bInterfaceClass != USB_CLASS_VENDOR_SPEC ||
		IFDESC(usb, wlan_if).bInterfaceSubClass != 2 ||
		IFDESC(usb, wlan_if).bInterfaceProtocol != 0xff) &&
		(IFDESC(usb, wlan_if).bInterfaceClass != USB_CLASS_MISC ||
		IFDESC(usb, wlan_if).bInterfaceSubClass != USB_SUBCLASS_COMMON ||
		IFDESC(usb, wlan_if).bInterfaceProtocol != USB_PROTO_IAD))
#else
	if (IFDESC(usb, CONTROL_IF).bInterfaceClass != USB_CLASS_VENDOR_SPEC ||
		IFDESC(usb, CONTROL_IF).bInterfaceSubClass != 2 ||
		IFDESC(usb, CONTROL_IF).bInterfaceProtocol != 0xff)
#endif /* BCMUSBDEV_COMPOSITE */
	{
#ifdef BCMUSBDEV_COMPOSITE
			DBUSERR(("%s: invalid control interface: class %d, subclass %d, proto %d\n",
				__FUNCTION__,
				IFDESC(usb, wlan_if).bInterfaceClass,
				IFDESC(usb, wlan_if).bInterfaceSubClass,
				IFDESC(usb, wlan_if).bInterfaceProtocol));
#else
			DBUSERR(("%s: invalid control interface: class %d, subclass %d, proto %d\n",
				__FUNCTION__,
				IFDESC(usb, CONTROL_IF).bInterfaceClass,
				IFDESC(usb, CONTROL_IF).bInterfaceSubClass,
				IFDESC(usb, CONTROL_IF).bInterfaceProtocol));
#endif /* BCMUSBDEV_COMPOSITE */
			ret = -1;
			goto fail;
	}

	/* Check control endpoint */
#ifdef BCMUSBDEV_COMPOSITE
	endpoint = &IFEPDESC(usb, wlan_if, 0);
#else
	endpoint = &IFEPDESC(usb, CONTROL_IF, 0);
#endif /* BCMUSBDEV_COMPOSITE */
	if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT) {
#ifdef BCMUSBDEV_COMPOSITE
		if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
			USB_ENDPOINT_XFER_BULK) {
#endif /* BCMUSBDEV_COMPOSITE */
			DBUSERR(("%s: invalid control endpoint %d\n",
				__FUNCTION__, endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK));
			ret = -1;
			goto fail;
#ifdef BCMUSBDEV_COMPOSITE
		}
#endif /* BCMUSBDEV_COMPOSITE */
	}

#ifdef BCMUSBDEV_COMPOSITE
	if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT) {
#endif /* BCMUSBDEV_COMPOSITE */
		g_probe_info.intr_pipe =
			usb_rcvintpipe(usb, endpoint->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
#ifdef BCMUSBDEV_COMPOSITE
		intr_ep = TRUE;
	}
#endif /* BCMUSBDEV_COMPOSITE */

#ifndef KERNEL26
	/* Claim interface */
#ifdef BCMUSBDEV_COMPOSITE
	usb_driver_claim_interface(&dbus_usbdev, IFPTR(usb, wlan_if), &g_probe_info);
#else
	usb_driver_claim_interface(&dbus_usbdev, IFPTR(usb, CONTROL_IF), &g_probe_info);
#endif /* BCMUSBDEV_COMPOSITE */
	claimed = 1;
#endif /* !KERNEL26 */
	g_probe_info.rx_pipe = 0;
	g_probe_info.rx_pipe2 = 0;
	g_probe_info.tx_pipe = 0;
#ifdef BCMUSBDEV_COMPOSITE
	if (intr_ep)
		ep = 1;
	else
		ep = 0;
	num_of_eps = IFDESC(usb, wlan_if).bNumEndpoints - 1;
#else
	num_of_eps = IFDESC(usb, BULK_IF).bNumEndpoints - 1;
#endif /* BCMUSBDEV_COMPOSITE */

	if ((num_of_eps != 2) && (num_of_eps != 3)) {
#ifdef BCMUSBDEV_COMPOSITE
		if (num_of_eps > 7)
#endif /* BCMUSBDEV_COMPOSITE */
			ASSERT(0);
	}
	/* Check data endpoints and get pipes */
#ifdef BCMUSBDEV_COMPOSITE
	for (; ep <= num_of_eps; ep++)
#else
	for (ep = 1; ep <= num_of_eps; ep++)
#endif /* BCMUSBDEV_COMPOSITE */
	{
#ifdef BCMUSBDEV_COMPOSITE
		endpoint = &IFEPDESC(usb, wlan_if, ep);
#else
		endpoint = &IFEPDESC(usb, BULK_IF, ep);
#endif /* BCMUSBDEV_COMPOSITE */
		if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
		    USB_ENDPOINT_XFER_BULK) {
			DBUSERR(("%s: invalid data endpoint %d\n",
			           __FUNCTION__, ep));
			ret = -1;
			goto fail;
		}

		if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN) {
			/* direction: dongle->host */
			if (!g_probe_info.rx_pipe) {
				g_probe_info.rx_pipe = usb_rcvbulkpipe(usb,
					(endpoint->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK));
			} else {
				g_probe_info.rx_pipe2 = usb_rcvbulkpipe(usb,
					(endpoint->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK));
			}

		} else
			g_probe_info.tx_pipe = usb_sndbulkpipe(usb, (endpoint->bEndpointAddress &
			     USB_ENDPOINT_NUMBER_MASK));
	}

	/* Allocate interrupt URB and data buffer */
	/* RNDIS says 8-byte intr, our old drivers used 4-byte */
#ifdef BCMUSBDEV_COMPOSITE
	g_probe_info.intr_size = (IFEPDESC(usb, wlan_if, 0).wMaxPacketSize == 16) ? 8 : 4;
	g_probe_info.interval = IFEPDESC(usb, wlan_if, 0).bInterval;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 21))
	usb->quirks |= USB_QUIRK_NO_SET_INTF;
#endif
#else
	g_probe_info.intr_size = (IFEPDESC(usb, CONTROL_IF, 0).wMaxPacketSize == 16) ? 8 : 4;
	g_probe_info.interval = IFEPDESC(usb, CONTROL_IF, 0).bInterval;
#endif /* BCMUSBDEV_COMPOSITE */

#ifndef KERNEL26
	/* usb_fill_int_urb does the interval decoding in 2.6 */
	if (usb->speed == USB_SPEED_HIGH)
		g_probe_info.interval = 1 << (g_probe_info.interval - 1);
#endif
	if (usb->speed == USB_SPEED_SUPER) {
		g_probe_info.device_speed = SUPER_SPEED;
		DBUSERR(("super speed device detected\n"));
	} else if (usb->speed == USB_SPEED_HIGH) {
		g_probe_info.device_speed = HIGH_SPEED;
		DBUSERR(("high speed device detected\n"));
	} else {
		g_probe_info.device_speed = FULL_SPEED;
		DBUSERR(("full speed device detected\n"));
	}
	if (g_probe_info.dereged == FALSE && probe_cb) {
		disc_arg = probe_cb(probe_arg, "", USB_BUS, usb->bus->busnum, usb->portnum, 0);
	}

	g_probe_info.disc_cb_done = FALSE;

#ifdef KERNEL26
	intf->needs_remote_wakeup = 1;
#endif /* KERNEL26 */
	DHD_MUTEX_UNLOCK();

	/* Success */
#ifdef KERNEL26
	return DBUS_OK;
#else
	usb_inc_dev_use(usb);
	return &g_probe_info;
#endif

fail:
	printf("%s: Exit ret=%d\n", __FUNCTION__, ret);
#ifdef BCMUSBDEV_COMPOSITE
	if (ret != BCME_UNSUPPORTED)
#endif /* BCMUSBDEV_COMPOSITE */
		DBUSERR(("%s: failed with errno %d\n", __FUNCTION__, ret));
#ifndef KERNEL26
	if (claimed)
#ifdef BCMUSBDEV_COMPOSITE
		usb_driver_release_interface(&dbus_usbdev, IFPTR(usb, wlan_if));
#else
		usb_driver_release_interface(&dbus_usbdev, IFPTR(usb, CONTROL_IF));
#endif /* BCMUSBDEV_COMPOSITE */
#endif /* !KERNEL26 */

	DHD_MUTEX_UNLOCK();
#ifdef KERNEL26
	usb_set_intfdata(intf, NULL);
	return ret;
#else
	return NULL;
#endif
} /* dbus_usbos_probe */

/** Called by Linux kernel, is the counter part of dbus_usbos_probe() */
DBUS_USBOS_DISCONNECT()
{
#ifdef KERNEL26
	struct usb_device *usb = interface_to_usbdev(intf);
	probe_info_t *probe_usb_init_data = usb_get_intfdata(intf);
#else
	probe_info_t *probe_usb_init_data = (probe_info_t *) ptr;
#endif
	usbos_info_t *usbos_info;

	DHD_MUTEX_LOCK();

	DBUSERR(("%s: bus num(busnum)=%d, slot num (portnum)=%d\n", __FUNCTION__,
		usb->bus->busnum, usb->portnum));

	if (probe_usb_init_data) {
		usbos_info = (usbos_info_t *) probe_usb_init_data->usbos_info;
		if (usbos_info) {
			if ((probe_usb_init_data->dereged == FALSE) && disconnect_cb && disc_arg) {
				disconnect_cb(disc_arg);
				disc_arg = NULL;
				probe_usb_init_data->disc_cb_done = TRUE;
			}
		}
	}

	if (usb) {
#ifndef KERNEL26
#ifdef BCMUSBDEV_COMPOSITE
		usb_driver_release_interface(&dbus_usbdev, IFPTR(usb, wlan_if));
#else
		usb_driver_release_interface(&dbus_usbdev, IFPTR(usb, CONTROL_IF));
#endif /* BCMUSBDEV_COMPOSITE */
		usb_dec_dev_use(usb);
#endif /* !KERNEL26 */
	}
	DHD_MUTEX_UNLOCK();
} /* dbus_usbos_disconnect */

#define LOOPBACK_PKT_START 0xBABE1234

bool is_loopback_pkt(void *buf)
{

	uint32 *buf_ptr = (uint32 *) buf;

	if (*buf_ptr == LOOPBACK_PKT_START)
		return TRUE;
	return FALSE;

}

int matches_loopback_pkt(void *buf)
{
	int i, j;
	unsigned char *cbuf = (unsigned char *) buf;

	for (i = 4; i < loopback_size; i++) {
		if (cbuf[i] != (i % 256)) {
			printf("%s: mismatch at i=%d %d : ", __FUNCTION__, i, cbuf[i]);
			for (j = i; ((j < i+ 16) && (j < loopback_size)); j++) {
				printf("%d ", cbuf[j]);
			}
			printf("\n");
			return 0;
		}
	}
	loopback_rx_cnt++;
	return 1;
}

int dbus_usbos_loopback_tx(void *usbos_info_ptr, int cnt, int size)
{
	usbos_info_t *usbos_info = (usbos_info_t *) usbos_info_ptr;
	unsigned char *buf;
	int j;
	void* p = NULL;
	int rc, last_rx_cnt;
	int tx_failed_cnt;
	int max_size = 1650;
	int usb_packet_size = 512;
	int min_packet_size = 10;

	if (size % usb_packet_size == 0) {
		size = size - 1;
		DBUSERR(("%s: overriding size=%d \n", __FUNCTION__, size));
	}

	if (size < min_packet_size) {
		size = min_packet_size;
		DBUSERR(("%s: overriding size=%d\n", __FUNCTION__, min_packet_size));
	}
	if (size > max_size) {
		size = max_size;
		DBUSERR(("%s: overriding size=%d\n", __FUNCTION__, max_size));
	}

	loopback_tx_cnt = 0;
	loopback_rx_cnt = 0;
	tx_failed_cnt = 0;
	loopback_size   = size;

	while (loopback_tx_cnt < cnt) {
		uint32 *x;
		int pkt_size = loopback_size;

		p = PKTGET(usbos_info->pub->osh, pkt_size, TRUE);
		if (p == NULL) {
			DBUSERR(("%s:%d Failed to allocate packet sz=%d\n",
			       __FUNCTION__, __LINE__, pkt_size));
			return BCME_ERROR;
		}
		x = (uint32*) PKTDATA(usbos_info->pub->osh, p);
		*x = LOOPBACK_PKT_START;
		buf = (unsigned char*) x;
		for (j = 4; j < pkt_size; j++) {
			buf[j] = j % 256;
		}
		rc = dbus_send_buf(usbos_info->pub, buf, pkt_size, p);
		if (rc != BCME_OK) {
			DBUSERR(("%s:%d Freeing packet \n", __FUNCTION__, __LINE__));
			PKTFREE(usbos_info->pub->osh, p, TRUE);
			dbus_usbos_wait(usbos_info, 1);
			tx_failed_cnt++;
		} else {
			loopback_tx_cnt++;
			tx_failed_cnt = 0;
		}
		if (tx_failed_cnt == 5) {
			DBUSERR(("%s : Failed to send loopback packets cnt=%d loopback_tx_cnt=%d\n",
			 __FUNCTION__, cnt, loopback_tx_cnt));
			break;
		}
	}
	printf("Transmitted %d loopback packets of size %d\n", loopback_tx_cnt, loopback_size);

	last_rx_cnt = loopback_rx_cnt;
	while (loopback_rx_cnt < loopback_tx_cnt) {
		dbus_usbos_wait(usbos_info, 1);
		if (loopback_rx_cnt <= last_rx_cnt) {
			DBUSERR(("%s: Matched rx cnt stuck at %d \n", __FUNCTION__, last_rx_cnt));
			return BCME_ERROR;
		}
		last_rx_cnt = loopback_rx_cnt;
	}
	printf("Received %d loopback packets of size %d\n", loopback_tx_cnt, loopback_size);

	return BCME_OK;
} /* dbus_usbos_loopback_tx */

/**
 * Higher layer (dbus_usb.c) wants to transmit an I/O Request Block
 *     @param[in] txirb txirb->pkt, if non-zero, contains a single or a chain of packets
 */
static int
dbus_usbos_intf_send_irb(void *bus, dbus_irb_tx_t *txirb)
{
	usbos_info_t *usbos_info = (usbos_info_t *) bus;
	urb_req_t *req, *req_zlp = NULL;
	int ret = DBUS_OK;
	unsigned long flags;
	void *pkt;
	uint32 buffer_length;
	uint8 *buf;

	if ((usbos_info == NULL) || !usbos_info->tx_pipe) {
		return DBUS_ERR;
	}

	if (txirb->pkt != NULL) {
		buffer_length = pkttotlen(usbos_info->pub->osh, txirb->pkt);
		/* In case of multiple packets the values below may be overwritten */
		txirb->send_buf = NULL;
		buf = PKTDATA(usbos_info->pub->osh, txirb->pkt);
	} else { /* txirb->buf != NULL */
		ASSERT(txirb->buf != NULL);
		ASSERT(txirb->send_buf == NULL);
		buffer_length = txirb->len;
		buf = txirb->buf;
	}

	if (!(req = dbus_usbos_qdeq(&usbos_info->req_txfreeq, &usbos_info->txfree_lock))) {
		DBUSERR(("%s No free URB!\n", __FUNCTION__));
		return DBUS_ERR_TXDROP;
	}

	/* If not using standard Linux kernel functionality for handling Zero Length Packet(ZLP),
	 * the dbus needs to generate ZLP when length is multiple of MaxPacketSize.
	 */
#ifndef WL_URB_ZPKT
	if (!(buffer_length % usbos_info->maxps)) {
		if (!(req_zlp =
			dbus_usbos_qdeq(&usbos_info->req_txfreeq, &usbos_info->txfree_lock))) {
			DBUSERR(("%s No free URB for ZLP!\n", __FUNCTION__));
			dbus_usbos_qenq(&usbos_info->req_txfreeq, req, &usbos_info->txfree_lock);
			return DBUS_ERR_TXDROP;
		}

		/* No txirb, so that dbus_usbos_send_complete can differentiate between
		 * DATA and ZLP.
		 */
		req_zlp->arg = NULL;
		req_zlp->usbinfo = usbos_info;
		req_zlp->buf_len = 0;

		usb_fill_bulk_urb(req_zlp->urb, usbos_info->usb, usbos_info->tx_pipe, NULL,
			0, (usb_complete_t)dbus_usbos_send_complete, req_zlp);

		req_zlp->urb->transfer_flags |= URB_QUEUE_BULK;
	}
#endif /* !WL_URB_ZPKT */

#ifndef USBOS_TX_THREAD
	/* Disable USB autosuspend until this request completes, request USB resume if needed.
	 * Because this call runs asynchronously, there is no guarantee the bus is resumed before
	 * the URB is submitted, and the URB might be dropped. Use USBOS_TX_THREAD to avoid
	 * this.
	 */
	USB_AUTOPM_GET_INTERFACE_ASYNC(g_probe_info.intf);
#endif /* !USBOS_TX_THREAD */

	spin_lock_irqsave(&usbos_info->txlock, flags);

	req->arg = txirb;
	req->usbinfo = usbos_info;
	req->buf_len = 0;

	/* Prepare the URB */
	if (txirb->pkt != NULL) {
		uint32 pktlen;
		uint8 *transfer_buf;

		/* For multiple packets, allocate contiguous buffer and copy packet data to it */
		if (PKTNEXT(usbos_info->pub->osh, txirb->pkt)) {
			transfer_buf = MALLOC(usbos_info->pub->osh, buffer_length);
			if (!transfer_buf) {
				ret = DBUS_ERR_TXDROP;
				DBUSERR(("fail to alloc to usb buffer\n"));
				goto fail;
			}

			pkt = txirb->pkt;
			txirb->send_buf = transfer_buf;
			req->buf_len = buffer_length;

			while (pkt) {
				pktlen = PKTLEN(usbos_info->pub->osh, pkt);
				bcopy(PKTDATA(usbos_info->pub->osh, pkt), transfer_buf, pktlen);
				transfer_buf += pktlen;
				pkt = PKTNEXT(usbos_info->pub->osh, pkt);
			}

			ASSERT(((uint8 *) txirb->send_buf + buffer_length) == transfer_buf);

			/* Overwrite buf pointer with pointer to allocated contiguous transfer_buf
			 */
			buf = txirb->send_buf;
		}
	}

	usb_fill_bulk_urb(req->urb, usbos_info->usb, usbos_info->tx_pipe, buf,
		buffer_length, (usb_complete_t)dbus_usbos_send_complete, req);

	req->urb->transfer_flags |= URB_QUEUE_BULK;

#ifdef USBOS_TX_THREAD
	/* Enqueue TX request, the TX thread will resume the bus if needed and submit
	 * it asynchronously
	 */
	dbus_usbos_qenq(&usbos_info->usbos_tx_list, req, &usbos_info->usbos_tx_list_lock);
	if (req_zlp != NULL) {
		dbus_usbos_qenq(&usbos_info->usbos_tx_list, req_zlp,
			&usbos_info->usbos_tx_list_lock);
	}
	spin_unlock_irqrestore(&usbos_info->txlock, flags);

	wake_up_interruptible(&usbos_info->usbos_tx_queue_head);
	return DBUS_OK;
#else
	if ((ret = USB_SUBMIT_URB(req->urb))) {
		ret = DBUS_ERR_TXDROP;
		goto fail;
	}

	dbus_usbos_qenq(&usbos_info->req_txpostedq, req, &usbos_info->txposted_lock);
	atomic_inc(&usbos_info->txposted);

	if (req_zlp != NULL) {
		if ((ret = USB_SUBMIT_URB(req_zlp->urb))) {
			DBUSERR(("failed to submit ZLP URB!\n"));
			ASSERT(0);
			ret = DBUS_ERR_TXDROP;
			goto fail2;
		}

		dbus_usbos_qenq(&usbos_info->req_txpostedq, req_zlp, &usbos_info->txposted_lock);
		/* Also increment txposted for zlp packet, as it will be decremented in
		 * dbus_usbos_send_complete()
		 */
		atomic_inc(&usbos_info->txposted);
	}

	spin_unlock_irqrestore(&usbos_info->txlock, flags);
	return DBUS_OK;
#endif /* USBOS_TX_THREAD */

fail:
	if (txirb->send_buf != NULL) {
		MFREE(usbos_info->pub->osh, txirb->send_buf, req->buf_len);
		txirb->send_buf = NULL;
		req->buf_len = 0;
	}
	dbus_usbos_qenq(&usbos_info->req_txfreeq, req, &usbos_info->txfree_lock);
#ifndef USBOS_TX_THREAD
fail2:
#endif
	if (req_zlp != NULL) {
		dbus_usbos_qenq(&usbos_info->req_txfreeq, req_zlp, &usbos_info->txfree_lock);
	}

	spin_unlock_irqrestore(&usbos_info->txlock, flags);

#ifndef USBOS_TX_THREAD
	USB_AUTOPM_PUT_INTERFACE_ASYNC(g_probe_info.intf);
#endif /* !USBOS_TX_THREAD */

	return ret;
} /* dbus_usbos_intf_send_irb */
