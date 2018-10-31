// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interconnect.h>

#include "ipa.h"
#include "ipa_clock.h"
#include "ipa_netdev.h"

/**
 * DOC: IPA Clocking
 *
 * The "IPA Clock" manages both the IPA core clock and the interconnects
 * (buses) the IPA depends on as a single logical entity.  A reference count
 * is incremented by "get" operations and decremented by "put" operations.
 * Transitions of that count from 0 to 1 result in the clock and interconnects
 * being enabled, and transitions of the count from 1 to 0 cause them to be
 * disabled.  We currently operate the core clock at a fixed clock rate, and
 * all buses at a fixed average and peak bandwidth.  As more advanced IPA
 * features are enabled, we can will better use of clock and bus scaling.
 *
 * An IPA clock reference must be held for any access to IPA hardware.
 */

#define	IPA_CORE_CLOCK_RATE		(75UL * 1000 * 1000)	/* Hz */

/* Interconnect path bandwidths (each times 1000 bytes per second) */
#define IPA_MEMORY_AVG			(80 * 1000)	/* 80 MBps */
#define IPA_MEMORY_PEAK			(600 * 1000)

#define IPA_IMEM_AVG			(80 * 1000)
#define IPA_IMEM_PEAK			(350 * 1000)

#define IPA_CONFIG_AVG			(40 * 1000)
#define IPA_CONFIG_PEAK			(40 * 1000)

/**
 * struct ipa_clock - IPA clocking information
 * @core:		IPA core clock
 * @memory_path:	Memory interconnect
 * @imem_path:		Internal memory interconnect
 * @config_path:	Configuration space interconnect
 * @mutex;		Protects clock enable/disable
 * @count:		Clocking reference count
 */
struct ipa_clock {
	struct ipa *ipa;
	atomic_t count;
	struct mutex mutex; /* protects clock enable/disable */
	struct clk *core;
	struct icc_path *memory_path;
	struct icc_path *imem_path;
	struct icc_path *config_path;
};

/* Initialize interconnects required for IPA operation */
static int ipa_interconnect_init(struct ipa_clock *clock, struct device *dev)
{
	struct icc_path *path;

	path = of_icc_get(dev, "memory");
	if (IS_ERR(path))
		goto err_return;
	clock->memory_path = path;

	path = of_icc_get(dev, "imem");
	if (IS_ERR(path))
		goto err_memory_path_put;
	clock->imem_path = path;

	path = of_icc_get(dev, "config");
	if (IS_ERR(path))
		goto err_imem_path_put;
	clock->config_path = path;

	return 0;

err_imem_path_put:
	icc_put(clock->imem_path);
err_memory_path_put:
	icc_put(clock->memory_path);
err_return:

	return PTR_ERR(path);
}

/* Inverse of ipa_interconnect_init() */
static void ipa_interconnect_exit(struct ipa_clock *clock)
{
	icc_put(clock->config_path);
	icc_put(clock->imem_path);
	icc_put(clock->memory_path);
}

/* Currently we only use one bandwidth level, so just "enable" interconnects */
static int ipa_interconnect_enable(struct ipa_clock *clock)
{
	int ret;

	ret = icc_set_bw(clock->memory_path, IPA_MEMORY_AVG, IPA_MEMORY_PEAK);
	if (ret)
		return ret;

	ret = icc_set_bw(clock->imem_path, IPA_IMEM_AVG, IPA_IMEM_PEAK);
	if (ret)
		goto err_disable_memory_path;

	ret = icc_set_bw(clock->config_path, IPA_CONFIG_AVG, IPA_CONFIG_PEAK);
	if (ret)
		goto err_disable_imem_path;

	return 0;

err_disable_imem_path:
	(void)icc_set_bw(clock->imem_path, 0, 0);
err_disable_memory_path:
	(void)icc_set_bw(clock->memory_path, 0, 0);

	return ret;
}

/* To disable an interconnect, we just its bandwidth to 0 */
static int ipa_interconnect_disable(struct ipa_clock *clock)
{
	int ret;

	ret = icc_set_bw(clock->memory_path, 0, 0);
	if (ret)
		return ret;

	ret = icc_set_bw(clock->imem_path, 0, 0);
	if (ret)
		goto err_reenable_memory_path;

	ret = icc_set_bw(clock->config_path, 0, 0);
	if (ret)
		goto err_reenable_imem_path;

	return 0;

err_reenable_imem_path:
	(void)icc_set_bw(clock->imem_path, IPA_IMEM_AVG, IPA_IMEM_PEAK);
err_reenable_memory_path:
	(void)icc_set_bw(clock->memory_path, IPA_MEMORY_AVG, IPA_MEMORY_PEAK);

	return ret;
}

/* Turn on IPA clocks, including interconnects */
static int ipa_clock_enable(struct ipa_clock *clock)
{
	int ret;

	ret = ipa_interconnect_enable(clock);
	if (ret)
		return ret;

	ret = clk_prepare_enable(clock->core);
	if (ret)
		ipa_interconnect_disable(clock);

	return ret;
}

/* Inverse of ipa_clock_enable() */
static void ipa_clock_disable(struct ipa_clock *clock)
{
	clk_disable_unprepare(clock->core);
	(void)ipa_interconnect_disable(clock);
}

/* Get an IPA clock reference, but only if the reference count is
 * already non-zero.  Returns true if the additional reference was
 * added successfully, or false otherwise.
 */
bool ipa_clock_get_additional(struct ipa_clock *clock)
{
	return !!atomic_inc_not_zero(&clock->count);
}

/* Get an IPA clock reference.  If the reference count is non-zero, it is
 * incremented and return is immediate.  Otherwise it is checked again
 * under protection of the mutex, and enable clocks and resume RX endpoints
 * before returning.  For the first reference, the count is intentionally
 * not incremented until after these activities are complete.
 */
void ipa_clock_get(struct ipa_clock *clock)
{
	struct ipa *ipa = clock->ipa;
	int ret;

	/* If the clock is running, just bump the reference count */
	if (ipa_clock_get_additional(clock))
		return;

	/* Otherwise get the mutex and check again */
	mutex_lock(&clock->mutex);

	/* A reference might have been added before we got the mutex. */
	if (ipa_clock_get_additional(clock))
		goto out_mutex_unlock;

	ret = ipa_clock_enable(clock);
	if (ret) {
		dev_err(&ipa->pdev->dev, "error %d enabling IPA clock\n", ret);
		goto out_mutex_unlock;
	}

	ipa_endpoint_resume(ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX]);
	ipa_endpoint_resume(ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]);

	if (ipa->modem_netdev)
		ipa_netdev_resume(ipa->modem_netdev);

	atomic_inc(&clock->count);

out_mutex_unlock:
	mutex_unlock(&clock->mutex);
}

/* Attempt to remove an IPA clock reference.  If this represents
 * the last reference, suspend endpoints and disable the clock
 * (and interconnects) under protection of a mutex.
 */
void ipa_clock_put(struct ipa_clock *clock)
{
	struct ipa *ipa = clock->ipa;

	/* If this is not the last reference there's nothing more to do */
	if (!atomic_dec_and_mutex_lock(&clock->count, &clock->mutex))
		return;

	ipa = clock->ipa;
	if (ipa->modem_netdev)
		ipa_netdev_suspend(ipa->modem_netdev);

	ipa_endpoint_suspend(ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]);
	ipa_endpoint_suspend(ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX]);

	ipa_clock_disable(clock);

	mutex_unlock(&clock->mutex);
}

/* Initialize IPA clocking */
struct ipa_clock *ipa_clock_init(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	struct ipa_clock *clock;
	int ret;

	clock = kzalloc(sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return ERR_PTR(-ENOMEM);

	clock->ipa = ipa;
	clock->core = clk_get(dev, "core");
	if (IS_ERR(clock->core)) {
		ret = PTR_ERR(clock->core);
		goto err_free_clock;
	}

	ret = clk_set_rate(clock->core, IPA_CORE_CLOCK_RATE);
	if (ret)
		goto err_clk_put;

	ret = ipa_interconnect_init(clock, dev);
	if (ret)
		goto err_clk_put;

	mutex_init(&clock->mutex);
	atomic_set(&clock->count, 0);

	return clock;

err_clk_put:
	clk_put(clock->core);
err_free_clock:
	kfree(clock);

	return ERR_PTR(ret);
}

/* Inverse of ipa_clock_init() */
void ipa_clock_exit(struct ipa_clock *clock)
{
	mutex_destroy(&clock->mutex);
	ipa_interconnect_exit(clock);
	clk_put(clock->core);
	kfree(clock);
}
