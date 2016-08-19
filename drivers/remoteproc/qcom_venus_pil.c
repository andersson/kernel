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
};

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

	ret = qcom_scm_pas_auth_and_reset(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev,
			"authentication image and release reset failed (%d)\n",
			ret);

	return ret;
}

static int venus_stop(struct rproc *rproc)
{
	struct qcom_venus *venus = rproc->priv;
	int ret;

	ret = qcom_scm_pas_shutdown(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev, "failed to shutdown: %d\n", ret);

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

static int venus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_venus *venus;
	struct rproc *rproc;
	int ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	if (!qcom_scm_pas_supported(VENUS_PAS_ID)) {
		dev_err(dev, "PAS is not available for venus\n");
		return -ENXIO;
	}

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

	ret = rproc_add(rproc);
	if (ret)
		goto free_mem;

	return 0;

free_mem:
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

	return 0;
}

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
	},
};

module_platform_driver(venus_driver);
MODULE_DESCRIPTION("Peripheral Image Loader for Venus");
MODULE_LICENSE("GPL v2");
