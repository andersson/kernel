// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>

#include "ipa.h"
#include "ipa_reg.h"
#include "ipa_cmd.h"
#include "ipa_mem.h"
#include "ipa_data.h"

/* "Canary" value placed between memory regions to detect overflow */
#define IPA_MEM_CANARY_VAL		cpu_to_le32(0xdeadbeef)

/**
 * ipa_mem_setup() - Set up IPA AP and modem shared memory areas
 *
 * Set up the IPA-local shared memory areas located in shared memory
 * located in the IPA.  This involves zero-filling each area (using
 * DMA) and then telling the IPA where it's located.  We set up the
 * regions for the header and processing context structures used by
 * both the modem and the AP.
 *
 * The modem and AP header areas are contiguous, with the modem area
 * located at the lower address.  The processing context memory areas
 * for the modem and AP are also contiguous, with the modem at the base
 * of the combined space.
 *
 * The modem portions are also zeroed in ipa_mem_zero_modem(); if it
 * crashes and restarts via SSR these areas need to be re-initialized.
 *
 * Note that not all regions are zeroed, but many regions have "canary"
 * values written by ipa_mem_config() before to their base address in an
 * effort to catch out-of-bounds writes.
 *
 * @Return:	0 if successful, or a negative error code
 */
int ipa_mem_setup(struct ipa *ipa)
{
	u32 offset;
	u32 size;
	int ret;

	/* Initialize IPA-local header memory.  The modem and AP header
	 * regions are contiguous, and initialized together.  QMI is
	 * used to tell the modem the location and size of its portion
	 * of this combined region.
	 */
	offset = ipa->mem[IPA_MEM_MODEM_HEADER].offset;
	size = ipa->mem[IPA_MEM_MODEM_HEADER].size;
	size += ipa->mem[IPA_MEM_AP_HEADER].size;
	ret = ipa_cmd_hdr_init_local(ipa, offset, size);
	if (ret)
		return ret;

	/* Processing context structures for the modem and AP are contiguous.
	 * Here too, QMI is used to tell the modem where to find its portion
	 * of this combined region.
	 */
	offset = ipa->mem[IPA_MEM_MODEM_PROC_CTX].offset;
	size = ipa->mem[IPA_MEM_MODEM_PROC_CTX].size;
	size += ipa->mem[IPA_MEM_AP_PROC_CTX].size;
	ret = ipa_cmd_mem_dma_zero(ipa, offset, size);
	if (ret)
		return ret;

	/* Tell the hardware where the processing context area is located */
	iowrite32(ipa->mem_offset + offset,
		  ipa->reg_virt + IPA_REG_LOCAL_PKT_PROC_CNTXT_BASE_OFFSET);

	return ret;
}

void ipa_mem_teardown(struct ipa *ipa)
{
	/* Nothing to do */
}

static bool ipa_mem_valid(struct ipa *ipa, enum ipa_mem_id mem_id)
{
	const struct ipa_mem *mem = &ipa->mem[mem_id];
	struct device *dev = &ipa->pdev->dev;
	u32 size_multiple;

	/* Other than modem memory, sizes must be a multiple of 8 */
	size_multiple = mem_id == IPA_MEM_MODEM ? 4 : 8;
	if (mem->size % size_multiple)
		dev_err(dev, "region %u size not a multiple of %u bytes\n",
			mem_id, size_multiple);
	else if (mem->offset % 8)
		dev_err(dev, "region %u offset not 8-byte aligned\n", mem_id);
	else if (mem->offset < mem->canary_count * sizeof(__le32))
		dev_err(dev, "region %u offset too small for %u canaries\n",
			mem_id, mem->canary_count);
	else if (mem->offset + mem->size > ipa->mem_size)
		dev_err(dev, "region %u ends beyond memory limit (0x%08x)\n",
			mem_id, ipa->mem_size);
	else
		return true;

	return false;
}

/**
 * ipa_mem_config() - Configure IPA shared memory
 *
 * @Return:	0 if successful, or a negative error code
 */
int ipa_mem_config(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	enum ipa_mem_id mem_id;
	u32 mem_size;
	u32 val;

	/* Check the advertised location and size of the shared memory area */
	val = ioread32(ipa->reg_virt + IPA_REG_SHARED_MEM_SIZE_OFFSET);

	/* The fields in the register are in 8 byte units */
	ipa->mem_offset = 8 * u32_get_bits(val, SHARED_MEM_BADDR_FMASK);
	dev_dbg(dev, "shared memory offset 0x%x bytes\n", ipa->mem_offset);
	/* Make sure the end is within the region's mapped space */
	mem_size = 8 * u32_get_bits(val, SHARED_MEM_SIZE_FMASK);
	dev_dbg(dev, "shared memory size 0x%x bytes\n", mem_size);
	if (ipa->mem_offset + mem_size > ipa->mem_size)
		return -EINVAL;
	ipa->mem_size = mem_size;	/* Update to hardware-reported size */

	/* Verify each defined memory region is valid, and if indicated
	 * for the region, write "canary" values in the space prior to
	 * the region's base address.
	 */
	for (mem_id = 0; mem_id < IPA_MEM_COUNT; mem_id++) {
		const struct ipa_mem *mem = &ipa->mem[mem_id];
		u32 canary_count;
		__le32 *canary;

		/* Validate all regions (even undefined ones) */
		if (!ipa_mem_valid(ipa, mem_id))
			return -EINVAL;

		/* Skip over undefined regions */
		if (!mem->offset && !mem->size)
			continue;

		canary_count = mem->canary_count;
		if (!canary_count)
			continue;

		canary = ipa->mem_virt + ipa->mem_offset + mem->offset;
		do
			*--canary = IPA_MEM_CANARY_VAL;
		while (--canary_count);
	}

	/* Verify the microcontroller ring alignment (0 is OK too) */
	if (ipa->mem[IPA_MEM_UC_EVENT_RING].offset % 1024)
		dev_err(dev, "microcontroller ring not 1024-byte aligned\n");

	return 0;
}

void ipa_mem_deconfig(struct ipa *ipa)
{
	/* Don't bother zeroing any of the shared memory on exit */
}

/**
 * ipa_mem_zero_modem() - Zero modem IPA-local memory regions
 *
 * Zero regions of IPA-local memory used by the modem.  These are
 * configured (and initially zeroed) by ipa_mem_setup(), but if
 * the modem crashes and restarts via SSR we need to re-initialize
 * them.  A QMI message tells the modem where to find regions of
 * IPA local memory it needs to know about (these included).
 */
int ipa_mem_zero_modem(struct ipa *ipa)
{
	int ret;

	ret = ipa_cmd_mem_dma_zero(ipa, ipa->mem[IPA_MEM_MODEM].offset,
				   ipa->mem[IPA_MEM_MODEM].size);
	if (ret)
		return ret;

	ret = ipa_cmd_mem_dma_zero(ipa, ipa->mem[IPA_MEM_MODEM_HEADER].offset,
				   ipa->mem[IPA_MEM_MODEM_HEADER].size);
	if (ret)
		return ret;

	ret = ipa_cmd_mem_dma_zero(ipa, ipa->mem[IPA_MEM_MODEM_PROC_CTX].offset,
				   ipa->mem[IPA_MEM_MODEM_PROC_CTX].size);

	return ret;
}

int ipa_mem_init(struct ipa *ipa, u32 count, const struct ipa_mem *mem)
{
	struct resource *res;
	int ret;

	if (count > IPA_MEM_COUNT)
		return -EINVAL;

	ret = dma_set_mask_and_coherent(&ipa->pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	res = platform_get_resource_byname(ipa->pdev, IORESOURCE_MEM,
					   "ipa-shared");
	if (!res)
		return -ENODEV;

	ipa->mem_virt = memremap(res->start, resource_size(res), MEMREMAP_WC);
	if (!ipa->mem_virt)
		return -ENOMEM;

	ipa->mem_addr = res->start;
	ipa->mem_size = resource_size(res);

	/* The ipa->mem[] array is indexed by enum ipa_mem_id values */
	ipa->mem = mem;

	return 0;
}

void ipa_mem_exit(struct ipa *ipa)
{
	memunmap(ipa->mem_virt);
}
