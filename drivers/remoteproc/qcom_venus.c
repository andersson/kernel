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

#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/remoteproc.h>

#include "qcom_mdt_loader.h"
#include "remoteproc_internal.h"

#define VENUS_CRASH_REASON_SMEM		425
#define VENUS_FIRMWARE_NAME		"venus.mdt"
#define VENUS_PAS_ID			9

struct qcom_venus {
	struct device *dev;
	struct rproc *rproc;

	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	void *mem_region;
	size_t mem_size;
};

static int venus_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_venus *venus = (struct qcom_venus *)rproc->priv;
	phys_addr_t fw_addr;
	size_t fw_size;
	bool relocate;
	int ret;

	ret = qcom_scm_pas_init_image(VENUS_PAS_ID, fw->data, fw->size);
	if (ret) {
		dev_err(&rproc->dev, "invalid firmware metadata\n");
		return -EINVAL;
	}

	ret = qcom_mdt_parse(fw, &fw_addr, &fw_size, &relocate);
	if (ret) {
		dev_err(&rproc->dev, "failed to parse mdt header\n");
		return ret;
	}

	if (relocate) {
		venus->mem_reloc = fw_addr;

		ret = qcom_scm_pas_mem_setup(VENUS_PAS_ID, venus->mem_phys, fw_size);
		if (ret) {
			dev_err(&rproc->dev, "unable to setup memory for image\n");
			return -EINVAL;
		}
	}

	return qcom_mdt_load(rproc, fw, rproc->firmware);
}

static const struct rproc_fw_ops venus_fw_ops = {
	.find_rsc_table = qcom_mdt_find_rsc_table,
	.load = venus_load,
};

static int venus_start(struct rproc *rproc)
{
	struct qcom_venus *venus = (struct qcom_venus *)rproc->priv;
	int ret;

	ret = qcom_scm_pas_auth_and_reset(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev,
			"failed to authenticate image and release reset\n");

	return ret;
}

static int venus_stop(struct rproc *rproc)
{
	struct qcom_venus *venus = (struct qcom_venus *)rproc->priv;
	int ret;

	ret = qcom_scm_pas_shutdown(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev, "failed to shutdown: %d\n", ret);

	return ret;
}

static const struct rproc_ops venus_ops = {
	.start = venus_start,
	.stop = venus_stop,
};

static int venus_alloc_memory_region(struct qcom_venus *venus)
{
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(venus->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(venus->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	venus->mem_phys = venus->mem_reloc = r.start;
	venus->mem_size = resource_size(&r);
	venus->mem_region = devm_ioremap_wc(venus->dev, venus->mem_phys, venus->mem_size);
	if (!venus->mem_region) {
		dev_err(venus->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, venus->mem_size);
		return -EBUSY;
	}

	return 0;
}

static int venus_probe(struct platform_device *pdev)
{
	struct qcom_venus *venus;
	struct rproc *rproc;
	int ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	if (!qcom_scm_pas_supported(VENUS_PAS_ID)) {
		dev_err(&pdev->dev, "PAS is not available for venus\n");
		return -ENXIO;
	}

	rproc = rproc_alloc(&pdev->dev, pdev->name, &venus_ops,
			    VENUS_FIRMWARE_NAME, sizeof(*venus));
	if (!rproc) {
		dev_err(&pdev->dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	rproc->fw_ops = &venus_fw_ops;

	venus = (struct qcom_venus *)rproc->priv;
	venus->dev = &pdev->dev;
	venus->rproc = rproc;
	platform_set_drvdata(pdev, venus);

	ret = venus_alloc_memory_region(venus);
	if (ret)
		goto free_rproc;

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	return 0;

free_rproc:
	rproc_put(rproc);

	return ret;
}

static int venus_remove(struct platform_device *pdev)
{
	struct qcom_venus *venus = platform_get_drvdata(pdev);

	rproc_del(venus->rproc);
	rproc_put(venus->rproc);

	return 0;
}

static const struct of_device_id venus_of_match[] = {
	{ .compatible = "qcom,venus-pil" },
	{ },
};

static struct platform_driver venus_driver = {
	.probe = venus_probe,
	.remove = venus_remove,
	.driver = {
		.name = "qcom-venus-pil",
		.of_match_table = venus_of_match,
	},
};

module_platform_driver(venus_driver);
