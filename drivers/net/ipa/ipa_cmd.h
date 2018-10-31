/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */
#ifndef _IPA_CMD_H_
#define _IPA_CMD_H_

#include <linux/types.h>

struct sk_buff;

struct ipa;

/**
 * enum ipa_cmd_opcode:	IPA immediate commands
 *
 * All immediate commands are issued using the AP command TX endpoint.
 * The numeric values here are the opcodes for IPA v3.5.1 hardware.
 *
 * IPA_CMD_NONE is a special (invalid) value that's used to indicate
 * a request is *not* an immediate command.
 */
enum ipa_cmd_opcode {
	IPA_CMD_NONE			= 0,
	IPA_CMD_IP_V4_FILTER_INIT	= 3,
	IPA_CMD_IP_V6_FILTER_INIT	= 4,
	IPA_CMD_IP_V4_ROUTING_INIT	= 7,
	IPA_CMD_IP_V6_ROUTING_INIT	= 8,
	IPA_CMD_HDR_INIT_LOCAL		= 9,
	IPA_CMD_DMA_TASK_32B_ADDR	= 17,
	IPA_CMD_DMA_SHARED_MEM		= 19,
};

/**
 * ipa_cmd_hdr_init_local() - Initialize header space in IPA-local memory
 * @ipa:	IPA structure
 * @offset:	Offset of memory to be initialized
 * @size:	Size of memory to be initialized
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Defines the location of a block of local memory to use for
 * headers and fills it with zeroes.
 */
int ipa_cmd_hdr_init_local(struct ipa *ipa, u32 offset, u32 size);

/**
 * ipa_cmd_mem_dma_zero() - Use a DMA command to zero a block of memory
 * @ipa:	IPA structure
 * @offset:	Offset of memory to be zeroed
 * @size:	Size in bytes of memory to be zeroed
 *
 * @Return:	0 if successful, or a negative error code
 */
int ipa_cmd_mem_dma_zero(struct ipa *ipa, u32 offset, u32 size);

/**
 * ipa_cmd_route_config_ipv4() - Configure IPv4 routing table
 * @ipa:	IPA structure
 * @size:	Size in bytes of table
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Defines the location and size of the IPv4 routing table and
 * zeroes its content.
 */
int ipa_cmd_route_config_ipv4(struct ipa *ipa, size_t size);

/**
 * ipa_cmd_route_config_ipv6() - Configure IPv6 routing table
 * @ipa:	IPA structure
 * @size:	Size in bytes of table
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Defines the location and size of the IPv6 routing table and
 * zeroes its content.
 */
int ipa_cmd_route_config_ipv6(struct ipa *ipa, size_t size);

/**
 * ipa_cmd_filter_config_ipv4() - Configure IPv4 filter table
 * @ipa:	IPA structure
 * @size:	Size in bytes of table
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Defines the location and size of the IPv4 filter table and
 * zeroes its content.
 */
int ipa_cmd_filter_config_ipv4(struct ipa *ipa, size_t size);

/**
 * ipa_cmd_filter_config_ipv6() - Configure IPv6 filter table
 * @ipa:	IPA structure
 * @size:	Size in bytes of table
 *
 * @Return:	0 if successful, or a negative error code
 *
 * Defines the location and size of the IPv6 filter table and
 * zeroes its content.
 */
int ipa_cmd_filter_config_ipv6(struct ipa *ipa, size_t size);

/**
 * ipa_cmd_dma_task_32() - Use a 32-bit DMA command to zero a block of memory
 * @ipa:	IPA structure
 * @size:	Size of memory to be zeroed
 * @addr:	DMA address defining start of range to be zeroed
 *
 * @Return:	0 if successful, or a negative error code
 */
int ipa_cmd_dma_task_32(struct ipa *ipa, size_t size, dma_addr_t addr);

#endif /* _IPA_CMD_H_ */
