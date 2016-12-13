/*
 * Qualcomm Venus Peripheral Image Loader
 *
 * Copyright (C) 2016 Linaro Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/remoteproc.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/of_platform.h>

#include "qcom_mdt_loader.h"
#include "remoteproc_internal.h"

#define VENUS_FIRMWARE_NAME		"venus.mdt"
#define VENUS_PAS_ID			9
#define VENUS_FW_MEM_SIZE		SZ_8M

struct qcom_venus {
	struct device *dev;
	struct rproc *rproc;
	phys_addr_t fw_addr;
	phys_addr_t mem_phys;
	void *mem_va;
	size_t mem_size;

	struct clk *clks[3];
	size_t num_clks;

	struct rproc_subdev smd_subdev;
};

static int venus_codec_probe(struct rproc_subdev *subdev)
{
	struct qcom_venus *venus = container_of(subdev, struct qcom_venus, smd_subdev);

	return of_platform_populate(venus->dev->of_node, NULL, NULL, venus->dev);
}

static void venus_codec_remove(struct rproc_subdev *subdev)
{
	struct qcom_venus *venus = container_of(subdev, struct qcom_venus, smd_subdev);

	of_platform_depopulate(venus->dev);
}

static int venus_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_venus *venus = rproc->priv;
	phys_addr_t pa;
	size_t fw_size;
	bool relocate;
	int ret;

	ret = qcom_scm_pas_init_image(VENUS_PAS_ID, fw->data, fw->size);
	if (ret) {
		dev_err(&rproc->dev, "invalid firmware metadata (%d)\n", ret);
		return -EINVAL;
	}

	ret = qcom_mdt_parse(fw, &venus->fw_addr, &fw_size, &relocate);
	if (ret) {
		dev_err(&rproc->dev, "failed to parse mdt header (%d)\n", ret);
		return ret;
	}

	if (fw_size > venus->mem_size)
		return -ENOMEM;

	pa = relocate ? venus->mem_phys : venus->fw_addr;

	ret = qcom_scm_pas_mem_setup(VENUS_PAS_ID, pa, fw_size);
	if (ret) {
		dev_err(&rproc->dev, "unable to setup memory (%d)\n", ret);
		return -EINVAL;
	}

	return qcom_mdt_load(rproc, fw, VENUS_FIRMWARE_NAME);
}

static const struct rproc_fw_ops venus_fw_ops = {
	.find_rsc_table = qcom_mdt_find_rsc_table,
	.load = venus_load,
};

static int venus_start(struct rproc *rproc)
{
	struct qcom_venus *venus = rproc->priv;
	int ret;

	ret = pm_runtime_get_sync(venus->dev);
	if (ret < 0)
		return ret;

	ret = qcom_scm_pas_auth_and_reset(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev,
			"authentication image and release reset failed (%d)\n",
			ret);

	pm_runtime_put_sync(venus->dev);

	return ret;
}

static int venus_stop(struct rproc *rproc)
{
	struct qcom_venus *venus = rproc->priv;
	int ret;

	ret = pm_runtime_get_sync(venus->dev);
	if (ret < 0)
		return ret;

	ret = qcom_scm_pas_shutdown(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev, "failed to shutdown: %d\n", ret);

	pm_runtime_put_sync(venus->dev);

	return ret;
}

static void *venus_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct qcom_venus *venus = rproc->priv;
	s64 offset;

	offset = da - venus->fw_addr;

	if (offset < 0 || offset + len > venus->mem_size)
		return NULL;

	return venus->mem_va + offset;
}

static const struct rproc_ops venus_ops = {
	.start = venus_start,
	.stop = venus_stop,
	.da_to_va = venus_da_to_va,
};

#ifdef CONFIG_PM
static int venus_pil_runtime_suspend(struct device *dev)
{
	struct qcom_venus *venus = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < venus->num_clks; i++)
		clk_disable_unprepare(venus->clks[i]);

	return 0;
}

static int venus_pil_runtime_resume(struct device *dev)
{
	struct qcom_venus *venus = dev_get_drvdata(dev);
	int ret;
	int i;

	for (i = 0; i < venus->num_clks; i++) {
		ret = clk_prepare_enable(venus->clks[i]);
		if (ret)
			goto unroll_clocks;
	}

	return 0;

unroll_clocks:
	for (; i >= 0; i--)
		clk_disable_unprepare(venus->clks[i]);

	return ret;
}
#endif

static int venus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_venus *venus;
	struct rproc *rproc;
	int ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

#if 0
	if (!qcom_scm_pas_supported(VENUS_PAS_ID)) {
		dev_err(dev, "PAS is not available for venus\n");
		return -ENXIO;
	}
#endif
	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return ret;

	rproc = rproc_alloc(dev, pdev->name, &venus_ops, VENUS_FIRMWARE_NAME,
			    sizeof(*venus));
	if (!rproc) {
		dev_err(dev, "unable to allocate remoteproc\n");
		ret = -ENOMEM;
		goto release_mem;
	}

	rproc->fw_ops = &venus_fw_ops;
	venus = rproc->priv;
	venus->dev = dev;
	venus->rproc = rproc;
	venus->mem_size = VENUS_FW_MEM_SIZE;

	platform_set_drvdata(pdev, venus);

	venus->mem_va = dma_alloc_coherent(dev, venus->mem_size,
					   &venus->mem_phys, GFP_KERNEL);
	if (!venus->mem_va) {
		ret = -ENOMEM;
		goto free_rproc;
	}

	venus->num_clks = 3;
	venus->clks[0] = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(venus->clks[0]))
		goto dma_free;

	venus->clks[1] = devm_clk_get(&pdev->dev, "iface");
	if (IS_ERR(venus->clks[1]))
		goto dma_free;

	venus->clks[2] = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(venus->clks[2]))
		goto dma_free;

	pm_runtime_enable(dev);

	rproc_add_subdev(rproc, &venus->smd_subdev,
			 venus_codec_probe, venus_codec_remove);

	ret = rproc_add(rproc);
	if (ret)
		goto disable_pm_runtime;

	return 0;

disable_pm_runtime:
	pm_runtime_disable(dev);
dma_free:
	dma_free_coherent(dev, venus->mem_size, venus->mem_va, venus->mem_phys);
free_rproc:
	rproc_put(rproc);
release_mem:
	of_reserved_mem_device_release(dev);

	return ret;
}

static int venus_remove(struct platform_device *pdev)
{
	struct qcom_venus *venus = platform_get_drvdata(pdev);
	struct device *dev = venus->dev;

	rproc_del(venus->rproc);
	rproc_put(venus->rproc);
	dma_free_coherent(dev, venus->mem_size, venus->mem_va, venus->mem_phys);
	of_reserved_mem_device_release(dev);

	pm_runtime_disable(dev);

	return 0;
}

static const struct dev_pm_ops venus_pil_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(venus_pil_runtime_suspend,
			   venus_pil_runtime_resume, NULL)
};

static const struct of_device_id venus_of_match[] = {
	{ .compatible = "qcom,venus-pil" },
	{ },
};
MODULE_DEVICE_TABLE(of, venus_of_match);

static struct platform_driver venus_driver = {
	.probe = venus_probe,
	.remove = venus_remove,
	.driver = {
		.name = "qcom-venus-pil",
		.of_match_table = venus_of_match,
		.pm = &venus_pil_pm_ops,
	},
};

module_platform_driver(venus_driver);
MODULE_DESCRIPTION("Peripheral Image Loader for Venus");
MODULE_LICENSE("GPL v2");
