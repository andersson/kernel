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

#define TRI_LED_SRC_SEL	0x45
#define TRI_LED_EN_CTL	0x46
#define TRI_LED_ATC_CTL	0x47

#define TRI_LED_COUNT	3

static struct platform_driver tri_led_driver;

/*
 * tri_led_dev - TRILED device context
 * @dev:	struct device reference
 * @map:	regmap for register access
 * @reg:	base address of TRILED block
 */
struct tri_led_dev {
	struct device *dev;
	struct regmap *map;

	u32 reg;
};

/*
 * qcom_tri_led - representation of a single color
 * @tdev:	TRILED device reference
 * @color:	color of this object 0 <= color < 3
 */
struct qcom_tri_led {
	struct tri_led_dev *tdev;
	u8 color;
};

static void tri_led_release(struct device *dev, void *res)
{
	struct qcom_tri_led *tri = res;
	struct tri_led_dev *tdev = tri->tdev;

	put_device(tdev->dev);
}

/**
 * qcom_tri_led_get() - acquire a reference to a single color of the TRILED
 * @dev:	struct device of the client
 *
 * Returned devres allocated TRILED color object, NULL if client lacks TRILED
 * reference or ERR_PTR on failure.
 */
struct qcom_tri_led *qcom_tri_led_get(struct device *dev)
{
	struct platform_device *pdev;
	struct of_phandle_args args;
	struct qcom_tri_led *tri;
	int ret;

	ret = of_parse_phandle_with_fixed_args(dev->of_node,
					       "qcom,tri-led", 1, 0, &args);
	if (ret)
		return NULL;

	pdev = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!pdev || !pdev->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);

	if (pdev->dev.driver != &tri_led_driver.driver) {
		dev_err(dev, "referenced node is not a tri-led\n");
		return ERR_PTR(-EINVAL);
	}

	if (args.args[0] >= TRI_LED_COUNT) {
		dev_err(dev, "invalid color\n");
		return ERR_PTR(-EINVAL);
	}

	tri = devres_alloc(tri_led_release, sizeof(*tri), GFP_KERNEL);
	if (!tri)
		return ERR_PTR(-ENOMEM);

	tri->tdev = platform_get_drvdata(pdev);
	tri->color = args.args[0];

	devres_add(dev, tri);

	return tri;
}
EXPORT_SYMBOL_GPL(qcom_tri_led_get);

/**
 * qcom_tri_led_set() - enable/disable a TRILED output
 * @tri:	TRILED color object reference
 * @enable:	new state of the output
 *
 * Returns 0 on success, negative errno on failure.
 */
int qcom_tri_led_set(struct qcom_tri_led *tri, bool enable)
{
	struct tri_led_dev *tdev = tri->tdev;
	unsigned int mask;
	unsigned int val;

	/* red, green, blue are mapped to bits 7, 6 and 5 respectively */
	mask = BIT(7 - tri->color);
	val = enable ? mask : 0;

	return regmap_update_bits(tdev->map, tdev->reg + TRI_LED_EN_CTL,
				  mask, val);
}
EXPORT_SYMBOL_GPL(qcom_tri_led_set);

static int tri_led_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct tri_led_dev *tri;
	u32 src_sel;
	int ret;

	tri = devm_kzalloc(&pdev->dev, sizeof(*tri), GFP_KERNEL);
	if (!tri)
		return -ENOMEM;

	tri->dev = &pdev->dev;

	tri->map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!tri->map) {
		dev_err(&pdev->dev, "parent regmap unavailable\n");
		return -ENXIO;
	}

	ret = of_property_read_u32(np, "reg", &tri->reg);
	if (ret) {
		dev_err(&pdev->dev, "no register offset specified\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "qcom,power-source", &src_sel);
	if (ret || src_sel == 2 || src_sel > 3) {
		dev_err(&pdev->dev, "invalid power source\n");
		return -EINVAL;
	}

	/* Disable automatic trickle charge LED */
	regmap_write(tri->map, tri->reg + TRI_LED_ATC_CTL, 0);

	/* Configure power source */
	regmap_write(tri->map, tri->reg + TRI_LED_SRC_SEL, src_sel);

	/* Default all outputs to off */
	regmap_write(tri->map, tri->reg + TRI_LED_EN_CTL, 0);

	platform_set_drvdata(pdev, tri);

	return 0;
}

static const struct of_device_id tri_led_of_table[] = {
	{ .compatible = "qcom,spmi-tri-led" },
	{},
};
MODULE_DEVICE_TABLE(of, tri_led_of_table);

static struct platform_driver tri_led_driver = {
	.probe = tri_led_probe,
	.driver = {
		.name = "qcom_tri_led",
		.of_match_table = tri_led_of_table,
	},
};
module_platform_driver(tri_led_driver);

MODULE_DESCRIPTION("Qualcomm TRI LED driver");
MODULE_LICENSE("GPL v2");
