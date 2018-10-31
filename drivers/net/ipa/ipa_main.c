// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/bug.h>
#include <linux/io.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/qcom_q6v5_ipa_notify.h>
#include <linux/qcom_scm.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "ipa.h"
#include "ipa_clock.h"
#include "ipa_data.h"
#include "ipa_endpoint.h"
#include "ipa_cmd.h"
#include "ipa_reg.h"
#include "ipa_mem.h"
#include "ipa_netdev.h"
#include "ipa_smp2p.h"
#include "ipa_uc.h"
#include "ipa_interrupt.h"

/**
 * DOC: The IP Accelerator
 *
 * This driver supports the Qualcomm IP Accelerator (IPA), which is a
 * networking component found in many Qualcomm SoCs.  The IPA is connected
 * to the application processor (AP), but is also connected (and partially
 * controlled by) other "execution environments" (EEs), such as a modem.
 *
 * The IPA is the conduit between the AP and the modem that carries network
 * traffic.  This driver presents a network interface representing the
 * connection of the modem to external (e.g. LTE) networks.  The IPA can
 * provide protocol checksum calculation, offloading this work from the AP.
 * The IPA is able to provide additional functionality, including routing,
 * filtering, and NAT support, but that more advanced functionality is not
 * currently supported.
 *
 * Certain resources--including routing tables and filter tables--are still
 * defined in this driver, because they must be initialized even when the
 * advanced hardware features are not used.
 *
 * There are two distinct layers that implement the IPA hardware, and this
 * is reflected in the organization of the driver.  The generic software
 * interface (GSI) is an integral component of the IPA, providing a
 * well-defined communication layer between the AP subsystem and the IPA
 * core.  The GSI implements a set of "channels" used for communication
 * between the AP and the IPA.
 *
 * The IPA layer uses GSI channels to implement its "endpoints".  And while
 * a GSI channel carries data between the AP and the IPA, a pair of IPA
 * endpoints is used to carry traffic between two EEs.  Specifically, the main
 * modem network interface is implemented by two pairs of endpoints:  a TX
 * endpoint on the AP coupled with an RX endpoint on the modem; and another
 * RX endpoint on the AP receiving data from a TX endpoint on the modem.
 */

#define IPA_TABLE_ALIGN		128		/* Minimum table alignment */
#define IPA_TABLE_ENTRY_SIZE	sizeof(u64)	/* Holds a physical address */
#define IPA_FILTER_SIZE		8		/* Filter descriptor size */
#define IPA_ROUTE_SIZE		8		/* Route descriptor size */

/* The name of the main firmware file relative to /lib/firmware */
#define IPA_FWS_PATH		"ipa_fws.mdt"
#define IPA_PAS_ID		15

/**
 * ipa_filter_tuple_zero() - Zero an endpoints filter tuple
 * @endpoint_id:	Endpoint whose filter tuple should be zeroed
 *
 * Endpoint must be for AP (not modem) and support filtering. Updates the
 * filter masks values without changing routing ones.
 */
static void ipa_filter_tuple_zero(struct ipa_endpoint *endpoint)
{
	u32 endpoint_id = endpoint->endpoint_id;
	u32 offset;
	u32 val;

	offset = IPA_REG_ENDP_FILTER_ROUTER_HSH_CFG_N_OFFSET(endpoint_id);

	val = ioread32(endpoint->ipa->reg_virt + offset);

	/* Zero all filter-related fields, preserving the rest */
	u32_replace_bits(val, 0, IPA_REG_ENDP_FILTER_HASH_MSK_ALL);

	iowrite32(val, endpoint->ipa->reg_virt + offset);
}

static void ipa_filter_hash_tuple_config(struct ipa *ipa)
{
	u32 ep_mask = ipa->filter_support;

	while (ep_mask) {
		u32 endpoint_id = __ffs(ep_mask);
		struct ipa_endpoint *endpoint;

		ep_mask ^= BIT(endpoint_id);

		endpoint = &ipa->endpoint[endpoint_id];
		if (endpoint->ee_id != GSI_EE_MODEM)
			ipa_filter_tuple_zero(endpoint);
	}
}

/**
 * ipa_route_tuple_zero() - Zero a routing table entry tuple
 * @route_id:	Identifier for routing table entry to be zeroed
 *
 * Updates the routing table values without changing filtering ones.
 */
static void ipa_route_tuple_zero(struct ipa *ipa, u32 route_id)
{
	u32 offset = IPA_REG_ENDP_FILTER_ROUTER_HSH_CFG_N_OFFSET(route_id);
	u32 val;

	val = ioread32(ipa->reg_virt + offset);

	/* Zero all route-related fields, preserving the rest */
	u32_replace_bits(val, 0, IPA_REG_ENDP_ROUTER_HASH_MSK_ALL);

	iowrite32(val, ipa->reg_virt + offset);
}

static void ipa_route_hash_tuple_config(struct ipa *ipa)
{
	u32 route_mask;
	u32 modem_mask;

	BUILD_BUG_ON(!IPA_MEM_MODEM_RT_COUNT);
	BUILD_BUG_ON(IPA_MEM_RT_COUNT < IPA_MEM_MODEM_RT_COUNT);
	BUILD_BUG_ON(IPA_MEM_RT_COUNT >= BITS_PER_LONG);

	/* Compute a mask representing non-modem routing table entries */
	route_mask = GENMASK(IPA_MEM_RT_COUNT - 1, 0);
	modem_mask = GENMASK(IPA_MEM_MODEM_RT_INDEX_MAX,
			     IPA_MEM_MODEM_RT_INDEX_MIN);
	route_mask &= ~modem_mask;

	while (route_mask) {
		u32 route_id = __ffs(route_mask);

		route_mask ^= BIT(route_id);

		ipa_route_tuple_zero(ipa, route_id);
	}
}

/**
 * ipa_route_setup() - Initialize an empty routing table
 * @ipa:	IPA pointer
 *
 * Each entry in the routing table contains the DMA address of a route
 * descriptor.  A special zero descriptor is allocated that represents "no
 * route" and this function initializes all its entries to point at that
 * zero route.  The zero route is allocated with the table, immediately past
 * its end.
 *
 * @Return:	0 if successful or -ENOMEM
 */
static int ipa_route_setup(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	u64 zero_route_addr;
	dma_addr_t addr;
	u32 route_id;
	size_t size;
	u64 *virt;

	BUILD_BUG_ON(!IPA_ROUTE_SIZE);
	BUILD_BUG_ON(sizeof(*virt) != IPA_TABLE_ENTRY_SIZE);

	/* Allocate a routing table, with space at the end of the table to
	 * hold a zero route descriptor (representing no route).  Initialize
	 * all entries in the routing table to point to the zero descriptor.
	 *
	 * We don't support any routing yet, so just use the same table
	 * for both the non-hashed and (if present) hashed filter tables.
	 * A zero route descriptor has the same meaning for IPv4 and IPv6,
	 * and since we don't support routing so we can also use the same
	 * "no route" table for both IP versions.
	 */

	size = IPA_MEM_RT_COUNT * IPA_TABLE_ENTRY_SIZE;
	virt = dma_alloc_coherent(dev, size + IPA_ROUTE_SIZE, &addr,
				  GFP_KERNEL);
	if (!virt)
		return -ENOMEM;
	ipa->route_virt = virt;
	ipa->route_addr = addr;

	/* Zero route descriptor immediately follows the routing table */
	zero_route_addr = addr + size;

	/* Now point every entry in the table at the empty route descriptor */
	for (route_id = 0; route_id < IPA_MEM_RT_COUNT; route_id++)
		*virt++ = zero_route_addr;

	ipa_cmd_route_config_ipv4(ipa, size);
	ipa_cmd_route_config_ipv6(ipa, size);

	/* IPA version 4.2 has no hashed routing tables */
	if (ipa->version != IPA_VERSION_4_2)
		ipa_route_hash_tuple_config(ipa);

	/* Configure default route for exception packets */
	ipa_endpoint_default_route_setup(ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]);

	return 0;
}

/**
 * ipa_route_teardown() - Inverse of ipa_route_setup().
 * @ipa:	IPA pointer
 */
static void ipa_route_teardown(struct ipa *ipa)
{
	struct ipa_endpoint *endpoint = ipa->name_map[IPA_ENDPOINT_AP_LAN_RX];
	struct device *dev = &ipa->pdev->dev;
	size_t size;

	ipa_endpoint_default_route_teardown(endpoint);

	size = IPA_MEM_RT_COUNT * IPA_TABLE_ENTRY_SIZE;
	size += IPA_ROUTE_SIZE;

	dma_free_coherent(dev, size, ipa->route_virt, ipa->route_addr);
	ipa->route_virt = NULL;
	ipa->route_addr = 0;
}

/**
 * ipa_filter_setup() - Initialize an empty filter table
 * @ipa:	IPA pointer
 *
 * The filter table consists of a bitmask representing which endpoints support
 * filtering, followed by one table entry for each set bit in the mask.  Each
 * entry in the filter table contains the DMA address of a filter descriptor.
 * A special zero descriptor is allocated that represents "no filter" and this
 * function initializes all its entries to point at that zero filter.  The
 * zero filter is allocated with the table, immediately past its end.
 *
 * @Return:	0 if successful or a negative error code
 */
static int ipa_filter_setup(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	u64 zero_filter_addr;
	u32 filter_count;
	dma_addr_t addr;
	size_t size;
	u64 *virt;
	u32 i;

	BUILD_BUG_ON(!IPA_FILTER_SIZE);

	/* Allocate a filter table, with an extra slot for the bitmap.
	 * Allocate space at the end of the table to hold a zero filter
	 * descriptor (representing no filtering).  Initialize all entries
	 * in the filter table to point to the zero descriptor.
	 *
	 * We don't support any filtering yet, so just use the same table
	 * for both the non-hashed and (if present) hashed filter tables.
	 * A zero filter descriptor has the same meaning for IPv4 and IPv6,
	 * and since we don't support filtering so we can also use the same
	 * "no filter" table for both IP versions.
	 */
	filter_count = hweight32(ipa->filter_support);
	size = (filter_count + 1) * IPA_TABLE_ENTRY_SIZE;
	virt = dma_alloc_coherent(dev, size + IPA_FILTER_SIZE, &addr,
				  GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	ipa->filter_virt = virt;
	ipa->filter_addr = addr;

	/* Zero filter descriptor immediately follows the filter table */
	zero_filter_addr = addr + size;

	/* Save the filter table bitmap.  The "soft" bitmap value must be
	 * converted to the hardware representation by shifting it left one
	 * position.  (Bit 0 represents global filtering, which is possible
	 * but not used.)
	 */
	*virt++ = ipa->filter_support << 1;

	/* Now point every entry in the table at the empty filter descriptor */
	for (i = 0; i < filter_count; i++)
		*virt++ = zero_filter_addr;

	ipa_cmd_filter_config_ipv4(ipa, size);
	ipa_cmd_filter_config_ipv6(ipa, size);

	/* IPA version 4.2 has no hashed filter tables */
	if (ipa->version != IPA_VERSION_4_2)
		ipa_filter_hash_tuple_config(ipa);

	return 0;
}

/**
 * ipa_filter_teardown() - Inverse of ipa_filter_setup().
 * @ipa:	IPA pointer
 */
static void ipa_filter_teardown(struct ipa *ipa)
{
	u32 filter_count = hweight32(ipa->filter_support);
	struct device *dev = &ipa->pdev->dev;
	size_t size;

	size = (filter_count + 1) * IPA_TABLE_ENTRY_SIZE;
	size += IPA_FILTER_SIZE;

	dma_free_coherent(dev, size, ipa->filter_virt, ipa->filter_addr);
	ipa->filter_virt = NULL;
	ipa->filter_addr = 0;
	ipa->filter_support = 0;
}

/**
 * ipa_suspend_handler() - Handle the suspend interrupt
 * @ipa:	IPA pointer
 * @irq_id:	IPA interrupt type (unused)
 *
 * When in suspended state, the IPA can trigger a resume by sending a SUSPEND
 * IPA interrupt.
 */
static void ipa_suspend_handler(struct ipa *ipa, enum ipa_irq_id irq_id)
{
	/* Take a a single clock reference to prevent suspend.  All
	 * endpoints will be resumed as a result.  This reference will
	 * be dropped when we get a power management suspend request.
	 */
	if (!atomic_xchg(&ipa->suspend_ref, 1))
		ipa_clock_get(ipa->clock);

	/* Acknowledge/clear the suspend interrupt on all endpoints */
	ipa_interrupt_suspend_clear_all(ipa->interrupt);
}

/* Remoteproc callbacks for SSR events: prepare, start, stop, unprepare */
int ipa_ssr_prepare(struct rproc_subdev *subdev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(ipa_ssr_prepare);

int ipa_ssr_start(struct rproc_subdev *subdev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(ipa_ssr_start);

void ipa_ssr_stop(struct rproc_subdev *subdev, bool crashed)
{
}
EXPORT_SYMBOL_GPL(ipa_ssr_stop);

void ipa_ssr_unprepare(struct rproc_subdev *subdev)
{
}
EXPORT_SYMBOL_GPL(ipa_ssr_unprepare);

/**
 * ipa_setup() - Set up IPA hardware
 * @ipa:	IPA pointer
 *
 * Perform initialization that requires issuing immediate commands using the
 * command TX endpoint.  This cannot be run until early initialization
 * (including loading GSI firmware) is complete.
 */
int ipa_setup(struct ipa *ipa)
{
	struct ipa_endpoint *command_endpoint;
	int ret;

	dev_dbg(&ipa->pdev->dev, "%s() started\n", __func__);

	/* IPA v4.0 and above don't use the doorbell engine. */
	ret = gsi_setup(&ipa->gsi, ipa->version == IPA_VERSION_3_5_1);
	if (ret)
		return ret;

	ipa->interrupt = ipa_interrupt_setup(ipa);
	if (IS_ERR(ipa->interrupt)) {
		ret = PTR_ERR(ipa->interrupt);
		goto err_gsi_teardown;
	}
	ipa_interrupt_add(ipa->interrupt, IPA_IRQ_TX_SUSPEND,
			  ipa_suspend_handler);

	ipa_uc_setup(ipa);

	ipa_endpoint_setup(ipa);

	/* We need to use the AP command out endpoint to perform other
	 * initialization, so we set that up first.
	 */
	command_endpoint = ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX];
	ret = ipa_endpoint_enable_one(command_endpoint);
	if (ret)
		goto err_endpoint_teardown;

	ret = ipa_mem_setup(ipa);
	if (ret)
		goto err_command_disable;

	ret = ipa_route_setup(ipa);
	if (ret)
		goto err_smem_teardown;

	ret = ipa_filter_setup(ipa);
	if (ret)
		goto err_route_teardown;

	ret = ipa_endpoint_enable_one(ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]);
	if (ret)
		goto err_filter_teardown;

	ret = ipa_netdev_setup(ipa);
	if (ret)
		goto err_default_disable;

	ipa->setup_complete = 1;

	dev_info(&ipa->pdev->dev, "IPA driver setup completed successfully\n");

	return 0;

err_default_disable:
	ipa_endpoint_disable_one(ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]);
err_filter_teardown:
	ipa_filter_teardown(ipa);
err_route_teardown:
	ipa_route_teardown(ipa);
err_smem_teardown:
	ipa_mem_teardown(ipa);
err_command_disable:
	ipa_endpoint_disable_one(command_endpoint);
err_endpoint_teardown:
	ipa_endpoint_teardown(ipa);
	ipa_uc_teardown(ipa);
	ipa_interrupt_remove(ipa->interrupt, IPA_IRQ_TX_SUSPEND);
	ipa_interrupt_teardown(ipa->interrupt);
err_gsi_teardown:
	gsi_teardown(&ipa->gsi);

	return ret;
}

/**
 * ipa_teardown() - Inverse of ipa_setup()
 * @ipa:	IPA pointer
 */
static void ipa_teardown(struct ipa *ipa)
{
	ipa_netdev_teardown(ipa);
	ipa_endpoint_disable_one(ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]);
	ipa_filter_teardown(ipa);
	ipa_route_teardown(ipa);
	ipa_mem_teardown(ipa);
	ipa_endpoint_disable_one(ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX]);
	ipa_endpoint_teardown(ipa);
	ipa_uc_teardown(ipa);
	ipa_interrupt_remove(ipa->interrupt, IPA_IRQ_TX_SUSPEND);
	ipa_interrupt_teardown(ipa->interrupt);
	gsi_teardown(&ipa->gsi);
}

/* Configure QMB/Master port selection */
static void ipa_hardware_config_comp(struct ipa *ipa)
{
	u32 val;

	/* Nothing to configure for IPA v3.5.1 */
	if (ipa->version == IPA_VERSION_3_5_1)
		return;

	val = ioread32(ipa->reg_virt + IPA_REG_COMP_CFG_OFFSET);

	if (ipa->version == IPA_VERSION_4_0) {
		val &= ~IPA_QMB_SELECT_CONS_EN_FMASK;
		val &= ~IPA_QMB_SELECT_PROD_EN_FMASK;
		val &= ~IPA_QMB_SELECT_GLOBAL_EN_FMASK;
	} else  {
		val |= GSI_MULTI_AXI_MASTERS_DIS_FMASK;
	}

	val |= GSI_MULTI_INORDER_RD_DIS_FMASK;
	val |= GSI_MULTI_INORDER_WR_DIS_FMASK;

	iowrite32(val, ipa->reg_virt + IPA_REG_COMP_CFG_OFFSET);
}

/* Configure DDR and PCIe max read/write QSB values */
static void ipa_hardware_config_qsb(struct ipa *ipa)
{
	u32 val;

	/* QMB_0 represents DDR; QMB_1 represents PCIe (not present in 4.2) */
	val = u32_encode_bits(8, GEN_QMB_0_MAX_WRITES_FMASK);
	if (ipa->version == IPA_VERSION_4_2)
		val |= u32_encode_bits(0, GEN_QMB_1_MAX_WRITES_FMASK);
	else
		val |= u32_encode_bits(4, GEN_QMB_1_MAX_WRITES_FMASK);
	iowrite32(val, ipa->reg_virt + IPA_REG_QSB_MAX_WRITES_OFFSET);

	if (ipa->version == IPA_VERSION_3_5_1) {
		val = u32_encode_bits(8, GEN_QMB_0_MAX_READS_FMASK);
		val |= u32_encode_bits(12, GEN_QMB_1_MAX_READS_FMASK);
	} else {
		val = u32_encode_bits(12, GEN_QMB_0_MAX_READS_FMASK);
		if (ipa->version == IPA_VERSION_4_2)
			val |= u32_encode_bits(0, GEN_QMB_1_MAX_READS_FMASK);
		else
			val |= u32_encode_bits(12, GEN_QMB_1_MAX_READS_FMASK);
		/* GEN_QMB_0_MAX_READS_BEATS is 0 */
		/* GEN_QMB_1_MAX_READS_BEATS is 0 */
	}
	iowrite32(val, ipa->reg_virt + IPA_REG_QSB_MAX_READS_OFFSET);
}

/**
 * ipa_hardware_config() - Primitive hardware initialization
 * @ipa:	IPA pointer
 */
static void ipa_hardware_config(struct ipa *ipa)
{
	u32 granularity;
	u32 val;

	/* SDM845 has IPA version 3.5.1 */
	val = ipa_reg_bcr_val(ipa->version);
	iowrite32(val, ipa->reg_virt + IPA_REG_BCR_OFFSET);

	if (ipa->version != IPA_VERSION_3_5_1) {
		/* Enable open global clocks (hardware workaround) */
		val = GLOBAL_FMASK;
		val |= GLOBAL_2X_CLK_FMASK;
		iowrite32(val, ipa->reg_virt + IPA_REG_CLKON_CFG_OFFSET);

		/* Disable PA mask to allow HOLB drop (hardware workaround) */
		val = ioread32(ipa->reg_virt + IPA_REG_TX_CFG_OFFSET);
		val &= ~PA_MASK_EN;
		iowrite32(val, ipa->reg_virt + IPA_REG_TX_CFG_OFFSET);
	}

	ipa_hardware_config_comp(ipa);

	/* Configure system bus limits */
	ipa_hardware_config_qsb(ipa);

	/* Configure aggregation granularity */
	val = ioread32(ipa->reg_virt + IPA_REG_COUNTER_CFG_OFFSET);
	granularity = ipa_aggr_granularity_val(IPA_AGGR_GRANULARITY);
	val = u32_encode_bits(granularity, AGGR_GRANULARITY);
	iowrite32(val, ipa->reg_virt + IPA_REG_COUNTER_CFG_OFFSET);

	/* Disable hashed routing and filtering for IPA v4.2 */
	if (ipa->version == IPA_VERSION_4_2)
		iowrite32(0, ipa->reg_virt + IPA_REG_FILT_ROUT_HASH_EN_OFFSET);
}

/**
 * ipa_hardware_deconfig() - Inverse of ipa_hardware_config()
 * @ipa:	IPA pointer
 *
 * This restores the power-on reset values (even if they aren't different)
 */
static void ipa_hardware_deconfig(struct ipa *ipa)
{
	/* Values we program above are the same as the power-on reset values */
}

/* # IPA resources used based on version (see IPA_RESOURCE_GROUP_COUNT) */
int ipa_resource_group_count(struct ipa *ipa)
{
	switch (ipa->version) {
	case IPA_VERSION_3_5_1:
		return 3;

	case IPA_VERSION_4_0:
	case IPA_VERSION_4_1:
		return 4;

	case IPA_VERSION_4_2:
		return 1;

	default:
		return 0;
	}
}

static bool
ipa_resource_limits_valid(struct ipa *ipa, const struct ipa_resource_data *data)
{
	u32 group_count = ipa_resource_group_count(ipa);
	u32 i;
	u32 j;

	if (!group_count)
		return false;

	/* Return an error if a non-zero resource group limit is specified
	 * for a resource not supported by hardware.
	 */
	for (i = 0; i < data->resource_src_count; i++) {
		const struct ipa_resource_src *resource;

		resource = &data->resource_src[i];
		for (j = group_count; j < IPA_RESOURCE_GROUP_COUNT; j++)
			if (resource->limits[j].min || resource->limits[j].max)
				return false;
	}

	for (i = 0; i < data->resource_dst_count; i++) {
		const struct ipa_resource_dst *resource;

		resource = &data->resource_dst[i];
		for (j = group_count; j < IPA_RESOURCE_GROUP_COUNT; j++)
			if (resource->limits[j].min || resource->limits[j].max)
				return false;
	}

	return true;
}

static void
ipa_resource_config_common(struct ipa *ipa, u32 offset,
			   const struct ipa_resource_limits *xlimits,
			   const struct ipa_resource_limits *ylimits)
{
	u32 val;

	val = u32_encode_bits(xlimits->min, X_MIN_LIM_FMASK);
	val |= u32_encode_bits(xlimits->max, X_MAX_LIM_FMASK);
	val |= u32_encode_bits(ylimits->min, Y_MIN_LIM_FMASK);
	val |= u32_encode_bits(ylimits->max, Y_MAX_LIM_FMASK);

	iowrite32(val, ipa->reg_virt + offset);
}

static void ipa_resource_config_src_01(struct ipa *ipa,
				       const struct ipa_resource_src *resource)
{
	u32 offset = IPA_REG_SRC_RSRC_GRP_01_RSRC_TYPE_N_OFFSET(resource->type);

	ipa_resource_config_common(ipa, offset,
				   &resource->limits[0], &resource->limits[1]);
}

static void ipa_resource_config_src_23(struct ipa *ipa,
				       const struct ipa_resource_src *resource)
{
	u32 offset = IPA_REG_SRC_RSRC_GRP_23_RSRC_TYPE_N_OFFSET(resource->type);

	ipa_resource_config_common(ipa, offset,
				   &resource->limits[2], &resource->limits[3]);
}

static void ipa_resource_config_dst_01(struct ipa *ipa,
				       const struct ipa_resource_dst *resource)
{
	u32 offset = IPA_REG_DST_RSRC_GRP_01_RSRC_TYPE_N_OFFSET(resource->type);

	ipa_resource_config_common(ipa, offset,
				   &resource->limits[0], &resource->limits[1]);
}

static void ipa_resource_config_dst_23(struct ipa *ipa,
				       const struct ipa_resource_dst *resource)
{
	u32 offset = IPA_REG_DST_RSRC_GRP_23_RSRC_TYPE_N_OFFSET(resource->type);

	ipa_resource_config_common(ipa, offset,
				   &resource->limits[2], &resource->limits[3]);
}

static int
ipa_resource_config(struct ipa *ipa, const struct ipa_resource_data *data)
{
	u32 i;

	if (!ipa_resource_limits_valid(ipa, data))
		return -EINVAL;

	for (i = 0; i < data->resource_src_count; i++) {
		ipa_resource_config_src_01(ipa, &data->resource_src[i]);
		ipa_resource_config_src_23(ipa, &data->resource_src[i]);
	}

	for (i = 0; i < data->resource_dst_count; i++) {
		ipa_resource_config_dst_01(ipa, &data->resource_dst[i]);
		ipa_resource_config_dst_23(ipa, &data->resource_dst[i]);
	}

	return 0;
}

static void ipa_resource_deconfig(struct ipa *ipa)
{
	/* Nothing to do */
}

static void ipa_idle_indication_cfg(struct ipa *ipa,
				    u32 enter_idle_debounce_thresh,
				    bool const_non_idle_enable)
{
	u32 offset;
	u32 val;

	val = u32_encode_bits(enter_idle_debounce_thresh,
			      ENTER_IDLE_DEBOUNCE_THRESH_FMASK);
	if (const_non_idle_enable)
		val |= CONST_NON_IDLE_ENABLE_FMASK;

	offset = ipa_reg_idle_indication_cfg_offset(ipa->version);
	iowrite32(val, ipa->reg_virt + offset);
}

/**
 * ipa_dcd_config() - Enable dynamic clock division on IPA
 *
 * Configures when the IPA signals it is idle to the global clock
 * controller, which can respond by scalling down the clock to
 * save power.
 */
static void ipa_dcd_config(struct ipa *ipa)
{
	/* Recommended values for IPA 3.5 according to IPA HPG */
	ipa_idle_indication_cfg(ipa, 256, false);
}

static void ipa_dcd_deconfig(struct ipa *ipa)
{
	/* Power-on reset values */
	ipa_idle_indication_cfg(ipa, 0, true);
}

/**
 * ipa_config() - Configure IPA hardware
 * @ipa:	IPA pointer
 *
 * Perform initialization requiring IPA clock to be enabled.
 */
static int ipa_config(struct ipa *ipa, const struct ipa_data *data)
{
	int ret;

	/* Get a clock reference to allow initialization.  This reference
	 * is held after initialization completes, and won't get dropped
	 * unless/until a system suspend request arrives.
	 */
	atomic_set(&ipa->suspend_ref, 1);
	ipa_clock_get(ipa->clock);

	ipa_hardware_config(ipa);

	ret = ipa_endpoint_config(ipa);
	if (ret)
		goto err_hardware_deconfig;

	ret = ipa_mem_config(ipa);
	if (ret)
		goto err_endpoint_deconfig;

	/* Assign resource limitation to each group */
	ret = ipa_resource_config(ipa, data->resource_data);
	if (ret)
		goto err_mem_deconfig;

	/* Note enabling dynamic clock division must not be
	 * attempted for IPA hardware versions prior to 3.5.
	 */
	ipa_dcd_config(ipa);

	return 0;

err_mem_deconfig:
	ipa_mem_deconfig(ipa);
err_endpoint_deconfig:
	ipa_endpoint_deconfig(ipa);
err_hardware_deconfig:
	ipa_hardware_deconfig(ipa);
	ipa_clock_put(ipa->clock);

	return ret;
}

/**
 * ipa_deconfig() - Inverse of ipa_config()
 * @ipa:	IPA pointer
 */
static void ipa_deconfig(struct ipa *ipa)
{
	ipa_dcd_deconfig(ipa);
	ipa_resource_deconfig(ipa);
	ipa_mem_deconfig(ipa);
	ipa_endpoint_deconfig(ipa);
	ipa_hardware_deconfig(ipa);

	ipa_clock_put(ipa->clock);
}

static int ipa_firmware_load(struct device *dev)
{
	const struct firmware *fw;
	struct device_node *node;
	struct resource res;
	phys_addr_t phys;
	ssize_t size;
	void *virt;
	int ret;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(dev, "memory-region not specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		return ret;

	ret = request_firmware(&fw, IPA_FWS_PATH, dev);
	if (ret)
		return ret;

	phys = res.start;
	size = (size_t)resource_size(&res);
	virt = memremap(phys, size, MEMREMAP_WC);
	if (!virt) {
		ret = -ENOMEM;
		goto out_release_firmware;
	}

	ret = qcom_mdt_load(dev, fw, IPA_FWS_PATH, IPA_PAS_ID,
			    virt, phys, size, NULL);
	if (!ret)
		ret = qcom_scm_pas_auth_and_reset(IPA_PAS_ID);

	memunmap(virt);
out_release_firmware:
	release_firmware(fw);

	return ret;
}

static const struct of_device_id ipa_match[] = {
	{
		.compatible	= "qcom,sdm845-ipa",
		.data		= &ipa_data_sdm845,
	},
	{
		.compatible	= "qcom,sc7180-ipa",
		.data		= &ipa_data_sc7180,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ipa_match);

static phandle of_property_read_phandle(const struct device_node *np,
					const char *name)
{
        struct property *prop;
        int len = 0;

        prop = of_find_property(np, name, &len);
        if (!prop || len != sizeof(__be32))
                return 0;

        return be32_to_cpup(prop->value);
}

static void ipa_modem_notify(void *data, enum qcom_rproc_event event)
{
	struct ipa *ipa = data;

	switch (event) {
	case MODEM_STARTING:
	case MODEM_RUNNING:
	case MODEM_STOPPING:
	case MODEM_CRASHED:
	case MODEM_OFFLINE:
	case MODEM_REMOVING:
		dev_info(&ipa->pdev->dev, "received modem event %u\n", event);
		break;

	default:
		dev_err(&ipa->pdev->dev, "unrecognized event %u\n", event);
		break;
	}
}

/**
 * ipa_probe() - IPA platform driver probe function
 * @pdev:	Platform device pointer
 *
 * @Return:	0 if successful, or a negative error code (possibly
 *		EPROBE_DEFER)
 *
 * This is the main entry point for the IPA driver.  When successful, it
 * initializes the IPA hardware for use.
 *
 * Initialization proceeds in several stages.  The "init" stage involves
 * activities that can be initialized without access to the IPA hardware.
 * The "setup" stage requires the IPA clock to be active so IPA registers
 * can beaccessed, but does not require access to the GSI layer.  The
 * "setup" stage requires access to GSI, and includes initialization that's
 * performed by issuing IPA immediate commands.
 */
static int ipa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ipa_data *data;
	struct rproc *rproc;
	bool prefetch_flag;
	phandle phandle;
	struct ipa *ipa;
	bool modem_init;
	int ret;

	/* We assume we're working on 64-bit hardware */
	BUILD_BUG_ON(!IS_ENABLED(CONFIG_64BIT));
	BUILD_BUG_ON(ARCH_DMA_MINALIGN % IPA_TABLE_ALIGN);

	/* If we need Trust Zone, make sure it's ready */
	modem_init = of_property_read_bool(dev->of_node, "modem-init");
	if (!modem_init)
		if (!qcom_scm_is_available())
			return -EPROBE_DEFER;

	/* If there's no modem remoteproc defined, we can't do SSR */
	phandle = of_property_read_phandle(dev->of_node, "modem-remoteproc");
	if (!phandle) {
		dev_err(dev, "missing \"modem-remoteproc\" property\n");
		return -EINVAL;
	}

	/* Don't proceed until the modem remoteproc is ready */
	rproc = rproc_get_by_phandle(phandle);
	if (!rproc)
		return -EPROBE_DEFER;

	/* Allocate and initialize the IPA structure */
	ipa = kzalloc(sizeof(*ipa), GFP_KERNEL);
	if (!ipa) {
		ret = -ENOMEM;
		goto err_put_rproc;
	}

	ipa->pdev = pdev;
	dev_set_drvdata(dev, ipa);
	ipa->modem_rproc = rproc;

	ret = qcom_register_ipa_notify(rproc, ipa_modem_notify, ipa);
	if (ret)
		goto err_free_ipa;

	data = of_device_get_match_data(dev);
	if (!ipa_reg_version_supported(data->version)) {
		dev_err(dev, "unsupported IPA version %u\n", data->version);
		ret = -ENOTSUPP;
		goto err_deregister_ipa_notify;
	}
	ipa->version = data->version;

	/* Initialize the clock and interconnects early.  They might
	 * not be ready when we're probed, so might return -EPROBE_DEFER.
	 */
	atomic_set(&ipa->suspend_ref, 0);

	ipa->clock = ipa_clock_init(ipa);
	if (IS_ERR(ipa->clock)) {
		ret = PTR_ERR(ipa->clock);
		goto err_free_ipa;
	}

	ret = ipa_reg_init(ipa);
	if (ret)
		goto err_clock_exit;

	ret = ipa_mem_init(ipa, data->mem_count, data->mem_data);
	if (ret)
		goto err_reg_exit;

	/* GSI v2.0+ (IPA v4.0+) adds a new QOS flag */
	prefetch_flag = ipa->version != IPA_VERSION_3_5_1;
	ret = gsi_init(&ipa->gsi, pdev, prefetch_flag, data->endpoint_count,
		       data->endpoint_data, ipa->version == IPA_VERSION_4_2);
	if (ret)
		goto err_mem_exit;

	ipa->smp2p = ipa_smp2p_init(ipa, modem_init);
	if (IS_ERR(ipa->smp2p)) {
		ret = PTR_ERR(ipa->smp2p);
		goto err_gsi_exit;
	}

	ret = ipa_endpoint_init(ipa, data->endpoint_count, data->endpoint_data);
	if (ret)
		goto err_smp2p_exit;

	/* Create a wakeup source. */
	ipa->wakeup = wakeup_source_register(dev, "ipa");
	if (!ipa->wakeup)
		goto err_endpoint_exit;

	/* Proceed to real initialization */
	ret = ipa_config(ipa, data);
	if (ret)
		goto err_wakeup_unregister;

	dev_info(dev, "IPA driver initialized");

	/* If the modem is verifying and loading firmware, we're
	 * done.  We will receive an SMP2P interrupt when it is OK
	 * to proceed with the setup phase (involving issuing
	 * immediate commands after GSI is initialized).
	 */
	if (modem_init)
		return 0;

	/* Otherwise we need to load the firmware and have Trust
	 * Zone validate and install it.  If that succeeds we can
	 * proceed with setup.
	 */
	ret = ipa_firmware_load(dev);
	if (ret)
		goto err_deconfig;

	ret = ipa_setup(ipa);
	if (ret)
		goto err_deconfig;

	return 0;

err_deconfig:
	ipa_deconfig(ipa);
err_wakeup_unregister:
	wakeup_source_unregister(ipa->wakeup);
err_endpoint_exit:
	ipa_endpoint_exit(ipa);
err_smp2p_exit:
	ipa_smp2p_exit(ipa->smp2p);
err_gsi_exit:
	gsi_exit(&ipa->gsi);
err_mem_exit:
	ipa_mem_exit(ipa);
err_reg_exit:
	ipa_reg_exit(ipa);
err_clock_exit:
	ipa_clock_exit(ipa->clock);
err_deregister_ipa_notify:
	qcom_deregister_ipa_notify(rproc);
err_free_ipa:
	kfree(ipa);
err_put_rproc:
	rproc_put(rproc);

	return ret;
}

static int ipa_remove(struct platform_device *pdev)
{
	struct ipa *ipa = dev_get_drvdata(&pdev->dev);

	ipa_smp2p_disable(ipa->smp2p);
	if (ipa->setup_complete)
		ipa_teardown(ipa);

	ipa_deconfig(ipa);
	wakeup_source_unregister(ipa->wakeup);
	ipa_endpoint_exit(ipa);
	ipa_smp2p_exit(ipa->smp2p);
	ipa_mem_exit(ipa);
	ipa_reg_exit(ipa);
	ipa_clock_exit(ipa->clock);
	qcom_deregister_ipa_notify(ipa->modem_rproc);
	rproc_put(ipa->modem_rproc);
	kfree(ipa);

	return 0;
}

/**
 * ipa_suspend() - Power management system suspend callback
 * @dev:	IPA device structure
 *
 * @Return:	Zero
 *
 * Called by the PM framework when a system suspend operation is invoked.
 */
int ipa_suspend(struct device *dev)
{
	struct ipa *ipa = dev_get_drvdata(dev);

	ipa_clock_put(ipa->clock);
	atomic_set(&ipa->suspend_ref, 0);

	return 0;
}

/**
 * ipa_resume() - Power management system resume callback
 * @dev:	IPA device structure
 *
 * @Return:	Always returns 0
 *
 * Called by the PM framework when a system resume operation is invoked.
 */
int ipa_resume(struct device *dev)
{
	struct ipa *ipa = dev_get_drvdata(dev);

	/* This clock reference will keep the IPA out of suspend
	 * until we get a power management suspend request.
	 */
	atomic_set(&ipa->suspend_ref, 1);
	ipa_clock_get(ipa->clock);

	return 0;
}

static const struct dev_pm_ops ipa_pm_ops = {
	.suspend_noirq	= ipa_suspend,
	.resume_noirq	= ipa_resume,
};

static struct platform_driver ipa_driver = {
	.probe	= ipa_probe,
	.remove	= ipa_remove,
	.driver	= {
		.name		= "ipa",
		.owner		= THIS_MODULE,
		.pm		= &ipa_pm_ops,
		.of_match_table	= ipa_match,
	},
};

module_platform_driver(ipa_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm IP Accelerator device driver");
