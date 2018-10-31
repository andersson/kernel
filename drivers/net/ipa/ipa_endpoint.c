// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/if_rmnet.h>
#include <linux/version.h>

#include "gsi.h"
#include "gsi_trans.h"
#include "ipa.h"
#include "ipa_data.h"
#include "ipa_endpoint.h"
#include "ipa_cmd.h"
#include "ipa_mem.h"
#include "ipa_netdev.h"
#include "ipa_gsi.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)

/* This is a hack; upstream uses a function instead of the skb field */
#define netdev_xmit_more()	(skb->xmit_more)

#endif

#define atomic_dec_not_zero(v)	atomic_add_unless((v), -1, 0)

#define IPA_REPLENISH_BATCH	16

#define IPA_RX_BUFFER_SIZE	(PAGE_SIZE << IPA_RX_BUFFER_ORDER)
#define IPA_RX_BUFFER_ORDER	1	/* 8KB endpoint RX buffers (2 pages) */

/* The amount of RX buffer space consumed by standard skb overhead */
#define IPA_RX_BUFFER_OVERHEAD	(PAGE_SIZE - SKB_MAX_ORDER(NET_SKB_PAD, 0))

#define IPA_ENDPOINT_STOP_RETRY_MAX		10
#define IPA_ENDPOINT_STOP_RX_SIZE		1	/* bytes */

#define IPA_ENDPOINT_RESET_AGGR_RETRY_MAX	3
#define IPA_AGGR_TIME_LIMIT_DEFAULT		1000	/* microseconds */

/** enum ipa_status_opcode - status element opcode hardware values */
enum ipa_status_opcode {
	IPA_STATUS_OPCODE_PACKET		= 0x01,
	IPA_STATUS_OPCODE_NEW_FRAG_RULE		= 0x02,
	IPA_STATUS_OPCODE_DROPPED_PACKET	= 0x04,
	IPA_STATUS_OPCODE_SUSPENDED_PACKET	= 0x08,
	IPA_STATUS_OPCODE_LOG			= 0x10,
	IPA_STATUS_OPCODE_DCMP			= 0x20,
	IPA_STATUS_OPCODE_PACKET_2ND_PASS	= 0x40,
};

/** enum ipa_status_exception - status element exception type */
enum ipa_status_exception {
	IPA_STATUS_EXCEPTION_NONE,
	IPA_STATUS_EXCEPTION_DEAGGR,
	IPA_STATUS_EXCEPTION_IPTYPE,
	IPA_STATUS_EXCEPTION_PACKET_LENGTH,
	IPA_STATUS_EXCEPTION_PACKET_THRESHOLD,
	IPA_STATUS_EXCEPTION_FRAG_RULE_MISS,
	IPA_STATUS_EXCEPTION_SW_FILT,
	IPA_STATUS_EXCEPTION_NAT,
	IPA_STATUS_EXCEPTION_IPV6CT,
	IPA_STATUS_EXCEPTION_COUNT,
};

/**
 * struct ipa_status - Abstracted IPA status element
 * @opcode:		Status element type
 * @exception:		The first exception that took place
 * @pkt_len:		Payload length
 * @dst_endpoint:	Destination endpoint
 * @metadata:		32-bit metadata value used by packet
 * @rt_miss:		Flag; if 1, indicates there was a routing rule miss
 *
 * Note that the hardware status element supplies additional information
 * that is currently unused.
 */
struct ipa_status {
	enum ipa_status_opcode opcode;
	enum ipa_status_exception exception;
	u32 pkt_len;
	u32 dst_endpoint;
	u32 metadata;
	u32 rt_miss;
};

/* Field masks for struct ipa_status_raw structure fields */

#define IPA_STATUS_SRC_IDX_FMASK		GENMASK(4, 0)

#define IPA_STATUS_DST_IDX_FMASK		GENMASK(4, 0)

#define IPA_STATUS_FLAGS1_FLT_LOCAL_FMASK	GENMASK(0, 0)
#define IPA_STATUS_FLAGS1_FLT_HASH_FMASK	GENMASK(1, 1)
#define IPA_STATUS_FLAGS1_FLT_GLOBAL_FMASK	GENMASK(2, 2)
#define IPA_STATUS_FLAGS1_FLT_RET_HDR_FMASK	GENMASK(3, 3)
#define IPA_STATUS_FLAGS1_FLT_RULE_ID_FMASK	GENMASK(13, 4)
#define IPA_STATUS_FLAGS1_RT_LOCAL_FMASK	GENMASK(14, 14)
#define IPA_STATUS_FLAGS1_RT_HASH_FMASK		GENMASK(15, 15)
#define IPA_STATUS_FLAGS1_UCP_FMASK		GENMASK(16, 16)
#define IPA_STATUS_FLAGS1_RT_TBL_IDX_FMASK	GENMASK(21, 17)
#define IPA_STATUS_FLAGS1_RT_RULE_ID_FMASK	GENMASK(31, 22)

#define IPA_STATUS_FLAGS2_NAT_HIT_FMASK		GENMASK_ULL(0, 0)
#define IPA_STATUS_FLAGS2_NAT_ENTRY_IDX_FMASK	GENMASK_ULL(13, 1)
#define IPA_STATUS_FLAGS2_NAT_TYPE_FMASK	GENMASK_ULL(15, 14)
#define IPA_STATUS_FLAGS2_TAG_INFO_FMASK	GENMASK_ULL(63, 16)

#define IPA_STATUS_FLAGS3_SEQ_NUM_FMASK		GENMASK(7, 0)
#define IPA_STATUS_FLAGS3_TOD_CTR_FMASK		GENMASK(31, 8)

#define IPA_STATUS_FLAGS4_HDR_LOCAL_FMASK	GENMASK(0, 0)
#define IPA_STATUS_FLAGS4_HDR_OFFSET_FMASK	GENMASK(10, 1)
#define IPA_STATUS_FLAGS4_FRAG_HIT_FMASK	GENMASK(11, 11)
#define IPA_STATUS_FLAGS4_FRAG_RULE_FMASK	GENMASK(15, 12)
#define IPA_STATUS_FLAGS4_HW_SPECIFIC_FMASK	GENMASK(31, 16)

/* Status element provided by hardware */
struct ipa_status_raw {
	u8 opcode;
	u8 exception;
	u16 mask;
	u16 pkt_len;
	u8 endp_src_idx;	/* Only bottom 5 bits valid */
	u8 endp_dst_idx;	/* Only bottom 5 bits valid */
	u32 metadata;
	u32 flags1;
	u64 flags2;
	u32 flags3;
	u32 flags4;
};

static void ipa_endpoint_replenish(struct ipa_endpoint *endpoint, bool add_one);

/* suspend_delay represents suspend for RX, delay for TX endpoints.
 * Note that SUSPEND is not supported starting with IPA v4.0.
 */
static bool
ipa_endpoint_init_ctrl(struct ipa_endpoint *endpoint, bool suspend_delay)
{
	u32 offset = IPA_REG_ENDP_INIT_CTRL_N_OFFSET(endpoint->endpoint_id);
	struct ipa *ipa = endpoint->ipa;
	u32 mask;
	u32 val;

	/* assert(endpoint->toward_ipa || ipa->version == IPA_VERSION_3_5_1 */
	mask = endpoint->toward_ipa ? ENDP_DELAY_FMASK : ENDP_SUSPEND_FMASK;

	val = ioread32(ipa->reg_virt + offset);
	if (suspend_delay == !!(val & mask))
		return false;	/* Already set to desired state */

	val ^= mask;
	iowrite32(val, ipa->reg_virt + offset);

	return true;
}

static void ipa_endpoint_init_cfg(struct ipa_endpoint *endpoint)
{
	u32 offset = IPA_REG_ENDP_INIT_CFG_N_OFFSET(endpoint->endpoint_id);
	u32 val = 0;

	/* FRAG_OFFLOAD_EN is 0 */
	if (endpoint->data->config.checksum) {
		if (endpoint->toward_ipa) {
			u32 checksum_offset;

			val |= u32_encode_bits(IPA_CS_OFFLOAD_UL,
					       CS_OFFLOAD_EN_FMASK);
			/* Checksum header offset is in 4-byte units */
			checksum_offset = sizeof(struct rmnet_map_header);
			checksum_offset /= sizeof(u32);
			val |= u32_encode_bits(checksum_offset,
					       CS_METADATA_HDR_OFFSET_FMASK);
		} else {
			val |= u32_encode_bits(IPA_CS_OFFLOAD_DL,
					       CS_OFFLOAD_EN_FMASK);
		}
	} else {
		val |= u32_encode_bits(IPA_CS_OFFLOAD_NONE,
				       CS_OFFLOAD_EN_FMASK);
	}
	/* CS_GEN_QMB_MASTER_SEL is 0 */

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

static void ipa_endpoint_init_hdr(struct ipa_endpoint *endpoint)
{
	u32 offset = IPA_REG_ENDP_INIT_HDR_N_OFFSET(endpoint->endpoint_id);
	u32 val = 0;

	if (endpoint->data->config.qmap) {
		size_t header_size = sizeof(struct rmnet_map_header);

		if (endpoint->toward_ipa && endpoint->data->config.checksum)
			header_size += sizeof(struct rmnet_map_ul_csum_header);

		val |= u32_encode_bits(header_size, HDR_LEN_FMASK);
		/* metadata is the 4 byte rmnet_map header itself */
		val |= HDR_OFST_METADATA_VALID_FMASK;
		val |= u32_encode_bits(0, HDR_OFST_METADATA_FMASK);
		/* HDR_ADDITIONAL_CONST_LEN is 0; (IPA->AP only) */
		if (!endpoint->toward_ipa) {
			u32 size_offset = offsetof(struct rmnet_map_header,
						   pkt_len);

			val |= HDR_OFST_PKT_SIZE_VALID_FMASK;
			val |= u32_encode_bits(size_offset,
					       HDR_OFST_PKT_SIZE_FMASK);
		}
		/* HDR_A5_MUX is 0 */
		/* HDR_LEN_INC_DEAGG_HDR is 0 */
		/* HDR_METADATA_REG_VALID is 0; (AP->IPA only) */
	}

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

static void ipa_endpoint_init_hdr_ext(struct ipa_endpoint *endpoint)
{
	u32 offset = IPA_REG_ENDP_INIT_HDR_EXT_N_OFFSET(endpoint->endpoint_id);
	u32 pad_align = endpoint->data->config.rx.pad_align;
	u32 val = 0;

	val |= HDR_ENDIANNESS_FMASK;		/* big endian */
	val |= HDR_TOTAL_LEN_OR_PAD_VALID_FMASK;
	/* HDR_TOTAL_LEN_OR_PAD is 0 (pad, not total_len) */
	/* HDR_PAYLOAD_LEN_INC_PADDING is 0 */
	/* HDR_TOTAL_LEN_OR_PAD_OFFSET is 0 */
	if (!endpoint->toward_ipa)
		val |= u32_encode_bits(pad_align, HDR_PAD_TO_ALIGNMENT_FMASK);

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

/**
 * Generate a metadata mask value that will select only the mux_id
 * field in an rmnet_map header structure.  The mux_id is at offset
 * 1 byte from the beginning of the structure, but the metadata
 * value is treated as a 4-byte unit.  So this mask must be computed
 * with endianness in mind.  Note that ipa_endpoint_init_hdr_metadata_mask()
 * will convert this value to the proper byte order.
 *
 * Marked __always_inline because this is really computing a
 * constant value.
 */
static __always_inline __be32 ipa_rmnet_mux_id_metadata_mask(void)
{
	size_t mux_id_offset = offsetof(struct rmnet_map_header, mux_id);
	u32 mux_id_mask = 0;
	u8 *bytes;

	bytes = (u8 *)&mux_id_mask;
	bytes[mux_id_offset] = 0xff;	/* mux_id is 1 byte */

	return cpu_to_be32(mux_id_mask);
}

static void ipa_endpoint_init_hdr_metadata_mask(struct ipa_endpoint *endpoint)
{
	u32 endpoint_id = endpoint->endpoint_id;
	u32 val = 0;
	u32 offset;

	offset = IPA_REG_ENDP_INIT_HDR_METADATA_MASK_N_OFFSET(endpoint_id);

	if (!endpoint->toward_ipa && endpoint->data->config.qmap)
		val = ipa_rmnet_mux_id_metadata_mask();

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

/* Compute the aggregation size value to use for a given buffer size */
static u32 ipa_aggr_size_kb(u32 rx_buffer_size)
{
	BUILD_BUG_ON(IPA_RX_BUFFER_SIZE >
		     field_max(AGGR_BYTE_LIMIT_FMASK) * SZ_1K +
		     IPA_MTU + IPA_RX_BUFFER_OVERHEAD);

	/* Because we don't have the "hard byte limit" enabled, we
	 * need to make sure there's enough space in the buffer to
	 * receive a complete MTU (plus normal skb overhead) beyond
	 * the aggregated size limit we specify.
	 */
	rx_buffer_size -= IPA_MTU + IPA_RX_BUFFER_OVERHEAD;

	return rx_buffer_size / SZ_1K;
}

static void ipa_endpoint_init_aggr(struct ipa_endpoint *endpoint)
{
	const struct ipa_endpoint_config_data *config = &endpoint->data->config;
	u32 offset = IPA_REG_ENDP_INIT_AGGR_N_OFFSET(endpoint->endpoint_id);
	u32 val = 0;

	if (config->aggregation) {
		if (!endpoint->toward_ipa) {
			u32 aggr_size = ipa_aggr_size_kb(IPA_RX_BUFFER_SIZE);
			u32 limit;

			val |= u32_encode_bits(IPA_ENABLE_AGGR, AGGR_EN_FMASK);
			val |= u32_encode_bits(IPA_GENERIC, AGGR_TYPE_FMASK);
			val |= u32_encode_bits(aggr_size,
					       AGGR_BYTE_LIMIT_FMASK);
			BUILD_BUG_ON(!IPA_AGGR_GRANULARITY);
			limit = IPA_AGGR_TIME_LIMIT_DEFAULT;
			val |= u32_encode_bits(limit / IPA_AGGR_GRANULARITY,
					       AGGR_TIME_LIMIT_FMASK);
			val |= u32_encode_bits(0, AGGR_PKT_LIMIT_FMASK);
			if (config->rx.aggr_close_eof)
				val |= AGGR_SW_EOF_ACTIVE_FMASK;
			/* AGGR_HARD_BYTE_LIMIT_ENABLE is 0 */
		} else {
			val |= u32_encode_bits(IPA_ENABLE_DEAGGR,
					       AGGR_EN_FMASK);
			val |= u32_encode_bits(IPA_QCMAP, AGGR_TYPE_FMASK);
			/* other fields ignored */
		}
		/* AGGR_FORCE_CLOSE is 0 */
	} else {
		val |= u32_encode_bits(IPA_BYPASS_AGGR, AGGR_EN_FMASK);
		/* other fields ignored */
	}

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

static void ipa_endpoint_init_mode(struct ipa_endpoint *endpoint)
{
	const struct ipa_endpoint_config_data *config = &endpoint->data->config;
	u32 offset = IPA_REG_ENDP_INIT_MODE_N_OFFSET(endpoint->endpoint_id);
	u32 val = 0;

	if (endpoint->toward_ipa && config->dma_mode) {
		enum ipa_endpoint_name name = config->dma_endpoint;
		u32 dma_endpoint_id;

		dma_endpoint_id = endpoint->ipa->name_map[name]->endpoint_id;

		val |= u32_encode_bits(IPA_DMA, MODE_FMASK);
		val |= u32_encode_bits(dma_endpoint_id, DEST_PIPE_INDEX_FMASK);
	} else {
		val |= u32_encode_bits(IPA_BASIC, MODE_FMASK);
	}
	/* Other bitfields unspecified (and 0) */

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

static void ipa_endpoint_init_deaggr(struct ipa_endpoint *endpoint)
{
	u32 offset = IPA_REG_ENDP_INIT_DEAGGR_N_OFFSET(endpoint->endpoint_id);
	u32 val = 0;

	/* DEAGGR_HDR_LEN is 0 */
	/* PACKET_OFFSET_VALID is 0 */
	/* PACKET_OFFSET_LOCATION is ignored (not valid) */
	/* MAX_PACKET_LEN is 0 (not enforced) */

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

static void ipa_endpoint_init_seq(struct ipa_endpoint *endpoint)
{
	u32 offset = IPA_REG_ENDP_INIT_SEQ_N_OFFSET(endpoint->endpoint_id);
	u32 seq_type = endpoint->data->seq_type;
	u32 val = 0;

	val |= u32_encode_bits(seq_type & 0xf, HPS_SEQ_TYPE_FMASK);
	val |= u32_encode_bits((seq_type >> 4) & 0xf, DPS_SEQ_TYPE_FMASK);
	/* HPS_REP_SEQ_TYPE is 0 */
	/* DPS_REP_SEQ_TYPE is 0 */

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

/* Complete transaction initiated in ipa_endpoint_skb_tx() */
void ipa_endpoint_skb_tx_complete(struct gsi_trans *trans)
{
	struct sk_buff *skb = trans->data;

	dev_kfree_skb_any(skb);
}

/**
 * ipa_endpoint_skb_tx() - Transmit a socket buffer
 * @endpoint:	Endpoint pointer
 * @skb:	Socket buffer to send
 *
 * Returns:	0 if successful, or a negative error code
 */
int ipa_endpoint_skb_tx(struct ipa_endpoint *endpoint, struct sk_buff *skb)
{
	struct gsi_trans *trans;
	u32 nr_frags;
	int ret;

	/* Make sure source endpoint's TLV FIFO has enough entries to
	 * hold the linear portion of the skb and all its fragments.
	 * If not, see if we can linearize it before giving up.
	 */
	nr_frags = skb_shinfo(skb)->nr_frags;
	if (1 + nr_frags > endpoint->trans_tre_max) {
		if (skb_linearize(skb))
			return -ENOMEM;
		nr_frags = 0;
	}

	trans = gsi_channel_trans_alloc(&endpoint->ipa->gsi,
					endpoint->channel_id, 1 + nr_frags);
	if (!trans)
		return -EBUSY;
	trans->data = skb;

	ret = skb_to_sgvec(skb, trans->sgl, 0, skb->len);
	if (ret < 0)
		goto err_trans_free;
	trans->sgc = ret;

	ret = gsi_trans_commit(trans, !netdev_xmit_more());
	if (ret)
		goto err_trans_free;
	return 0;

err_trans_free:
	gsi_trans_free(trans);

	return -ENOMEM;
}

static void ipa_endpoint_status(struct ipa_endpoint *endpoint)
{
	const struct ipa_endpoint_config_data *config = &endpoint->data->config;
	u32 endpoint_id = endpoint->endpoint_id;
	struct ipa *ipa = endpoint->ipa;
	u32 val = 0;
	u32 offset;

	offset = IPA_REG_ENDP_STATUS_N_OFFSET(endpoint_id);

	if (config->status_enable) {
		val |= STATUS_EN_FMASK;
		if (endpoint->toward_ipa) {
			enum ipa_endpoint_name name;
			u32 status_endpoint_id;

			name = config->tx.status_endpoint;
			status_endpoint_id = ipa->name_map[name]->endpoint_id;

			val |= u32_encode_bits(status_endpoint_id,
					       STATUS_ENDP_FMASK);
		}
		/* STATUS_LOCATION is 0 (status element precedes packet) */
		/* The next field is present for IPA v4.0 and above */
		/* STATUS_PKT_SUPPRESS_FMASK is 0 */
	}

	iowrite32(val, ipa->reg_virt + offset);
}

static void ipa_endpoint_skb_copy(struct ipa_endpoint *endpoint,
				  void *data, u32 len, u32 extra)
{
	struct sk_buff *skb;

	skb = __dev_alloc_skb(len, GFP_ATOMIC);
	if (skb) {
		skb_put(skb, len);
		memcpy(skb->data, data, len);
		skb->truesize += extra;
	}

	/* Now receive it, or drop it if there's no netdev */
	if (endpoint->netdev)
		ipa_netdev_skb_rx(endpoint->netdev, skb);
	else if (skb)
		dev_kfree_skb_any(skb);
}

static void ipa_endpoint_skb_build(struct ipa_endpoint *endpoint,
				   struct page *page, u32 len)
{
	struct sk_buff *skb;

	/* assert(len <= SKB_WITH_OVERHEAD(IPA_RX_BUFFER_SIZE-NET_SKB_PAD)); */
	skb = build_skb(page_address(page), IPA_RX_BUFFER_SIZE);
	if (skb) {
		/* Reserve the headroom and account for the data */
		skb_reserve(skb, NET_SKB_PAD);
		skb_put(skb, len);
	}

	/* Now receive it, or drop it if there's no netdev */
	if (endpoint->netdev)
		ipa_netdev_skb_rx(endpoint->netdev, skb);
	else if (skb)
		dev_kfree_skb_any(skb);

	/* If no socket buffer took the pages, free them */
	if (!skb)
		__free_pages(page, IPA_RX_BUFFER_ORDER);
}

/* Maps an exception type returned in a ipa_status_raw structure
 * to the ipa_status_exception value that represents it in
 * the exception field of a ipa_status structure.  Returns
 * IPA_STATUS_EXCEPTION_COUNT for an unrecognized value.
 */
static enum ipa_status_exception exception_map(u8 exception, bool is_ipv6)
{
	switch (exception) {
	case 0x00:	return IPA_STATUS_EXCEPTION_NONE;
	case 0x01:	return IPA_STATUS_EXCEPTION_DEAGGR;
	case 0x04:	return IPA_STATUS_EXCEPTION_IPTYPE;
	case 0x08:	return IPA_STATUS_EXCEPTION_PACKET_LENGTH;
	case 0x10:	return IPA_STATUS_EXCEPTION_FRAG_RULE_MISS;
	case 0x20:	return IPA_STATUS_EXCEPTION_SW_FILT;
	case 0x40:	return is_ipv6 ? IPA_STATUS_EXCEPTION_IPV6CT
				       : IPA_STATUS_EXCEPTION_NAT;
	default:	return IPA_STATUS_EXCEPTION_COUNT;
	}
}

/* A rule miss is indicated as an all-1's value in the rt_rule_id
 * or flt_rule_id field of the ipa_status structure.
 */
static bool ipa_rule_miss_id(u32 id)
{
	return id == field_max(IPA_STATUS_FLAGS1_RT_RULE_ID_FMASK);
}

static void ipa_status_parse(struct ipa_status *status, void *data, u32 count)
{
	const struct ipa_status_raw *status_raw = data;
	bool is_ipv6;
	u32 val;

	BUILD_BUG_ON(sizeof(*status_raw) % 4);

	status->opcode = status_raw->opcode;
	is_ipv6 = status_raw->mask & BIT(7) ? false : true;
	status->exception = exception_map(status_raw->exception, is_ipv6);
	status->pkt_len = status_raw->pkt_len;
	val = u32_get_bits(status_raw->endp_dst_idx, IPA_STATUS_DST_IDX_FMASK);
	status->dst_endpoint = val;
	status->metadata = status_raw->metadata;
	val = u32_get_bits(status_raw->flags1,
			   IPA_STATUS_FLAGS1_RT_RULE_ID_FMASK);
	status->rt_miss = ipa_rule_miss_id(val) ? 1 : 0;
}

/* The format of a packet status element is the same for several status
 * types (opcodes).  The NEW_FRAG_RULE, LOG, DCMP (decompression) types
 * aren't currently supported
 */
static bool ipa_status_format_packet(enum ipa_status_opcode opcode)
{
	switch (opcode) {
	case IPA_STATUS_OPCODE_PACKET:
	case IPA_STATUS_OPCODE_DROPPED_PACKET:
	case IPA_STATUS_OPCODE_SUSPENDED_PACKET:
	case IPA_STATUS_OPCODE_PACKET_2ND_PASS:
		return true;
	default:
		return false;
	}
}

static bool ipa_endpoint_status_skip(struct ipa_endpoint *endpoint,
				     struct ipa_status *status)
{
	if (!ipa_status_format_packet(status->opcode))
		return true;
	if (!status->pkt_len)
		return true;
	if (status->dst_endpoint != endpoint->endpoint_id)
		return true;

	return false;	/* Don't skip this packet, process it */
}

static void ipa_endpoint_status_parse(struct ipa_endpoint *endpoint,
				      struct page *page, u32 total_len)
{
	void *data = page_address(page) + NET_SKB_PAD;
	u32 unused = IPA_RX_BUFFER_SIZE - total_len;
	u32 resid = total_len;

	while (resid) {
		const size_t status_size = sizeof(struct ipa_status_raw);
		struct ipa_status status;
		bool drop_packet = false;
		u32 align;
		u32 len;

		if (resid < status_size) {
			dev_err(&endpoint->ipa->pdev->dev,
				"short message (%u bytes < %u byte status)\n",
				resid, status_size);
			break;
		}
		ipa_status_parse(&status, data, resid);

		/* Skip over status packets that lack packet data */
		if (ipa_endpoint_status_skip(endpoint, &status)) {
			data += status_size;
			resid -= status_size;
			continue;
		}

		/* Packet data follows the status structure.  Unless
		 * the packet failed to match a routing rule, or it
		 * had a deaggregation exception, we'll consume it.
		 */
		if (status.exception == IPA_STATUS_EXCEPTION_NONE) {
			if (status.rt_miss)
				drop_packet = true;
		} else if (status.exception == IPA_STATUS_EXCEPTION_DEAGGR) {
			drop_packet = true;
		}

		/* Compute the amount of buffer space consumed by the
		 * packet, including the status element.  If the hardware
		 * is configured to pad packet data to an aligned boundary,
		 * account for that.  And if checksum offload is is enabled
		 * a trailer containing computed checksum information will
		 * be appended.
		 */
		align = endpoint->data->config.rx.pad_align ? : 1;
		len = status_size + ALIGN(status.pkt_len, align);
		if (endpoint->data->config.checksum)
			len += sizeof(struct rmnet_map_dl_csum_trailer);

		/* Charge the new packet with a proportional fraction of
		 * the unused space in the original receive buffer.
		 * XXX Charge a proportion of the *whole* receive buffer?
		 */
		if (!drop_packet) {
			u32 extra = unused * len / total_len;
			void *data2 = data + status_size;
			u32 len2 = status.pkt_len;

			/* Client receives only packet data (no status) */
			ipa_endpoint_skb_copy(endpoint, data2, len2, extra);
		}

		/* Consume status and the full packet it describes */
		data += len;
		resid -= len;
	}

	__free_pages(page, IPA_RX_BUFFER_ORDER);
}

/* Complete transaction initiated in ipa_endpoint_replenish_one() */
void ipa_endpoint_rx_complete(struct gsi_trans *trans)
{
	struct page *page = trans->data;
	struct ipa_endpoint *endpoint;
	struct ipa *ipa;

	ipa = container_of(trans->gsi, struct ipa, gsi);
	endpoint = ipa->channel_map[trans->channel_id];

	ipa_endpoint_replenish(endpoint, true);

	if (trans->result == -ECANCELED) {
		__free_pages(page, IPA_RX_BUFFER_ORDER);
		return;
	}

	/* Parse or build a socket buffer using the actual received length */
	if (endpoint->data->config.status_enable)
		ipa_endpoint_status_parse(endpoint, page, trans->len);
	else
		ipa_endpoint_skb_build(endpoint, page, trans->len);
}

static int ipa_endpoint_replenish_one(struct ipa_endpoint *endpoint)
{
	struct gsi_trans *trans;
	bool doorbell = false;
	struct page *page;
	u32 offset;
	u32 len;

	page = dev_alloc_pages(IPA_RX_BUFFER_ORDER);
	if (!page)
		return -ENOMEM;
	offset = NET_SKB_PAD;
	len = IPA_RX_BUFFER_SIZE - offset;

	trans = gsi_channel_trans_alloc(&endpoint->ipa->gsi,
					endpoint->channel_id, 1);
	if (!trans)
		goto err_page_free;
	trans->data = page;

	/* Set up and map a scatterlist entry representing the buffer */
	sg_init_table(trans->sgl, trans->sgc);
	sg_set_page(trans->sgl, page, len, offset);

	if (++endpoint->replenish_ready == IPA_REPLENISH_BATCH) {
		doorbell = true;
		endpoint->replenish_ready = 0;
	}

	if (!gsi_trans_commit(trans, doorbell))
		return 0;

err_page_free:
	__free_pages(page, IPA_RX_BUFFER_ORDER);

	return -ENOMEM;
}

/**
 * ipa_endpoint_replenish() - Replenish the Rx packets cache.
 *
 * Allocate RX packet wrapper structures with maximal socket buffers
 * for an endpoint.  These are supplied to the hardware, which fills
 * them with incoming data.
 */
static void ipa_endpoint_replenish(struct ipa_endpoint *endpoint, bool add_one)
{
	struct gsi *gsi;
	u32 backlog;

	if (add_one) {
		if (endpoint->replenish_enabled)
			atomic_inc(&endpoint->replenish_backlog);
		else
			atomic_inc(&endpoint->replenish_saved);
	}

	if (!endpoint->replenish_enabled)
		return;

	while (atomic_dec_not_zero(&endpoint->replenish_backlog))
		if (ipa_endpoint_replenish_one(endpoint))
			goto try_again_later;

	return;

try_again_later:
	/* The last one didn't succeed, so fix the backlog */
	backlog = atomic_inc_return(&endpoint->replenish_backlog);

	/* Whenever a receive buffer transaction completes we'll try to
	 * replenish again.  It's unlikely, but if we fail to supply even
	 * one buffer, nothing will trigger another replenish attempt.
	 * If this happens, schedule work to try again.
	 */
	gsi = &endpoint->ipa->gsi;
	if (backlog == gsi_channel_trans_max(gsi, endpoint->channel_id))
		schedule_delayed_work(&endpoint->replenish_work,
				      msecs_to_jiffies(1));
}

static void ipa_endpoint_replenish_enable(struct ipa_endpoint *endpoint)
{
	struct gsi *gsi = &endpoint->ipa->gsi;
	u32 max_backlog;
	u32 saved;

	endpoint->replenish_enabled = true;
	while ((saved = atomic_xchg(&endpoint->replenish_saved, 0)))
		atomic_add(saved, &endpoint->replenish_backlog);

	/* Start replenishing if hardware currently has no buffers */
	max_backlog = gsi_channel_trans_max(gsi, endpoint->channel_id);
	if (atomic_read(&endpoint->replenish_backlog) == max_backlog) {
		ipa_endpoint_replenish(endpoint, false);
		return;
	}
}

static void ipa_endpoint_replenish_disable(struct ipa_endpoint *endpoint)
{
	endpoint->replenish_enabled = false;
}

static void ipa_endpoint_replenish_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct ipa_endpoint *endpoint;

	endpoint = container_of(dwork, struct ipa_endpoint, replenish_work);

	ipa_endpoint_replenish(endpoint, false);
}

static void ipa_endpoint_default_route_set(struct ipa *ipa, u32 endpoint_id)
{
	u32 val;

	/* ROUTE_DIS is 0 */
	val = u32_encode_bits(endpoint_id, ROUTE_DEF_PIPE_FMASK);
	val |= ROUTE_DEF_HDR_TABLE_FMASK;
	val |= u32_encode_bits(0, ROUTE_DEF_HDR_OFST_FMASK);
	val |= u32_encode_bits(endpoint_id, ROUTE_FRAG_DEF_PIPE_FMASK);
	val |= ROUTE_DEF_RETAIN_HDR_FMASK;

	iowrite32(val, ipa->reg_virt + IPA_REG_ROUTE_OFFSET);
}
/**
 * ipa_endpoint_default_route_init() - Configure IPA default route
 * @ipa:	IPA pointer
 * @client:	Client to which exceptions should be directed
 */
void ipa_endpoint_default_route_setup(struct ipa_endpoint *endpoint)
{
	ipa_endpoint_default_route_set(endpoint->ipa, endpoint->endpoint_id);
}

/**
 * ipa_endpoint_default_route_teardown() -
 *			Inverse of ipa_endpoint_default_route_setup()
 * @ipa:	IPA pointer
 */
void ipa_endpoint_default_route_teardown(struct ipa_endpoint *endpoint)
{
	ipa_endpoint_default_route_set(endpoint->ipa, 0);
}

/**
 * ipa_endpoint_stop()- Stops a GSI channel in IPA
 * @client:	Client whose endpoint should be stopped
 *
 * This function implements the sequence to stop a GSI channel
 * in IPA. This function returns when the channel is is STOP state.
 *
 * Return value: 0 on success, negative otherwise
 */
int ipa_endpoint_stop(struct ipa_endpoint *endpoint)
{
	size_t size = IPA_ENDPOINT_STOP_RX_SIZE;
	struct ipa *ipa = endpoint->ipa;
	struct gsi *gsi = &ipa->gsi;
	struct device *dev;
	void *virt = NULL;
	dma_addr_t addr;
	int ret;
	int i;

	/* If an RX endpoint doesn't stop right away, delay for a bit
	 * (1-2 milliseconds) and try again.  For IPA v3.5.1, we issue
	 * a small (1-byte) DMA command before the retry.
	 */
	if (ipa->version == IPA_VERSION_3_5_1 && !endpoint->toward_ipa) {
		dev = &ipa->pdev->dev;
		virt = dma_alloc_coherent(dev, size, &addr, GFP_KERNEL);
		if (!virt)
			return -ENOMEM;
	}

	for (i = 0; i < IPA_ENDPOINT_STOP_RETRY_MAX; i++) {
		ret = gsi_channel_stop(gsi, endpoint->channel_id);
		if (ret != -EAGAIN)
			break;

		if (endpoint->toward_ipa)
			continue;

		/* For IPA v3.5.1, send a 1 byte 32-bit DMA task */
		if (virt) {
			ret = ipa_cmd_dma_task_32(endpoint->ipa, size, addr);
			if (ret)
				break;
		}

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
	}
	if (i >= IPA_ENDPOINT_STOP_RETRY_MAX)
		ret = -EIO;

	if (virt)
		dma_free_coherent(dev, size, virt, addr);

	return ret;
}

static bool ipa_endpoint_enabled(struct ipa_endpoint *endpoint)
{
	return !!(endpoint->ipa->enabled & BIT(endpoint->endpoint_id));
}

int ipa_endpoint_enable_one(struct ipa_endpoint *endpoint)
{
	struct ipa *ipa = endpoint->ipa;
	int ret;

	ret = gsi_channel_start(&ipa->gsi, endpoint->channel_id);
	if (ret)
		return ret;

	ipa_interrupt_suspend_enable(ipa->interrupt, endpoint->endpoint_id);

	if (!endpoint->toward_ipa)
		ipa_endpoint_replenish_enable(endpoint);

	ipa->enabled |= BIT(endpoint->endpoint_id);

	return 0;
}

void ipa_endpoint_disable_one(struct ipa_endpoint *endpoint)
{
	struct ipa *ipa = endpoint->ipa;
	int ret;

	if (!endpoint->toward_ipa)
		ipa_endpoint_replenish_disable(endpoint);

	ipa_interrupt_suspend_disable(ipa->interrupt, endpoint->endpoint_id);

	ret = ipa_endpoint_stop(endpoint);
	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d attempting to stop endpoint %u\n", ret,
			endpoint->endpoint_id);

	if (!ret)
		endpoint->ipa->enabled &= ~BIT(endpoint->endpoint_id);
}

static bool ipa_endpoint_aggr_active(struct ipa_endpoint *endpoint)
{
	u32 mask = BIT(endpoint->endpoint_id);
	struct ipa *ipa = endpoint->ipa;
	u32 offset;
	u32 val;

	/* assert(mask & ipa->available); */
	offset = ipa_reg_state_aggr_active_offset(ipa->version);
	val = ioread32(ipa->reg_virt + offset);

	return !!(val & mask);
}

static void ipa_endpoint_force_close(struct ipa_endpoint *endpoint)
{
	u32 mask = BIT(endpoint->endpoint_id);
	struct ipa *ipa = endpoint->ipa;

	/* assert(mask & ipa->available); */
	iowrite32(mask, ipa->reg_virt + IPA_REG_AGGR_FORCE_CLOSE_OFFSET);
}

/**
 * ipa_endpoint_reset_rx_aggr() - Reset RX endpoint with aggregation active
 * @endpoint:	Endpoint to be reset
 *
 * If aggregation is active on an RX endpoint when a reset is performed
 * on its underlying GSI channel, a special sequence of actions must be
 * taken to ensure the IPA pipeline is properly cleared.
 *
 * @Return:	0 if successful, or a negative error code
 */
static int ipa_endpoint_reset_rx_aggr(struct ipa_endpoint *endpoint)
{
	struct device *dev = &endpoint->ipa->pdev->dev;
	struct ipa *ipa = endpoint->ipa;
	bool endpoint_suspended = false;
	struct gsi *gsi = &ipa->gsi;
	dma_addr_t addr;
	bool db_enable;
	u32 len = 1;
	void *virt;
	int ret;
	int i;

	virt = kzalloc(len, GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	addr = dma_map_single(dev, virt, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, addr)) {
		ret = -ENOMEM;
		goto out_free_virt;
	}

	/* Force close aggregation before issuing the reset */
	ipa_endpoint_force_close(endpoint);

	/* Reset and reconfigure the channel with the doorbell engine disabled.
	 * Then poll until we know aggregation is no longer active.  We'll
	 * re-enable the doorbell (if appropriate) when we reset below.
	 */
	ret = gsi_channel_reset(gsi, endpoint->channel_id, false);
	if (ret)
		goto out_unmap_addr;

	if (endpoint->ipa->version == IPA_VERSION_3_5_1)
		if (ipa_endpoint_init_ctrl(endpoint, false))
			endpoint_suspended = true;

	/* Start channel and do a 1 byte read */
	ret = gsi_channel_start(gsi, endpoint->channel_id);
	if (ret)
		goto out_suspend_again;

	ret = gsi_trans_read_byte(gsi, endpoint->channel_id, addr);
	if (ret)
		goto err_stop_channel;

	/* Wait for aggregation to be closed on the channel */
	for (i = 0; i < IPA_ENDPOINT_RESET_AGGR_RETRY_MAX; i++) {
		if (!ipa_endpoint_aggr_active(endpoint))
			break;
		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
	}
	if (ipa_endpoint_aggr_active(endpoint))
		dev_err(dev, "endpoint %u still active during reset\n",
			endpoint->endpoint_id);

	gsi_trans_read_byte_done(gsi, endpoint->channel_id);

	ret = ipa_endpoint_stop(endpoint);
	if (ret)
		goto out_suspend_again;

	/* Finally, reset and reconfigure the channel again (re-enabling the
	 * the doorbell engine if appropriate).  Sleep for 1 millisecond to
	 * complete the channel reset sequence.  Finish by suspending the
	 * channel again (if necessary).
	 */
	db_enable = ipa->version == IPA_VERSION_3_5_1;
	ret = gsi_channel_reset(gsi, endpoint->channel_id, db_enable);

	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	goto out_suspend_again;

err_stop_channel:
	ipa_endpoint_stop(endpoint);
out_suspend_again:
	if (endpoint_suspended)
		(void)ipa_endpoint_init_ctrl(endpoint, true);
out_unmap_addr:
	dma_unmap_single(dev, addr, len, DMA_FROM_DEVICE);
out_free_virt:
	kfree(virt);

	return ret;
}

static void ipa_endpoint_reset(struct ipa_endpoint *endpoint)
{
	u32 channel_id = endpoint->channel_id;
	struct ipa *ipa = endpoint->ipa;
	struct gsi *gsi = &ipa->gsi;
	bool db_enable;
	int ret;

	/* For TX endpoints, or RX endpoints without aggregation active,
	 * we only need to reset the underlying GSI channel.
	 */
	db_enable = ipa->version == IPA_VERSION_3_5_1;
	if (!endpoint->toward_ipa && endpoint->data->config.aggregation) {
		if (ipa_endpoint_aggr_active(endpoint))
			ret = ipa_endpoint_reset_rx_aggr(endpoint);
		else
			ret = gsi_channel_reset(gsi, channel_id, db_enable);
	} else {
		ret = gsi_channel_reset(gsi, channel_id, db_enable);
	}

	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d resetting channel %u for endpoint %u\n",
			ret, endpoint->channel_id, endpoint->endpoint_id);
}

/**
 * ipa_endpoint_suspend_aggr() - Emulate suspend interrupt
 * @endpoint_id:	Endpoint on which to emulate a suspend
 *
 *  Emulate suspend IPA interrupt to unsuspend an endpoint suspended
 *  with an open aggregation frame.  This is to work around a hardware
 *  issue in IPA version 3.5.1 where the suspend interrupt will not be
 *  generated when it should be.
 */
static void ipa_endpoint_suspend_aggr(struct ipa_endpoint *endpoint)
{
	struct ipa *ipa = endpoint->ipa;

	/* assert(ipa->version == IPA_VERSION_3_5_1); */

	/* Nothing to do if the endpoint doesn't have aggregation open */
	if (!ipa_endpoint_aggr_active(endpoint))
		return;

	/* Force close aggregation */
	ipa_endpoint_force_close(endpoint);

	ipa_interrupt_simulate_suspend(ipa->interrupt);
}

void ipa_endpoint_suspend(struct ipa_endpoint *endpoint)
{
	struct gsi *gsi = &endpoint->ipa->gsi;

	if (!ipa_endpoint_enabled(endpoint))
		return;

	if (!endpoint->toward_ipa) {
		int ret;

		ipa_endpoint_replenish_disable(endpoint);

		if (endpoint->ipa->version == IPA_VERSION_3_5_1) {
			ret = ipa_endpoint_init_ctrl(endpoint, true);

			/* Due to a hardware bug, a client suspended with
			 * an open aggregation frame will not generate a
			 * SUSPEND IPA interrupt.  We work around this by
			 * force-closing the aggregation frame, then
			 * simulating the arrival of such an interrupt.
			 */
			if (!ret && endpoint->data->config.aggregation)
				ipa_endpoint_suspend_aggr(endpoint);
		} else {
			ret = gsi_channel_stop(gsi, endpoint->endpoint_id);
		}

		if (ret)
			dev_err(&endpoint->ipa->pdev->dev,
				"error %d suspending endpoint %u\n",
				ret, endpoint->endpoint_id);
		else
			ipa_endpoint_replenish_enable(endpoint);
	}

	gsi_channel_trans_quiesce(gsi, endpoint->channel_id, false);
}

void ipa_endpoint_resume(struct ipa_endpoint *endpoint)
{
	if (!ipa_endpoint_enabled(endpoint))
		return;

	if (!endpoint->toward_ipa) {
		struct gsi *gsi = &endpoint->ipa->gsi;
		int ret;

		if (endpoint->ipa->version == IPA_VERSION_3_5_1)
			ret = ipa_endpoint_init_ctrl(endpoint, false);
		else
			ret = gsi_channel_start(gsi, endpoint->endpoint_id);

		if (ret)
			dev_err(&endpoint->ipa->pdev->dev,
				"error %d resuming endpoint %u\n",
				ret, endpoint->endpoint_id);
		else
			ipa_endpoint_replenish_enable(endpoint);
	}
}

static void ipa_endpoint_program(struct ipa_endpoint *endpoint)
{
	if (endpoint->toward_ipa) {
		bool delay_mode = !!endpoint->data->config.tx.delay;

		(void)ipa_endpoint_init_ctrl(endpoint, delay_mode);
		ipa_endpoint_init_hdr_ext(endpoint);
		ipa_endpoint_init_aggr(endpoint);
		ipa_endpoint_init_deaggr(endpoint);
		ipa_endpoint_init_seq(endpoint);
	} else {
		if (endpoint->ipa->version == IPA_VERSION_3_5_1)
			(void)ipa_endpoint_init_ctrl(endpoint, false);
		ipa_endpoint_init_hdr_ext(endpoint);
		ipa_endpoint_init_aggr(endpoint);
	}
	ipa_endpoint_init_cfg(endpoint);
	ipa_endpoint_init_hdr(endpoint);
	ipa_endpoint_init_hdr_metadata_mask(endpoint);
	ipa_endpoint_init_mode(endpoint);
	ipa_endpoint_status(endpoint);
}

static void ipa_endpoint_setup_one(struct ipa_endpoint *endpoint)
{
	struct gsi *gsi = &endpoint->ipa->gsi;
	u32 channel_id = endpoint->channel_id;

	/* Only AP endpoints get configured */
	if (endpoint->ee_id != GSI_EE_AP)
		return;

	endpoint->trans_tre_max = gsi_channel_trans_tre_max(gsi, channel_id);
	if (!endpoint->toward_ipa) {
		endpoint->replenish_enabled = false;
		atomic_set(&endpoint->replenish_saved,
			   gsi_channel_trans_max(gsi, endpoint->channel_id));
		atomic_set(&endpoint->replenish_backlog, 0);
		INIT_DELAYED_WORK(&endpoint->replenish_work,
				  ipa_endpoint_replenish_work);
	}

	ipa_endpoint_program(endpoint);

	endpoint->ipa->set_up |= BIT(endpoint->endpoint_id);
}

static void ipa_endpoint_teardown_one(struct ipa_endpoint *endpoint)
{
	if (!endpoint->toward_ipa)
		cancel_delayed_work_sync(&endpoint->replenish_work);

	ipa_endpoint_reset(endpoint);

	endpoint->ipa->set_up &= ~BIT(endpoint->endpoint_id);
}

void ipa_endpoint_setup(struct ipa *ipa)
{
	u32 initialized = ipa->initialized;

	ipa->set_up = 0;
	while (initialized) {
		u32 endpoint_id = __ffs(initialized);

		initialized ^= BIT(endpoint_id);

		ipa_endpoint_setup_one(&ipa->endpoint[endpoint_id]);
	}
}

void ipa_endpoint_teardown(struct ipa *ipa)
{
	u32 set_up = ipa->set_up;

	while (set_up) {
		u32 endpoint_id = __fls(set_up);

		set_up ^= BIT(endpoint_id);

		ipa_endpoint_teardown_one(&ipa->endpoint[endpoint_id]);
	}
}

int ipa_endpoint_config(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	u32 initialized;
	u32 rx_base;
	u32 rx_mask;
	u32 tx_mask;
	int ret = 0;
	u32 max;
	u32 val;

	/* Find out about the endpoints supplied by the hardware, and ensure
	 * the highest one doesn't exceed the number we support.
	 */
	val = ioread32(ipa->reg_virt + IPA_REG_FLAVOR_0_OFFSET);

	/* Our RX is an IPA producer */
	rx_base = u32_get_bits(val, BAM_PROD_LOWEST_FMASK);
	max = rx_base + u32_get_bits(val, BAM_MAX_PROD_PIPES_FMASK);
	if (max > IPA_ENDPOINT_MAX)
		return -EINVAL;
	rx_mask = GENMASK(max - 1, rx_base);

	/* Our TX is an IPA consumer */
	max = u32_get_bits(val, BAM_MAX_CONS_PIPES_FMASK);
	tx_mask = GENMASK(max - 1, 0);

	ipa->available = rx_mask | tx_mask;

	/* Verify endpoints, now that we know what the hardware supports */
	initialized = ipa->initialized;
	while (initialized) {
		u32 endpoint_id = __ffs(initialized);
		struct ipa_endpoint *endpoint;
		u32 mask = BIT(endpoint_id);

		initialized ^= mask;

		/* Make sure it's an endpoint supported by the hardware */
		if (!(mask & ipa->available)) {
			dev_err(dev, "unsupported endpoint id %u\n",
				endpoint_id);
			ret = -EINVAL;
			continue;	/* Report all bad endpoints */
		}

		/* And make sure it's pointing in the right direction */
		endpoint = &ipa->endpoint[endpoint_id];
		if (endpoint_id < rx_base != !!endpoint->toward_ipa) {
			dev_err(dev, "endpoint id %u wrong direction\n",
				endpoint_id);
			ret = -EINVAL;
		}
	}

	return ret;
}

void ipa_endpoint_deconfig(struct ipa *ipa)
{
	/* Nothing to do */
}

static int ipa_endpoint_init_one(struct ipa *ipa, enum ipa_endpoint_name name,
				 const struct ipa_gsi_endpoint_data *data)
{
	struct ipa_endpoint *endpoint;

	endpoint = &ipa->endpoint[data->endpoint_id];

	if (data->ee_id == GSI_EE_AP)
		ipa->channel_map[data->channel_id] = endpoint;
	ipa->name_map[name] = endpoint;

	endpoint->ipa = ipa;
	endpoint->ee_id = data->ee_id;
	endpoint->channel_id = data->channel_id;
	endpoint->endpoint_id = data->endpoint_id;
	endpoint->toward_ipa = data->toward_ipa;
	endpoint->data = &data->endpoint;

	if (endpoint->data->support_flt)
		ipa->filter_support |= BIT(endpoint->endpoint_id);

	ipa->initialized |= BIT(endpoint->endpoint_id);

	return 0;
}

void ipa_endpoint_exit_one(struct ipa_endpoint *endpoint)
{
	endpoint->ipa->initialized &= ~BIT(endpoint->endpoint_id);
}

static int ipa_endpoint_check_one(struct ipa *ipa, enum ipa_endpoint_name name)
{
	struct ipa_endpoint *endpoint = &ipa->endpoint[name];
	const struct ipa_endpoint_config_data *config;

	if (!endpoint->data)
		return 0;		/* Not initialized */

	if (!endpoint->toward_ipa)
		return 0;		/* Nothing more to check */

	config = &endpoint->data->config;

	/* If status is enabled, make sure the status endpoint is defined */
	if (config->status_enable)
		if (!ipa->name_map[config->tx.status_endpoint])
			return -EINVAL;

	/* If DMA mode is enabled, make sure the DMA endpoint is defined */
	if (config->dma_mode)
		if (!ipa->name_map[config->dma_endpoint])
			return -EINVAL;

	return 0;
}

static int ipa_endpoint_check(struct ipa *ipa)
{
	enum ipa_endpoint_name name;

	/* The AP command TX endpoint is fundamental */
	if (!ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX])
		return -EINVAL;

	/* The AP LAN RX endpoint is used as the default route */
	if (!ipa->name_map[IPA_ENDPOINT_AP_LAN_RX])
		return -EINVAL;

	/* The AP modem TX and RX endpoints are required for the network */
	if (!ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX])
		return -EINVAL;
	if (!ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX])
		return -EINVAL;

	/* Now check endpoints whose configuration refers to other endpoints */
	for (name = 0; name < IPA_ENDPOINT_COUNT; name++)
		if (ipa_endpoint_check_one(ipa, name))
			return -EINVAL;

	return 0;
}

int ipa_endpoint_init(struct ipa *ipa, u32 count,
		      const struct ipa_gsi_endpoint_data *data)
{
	enum ipa_endpoint_name name;
	u32 initialized;
	int ret;

	if (count > IPA_ENDPOINT_MAX)
		return -EIO;

	ipa->initialized = 0;

	ipa->filter_support = 0;
	for (name = 0; name < count; name++) {
		if (ipa_gsi_endpoint_data_empty(&data[name]))
			continue;	/* Skip over empty slots */

		ret = ipa_endpoint_init_one(ipa, name, &data[name]);
		if (ret)
			goto err_endpoint_unwind;
	}

	/* Verify all required endpoints are defined */
	ret = ipa_endpoint_check(ipa);
	if (ret)
		goto err_endpoint_unwind;

	dev_dbg(&ipa->pdev->dev, "initialized 0x%08x\n", ipa->initialized);

	/* Verify the bitmap of endpoints that support filtering. */
	dev_dbg(&ipa->pdev->dev, "filter_support 0x%08x\n",
		ipa->filter_support);
	if (!ipa->filter_support)
		goto err_endpoint_unwind;
	if (hweight32(ipa->filter_support) > IPA_MEM_FLT_COUNT)
		goto err_endpoint_unwind;

	return 0;

err_endpoint_unwind:
	initialized = ipa->initialized;
	while (initialized) {
		u32 endpoint_id = __fls(initialized);

		initialized ^= BIT(endpoint_id);

		ipa_endpoint_exit_one(&ipa->endpoint[endpoint_id]);
	}
	ipa->filter_support = 0;

	return ret;
}

void ipa_endpoint_exit(struct ipa *ipa)
{
	u32 initialized = ipa->initialized;

	while (initialized) {
		u32 endpoint_id = __fls(initialized);

		initialized ^= BIT(endpoint_id);

		ipa_endpoint_exit_one(&ipa->endpoint[endpoint_id]);
	}
	ipa->filter_support = 0;
}
