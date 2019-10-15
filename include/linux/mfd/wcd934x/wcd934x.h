/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __WCD934X_H__
#define __WCD934X_H__
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/slimbus.h>

#define WCD934X_MAX_SUPPLY	5

struct wcd934x_data {
	int reset_gpio;
	int irq;
	struct device *dev;
	struct clk *extclk;
	struct regmap *regmap;
	struct slim_device *sdev;
	struct regmap_irq_chip_data *irq_data;
	struct regulator_bulk_data supplies[WCD934X_MAX_SUPPLY];
};

#endif /* __WCD934X_H__ */

