/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */
#ifndef _GSI_H_
#define _GSI_H_

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>

/* Maximum number of channels and event rings supported by the driver */
#define GSI_CHANNEL_MAX		17
#define GSI_EVT_RING_MAX	13

struct device;
struct scatterlist;
struct platform_device;

struct gsi;
struct gsi_trans;
struct gsi_channel_data;
struct ipa_gsi_endpoint_data;

/* Execution environment IDs */
enum gsi_ee_id {
	GSI_EE_AP	= 0,
	GSI_EE_MODEM	= 1,
	GSI_EE_UC	= 2,
	GSI_EE_TZ	= 3,
};

/* Channel operation statistics, aggregated across all channels */
struct gsi_channel_stats {
	u64 allocate;
	u64 start;
	u64 stop;
	u64 reset;
	u64 free;
};

struct gsi_ring {
	void *virt;			/* ring array base address */
	dma_addr_t addr;		/* primarily low 32 bits used */
	u32 count;			/* number of elements in ring */

	/* The ring index value indicates the next "open" entry in the ring.
	 *
	 * A channel ring consists of TRE entries filled by the AP and passed
	 * to the hardware for processing.  For a channel ring, the ring index
	 * identifies the next unused entry to be filled by the AP.
	 *
	 * An event ring consists of event structures filled by the hardware
	 * and passed to the AP.  For event rings, the ring index identifies
	 * the next ring entry that is not known to have been filled by the
	 * hardware.
	 */
	u32 index;
};

struct gsi_trans_info {
	struct gsi_trans **map;		/* TRE -> transaction map */
	u32 pool_count;			/* # transactions in the pool */
	struct gsi_trans *pool;		/* transaction allocation pool */
	u32 pool_free;			/* next free trans in pool (modulo) */
	u32 sg_pool_count;		/* # SGs in the allocation pool */
	struct scatterlist *sg_pool;	/* SG allocation pool */
	u32 sg_pool_free;		/* next free SG pool entry */

	atomic_t tre_avail;		/* # unallocated TREs in ring */
	spinlock_t spinlock;		/* protects updates to the lists */
	struct list_head alloc;		/* allocated, not committed */
	struct list_head pending;	/* committed, awaiting completion */
	struct list_head complete;	/* completed, awaiting poll */
	struct list_head polled;	/* returned by gsi_channel_poll_one() */
};

/* Hardware values signifying the state of a channel */
enum gsi_channel_state {
	GSI_CHANNEL_STATE_NOT_ALLOCATED	= 0x0,
	GSI_CHANNEL_STATE_ALLOCATED	= 0x1,
	GSI_CHANNEL_STATE_STARTED	= 0x2,
	GSI_CHANNEL_STATE_STOPPED	= 0x3,
	GSI_CHANNEL_STATE_STOP_IN_PROC	= 0x4,
	GSI_CHANNEL_STATE_ERROR		= 0xf,
};

/* We only care about channels between IPA and AP */
struct gsi_channel {
	struct gsi *gsi;
	u32 toward_ipa;			/* 0: IPA->AP; 1: AP->IPA */
	u32 use_prefetch;		/* 0: escape buf; 1: use prefetch */

	const struct gsi_channel_data *data;	/* initialization data */

	struct completion completion;	/* signals channel state changes */
	enum gsi_channel_state state;

	struct gsi_ring tre_ring;
	u32 evt_ring_id;

	u64 byte_count;			/* total # bytes transferred */
	u64 trans_count;		/* total # transactions */
	/* The following counts are used only for TX endpoints */
	u64 queued_byte_count;		/* last reported queued byte count */
	u64 queued_trans_count;		/* ...and queued trans count */
	u64 compl_byte_count;		/* last reported completed byte count */
	u64 compl_trans_count;		/* ...and completed trans count */

	struct gsi_trans_info trans_info;

	struct napi_struct napi;
};

/* Hardware values signifying the state of an event ring */
enum gsi_evt_ring_state {
	GSI_EVT_RING_STATE_NOT_ALLOCATED	= 0x0,
	GSI_EVT_RING_STATE_ALLOCATED		= 0x1,
	GSI_EVT_RING_STATE_ERROR		= 0xf,
};

struct gsi_evt_ring {
	struct gsi_channel *channel;
	struct completion completion;	/* signals event ring state changes */
	enum gsi_evt_ring_state state;
	struct gsi_ring ring;
};

struct gsi {
	struct device *dev;		/* Same as IPA device */
	struct net_device dummy_dev;	/* needed for NAPI */
	void __iomem *virt;
	u32 irq;
	u32 irq_wake_enabled;		/* 1: irq wake was enabled */
	u32 channel_count;
	u32 evt_ring_count;
	struct gsi_channel channel[GSI_CHANNEL_MAX];
	struct gsi_channel_stats channel_stats;
	struct gsi_evt_ring evt_ring[GSI_EVT_RING_MAX];
	u32 event_bitmap;
	u32 event_enable_bitmap;
	u32 modem_channel_bitmap;
	struct completion completion;	/* for global EE commands */
	struct mutex mutex;		/* protects commands, programming */
};

/**
 * gsi_setup() - Set up the GSI subsystem
 * @gsi:	Address of GSI structure embedded in an IPA structure
 * @db_enable:	Whether to use the GSI doorbell engine
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Performs initialization that must wait until the GSI hardware is
 * ready (including firmware loaded).
 */
int gsi_setup(struct gsi *gsi, bool db_enable);

/**
 * gsi_teardown() - Tear down GSI subsystem
 * @gsi:	GSI address previously passed to a successful gsi_setup() call
 */
void gsi_teardown(struct gsi *gsi);

/**
 * gsi_channel_trans_max() - Channel maximum number of transactions
 * @gsi:	GSI pointer
 * @channel_id:	Channel whose limit is to be returned
 *
 * @Return:	 The maximum number of pending transactions on the channel
 */
u32 gsi_channel_trans_max(struct gsi *gsi, u32 channel_id);

/**
 * gsi_channel_trans_tre_max() - Return the maximum TREs per transaction
 * @gsi:	GSI pointer
 * @channel_id:	Channel whose limit is to be returned
 *
 * @Return:	 The maximum TRE count per transaction on the channel
 */
u32 gsi_channel_trans_tre_max(struct gsi *gsi, u32 channel_id);

/**
 * gsi_channel_trans_quiesce() - Wait for channel transactions to complete
 * @gsi:	GSI pointer
 * @channel_id:	Channel to quiesce
 * @stopping:	Whether we're stopping (rather than suspending) the channel
 *
 * Wait for a channel's currently-allocated transactions to be committed,
 * completed, and be freed.
 *
 * When a channel is reset, all pending transactions are marked canceled
 * and moved to completed state.  This occurs when a channel is closed.
 *
 * However, when a channel is suspended (and specifically an RX channel),
 * we do not expect pending transactions to complete.  In this case, we
 * don't want to wait for pending (or just allocated) transactions.
 * So we need to indicate whether we're stopping or not.
 *
 * This returns after the "last" transaction has completed, where
 * the last one is either the most recently allocated, or for an RX
 * channel that's being suspended, the most recently completed
 * transaction.
 *
 * NOTE:  Assumes no new transactions will be issued before it returns.
 */
void gsi_channel_trans_quiesce(struct gsi *gsi, u32 channel_id, bool stopping);

/**
 * gsi_channel_start() - Make a GSI channel operational
 * @gsi:	GSI pointer
 * @channel_id:	Channel to start
 *
 * @Return:	0 if successful, or a negative error code
 */
int gsi_channel_start(struct gsi *gsi, u32 channel_id);

/**
 * gsi_channel_stop() - Stop an operational GSI channel
 * @gsi:	GSI pointer returned by gsi_setup()
 * @channel_id:	Channel to stop
 *
 * @Return:	0 if successful, or a negative error code
 */
int gsi_channel_stop(struct gsi *gsi, u32 channel_id);

/**
 * gsi_channel_reset() - Reset a GSI channel
 * @gsi:	GSI pointer
 * @channel_id:	Channel to be reset
 * @db_enable:	Whether doorbell engine should be enabled
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Reset a channel and reconfigure it.  The @db_enable flag indicates
 * whether the doorbell engine will be enabled following reconfiguration.
 *
 * GSI hardware relinquishes ownership of all pending receive buffer
 * transactions as a result of reset.  They will be completed with
 * result code -ECANCELED.
 */
int gsi_channel_reset(struct gsi *gsi, u32 channel_id, bool db_enable);

/**
 * gsi_init() - Initialize the GSI subsystem
 * @gsi:	Address of GSI structure embedded in an IPA structure
 * @pdev:	IPA platform device
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Early stage initialization of the GSI subsystem, performing tasks
 * that can be done before the GSI hardware is ready to use.
 */
int gsi_init(struct gsi *gsi, struct platform_device *pdev, bool prefetch_flag,
	     u32 count, const struct ipa_gsi_endpoint_data *data,
	     bool modem_alloc);

/**
 * gsi_exit() - Exit the GSI subsystem
 * @gsi:	GSI address previously passed to a successful gsi_init() call
 */
void gsi_exit(struct gsi *gsi);

#endif /* _GSI_H_ */
