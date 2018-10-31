// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>

#include "gsi.h"
#include "gsi_private.h"
#include "gsi_trans.h"
#include "ipa_gsi.h"
#include "ipa_data.h"
#include "ipa_cmd.h"

/**
 * DOC: GSI Transactions
 *
 * A GSI transaction abstracts the behavior of a GSI channel by representing
 * everything about a related group of data transfers in a single structure.
 * Most details of interaction with the GSI hardware are managed by the GSI
 * transaction core, allowing users to simply describe transfers to be
 * performed.  When a transaction has completed a callback function
 * (dependent on the type of endpoint associated with the channel) allows
 * cleanup of resources associated with the transaction.
 *
 * To perform a data transfer (or a related set of them), a user of the GSI
 * transaction interface allocates a transaction, indicating the number of
 * TREs required (one per data transfer).  If sufficient TREs are available,
 * they are reserved for use in the transaction and the allocation succeeds.
 * This way exhaustion of the available TREs in a channel ring is detected
 * as early as possible.  All resources required to complete a transaction
 * are allocated at transaction allocation time.
 *
 * Transfers performed as part of a transaction are represented in an array
 * of Linux scatterlist structures.  This array is allocated with the
 * transaction, and its entries must be initialized using standard
 * scatterlist functions (such as sg_init_one() or skb_to_sgvec()).
 *
 * Once a transaction's scatterlist structures have been initialized, the
 * transaction is committed.  The GSI transaction layer is responsible for
 * DMA mapping (and unmapping) memory described in the transaction's
 * scatterlist array.  The only way committing a transaction fails is if
 * this DMA mapping step returns an error.  Otherwise, ownership of the
 * entire transaction is transferred to the GSI transaction core.  The GSI
 * transaction code formats the content of the scatterlist array into the
 * channel ring buffer and informs the hardware that new TREs are available
 * to process.
 *
 * The last TRE in each transaction is marked to interrupt the AP when the
 * GSI hardware has completed it.  Because transfers described by TREs are
 * performed strictly in order, signaling the completion of just the last
 * TRE in the transaction is sufficient to indicate the full transaction
 * is complete.
 *
 * When a transaction is complete, ipa_gsi_trans_complete() is called by the
 * GSI code into the IPA layer, allowing it to perform any final cleanup
 * required before the transaction is freed.
 */

/* gsi_tre->flags mask values (in CPU byte order) */
#define GSI_TRE_FLAGS_CHAIN_FMASK	GENMASK(0, 0)
#define GSI_TRE_FLAGS_IEOB_FMASK	GENMASK(8, 8)
#define GSI_TRE_FLAGS_IEOT_FMASK	GENMASK(9, 9)
#define GSI_TRE_FLAGS_BEI_FMASK		GENMASK(10, 10)
#define GSI_TRE_FLAGS_TYPE_FMASK	GENMASK(23, 16)

/* Hardware values representing a transfer element type */
enum gsi_tre_type {
	GSI_RE_XFER	= 0x2,
	GSI_RE_IMMD_CMD	= 0x3,
	GSI_RE_NOP	= 0x4,
};

/* Map a given ring entry index to the transaction associated with it */
static void gsi_channel_trans_map(struct gsi_channel *channel, u32 index,
				  struct gsi_trans *trans)
{
	/* Note: index *must* be used modulo the ring count here */
	channel->trans_info.map[index % channel->tre_ring.count] = trans;
}

/* Return the transaction mapped to a given ring entry */
struct gsi_trans *
gsi_channel_trans_mapped(struct gsi_channel *channel, u32 index)
{
	/* Note: index *must* be used modulo the ring count here */
	return channel->trans_info.map[index % channel->tre_ring.count];
}

/* Return the oldest completed transaction for a channel (or null) */
struct gsi_trans *gsi_channel_trans_complete(struct gsi_channel *channel)
{
	return list_first_entry_or_null(&channel->trans_info.complete,
					struct gsi_trans, links);
}

/* Move a transaction from the allocated list to the pending list */
static void gsi_trans_move_pending(struct gsi_trans *trans)
{
	struct gsi_channel *channel = &trans->gsi->channel[trans->channel_id];
	struct gsi_trans_info *trans_info = &channel->trans_info;

	spin_lock_bh(&trans_info->spinlock);

	list_move_tail(&trans->links, &trans_info->pending);

	spin_unlock_bh(&trans_info->spinlock);
}

/* Move a transaction and all of its predecessors from the pending list
 * to the completed list.
 */
void gsi_trans_move_complete(struct gsi_trans *trans)
{
	struct gsi_channel *channel = &trans->gsi->channel[trans->channel_id];
	struct gsi_trans_info *trans_info = &channel->trans_info;
	struct list_head list;

	spin_lock_bh(&trans_info->spinlock);

	/* Move this transaction and all predecessors to completed list */
	list_cut_position(&list, &trans_info->pending, &trans->links);
	list_splice_tail(&list, &trans_info->complete);

	spin_unlock_bh(&trans_info->spinlock);
}

/* Move a transaction from the completed list to the polled list */
void gsi_trans_move_polled(struct gsi_trans *trans)
{
	struct gsi_channel *channel = &trans->gsi->channel[trans->channel_id];
	struct gsi_trans_info *trans_info = &channel->trans_info;

	spin_lock_bh(&trans_info->spinlock);

	list_move_tail(&trans->links, &trans_info->polled);

	spin_unlock_bh(&trans_info->spinlock);
}

/* Return the last (most recent) transaction allocated on a channel.
 * For RX channels, select from the alloc and pending list only if
 * @rx_pending is true.
 */
struct gsi_trans *
gsi_channel_trans_last(struct gsi *gsi, u32 channel_id, bool rx_pending)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];
	struct gsi_trans_info *trans_info = &channel->trans_info;
	struct list_head *list = NULL;
	struct gsi_trans *trans;

	spin_lock_bh(&trans_info->spinlock);

	/* Find the last list to which a transaction was added.  For
	 * RX channels, skip over the allocated and pending lists
	 * unless the caller asks for them to be considered.  When
	 * suspending we don't expect pending (or soon-to-be pending)
	 * RX transactions to complete.
	 */
	if (channel->toward_ipa || rx_pending) {
		if (!list_empty(&trans_info->alloc))
			list = &trans_info->alloc;
		else if (!list_empty(&trans_info->pending))
			list = &trans_info->pending;
	}

	if (!list) {
		if (!list_empty(&trans_info->complete))
			list = &trans_info->complete;
		else if (!list_empty(&trans_info->polled))
			list = &trans_info->polled;
	}

	if (list) {
		/* The last entry on this list is the last one allocated.
		 * Grab a reference so it can be waited for.
		 */
		trans = list_last_entry(list, struct gsi_trans, links);
		refcount_inc(&trans->refcount);
	} else {
		trans = NULL;
	}

	spin_unlock_bh(&trans_info->spinlock);

	return trans;
}

/* Reserve some number of TREs on a channel.  Returns true if successful */
static bool
gsi_trans_tre_reserve(struct gsi_trans_info *trans_info, u32 tre_count)
{
	int avail = atomic_read(&trans_info->tre_avail);
	int new;

	do {
		new = avail - (int)tre_count;
		if (unlikely(new < 0))
			return false;
	} while (!atomic_try_cmpxchg(&trans_info->tre_avail, &avail, new));

	return true;
}

/* Release previously-reserved TRE entries to a channel */
static void
gsi_trans_tre_release(struct gsi_trans_info *trans_info, u32 tre_count)
{
	atomic_add(tre_count, &trans_info->tre_avail);
}

/* Allocate a GSI transaction on a channel */
struct gsi_trans *
gsi_channel_trans_alloc(struct gsi *gsi, u32 channel_id, u32 tre_count)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];
	struct gsi_trans_info *trans_info;
	struct gsi_trans *trans;
	u32 which;

	/* assert(tre_count <= channel->data->tlv_count); */

	trans_info = &channel->trans_info;

	/* We reserve the TREs now, but consume them at commit time.
	 * If there aren't enough available, we're done.
	 */
	if (!gsi_trans_tre_reserve(trans_info, tre_count))
		return NULL;

	/* Allocate the transaction and initialize it */
	which = trans_info->pool_free++ % trans_info->pool_count;
	trans = &trans_info->pool[which];

	trans->gsi = gsi;
	trans->channel_id = channel_id;
	refcount_set(&trans->refcount, 1);
	trans->tre_count = tre_count;
	init_completion(&trans->completion);

	/* We're reusing, so make sure all fields are reinitialized */
	trans->dev = gsi->dev;
	trans->result = 0;	/* Success assumed unless overwritten */
	trans->data = NULL;

	/* Allocate the scatter/gather entries it will use.  If what's
	 * needed would cross the end-of-pool boundary, allocate them
	 * from the beginning of the pool.
	 */
	if (tre_count > trans_info->sg_pool_count - trans_info->sg_pool_free)
		trans_info->sg_pool_free = 0;
	trans->sgl = &trans_info->sg_pool[trans_info->sg_pool_free];
	trans->sgc = tre_count;
	trans_info->sg_pool_free += tre_count;

	spin_lock_bh(&trans_info->spinlock);

	list_add_tail(&trans->links, &trans_info->alloc);

	spin_unlock_bh(&trans_info->spinlock);

	return trans;
}

/* Free a previously-allocated transaction (used only in case of error) */
void gsi_trans_free(struct gsi_trans *trans)
{
	struct gsi_trans_info *trans_info;

	if (!refcount_dec_and_test(&trans->refcount))
		return;

	trans_info = &trans->gsi->channel[trans->channel_id].trans_info;

	spin_lock_bh(&trans_info->spinlock);

	list_del(&trans->links);

	spin_unlock_bh(&trans_info->spinlock);

	gsi_trans_tre_release(trans_info, trans->tre_count);
}

/* Compute the length/opcode value to use for a TRE */
static __le16 gsi_tre_len_opcode(enum ipa_cmd_opcode opcode, u32 len)
{
	return opcode == IPA_CMD_NONE ? cpu_to_le16((u16)len)
				      : cpu_to_le16((u16)opcode);
}

/* Compute the flags value to use for a given TRE */
static __le32 gsi_tre_flags(bool last_tre, bool bei, enum ipa_cmd_opcode opcode)
{
	enum gsi_tre_type tre_type;
	u32 tre_flags;

	tre_type = opcode == IPA_CMD_NONE ? GSI_RE_XFER : GSI_RE_IMMD_CMD;
	tre_flags = u32_encode_bits(tre_type, GSI_TRE_FLAGS_TYPE_FMASK);

	/* Last TRE contains interrupt flags */
	if (last_tre) {
		/* All transactions end in a transfer completion interrupt */
		tre_flags |= GSI_TRE_FLAGS_IEOT_FMASK;
		/* Don't interrupt when outbound commands are acknowledged */
		if (bei)
			tre_flags |= GSI_TRE_FLAGS_BEI_FMASK;
	} else {	/* All others indicate there's more to come */
		tre_flags |= GSI_TRE_FLAGS_CHAIN_FMASK;
	}

	return cpu_to_le32(tre_flags);
}

static void gsi_trans_tre_fill(struct gsi_tre *dest_tre, dma_addr_t addr,
			       u32 len, bool last_tre, bool bei,
			       enum ipa_cmd_opcode opcode)
{
	struct gsi_tre tre;

	tre.addr = cpu_to_le64(addr);
	tre.len_opcode = gsi_tre_len_opcode(opcode, len);
	tre.reserved = 0;
	tre.flags = gsi_tre_flags(last_tre, bei, opcode);

	/* ARM64 can write 16 bytes as a unit with a single instruction.
	 * Doing the assignment this way is an attempt to make that happen.
	 */
	*dest_tre = tre;
}

/* Issue a command to read a single byte from a channel */
int gsi_trans_read_byte(struct gsi *gsi, u32 channel_id, dma_addr_t addr)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];
	struct gsi_trans_info *trans_info;
	struct gsi_ring *tre_ring;
	struct gsi_tre *dest_tre;
	struct gsi_ring *ring;

	trans_info = &channel->trans_info;

	/* First reserve the TRE, if possible */
	if (!gsi_trans_tre_reserve(trans_info, 1))
		return -EBUSY;

	/* Now allocate the next TRE, fill it, and tell the hardware */
	tre_ring = &channel->tre_ring;
	ring = &gsi->evt_ring[channel->evt_ring_id].ring;

	dest_tre = gsi_ring_virt(tre_ring, tre_ring->index);
	gsi_trans_tre_fill(dest_tre, addr, 1, true, false, IPA_CMD_NONE);

	tre_ring->index++;
	gsi_channel_doorbell(channel);

	return 0;
}

/* Mark a gsi_trans_read_byte() request done */
void gsi_trans_read_byte_done(struct gsi *gsi, u32 channel_id)
{
	struct gsi_channel *channel = &gsi->channel[channel_id];

	gsi_trans_tre_release(&channel->trans_info, 1);
}

/**
 * __gsi_trans_commit() - Common GSI transaction commit code
 * @trans:	Transaction to commit
 * @opcode:	Immediate command opcode, or IPA_CMD_NONE
 * @ring_db:	Whether to tell the hardware about these queued transfers
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Maps the transactions's scatterlist array for DMA, and returns -ENOMEM
 * if that fails.  Formats channel ring TRE entries based on the content of
 * the scatterlist.  Maps a transaction pointer to the last ring entry used
 * for the transaction, so it can be recovered when it completes.  Moves
 * the transaction to the pending list.  Finally, updates the channel ring
 * pointer and optionally rings the doorbell.
 */
static int __gsi_trans_commit(struct gsi_trans *trans,
			      enum ipa_cmd_opcode opcode, bool ring_db)
{
	struct gsi_channel *channel = &trans->gsi->channel[trans->channel_id];
	struct gsi_ring *tre_ring = &channel->tre_ring;
	enum dma_data_direction direction;
	bool bei = channel->toward_ipa;
	struct gsi_tre *dest_tre;
	struct scatterlist *sg;
	struct gsi_ring *ring;
	u32 byte_count = 0;
	u32 avail;
	int ret;
	u32 i;

	direction = channel->toward_ipa ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	ret = dma_map_sg(trans->dev, trans->sgl, trans->sgc, direction);
	if (!ret)
		return -ENOMEM;

	ring = &channel->gsi->evt_ring[channel->evt_ring_id].ring;

	/* Consume the entries.  If we cross the end of the ring while
	 * filling them we'll switch to the beginning to finish.
	 */
	avail = ring->count - tre_ring->index % tre_ring->count;
	dest_tre = gsi_ring_virt(tre_ring, tre_ring->index);
	for_each_sg(trans->sgl, sg, trans->sgc, i) {
		bool last_tre = i == trans->tre_count - 1;
		dma_addr_t addr = sg_dma_address(sg);
		u32 len = sg_dma_len(sg);

		byte_count += len;
		if (!avail--)
			dest_tre = gsi_ring_virt(tre_ring, 0);

		gsi_trans_tre_fill(dest_tre, addr, len, last_tre, bei, opcode);
		dest_tre++;
	}
	tre_ring->index += trans->tre_count;

	if (channel->toward_ipa) {
		/* We record TX bytes when they are sent */
		trans->len = byte_count;
		trans->trans_count = channel->trans_count;
		trans->byte_count = channel->byte_count;
		channel->trans_count++;
		channel->byte_count += byte_count;
	}

	/* Associate the last TRE with the transaction */
	gsi_channel_trans_map(channel, tre_ring->index - 1, trans);

	gsi_trans_move_pending(trans);

	/* Ring doorbell if requested, or if all TREs are allocated */
	if (ring_db || !atomic_read(&channel->trans_info.tre_avail)) {
		/* Report what we're handing off to hardware for TX channels */
		if (channel->toward_ipa)
			gsi_channel_tx_queued(channel);
		gsi_channel_doorbell(channel);
	}

	return 0;
}

/* Commit a GSI transaction */
int gsi_trans_commit(struct gsi_trans *trans, bool ring_db)
{
	return __gsi_trans_commit(trans, IPA_CMD_NONE, ring_db);
}

/* Commit a GSI command transaction and wait for it to complete */
int gsi_trans_commit_command(struct gsi_trans *trans,
			     enum ipa_cmd_opcode opcode)
{
	int ret;

	refcount_inc(&trans->refcount);

	ret = __gsi_trans_commit(trans, opcode, true);
	if (ret)
		goto out_free_trans;

	wait_for_completion(&trans->completion);

out_free_trans:
	gsi_trans_free(trans);

	return ret;
}

/* Commit a GSI command transaction, wait for it to complete, with timeout */
int gsi_trans_commit_command_timeout(struct gsi_trans *trans,
				     enum ipa_cmd_opcode opcode,
				     unsigned long timeout)
{
	unsigned long timeout_jiffies = msecs_to_jiffies(timeout);
	unsigned long remaining;
	int ret;

	refcount_inc(&trans->refcount);

	ret = __gsi_trans_commit(trans, opcode, true);
	if (ret)
		goto out_free_trans;

	remaining = wait_for_completion_timeout(&trans->completion,
						timeout_jiffies);
out_free_trans:
	gsi_trans_free(trans);

	return ret ? ret : remaining ? 0 : -ETIMEDOUT;
}

/* Return a channel's next completed transaction (or NULL) */
void gsi_trans_complete(struct gsi_trans *trans)
{
	struct gsi_channel *channel = &trans->gsi->channel[trans->channel_id];
	enum dma_data_direction direction;

	direction = channel->toward_ipa ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

	dma_unmap_sg(trans->dev, trans->sgl, trans->sgc, direction);

	ipa_gsi_trans_complete(trans);

	complete(&trans->completion);

	gsi_trans_free(trans);
}

/* Cancel a channel's pending transactions */
void gsi_channel_trans_cancel_pending(struct gsi_channel *channel)
{
	struct gsi_trans_info *trans_info = &channel->trans_info;
	u32 evt_ring_id = channel->evt_ring_id;
	struct gsi *gsi = channel->gsi;
	struct gsi_trans *trans;
	struct gsi_ring *ring;

	ring = &gsi->evt_ring[evt_ring_id].ring;

	spin_lock_bh(&trans_info->spinlock);

	list_for_each_entry(trans, &trans_info->pending, links)
		trans->result = -ECANCELED;

	list_splice_tail_init(&trans_info->pending, &trans_info->complete);

	spin_unlock_bh(&trans_info->spinlock);

	/* Schedule NAPI polling to complete the cancelled transactions */
	napi_schedule(&channel->napi);
}

/* Initialize a channel's GSI transaction info */
int gsi_channel_trans_init(struct gsi_channel *channel)
{
	struct gsi_trans_info *trans_info = &channel->trans_info;
	u32 tre_count = channel->data->tre_count;

	trans_info->map = kcalloc(tre_count, sizeof(*trans_info->map),
				  GFP_KERNEL);
	if (!trans_info->map)
		return -ENOMEM;

	/* We will never need more transactions than there are TRE
	 * entries in the transfer ring.  For that reason, we can
	 * preallocate an array of (at least) that many transactions,
	 * and use a single free index to determine the next one
	 * available for allocation.
	 */
	trans_info->pool_count = tre_count;
	trans_info->pool = kcalloc(trans_info->pool_count,
				   sizeof(*trans_info->pool), GFP_KERNEL);
	if (!trans_info->pool)
		goto err_free_map;
	/* If we get extra memory from the allocator, use it */
	trans_info->pool_count =
		ksize(trans_info->pool) / sizeof(*trans_info->pool);
	trans_info->pool_free = 0;

	/* While transactions are allocated one at a time, a transaction
	 * can have multiple TREs.  The number of TRE entries in a single
	 * transaction is limited by the number of TLV FIFO entries the
	 * channel has.  We reserve TREs when a transaction is allocated,
	 * but we don't actually use/fill them until the transaction is
	 * committed.
	 *
	 * A transaction uses a scatterlist array to represent the data
	 * transfers implemented by the transaction.  Each scatterlist
	 * element is used to fill a single TRE when the transaction is
	 * committed.  As a result, we need the same number of scatterlist
	 * elements as there are TREs in the transfer ring, and we can
	 * preallocate them in a pool.
	 *
	 * If we allocate a few (tlv_count - 1) extra entries in our pool,
	 * we can always satisfy requests without ever worrying about
	 * straddling the end of the array.  If there aren't enough
	 * entries starting at the free index, we just allocate free
	 * entries from the beginning of the pool.
	 */
	trans_info->sg_pool_count = tre_count + channel->data->tlv_count - 1;
	trans_info->sg_pool = kcalloc(trans_info->sg_pool_count,
				      sizeof(*trans_info->sg_pool), GFP_KERNEL);
	if (!trans_info->sg_pool)
		goto err_free_pool;
	/* Use any extra memory we get from the allocator */
	trans_info->sg_pool_count =
		ksize(trans_info->sg_pool) / sizeof(*trans_info->sg_pool);
	trans_info->sg_pool_free = 0;

	/* The tre_avail field limits the number of outstanding transactions.
	 * In theory we should be able use all of the TREs in the ring.  But
	 * in practice, doing that caused the hardware to report running out
	 * of event ring slots for writing completion information.  So give
	 * the poor hardware a break, and allow one less than the maximum.
	 */
	atomic_set(&trans_info->tre_avail, tre_count - 1);

	spin_lock_init(&trans_info->spinlock);
	INIT_LIST_HEAD(&trans_info->alloc);
	INIT_LIST_HEAD(&trans_info->pending);
	INIT_LIST_HEAD(&trans_info->complete);
	INIT_LIST_HEAD(&trans_info->polled);

	return 0;

err_free_pool:
	kfree(trans_info->pool);
err_free_map:
	kfree(trans_info->map);

	return -ENOMEM;
}

/* Inverse of gsi_channel_trans_init() */
void gsi_channel_trans_exit(struct gsi_channel *channel)
{
	struct gsi_trans_info *trans_info = &channel->trans_info;

	kfree(trans_info->sg_pool);
	kfree(trans_info->pool);
	kfree(trans_info->map);
}
