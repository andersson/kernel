/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */
#ifndef _IPA_H_
#define _IPA_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/pm_wakeup.h>

#include "ipa_version.h"
#include "gsi.h"
#include "ipa_mem.h"
#include "ipa_qmi.h"
#include "ipa_endpoint.h"
#include "ipa_interrupt.h"

struct clk;
struct icc_path;
struct net_device;
struct platform_device;

struct ipa_clock;
struct ipa_smp2p;
struct ipa_interrupt;

/**
 * struct ipa - IPA information
 * @gsi:		Embedded GSI structure
 * @version:		IPA hardware version
 * @pdev:		Platform device
 * @smp2p:		SMP2P information
 * @clock:		IPA clocking information
 * @suspend_ref:	Whether clock reference preventing suspend taken
 * @route_virt:		Virtual address of routing table
 * @route_addr:		DMA address for routing table
 * @filter_virt:	Virtual address of filter table
 * @filter_addr:	DMA address for filter table
 * @interrupt:		IPA Interrupt information
 * @uc_loaded:		Non-zero when microcontroller has reported it's ready
 * @reg_addr:		DMA address used for IPA register access
 * @reg_virt:		Virtual address used for IPA register access
 * @mem_addr:		DMA address of IPA-resident shared memory space
 * @mem_virt:		Virtual address of IPA-resident memory space
 * @mem_offset:		Offset from @mem_virt used for IPA shared memory access
 * @mem_size:		Total size of IPA shared memory at @mem_virt
 * @wakeup:		Wakeup source information
 * @available:		Bit mask indicating endpoints hardware supports
 * @filter_support:	Bit mask indicating endpoints that support filtering
 * @initialized:	Bit mask indicating endpoints initialized
 * @set_up:		Bit mask indicating endpoints set up
 * @enabled:		Bit mask indicating endpoints enabled
 * @endpoint:		Array of endpoint information
 * @name_map:		Mapping of IPA endpoint name to IPA endpoint
 * @channel_map:	Mapping of GSI channel to IPA endpoint
 * @modem_netdev:	Network device structure used for modem
 * @setup_complete:	Flag indicating whether setup stage has completed
 * @qmi:		QMI information
 */
struct ipa {
	struct gsi gsi;
	enum ipa_version version;
	struct platform_device *pdev;
	struct rproc *modem_rproc;
	struct ipa_smp2p *smp2p;
	struct ipa_clock *clock;
	atomic_t suspend_ref;

	u64 *route_virt;
	dma_addr_t route_addr;
	u64 *filter_virt;
	dma_addr_t filter_addr;

	struct ipa_interrupt *interrupt;
	u32 uc_loaded;

	dma_addr_t reg_addr;
	void __iomem *reg_virt;

	dma_addr_t mem_addr;
	void *mem_virt;
	u32 mem_offset;
	u32 mem_size;
	const struct ipa_mem *mem;

	struct wakeup_source *wakeup;

	/* Bit masks indicating endpoint state */
	u32 available;		/* supported by hardware */
	u32 filter_support;
	u32 initialized;
	u32 set_up;
	u32 enabled;

	struct ipa_endpoint endpoint[IPA_ENDPOINT_MAX];
	struct ipa_endpoint *name_map[IPA_ENDPOINT_COUNT];
	struct ipa_endpoint *channel_map[GSI_CHANNEL_MAX];

	struct net_device *modem_netdev;
	u32 setup_complete;

	struct ipa_qmi qmi;
};

/**
 * ipa_setup() - Perform IPA setup
 * @ipa:		IPA pointer
 *
 * IPA initialization is broken into stages:  init; config; setup; and
 * sometimes enable.  (These have inverses exit, deconfig, teardown, and
 * disable.)  Activities performed at the init stage can be done without
 * requiring any access to hardware.  For IPA, activities performed at the
 * config stage require the IPA clock to be running, because they involve
 * access to IPA registers.  The setup stage is performed only after the
 * GSI hardware is ready (more on this below).  And finally IPA endpoints
 * can be enabled once they're successfully set up.
 *
 * This function, @ipa_setup(), starts the setup stage.
 *
 * In order for the GSI hardware to be functional it needs firmware to be
 * loaded (in addition to some other low-level initialization).  This early
 * GSI initialization can be done either by Trust Zone or by the modem.  If
 * it's done by Trust Zone, the AP loads the GSI firmware and supplies it to
 * Trust Zone to verify and install.  The AP knows when this completes, and
 * whether it was successful.  In this case the AP proceeds to setup once it
 * knows GSI is ready.
 *
 * If the modem performs early GSI initialization, the AP needs to know when
 * this has occurred.  An SMP2P interrupt is used for this purpose, and
 * receipt of that interrupt triggers the call to ipa_setup().
 */
int ipa_setup(struct ipa *ipa);

#endif /* _IPA_H_ */
