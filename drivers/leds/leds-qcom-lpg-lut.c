/* Copyright (c) 2017 Linaro Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "leds-qcom-lpg.h"

#define LPG_LUT_REG(x)		(0x40 + (x) * 2)
#define RAMP_CONTROL_REG	0xc8

static struct platform_driver lpg_lut_driver;

/*
 * lpg_lut_dev - LUT device context
 * @dev:	struct device for the LUT device
 * @map:	regmap for register access
 * @reg:	base address for the LUT block
 * @size:	number of LUT entries in LUT block
 * @bitmap:	bitmap tracking occupied LUT entries
 */
struct lpg_lut_dev {
	struct device *dev;
	struct regmap *map;

	u32 reg;
	u32 size;

	unsigned long bitmap[];
};

/*
 * qcom_lpg_lut - context for a client and LUT device pair
 * @ldev:	reference to a LUT device
 * @start_mask:	mask of bits to use for synchronizing ramp generators
 */
struct qcom_lpg_lut {
	struct lpg_lut_dev *ldev;
	int start_mask;
};

static void lpg_lut_release(struct device *dev, void *res)
{
	struct qcom_lpg_lut *lut = res;

	put_device(lut->ldev->dev);
}

/**
 * qcom_lpg_lut_get() - acquire a handle to the LUT implementation
 * @dev:	struct device reference of the client
 *
 * Returns a LUT context, or ERR_PTR on failure.
 */
struct qcom_lpg_lut *qcom_lpg_lut_get(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *lut_node;
	struct qcom_lpg_lut *lut;
	u32 cell;
	int ret;

	lut_node = of_parse_phandle(dev->of_node, "qcom,lut", 0);
	if (!lut_node)
		return NULL;

	ret = of_property_read_u32(dev->of_node, "qcom,lpg-channel", &cell);
	if (ret) {
		dev_err(dev, "lpg without qcom,lpg-channel\n");
		return ERR_PTR(ret);
	}

	pdev = of_find_device_by_node(lut_node);
	of_node_put(lut_node);
	if (!pdev || !pdev->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);

	if (pdev->dev.driver != &lpg_lut_driver.driver) {
		dev_err(dev, "referenced node is not a lpg lut\n");
		return ERR_PTR(-EINVAL);
	}

	lut = devres_alloc(lpg_lut_release, sizeof(*lut), GFP_KERNEL);
	if (!lut)
		return ERR_PTR(-ENOMEM);

	lut->ldev = platform_get_drvdata(pdev);
	lut->start_mask = BIT(cell - 1);

	devres_add(dev, lut);

	return lut;
}
EXPORT_SYMBOL_GPL(qcom_lpg_lut_get);

/**
 * qcom_lpg_lut_store() - store a sequence of levels in the LUT
 * @lut:	LUT context acquired from qcom_lpg_lut_get()
 * @values:	an array of values, in the range 0 <= x < 512
 * @len:	length of the @values array
 *
 * Returns a qcom_lpg_pattern object, or ERR_PTR on failure.
 *
 * Patterns must be freed by calling qcom_lpg_lut_free()
 */
struct qcom_lpg_pattern *qcom_lpg_lut_store(struct qcom_lpg_lut *lut,
					    const u16 *values, size_t len)
{
	struct qcom_lpg_pattern *pattern;
	struct lpg_lut_dev *ldev = lut->ldev;
	unsigned long lo_idx;
	u8 val[2];
	int i;

	/* Hardware does not behave when LO_IDX == HI_IDX */
	if (len == 1)
		return ERR_PTR(-EINVAL);

	lo_idx = bitmap_find_next_zero_area(ldev->bitmap, ldev->size, 0, len, 0);
	if (lo_idx >= ldev->size)
		return ERR_PTR(-ENOMEM);

	pattern = kzalloc(sizeof(*pattern), GFP_KERNEL);
	if (!pattern)
		return ERR_PTR(-ENOMEM);

	pattern->lut = lut;
	pattern->lo_idx = lo_idx;
	pattern->hi_idx = lo_idx + len - 1;

	for (i = 0; i < len; i++) {
		val[0] = values[i] & 0xff;
		val[1] = values[i] >> 8;

		regmap_bulk_write(ldev->map,
				  ldev->reg + LPG_LUT_REG(lo_idx + i), val, 2);
	}

	bitmap_set(ldev->bitmap, lo_idx, len);

	return pattern;
}
EXPORT_SYMBOL_GPL(qcom_lpg_lut_store);

/**
 * qcom_lpg_lut_read() - read the data points from a pattern
 * @pattern:	pattern to be read
 * @len:	to be filled in with length of the pattern
 *
 * Returns an allocated u16 array of brightness values, NULL if no pattern
 * specified or ERR_PTR() on failure. Caller must free the returned array.
 */
u16 *qcom_lpg_lut_read(struct qcom_lpg_pattern *pattern, size_t *len)
{
	struct qcom_lpg_lut *lut;
	struct lpg_lut_dev *ldev;
	unsigned long lo_idx;
	u16 *values;
	u8 val[2];
	int ret;
	int i;

	if (!pattern)
		return NULL;

	lut = pattern->lut;
	ldev = lut->ldev;
	lo_idx = pattern->lo_idx;

	*len = pattern->hi_idx - pattern->lo_idx + 1;

	values = kcalloc(*len, sizeof(u16), GFP_KERNEL);
	if (!values)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < *len; i++) {
		ret = regmap_bulk_read(ldev->map,
				       ldev->reg + LPG_LUT_REG(lo_idx + i),
				       &val, 2);
		if (ret < 0) {
			kfree(values);
			return ERR_PTR(ret);
		}

		values[i] = val[0] | val[1] << 8;
	}

	return values;
}
EXPORT_SYMBOL_GPL(qcom_lpg_lut_read);

/**
 * qcom_lpg_lut_free() - release LUT pattern and free entries
 * @pattern:	reference to pattern to release
 */
void qcom_lpg_lut_free(struct qcom_lpg_pattern *pattern)
{
	struct qcom_lpg_lut *lut;
	struct lpg_lut_dev *ldev;
	int len;

	if (!pattern)
		return;

	lut = pattern->lut;
	ldev = lut->ldev;

	len = pattern->hi_idx - pattern->lo_idx + 1;
	bitmap_clear(ldev->bitmap, pattern->lo_idx, len);
}
EXPORT_SYMBOL_GPL(qcom_lpg_lut_free);

/**
 * qcom_lpg_lut_sync() - (re)start the ramp generator, to sync pattern
 * @lut:	LUT device reference, to sync
 */
int qcom_lpg_lut_sync(struct qcom_lpg_lut *lut)
{
	struct lpg_lut_dev *ldev = lut->ldev;

	return regmap_update_bits(ldev->map, ldev->reg + RAMP_CONTROL_REG,
				  lut->start_mask, 0xff);
}
EXPORT_SYMBOL_GPL(qcom_lpg_lut_sync);

static int lpg_lut_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct lpg_lut_dev *ldev;
	size_t bitmap_size;
	u32 size;
	int ret;

	ret = of_property_read_u32(np, "qcom,lut-size", &size);
	if (ret) {
		dev_err(&pdev->dev, "invalid LUT size\n");
		return -EINVAL;
	}

	bitmap_size = BITS_TO_LONGS(size) / sizeof(unsigned long);
	ldev = devm_kzalloc(&pdev->dev, sizeof(*ldev) + bitmap_size, GFP_KERNEL);
	if (!ldev)
		return -ENOMEM;

	ldev->dev = &pdev->dev;
	ldev->size = size;

	ldev->map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!ldev->map) {
		dev_err(&pdev->dev, "parent regmap unavailable\n");
		return -ENXIO;
	}

	ret = of_property_read_u32(np, "reg", &ldev->reg);
	if (ret) {
		dev_err(&pdev->dev, "no register offset specified\n");
		return -EINVAL;
	}

	platform_set_drvdata(pdev, ldev);

	return 0;
}

static const struct of_device_id lpg_lut_of_table[] = {
	{ .compatible = "qcom,spmi-lpg-lut" },
	{},
};
MODULE_DEVICE_TABLE(of, lpg_lut_of_table);

static struct platform_driver lpg_lut_driver = {
	.probe = lpg_lut_probe,
	.driver = {
		.name = "qcom_lpg_lut",
		.of_match_table = lpg_lut_of_table,
	},
};
module_platform_driver(lpg_lut_driver);

MODULE_DESCRIPTION("Qualcomm TRI LED driver");
MODULE_LICENSE("GPL v2");
