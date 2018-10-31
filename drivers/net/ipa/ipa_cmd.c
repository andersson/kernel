// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/bitfield.h>

#include "gsi.h"
#include "gsi_trans.h"
#include "ipa.h"
#include "ipa_endpoint.h"
#include "ipa_cmd.h"
#include "ipa_mem.h"

/**
 * DOC:  IPA Immediate Commands
 *
 * The AP command TX endpoint is used to issue immediate commands to the IPA.
 * An immediate command is generally used to request the IPA do something
 * other than data transfer to another endpoint.
 *
 * Immediate commands are represented by GSI transactions just like other
 * transfer requests, represented by a single GSI TRE.  Each immediate
 * command has a well-defined format, having a payload of a known length.
 * This allows the transfer element's length field to be used to hold an
 * immediate command's opcode.  The payload for a command resides in DRAM
 * and is described by a single scatterlist entry in its transaction.
 * Commands do not require a transaction completion callback.  To commit
 * an immediate command transaction, either gsi_trans_commit_command() or
 * gsi_trans_commit_command_timeout() is used.
 */

#define IPA_GSI_DMA_TASK_TIMEOUT	15	/* milliseconds */

/**
 * __ipa_cmd_timeout() - Send an immediate command with timeout
 * @ipa:	IPA structure
 * @opcode:	Immediate command opcode (must not be IPA_CMD_NONE)
 * @payload:	Pointer to command payload
 * @size:	Size of payload
 * @timeout:	Milliseconds to wait for completion (0 waits indefinitely)
 *
 * This common function implements ipa_cmd() and ipa_cmd_timeout().  It
 * allocates, initializes, and commits a transaction for the immediate
 * command.  The transaction is committed using gsi_trans_commit_command(),
 * or if a non-zero timeout is supplied, gsi_trans_commit_command_timeout().
 *
 * @Return:	0 if successful, or a negative error code
 */
static int __ipa_cmd_timeout(struct ipa *ipa, enum ipa_cmd_opcode opcode,
			     void *payload, size_t size, u32 timeout)
{
	struct ipa_endpoint *endpoint;
	struct gsi_trans *trans;
	int ret;

	endpoint = ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX];

	/* assert(opcode != IPA_CMD_NONE) */
	trans = gsi_channel_trans_alloc(&ipa->gsi, endpoint->channel_id, 1);
	if (!trans)
		return -EBUSY;

	sg_init_one(trans->sgl, payload, size);

	if (timeout)
		ret = gsi_trans_commit_command_timeout(trans, opcode, timeout);
	else
		ret = gsi_trans_commit_command(trans, opcode);
	if (ret)
		goto err_trans_free;

	return 0;

err_trans_free:
	gsi_trans_free(trans);

	return ret;
}

static int
ipa_cmd(struct ipa *ipa, enum ipa_cmd_opcode opcode, void *payload, size_t size)
{
	return __ipa_cmd_timeout(ipa, opcode, payload, size, 0);
}

static int ipa_cmd_timeout(struct ipa *ipa, enum ipa_cmd_opcode opcode,
			   void *payload, size_t size)
{
	return __ipa_cmd_timeout(ipa, opcode, payload, size,
				 IPA_GSI_DMA_TASK_TIMEOUT);
}

/* Field masks for ipa_imm_cmd_hw_hdr_init_local structure fields */
#define IPA_CMD_HDR_INIT_FLAGS_TABLE_SIZE_FMASK	GENMASK(11, 0)
#define IPA_CMD_HDR_INIT_FLAGS_HDR_ADDR_FMASK	GENMASK(27, 12)
#define IPA_CMD_HDR_INIT_FLAGS_RESERVED_FMASK	GENMASK(28, 31)

struct ipa_imm_cmd_hw_hdr_init_local {
	u64 hdr_table_addr;
	u32 flags;
	u32 reserved;
};

/* Initialize header space in IPA-local memory */
int ipa_cmd_hdr_init_local(struct ipa *ipa, u32 offset, u32 size)
{
	struct ipa_imm_cmd_hw_hdr_init_local *payload;
	struct device *dev = &ipa->pdev->dev;
	dma_addr_t addr;
	void *virt;
	u32 flags;
	u32 max;
	int ret;

	if (size > field_max(IPA_CMD_HDR_INIT_FLAGS_TABLE_SIZE_FMASK))
		return -EINVAL;

	max = field_max(IPA_CMD_HDR_INIT_FLAGS_HDR_ADDR_FMASK);
	if (offset > max || ipa->mem_offset > max - offset)
		return -EINVAL;
	offset += ipa->mem_offset;

	/* With this command we tell the IPA where in its local memory the
	 * header tables reside.  We also supply a (host) buffer whose
	 * content is copied via DMA into that table space.  We just want
	 * to zero fill it, so a zeroed DMA buffer is all that's required
	 * The IPA owns the table, but the AP must initialize it.
	 */
	virt = dma_alloc_coherent(dev, size, &addr, GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!payload) {
		ret = -ENOMEM;
		goto out_dma_free;
	}

	payload->hdr_table_addr = addr;
	flags = u32_encode_bits(size, IPA_CMD_HDR_INIT_FLAGS_TABLE_SIZE_FMASK);
	flags |= u32_encode_bits(offset, IPA_CMD_HDR_INIT_FLAGS_HDR_ADDR_FMASK);
	payload->flags = flags;

	ret = ipa_cmd(ipa, IPA_CMD_HDR_INIT_LOCAL, payload, sizeof(*payload));

	kfree(payload);
out_dma_free:
	dma_free_coherent(dev, size, virt, addr);

	return ret;
}

/* Flag allowing atomic clear of target region after reading data (v4.0+)*/
#define IPA_CMD_DMA_SHARED_CLEAR_AFTER_READ	GENMASK(15, 15)

/* Field masks for ipa_imm_cmd_hw_dma_mem_mem structure fields */
#define IPA_CMD_DMA_SHARED_FLAGS_DIRECTION_FMASK	GENMASK(0, 0)
/* The next two fields are present for IPA v3.5.1 only. */
#define IPA_CMD_DMA_SHARED_FLAGS_SKIP_CLEAR_FMASK	GENMASK(1, 1)
#define IPA_CMD_DMA_SHARED_FLAGS_CLEAR_OPTIONS_FMASK	GENMASK(3, 2)

struct ipa_imm_cmd_hw_dma_mem_mem {
	u16 clear_after_read;	/* 0 or IPA_CMD_DMA_SHARED_CLEAR_AFTER_READ */
	u16 size;
	u16 local_addr;
	u16 flags;
	u64 system_addr;
};

/* Use a DMA command to zero a block of IPA-resident memory */
int ipa_cmd_mem_dma_zero(struct ipa *ipa, u32 offset, u32 size)
{
	struct ipa_imm_cmd_hw_dma_mem_mem *payload;
	struct device *dev = &ipa->pdev->dev;
	dma_addr_t addr;
	void *virt;
	int ret;

	/* size must be non-zero, and must fit in a 16 bit field */
	if (!size || size > U16_MAX)
		return -EINVAL;

	/* offset must fit in a 16 bit local_addr field */
	if (offset > U16_MAX || ipa->mem_offset > U16_MAX - offset)
		return -EINVAL;
	offset += ipa->mem_offset;

	/* A zero-filled buffer of the right size is all that's required */
	virt = dma_alloc_coherent(dev, size, &addr, GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!payload) {
		ret = -ENOMEM;
		goto out_dma_free;
	}

	/* payload->clear_after_read was reserved prior to IPA v4.0.  It's
	 * never needed for current code, so it's 0 regardless of version.
	 */
	payload->size = size;
	payload->local_addr = offset;
	/* payload->flags:
	 *   direction:		0 = write to IPA
	 * Starting at v4.0 these are reserved; either way, all zero:
	 *   pipeline clear:	0 = wait for pipeline clear (don't skip)
	 *   clear_options:	0 = HPS clear mode
	 */
	payload->system_addr = addr;

	ret = ipa_cmd(ipa, IPA_CMD_DMA_SHARED_MEM, payload, sizeof(*payload));

	kfree(payload);
out_dma_free:
	dma_free_coherent(dev, size, virt, addr);

	return ret;
}

/* Field masks for ipa_imm_cmd_hw_ip_fltrt_init structure fields */
#define IPA_CMD_IP_FLTRT_FLAGS_HASH_SIZE_FMASK	GENMASK_ULL(11, 0)
#define IPA_CMD_IP_FLTRT_FLAGS_HASH_ADDR_FMASK	GENMASK_ULL(27, 12)
#define IPA_CMD_IP_FLTRT_FLAGS_NHASH_SIZE_FMASK	GENMASK_ULL(39, 28)
#define IPA_CMD_IP_FLTRT_FLAGS_NHASH_ADDR_FMASK	GENMASK_ULL(55, 40)

struct ipa_imm_cmd_hw_ip_fltrt_init {
	u64 hash_rules_addr;
	u64 flags;
	u64 nhash_rules_addr;
};

/* Configure a routing or filter table, for IPv4 or IPv6 */
static int ipa_cmd_table_config(struct ipa *ipa, enum ipa_cmd_opcode opcode,
				dma_addr_t addr, size_t hash_size, size_t size,
				u32 hash_offset, u32 nhash_offset)
{
	struct ipa_imm_cmd_hw_ip_fltrt_init *payload;
	u64 val;
	u32 max;
	int ret;

	if (hash_size > field_max(IPA_CMD_IP_FLTRT_FLAGS_HASH_SIZE_FMASK))
		return -EINVAL;
	if (size > field_max(IPA_CMD_IP_FLTRT_FLAGS_NHASH_SIZE_FMASK))
		return -EINVAL;

	if (hash_size) {
		max = field_max(IPA_CMD_IP_FLTRT_FLAGS_HASH_ADDR_FMASK);
		if (hash_offset > max || ipa->mem_offset > max - hash_offset)
			return -EINVAL;
		hash_offset += ipa->mem_offset;
	}

	max = field_max(IPA_CMD_IP_FLTRT_FLAGS_NHASH_ADDR_FMASK);
	if (nhash_offset > max || ipa->mem_offset > max - nhash_offset)
		return -EINVAL;
	nhash_offset += ipa->mem_offset;

	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	/* Pass zeroes for the hashed table if its size is 0 */
	if (hash_size) {
		payload->hash_rules_addr = addr;
		val = u64_encode_bits(hash_size,
				      IPA_CMD_IP_FLTRT_FLAGS_HASH_SIZE_FMASK);
		val |= u64_encode_bits(hash_offset,
				       IPA_CMD_IP_FLTRT_FLAGS_HASH_ADDR_FMASK);
	} else {
		payload->hash_rules_addr = 0;
		val = 0;
	}
	val |= u64_encode_bits(size, IPA_CMD_IP_FLTRT_FLAGS_NHASH_SIZE_FMASK);
	val |= u64_encode_bits(nhash_offset,
			       IPA_CMD_IP_FLTRT_FLAGS_NHASH_ADDR_FMASK);
	payload->flags = val;
	payload->nhash_rules_addr = addr;

	ret = ipa_cmd(ipa, opcode, payload, sizeof(*payload));

	kfree(payload);

	return ret;
}

/* Configure IPv4 routing table */
int ipa_cmd_route_config_ipv4(struct ipa *ipa, size_t size)
{
	const struct ipa_mem *hashed_mem = &ipa->mem[IPA_MEM_V4_ROUTE_HASHED];
	const struct ipa_mem *mem = &ipa->mem[IPA_MEM_V4_ROUTE];
	enum ipa_cmd_opcode opcode = IPA_CMD_IP_V4_ROUTING_INIT;
	u32 hash_size = hashed_mem->size ? size : 0;

	return ipa_cmd_table_config(ipa, opcode, ipa->route_addr, size,
				    hash_size, hashed_mem->offset, mem->offset);
}

/* Configure IPv6 routing table */
int ipa_cmd_route_config_ipv6(struct ipa *ipa, size_t size)
{
	const struct ipa_mem *hashed_mem = &ipa->mem[IPA_MEM_V6_ROUTE_HASHED];
	const struct ipa_mem *mem = &ipa->mem[IPA_MEM_V6_ROUTE];
	enum ipa_cmd_opcode opcode = IPA_CMD_IP_V6_ROUTING_INIT;
	u32 hash_size = hashed_mem->size ? size : 0;

	return ipa_cmd_table_config(ipa, opcode, ipa->route_addr, size,
				    hash_size, hashed_mem->offset, mem->offset);
}

/* Configure IPv4 filter table */
int ipa_cmd_filter_config_ipv4(struct ipa *ipa, size_t size)
{
	const struct ipa_mem *hashed_mem = &ipa->mem[IPA_MEM_V4_FILTER_HASHED];
	const struct ipa_mem *mem = &ipa->mem[IPA_MEM_V4_FILTER];
	enum ipa_cmd_opcode opcode = IPA_CMD_IP_V4_FILTER_INIT;
	u32 hash_size = hashed_mem->size ? size : 0;

	return ipa_cmd_table_config(ipa, opcode, ipa->filter_addr, size,
				    hash_size, hashed_mem->offset, mem->offset);
}

/* Configure IPv6 filter table */
int ipa_cmd_filter_config_ipv6(struct ipa *ipa, size_t size)
{
	const struct ipa_mem *hashed_mem = &ipa->mem[IPA_MEM_V6_FILTER_HASHED];
	const struct ipa_mem *mem = &ipa->mem[IPA_MEM_V6_FILTER];
	enum ipa_cmd_opcode opcode = IPA_CMD_IP_V6_FILTER_INIT;
	u32 hash_size = hashed_mem->size ? size : 0;

	return ipa_cmd_table_config(ipa, opcode, ipa->filter_addr, size,
				    hash_size, hashed_mem->offset, mem->offset);
}

/* Field masks for ipa_imm_cmd_hw_dma_task_32b_addr structure fields */
#define IPA_CMD_DMA32_TASK_SW_RSVD_FMASK	GENMASK(10, 0)
#define IPA_CMD_DMA32_TASK_CMPLT_FMASK		GENMASK(11, 11)
#define IPA_CMD_DMA32_TASK_EOF_FMASK		GENMASK(12, 12)
#define IPA_CMD_DMA32_TASK_FLSH_FMASK		GENMASK(13, 13)
#define IPA_CMD_DMA32_TASK_LOCK_FMASK		GENMASK(14, 14)
#define IPA_CMD_DMA32_TASK_UNLOCK_FMASK		GENMASK(15, 15)
#define IPA_CMD_DMA32_SIZE1_FMASK		GENMASK(31, 16)
#define IPA_CMD_DMA32_PACKET_SIZE_FMASK		GENMASK(15, 0)

struct ipa_imm_cmd_hw_dma_task_32b_addr {
	u32 size1_flags;
	u32 addr1;
	u32 packet_size;
	u32 reserved;
};

/* Use a 32-bit DMA command to zero a block of memory */
int ipa_cmd_dma_task_32(struct ipa *ipa, size_t size, dma_addr_t addr)
{
	struct ipa_imm_cmd_hw_dma_task_32b_addr *payload;
	u32 size1_flags;
	int ret;

	if (size > field_max(IPA_CMD_DMA32_SIZE1_FMASK))
		return -EINVAL;
	if (size > field_max(IPA_CMD_DMA32_PACKET_SIZE_FMASK))
		return -EINVAL;

	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	/* complete: 0 = don't interrupt; eof: 0 = don't assert eot */
	size1_flags = IPA_CMD_DMA32_TASK_FLSH_FMASK;
	/* lock: 0 = don't lock endpoint; unlock: 0 = don't unlock */
	size1_flags |= u32_encode_bits(size, IPA_CMD_DMA32_SIZE1_FMASK);

	payload->size1_flags = size1_flags;
	payload->addr1 = addr;
	payload->packet_size =
			u32_encode_bits(size, IPA_CMD_DMA32_PACKET_SIZE_FMASK);

	ret = ipa_cmd_timeout(ipa, IPA_CMD_DMA_TASK_32B_ADDR, payload,
			      sizeof(payload));

	kfree(payload);

	return ret;
}
