// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited

#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/wcd9335/registers.h>
#include <linux/mfd/wcd9335/wcd9335.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slimbus.h>

#define SLIM_MANF_ID_QCOM			0x217
#define SLIM_PROD_CODE_WCD9335			0x1a0
#define WCD9335_SLIM_INTERFACE_DEVICE_INDEX	0

static const struct regmap_range_cfg wcd9335_ranges[] = {
	{
		.name = "WCD9335",
		.range_min =  0x0,
		.range_max =  WCD9335_MAX_REGISTER,
		.selector_reg = WCD9335_REG(0x0, 0),
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0x0,
		.window_len = 0x1000,
	},
};

static bool wcd9335_is_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case WCD9335_INTR_PIN1_STATUS0...WCD9335_INTR_PIN2_CLEAR3:
	case WCD9335_ANA_MBHC_RESULT_3:
	case WCD9335_ANA_MBHC_RESULT_2:
	case WCD9335_ANA_MBHC_RESULT_1:
	case WCD9335_ANA_MBHC_MECH:
	case WCD9335_ANA_MBHC_ELECT:
	case WCD9335_ANA_MBHC_ZDET:
	case WCD9335_ANA_MICB2:
	case WCD9335_ANA_RCO:
	case WCD9335_ANA_BIAS:
		return true;
	default:
		return false;
	}
}

static struct regmap_config wcd9335_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.max_register = WCD9335_MAX_REGISTER,
	.can_multi_write = true,
	.ranges = wcd9335_ranges,
	.num_ranges = ARRAY_SIZE(wcd9335_ranges),
	.volatile_reg = wcd9335_is_volatile_register,
};

static const struct regmap_range_cfg wcd9335_ifc_ranges[] = {
	{
		.name = "WCD9335-IFC-DEV",
		.range_min =  0x0,
		.range_max = WCD9335_REG(0, 0x7ff),
		.selector_reg = WCD9335_REG(0, 0x0),
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0x0,
		.window_len = 0x1000,
	},
};

static struct regmap_config wcd9335_ifc_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.can_multi_write = true,
	.max_register = WCD9335_REG(0, 0x7FF),
	.ranges = wcd9335_ifc_ranges,
	.num_ranges = ARRAY_SIZE(wcd9335_ifc_ranges),
};

static const struct regmap_irq wcd9335_irqs[] = {
	/* INTR_REG 0 */
	REGMAP_IRQ_REG(WCD9335_IRQ_SLIMBUS, 0, BIT(0)),
	REGMAP_IRQ_REG(WCD9335_IRQ_FLL_LOCK_LOSS, 0, BIT(1)),
	REGMAP_IRQ_REG(WCD9335_IRQ_HPH_PA_OCPL_FAULT, 0, BIT(2)),
	REGMAP_IRQ_REG(WCD9335_IRQ_HPH_PA_OCPR_FAULT, 0, BIT(3)),
	REGMAP_IRQ_REG(WCD9335_IRQ_EAR_PA_OCP_FAULT, 0, BIT(4)),
	REGMAP_IRQ_REG(WCD9335_IRQ_HPH_PA_CNPL_COMPLETE, 0, BIT(5)),
	REGMAP_IRQ_REG(WCD9335_IRQ_HPH_PA_CNPR_COMPLETE, 0, BIT(6)),
	REGMAP_IRQ_REG(WCD9335_IRQ_EAR_PA_CNP_COMPLETE, 0, BIT(7)),
	/* INTR_REG 1 */
	REGMAP_IRQ_REG(WCD9335_IRQ_MBHC_SW_DET, 1, BIT(0)),
	REGMAP_IRQ_REG(WCD9335_IRQ_MBHC_ELECT_INS_REM_DET, 1, BIT(1)),
	REGMAP_IRQ_REG(WCD9335_IRQ_MBHC_BUTTON_PRESS_DET, 1, BIT(2)),
	REGMAP_IRQ_REG(WCD9335_IRQ_MBHC_BUTTON_RELEASE_DET, 1, BIT(3)),
	REGMAP_IRQ_REG(WCD9335_IRQ_MBHC_ELECT_INS_REM_LEG_DET, 1, BIT(4)),
	/* INTR_REG 2 */
	REGMAP_IRQ_REG(WCD9335_IRQ_LINE_PA1_CNP_COMPLETE, 2, BIT(0)),
	REGMAP_IRQ_REG(WCD9335_IRQ_LINE_PA2_CNP_COMPLETE, 2, BIT(1)),
	REGMAP_IRQ_REG(WCD9335_IRQ_LINE_PA3_CNP_COMPLETE, 2, BIT(2)),
	REGMAP_IRQ_REG(WCD9335_IRQ_LINE_PA4_CNP_COMPLETE, 2, BIT(3)),
	REGMAP_IRQ_REG(WCD9335_IRQ_SOUNDWIRE, 2, BIT(4)),
	REGMAP_IRQ_REG(WCD9335_IRQ_VDD_DIG_RAMP_COMPLETE, 2, BIT(5)),
	REGMAP_IRQ_REG(WCD9335_IRQ_RCO_ERROR, 2, BIT(6)),
	REGMAP_IRQ_REG(WCD9335_IRQ_SVA_ERROR, 2, BIT(7)),
	/* INTR_REG 3 */
	REGMAP_IRQ_REG(WCD9335_IRQ_MAD_AUDIO, 3, BIT(0)),
	REGMAP_IRQ_REG(WCD9335_IRQ_MAD_BEACON, 3, BIT(1)),
	REGMAP_IRQ_REG(WCD9335_IRQ_MAD_ULTRASOUND, 3, BIT(2)),
	REGMAP_IRQ_REG(WCD9335_IRQ_VBAT_ATTACK, 3, BIT(3)),
	REGMAP_IRQ_REG(WCD9335_IRQ_VBAT_RESTORE, 3, BIT(4)),
	REGMAP_IRQ_REG(WCD9335_IRQ_SVA_OUTBOX1, 3, BIT(5)),
	REGMAP_IRQ_REG(WCD9335_IRQ_SVA_OUTBOX2, 3, BIT(6)),
};

static const struct regmap_irq_chip wcd9335_regmap_irq1_chip = {
	.name = "wcd9335_pin1_irq",
	.status_base = WCD9335_INTR_PIN1_STATUS0,
	.mask_base = WCD9335_INTR_PIN1_MASK0,
	.ack_base = WCD9335_INTR_PIN1_CLEAR0,
	.type_base = WCD9335_INTR_LEVEL0,
	.num_regs = 4,
	.irqs = wcd9335_irqs,
	.num_irqs = ARRAY_SIZE(wcd9335_irqs),
};

static int wcd9335_parse_dt(struct wcd9335 *wcd)
{
	struct device *dev = wcd->dev;
	struct device_node *np = dev->of_node;
	int ret;

	wcd->reset_gpio = of_get_named_gpio(np,	"reset-gpios", 0);
	if (wcd->reset_gpio < 0) {
		dev_err(dev, "Reset GPIO missing from DT\n");
		return wcd->reset_gpio;
	}

	wcd->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(wcd->mclk)) {
		dev_err(dev, "mclk not found\n");
		return PTR_ERR(wcd->mclk);
	}

	wcd->native_clk = devm_clk_get(dev, "slimbus");
	if (IS_ERR(wcd->native_clk)) {
		dev_err(dev, "slimbus clock not found\n");
		return PTR_ERR(wcd->native_clk);
	}

	wcd->supplies[0].supply = "vdd-buck";
	wcd->supplies[1].supply = "vdd-buck-sido";
	wcd->supplies[2].supply = "vdd-tx";
	wcd->supplies[3].supply = "vdd-rx";
	wcd->supplies[4].supply = "vdd-io";

	ret = regulator_bulk_get(dev, WCD9335_MAX_SUPPLY, wcd->supplies);
	if (ret) {
		dev_err(dev, "Failed to get supplies: err = %d\n", ret);
		return ret;
	}

	return 0;
}

static int wcd9335_power_on_reset(struct wcd9335 *wcd)
{
	struct device *dev = wcd->dev;
	int ret;

	ret = regulator_bulk_enable(WCD9335_MAX_SUPPLY, wcd->supplies);
	if (ret) {
		dev_err(dev, "Failed to get supplies: err = %d\n", ret);
		return ret;
	}

	/*
	 * For WCD9335, it takes about 600us for the Vout_A and
	 * Vout_D to be ready after BUCK_SIDO is powered up.
	 * SYS_RST_N shouldn't be pulled high during this time
	 * Toggle the reset line to make sure the reset pulse is
	 * correctly applied
	 */
	usleep_range(600, 650);

	gpio_direction_output(wcd->reset_gpio, 0);
	msleep(20);
	gpio_set_value(wcd->reset_gpio, 1);
	msleep(20);

	return 0;
}

static int wcd9335_bring_up(struct wcd9335 *wcd)
{
	struct regmap *rm = wcd->regmap;
	int val, byte0;

	regmap_read(rm, WCD9335_CHIP_TIER_CTRL_EFUSE_VAL_OUT0, &val);
	regmap_read(rm, WCD9335_CHIP_TIER_CTRL_CHIP_ID_BYTE0, &byte0);

	if ((val < 0) || (byte0 < 0)) {
		dev_err(wcd->dev, "WCD9335 CODEC version detection fail!\n");
		return -EINVAL;
	}

	if (byte0 == 0x1) {
		dev_info(wcd->dev, "WCD9335 CODEC version is v2.0\n");
		wcd->version = WCD9335_VERSION_2_0;
		regmap_write(rm, WCD9335_CODEC_RPM_RST_CTL, 0x01);
		regmap_write(rm, WCD9335_SIDO_SIDO_TEST_2, 0x00);
		regmap_write(rm, WCD9335_SIDO_SIDO_CCL_8, 0x6F);
		regmap_write(rm, WCD9335_BIAS_VBG_FINE_ADJ, 0x65);
		regmap_write(rm, WCD9335_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x5);
		regmap_write(rm, WCD9335_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x7);
		regmap_write(rm, WCD9335_CODEC_RPM_PWR_CDC_DIG_HM_CTL, 0x3);
		regmap_write(rm, WCD9335_CODEC_RPM_RST_CTL, 0x3);
	} else {
		dev_err(wcd->dev, "WCD9335 CODEC version not supported\n");
		return -EINVAL;
	}

	return 0;
}

static int wcd9335_irq_init(struct wcd9335 *wcd)
{
	int ret;

	/*
	 * INTR1 consists of all possible interrupt sources Ear OCP,
	 * HPH OCP, MBHC, MAD, VBAT, and SVA
	 * INTR2 is a subset of first interrupt sources MAD, VBAT, and SVA
	 */
	wcd->intr1 = of_irq_get_byname(wcd->dev->of_node, "intr1");
	if (wcd->intr1 < 0) {
		if (wcd->intr1 != -EPROBE_DEFER)
			dev_err(wcd->dev, "Unable to configure IRQ\n");

		return wcd->intr1;
	}

	ret = devm_regmap_add_irq_chip(wcd->dev, wcd->regmap, wcd->intr1,
				 IRQF_TRIGGER_HIGH, 0,
				 &wcd9335_regmap_irq1_chip, &wcd->irq_data);
	if (ret)
		dev_err(wcd->dev, "Failed to register IRQ chip: %d\n", ret);

	return ret;
}

static int wcd9335_slim_probe(struct slim_device *slim)
{
	struct device *dev = &slim->dev;
	struct wcd9335 *wcd;
	int ret;

	/* Interface device */
	if (slim->e_addr.dev_index == WCD9335_SLIM_INTERFACE_DEVICE_INDEX)
		return 0;

	wcd = devm_kzalloc(dev, sizeof(*wcd), GFP_KERNEL);
	if (!wcd)
		return	-ENOMEM;

	wcd->dev = dev;

	ret = wcd9335_parse_dt(wcd);
	if (ret) {
		dev_err(dev, "Error parsing DT: %d\n", ret);
		return ret;
	}

	ret = wcd9335_power_on_reset(wcd);
	if (ret)
		return ret;

	wcd->slim = slim;
	wcd->intf_type = WCD9335_INTERFACE_TYPE_SLIMBUS;

	dev_set_drvdata(dev, wcd);

	return 0;
}

static const struct mfd_cell wcd9335_devices[] = {
	{ .name = "wcd9335-codec", },
};

static int wcd9335_slim_status(struct slim_device *sdev,
			       enum slim_device_status status)
{
	struct device_node *ifc_dev_np;
	struct wcd9335 *wcd;
	int ret;

	if (sdev->e_addr.dev_index == WCD9335_SLIM_INTERFACE_DEVICE_INDEX)
		return 0;

	wcd = dev_get_drvdata(&sdev->dev);

	ifc_dev_np = of_parse_phandle(wcd->dev->of_node, "slim-ifc-dev", 0);
	if (!ifc_dev_np) {
		dev_err(wcd->dev, "No Interface device found\n");
		return -EINVAL;
	}

	wcd->slim_ifc_dev = of_slim_get_device(sdev->ctrl, ifc_dev_np);
	if (!wcd->slim_ifc_dev) {
		dev_err(wcd->dev, "Unable to get SLIM Interface device\n");
		return -EINVAL;
	}

	wcd->regmap = regmap_init_slimbus(sdev, &wcd9335_regmap_config);
	if (IS_ERR(wcd->regmap)) {
		dev_err(wcd->dev, "Failed to allocate slim register map\n");
		return PTR_ERR(wcd->regmap);
	}

	wcd->ifc_dev_regmap = regmap_init_slimbus(wcd->slim_ifc_dev,
						  &wcd9335_ifc_regmap_config);
	if (IS_ERR(wcd->ifc_dev_regmap)) {
		dev_err(wcd->dev, "Failed to allocate ifc register map\n");
		return PTR_ERR(wcd->ifc_dev_regmap);
	}

	ret = wcd9335_bring_up(wcd);
	if (ret) {
		dev_err(wcd->dev, "Failed to bringup WCD9335\n");
		return ret;
	}

	ret = wcd9335_irq_init(wcd);
	if (ret)
		return ret;

	ret = devm_mfd_add_devices(wcd->dev, 0, wcd9335_devices,
			       ARRAY_SIZE(wcd9335_devices), NULL, 0, NULL);
	if (ret < 0)
		dev_err(wcd->dev, "Failed to add mtd devices: %d\n", ret);

	return ret;
}

static const struct slim_device_id wcd9335_slim_id[] = {
	{SLIM_MANF_ID_QCOM, SLIM_PROD_CODE_WCD9335, 0x1, 0x0},
	{}
};

static struct slim_driver wcd9335_slim_driver = {
	.driver = {
		.name = "wcd9335-slim",
	},
	.probe = wcd9335_slim_probe,
	.device_status = wcd9335_slim_status,
	.id_table = wcd9335_slim_id,
};

module_slim_driver(wcd9335_slim_driver);
MODULE_DESCRIPTION("WCD9335 slim driver");
MODULE_LICENSE("GPL v2");
