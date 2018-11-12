/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __WCD9335_H__
#define __WCD9335_H__

#include <linux/slimbus.h>
#include <linux/regulator/consumer.h>

#define WCD9335_VERSION_2_0     2
#define WCD9335_MAX_SUPPLY	5

#define	WCD9335_IRQ_SLIMBUS			0
#define	WCD9335_IRQ_FLL_LOCK_LOSS		1
#define	WCD9335_IRQ_HPH_PA_OCPL_FAULT		2
#define	WCD9335_IRQ_HPH_PA_OCPR_FAULT		3
#define	WCD9335_IRQ_EAR_PA_OCP_FAULT		4
#define	WCD9335_IRQ_HPH_PA_CNPL_COMPLETE	5
#define	WCD9335_IRQ_HPH_PA_CNPR_COMPLETE	6
#define	WCD9335_IRQ_EAR_PA_CNP_COMPLETE		7
#define	WCD9335_IRQ_MBHC_SW_DET			8
#define	WCD9335_IRQ_MBHC_ELECT_INS_REM_DET	9
#define	WCD9335_IRQ_MBHC_BUTTON_PRESS_DET	10
#define	WCD9335_IRQ_MBHC_BUTTON_RELEASE_DET	11
#define	WCD9335_IRQ_MBHC_ELECT_INS_REM_LEG_DET	12
#define	WCD9335_IRQ_RESERVED_0			13
#define	WCD9335_IRQ_RESERVED_1			14
#define	WCD9335_IRQ_RESERVED_2			15
#define	WCD9335_IRQ_LINE_PA1_CNP_COMPLETE	16
#define	WCD9335_IRQ_LINE_PA2_CNP_COMPLETE	17
#define	WCD9335_IRQ_LINE_PA3_CNP_COMPLETE	18
#define	WCD9335_IRQ_LINE_PA4_CNP_COMPLETE	19
#define	WCD9335_IRQ_SOUNDWIRE			20
#define	WCD9335_IRQ_VDD_DIG_RAMP_COMPLETE	21
#define	WCD9335_IRQ_RCO_ERROR			22
#define	WCD9335_IRQ_SVA_ERROR			23
#define	WCD9335_IRQ_MAD_AUDIO			24
#define	WCD9335_IRQ_MAD_BEACON			25
#define	WCD9335_IRQ_MAD_ULTRASOUND		26
#define	WCD9335_IRQ_VBAT_ATTACK			27
#define	WCD9335_IRQ_VBAT_RESTORE		28
#define	WCD9335_IRQ_SVA_OUTBOX1			29
#define	WCD9335_IRQ_SVA_OUTBOX2			30

enum wcd_interface_type {
	WCD9335_INTERFACE_TYPE_SLIMBUS = 1,
	WCD9335_INTERFACE_TYPE_I2C,
};

struct wcd9335 {
	int version;
	int intr1;
	int reset_gpio;
	enum wcd_interface_type intf_type;
	struct device *dev;
	struct clk *mclk;
	struct clk *native_clk;
	struct slim_device *slim;
	struct slim_device *slim_ifc_dev;
	struct regmap *regmap;
	struct regmap *ifc_dev_regmap;
	struct regmap_irq_chip_data *irq_data;
	struct regulator_bulk_data supplies[WCD9335_MAX_SUPPLY];
};

#endif /* __WCD9335_H__ */
