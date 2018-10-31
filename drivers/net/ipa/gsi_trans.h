/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */
#ifndef _GSI_TRANS_H_
#define _GSI_TRANS_H_

#include <linux/types.h>
#include <linux/refcount.h>
#include <linux/completion.h>

struct scatterlist;
struct device;

struct gsi;
struct gsi_trans;
enum ipa_cmd_opcode;

struct gsi_trans {
	struct list_head links;		/* gsi_channel lists */

	struct gsi *gsi;
	u32 channel_id;

	u32 tre_count;			/* # TREs requested */
	u32 len;			/* total # bytes in sgl */
	struct scatterlist *sgl;
	u32 sgc;			/* # entries in sgl[] */

	struct completion completion;
	refcount_t refcount;

	/* fields above are internal only */

	struct device *dev;		/* Use this for DMA mapping */
	long result;			/* RX count, 0, or error code */

	u64 byte_count;			/* channel byte_count when committed */
	u64 trans_count;		/* channel trans_count when committed */

	void *data;
};

/**
 * gsi_channel_trans_alloc() - Allocate a GSI transaction on a channel
 * @gsi:	GSI pointer
 * @channel_id:	Channel the transaction is associated with
 * @tre_count:	Number of elements in the transaction
 *
 * @Return:	A GSI transaction structure, or a null pointer if all
 *		available transactions are in use
 */
struct gsi_trans *gsi_channel_trans_alloc(struct gsi *gsi, u32 channel_id,
					  u32 tre_count);

/**
 * gsi_trans_free() - Free a previously-allocated GSI transaction
 * @trans:	Transaction to be freed
 *
 * Note: this should only be used in error paths, before the transaction is
 * committed or in the event committing the transaction produces an error.
 * Successfully committing a transaction passes ownership of the structure
 * to the core transaction code.
 */
void gsi_trans_free(struct gsi_trans *trans);

/**
 * gsi_trans_commit() - Commit a GSI transaction
 * @trans:	Transaction to commit
 * @ring_db:	Whether to tell the hardware about these queued transfers
 * @callback:	Function called when transaction has completed.
 */
int gsi_trans_commit(struct gsi_trans *trans, bool ring_db);

/**
 * gsi_trans_commit_command() - Commit a GSI command transaction and wait
 *				wait for it to complete
 * @trans:	Transaction to commit
 */
int gsi_trans_commit_command(struct gsi_trans *trans,
			     enum ipa_cmd_opcode opcode);

/**
 * gsi_trans_commit_command_timeout() - Commit a GSI command transaction,
 *					wait for it to complete, with timeout
 * @trans:	Transaction to commit
 * @ring_db:	Whether to tell the hardware about these queued transfers
 * @timeout:	Timeout period (in milliseconds)
 */
int gsi_trans_commit_command_timeout(struct gsi_trans *trans,
				     enum ipa_cmd_opcode opcode,
				     unsigned long timeout);

/**
 * gsi_trans_read_byte() - Issue a single byte read TRE on a channel
 * @gsi:	GSI pointer
 * @channel_id:	Channel on which to read a byte
 * @addr:	DMA address into which to transfer the one byte
 *
 * This is not a transaction operation at all.  It's defined here because
 * it needs to be done in coordination with other transaction activity.
 */
int gsi_trans_read_byte(struct gsi *gsi, u32 channel_id, dma_addr_t addr);

/**
 * gsi_trans_read_byte_done() - Clean up after a single byte read TRE
 * @gsi:	GSI pointer
 * @channel_id:	Channel on which byte was read
 *
 * This function needs to be called to signal that the work related
 * to reading a byte initiated by gsi_trans_read_byte() is complete.
 */
void gsi_trans_read_byte_done(struct gsi *gsi, u32 channel_id);

#endif /* _GSI_TRANS_H_ */
