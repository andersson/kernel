/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <dt-bindings/phy/phy.h>

/* QMP PHY QSERDES COM registers */
#define QSERDES_COM_BG_TIMER				0x00c
#define QSERDES_COM_SSC_EN_CENTER			0x010
#define QSERDES_COM_SSC_ADJ_PER1			0x014
#define QSERDES_COM_SSC_ADJ_PER2			0x018
#define QSERDES_COM_SSC_PER1				0x01c
#define QSERDES_COM_SSC_PER2				0x020
#define QSERDES_COM_SSC_STEP_SIZE1			0x024
#define QSERDES_COM_SSC_STEP_SIZE2			0x028
#define QSERDES_COM_BIAS_EN_CLKBUFLR_EN			0x034
#define QSERDES_COM_CLK_ENABLE1				0x038
#define QSERDES_COM_SYS_CLK_CTRL			0x03c
#define QSERDES_COM_SYSCLK_BUF_ENABLE			0x040
#define QSERDES_COM_PLL_IVCO				0x048
#define QSERDES_COM_LOCK_CMP1_MODE0			0x04c
#define QSERDES_COM_LOCK_CMP2_MODE0			0x050
#define QSERDES_COM_LOCK_CMP3_MODE0			0x054
#define QSERDES_COM_LOCK_CMP1_MODE1			0x058
#define QSERDES_COM_LOCK_CMP2_MODE1			0x05c
#define QSERDES_COM_LOCK_CMP3_MODE1			0x060
#define QSERDES_COM_BG_TRIM				0x070
#define QSERDES_COM_CLK_EP_DIV				0x074
#define QSERDES_COM_CP_CTRL_MODE0			0x078
#define QSERDES_COM_CP_CTRL_MODE1			0x07c
#define QSERDES_COM_PLL_RCTRL_MODE0			0x084
#define QSERDES_COM_PLL_RCTRL_MODE1			0x088
#define QSERDES_COM_PLL_CCTRL_MODE0			0x090
#define QSERDES_COM_PLL_CCTRL_MODE1			0x094
#define QSERDES_COM_SYSCLK_EN_SEL			0x0ac
#define QSERDES_COM_RESETSM_CNTRL			0x0b4
#define QSERDES_COM_RESTRIM_CTRL			0x0bc
#define QSERDES_COM_RESCODE_DIV_NUM			0x0c4
#define QSERDES_COM_LOCK_CMP_EN				0x0c8
#define QSERDES_COM_LOCK_CMP_CFG			0x0cc
#define QSERDES_COM_DEC_START_MODE0			0x0d0
#define QSERDES_COM_DEC_START_MODE1			0x0d4
#define QSERDES_COM_DIV_FRAC_START1_MODE0		0x0dc
#define QSERDES_COM_DIV_FRAC_START2_MODE0		0x0e0
#define QSERDES_COM_DIV_FRAC_START3_MODE0		0x0e4
#define QSERDES_COM_DIV_FRAC_START1_MODE1		0x0e8
#define QSERDES_COM_DIV_FRAC_START2_MODE1		0x0ec
#define QSERDES_COM_DIV_FRAC_START3_MODE1		0x0f0
#define QSERDES_COM_INTEGLOOP_GAIN0_MODE0		0x108
#define QSERDES_COM_INTEGLOOP_GAIN1_MODE0		0x10c
#define QSERDES_COM_INTEGLOOP_GAIN0_MODE1		0x110
#define QSERDES_COM_INTEGLOOP_GAIN1_MODE1		0x114
#define QSERDES_COM_VCO_TUNE_CTRL			0x124
#define QSERDES_COM_VCO_TUNE_MAP			0x128
#define QSERDES_COM_VCO_TUNE1_MODE0			0x12c
#define QSERDES_COM_VCO_TUNE2_MODE0			0x130
#define QSERDES_COM_VCO_TUNE1_MODE1			0x134
#define QSERDES_COM_VCO_TUNE2_MODE1			0x138
#define QSERDES_COM_VCO_TUNE_TIMER1			0x144
#define QSERDES_COM_VCO_TUNE_TIMER2			0x148
#define QSERDES_COM_BG_CTRL				0x170
#define QSERDES_COM_CLK_SELECT				0x174
#define QSERDES_COM_HSCLK_SEL				0x178
#define QSERDES_COM_CORECLK_DIV				0x184
#define QSERDES_COM_CORE_CLK_EN				0x18c
#define QSERDES_COM_C_READY_STATUS			0x190
#define QSERDES_COM_CMN_CONFIG				0x194
#define QSERDES_COM_SVS_MODE_CLK_SEL			0x19c
#define QSERDES_COM_DEBUG_BUS0				0x1a0
#define QSERDES_COM_DEBUG_BUS1				0x1a4
#define QSERDES_COM_DEBUG_BUS2				0x1a8
#define QSERDES_COM_DEBUG_BUS3				0x1ac
#define QSERDES_COM_DEBUG_BUS_SEL			0x1b0
#define QSERDES_COM_CORECLK_DIV_MODE1			0x1bc

/* QMP PHY TX registers */
#define QSERDES_TX_RES_CODE_LANE_OFFSET			0x054
#define QSERDES_TX_DEBUG_BUS_SEL			0x064
#define QSERDES_TX_HIGHZ_TRANSCEIVEREN_BIAS_DRVR_EN	0x068
#define QSERDES_TX_LANE_MODE				0x094
#define QSERDES_TX_RCV_DETECT_LVL_2			0x0ac

/* QMP PHY RX registers */
#define QSERDES_RX_UCDR_SO_GAIN_HALF			0x010
#define QSERDES_RX_UCDR_SO_GAIN				0x01c
#define QSERDES_RX_UCDR_FASTLOCK_FO_GAIN		0x040
#define QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE	0x048
#define QSERDES_RX_RX_TERM_BW				0x090
#define QSERDES_RX_RX_EQ_GAIN1_LSB			0x0c4
#define QSERDES_RX_RX_EQ_GAIN1_MSB			0x0c8
#define QSERDES_RX_RX_EQ_GAIN2_LSB			0x0cc
#define QSERDES_RX_RX_EQ_GAIN2_MSB			0x0d0
#define QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2		0x0d8
#define QSERDES_RX_RX_EQU_ADAPTOR_CNTRL3		0x0dc
#define QSERDES_RX_RX_EQU_ADAPTOR_CNTRL4		0x0e0
#define QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1		0x108
#define QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2		0x10c
#define QSERDES_RX_SIGDET_ENABLES			0x110
#define QSERDES_RX_SIGDET_CNTRL				0x114
#define QSERDES_RX_SIGDET_LVL				0x118
#define QSERDES_RX_SIGDET_DEGLITCH_CNTRL		0x11c
#define QSERDES_RX_RX_BAND				0x120
#define QSERDES_RX_RX_INTERFACE_MODE			0x12c

/* QMP PHY PCS registers */
#define QPHY_SW_RESET					0x00
#define QPHY_POWER_DOWN_CONTROL				0x04
#define QPHY_START_CTRL					0x08
#define QPHY_TXDEEMPH_M6DB_V0				0x24
#define QPHY_TXDEEMPH_M3P5DB_V0				0x28
#define QPHY_ENDPOINT_REFCLK_DRIVE			0x54
#define QPHY_RX_IDLE_DTCT_CNTRL				0x58
#define QPHY_POWER_STATE_CONFIG1			0x60
#define QPHY_POWER_STATE_CONFIG2			0x64
#define QPHY_POWER_STATE_CONFIG4			0x6c
#define QPHY_LOCK_DETECT_CONFIG1			0x80
#define QPHY_LOCK_DETECT_CONFIG2			0x84
#define QPHY_LOCK_DETECT_CONFIG3			0x88
#define QPHY_PWRUP_RESET_DLY_TIME_AUXCLK		0xa0
#define QPHY_LP_WAKEUP_DLY_TIME_AUXCLK			0xa4

/* QPHY_SW_RESET bit */
#define SW_RESET				BIT(0)
/* QPHY_POWER_DOWN_CONTROL */
#define SW_PWRDN				BIT(0)
#define REFCLK_DRV_DSBL				BIT(1)
/* QPHY_START_CONTROL bits */
#define SERDES_START				BIT(0)
#define PCS_START				BIT(1)
#define PLL_READY_GATE_EN			BIT(3)
/* QPHY_PCS_STATUS bit */
#define PHYSTATUS				BIT(6)
/* QPHY_COM_PCS_READY_STATUS bit */
#define PCS_READY				BIT(0)

#define PHY_INIT_COMPLETE_TIMEOUT		1000
#define POWER_DOWN_DELAY_US_MIN			10
#define POWER_DOWN_DELAY_US_MAX			11

#define MAX_PROP_NAME				32

struct qmp_phy_init_tbl {
	unsigned int offset;
	unsigned int val;
	/*
	 * register part of layout ?
	 * if yes, then offset gives index in the reg-layout
	 */
	int in_layout;
};
#define QMP_PHY_INIT_CFG(o, v)		\
	{				\
		.offset = o,		\
		.val = v,		\
	}
#define QMP_PHY_INIT_CFG_L(o, v)	\
	{				\
		.offset = o,		\
		.val = v,		\
		.in_layout = 1,		\
	}

/* set of registers with offsets different per-PHY */
enum qphy_reg_layout {
	/* Common block control registers */
	QPHY_COM_SW_RESET,
	QPHY_COM_POWER_DOWN_CONTROL,
	QPHY_COM_START_CONTROL,
	QPHY_COM_PCS_READY_STATUS,
	/* PCS registers */
	QPHY_PLL_LOCK_CHK_DLY_TIME,
	QPHY_FLL_CNTRL1,
	QPHY_FLL_CNTRL2,
	QPHY_FLL_CNT_VAL_L,
	QPHY_FLL_CNT_VAL_H_TOL,
	QPHY_FLL_MAN_CODE,
	QPHY_PCS_READY_STATUS,
};

static const unsigned int pciephy_regs_layout[] = {
	[QPHY_COM_SW_RESET]		= 0x400,
	[QPHY_COM_POWER_DOWN_CONTROL]	= 0x404,
	[QPHY_COM_START_CONTROL]	= 0x408,
	[QPHY_COM_PCS_READY_STATUS]	= 0x448,
	[QPHY_PLL_LOCK_CHK_DLY_TIME]	= 0xa8,
	[QPHY_FLL_CNTRL1]		= 0xc4,
	[QPHY_FLL_CNTRL2]		= 0xc8,
	[QPHY_FLL_CNT_VAL_L]		= 0xcc,
	[QPHY_FLL_CNT_VAL_H_TOL]	= 0xd0,
	[QPHY_FLL_MAN_CODE]		= 0xd4,
	[QPHY_PCS_READY_STATUS]		= 0x174,
};

static const unsigned int usb3phy_regs_layout[] = {
	[QPHY_FLL_CNTRL1]		= 0xc0,
	[QPHY_FLL_CNTRL2]		= 0xc4,
	[QPHY_FLL_CNT_VAL_L]		= 0xc8,
	[QPHY_FLL_CNT_VAL_H_TOL]	= 0xcc,
	[QPHY_FLL_MAN_CODE]		= 0xd0,
	[QPHY_PCS_READY_STATUS]		= 0x17c,
};

static const struct qmp_phy_init_tbl pciephy_serdes_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_COM_BIAS_EN_CLKBUFLR_EN, 0x1c),
	QMP_PHY_INIT_CFG(QSERDES_COM_CLK_ENABLE1, 0x10),
	QMP_PHY_INIT_CFG(QSERDES_COM_CLK_SELECT, 0x33),
	QMP_PHY_INIT_CFG(QSERDES_COM_CMN_CONFIG, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP_EN, 0x42),
	QMP_PHY_INIT_CFG(QSERDES_COM_VCO_TUNE_MAP, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_VCO_TUNE_TIMER1, 0xff),
	QMP_PHY_INIT_CFG(QSERDES_COM_VCO_TUNE_TIMER2, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_COM_HSCLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_SVS_MODE_CLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_CORE_CLK_EN, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_CORECLK_DIV, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_COM_BG_TIMER, 0x09),
	QMP_PHY_INIT_CFG(QSERDES_COM_DEC_START_MODE0, 0x82),
	QMP_PHY_INIT_CFG(QSERDES_COM_DIV_FRAC_START3_MODE0, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_COM_DIV_FRAC_START2_MODE0, 0x55),
	QMP_PHY_INIT_CFG(QSERDES_COM_DIV_FRAC_START1_MODE0, 0x55),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP3_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP2_MODE0, 0x1a),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP1_MODE0, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_COM_CLK_SELECT, 0x33),
	QMP_PHY_INIT_CFG(QSERDES_COM_SYS_CLK_CTRL, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_COM_SYSCLK_BUF_ENABLE, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_COM_SYSCLK_EN_SEL, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_COM_CP_CTRL_MODE0, 0x0b),
	QMP_PHY_INIT_CFG(QSERDES_COM_PLL_RCTRL_MODE0, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_COM_PLL_CCTRL_MODE0, 0x28),
	QMP_PHY_INIT_CFG(QSERDES_COM_INTEGLOOP_GAIN1_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_INTEGLOOP_GAIN0_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_EN_CENTER, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_PER1, 0x31),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_PER2, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_ADJ_PER1, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_ADJ_PER2, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_STEP_SIZE1, 0x2f),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_STEP_SIZE2, 0x19),
	QMP_PHY_INIT_CFG(QSERDES_COM_RESCODE_DIV_NUM, 0x15),
	QMP_PHY_INIT_CFG(QSERDES_COM_BG_TRIM, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_COM_PLL_IVCO, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_COM_CLK_EP_DIV, 0x19),
	QMP_PHY_INIT_CFG(QSERDES_COM_CLK_ENABLE1, 0x10),
	QMP_PHY_INIT_CFG(QSERDES_COM_HSCLK_SEL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_RESCODE_DIV_NUM, 0x40),
};

static const struct qmp_phy_init_tbl pciephy_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_TX_HIGHZ_TRANSCEIVEREN_BIAS_DRVR_EN, 0x45),
	QMP_PHY_INIT_CFG(QSERDES_TX_LANE_MODE, 0x06),
};

static const struct qmp_phy_init_tbl pciephy_rx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_RX_SIGDET_ENABLES, 0x1c),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQU_ADAPTOR_CNTRL3, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQU_ADAPTOR_CNTRL4, 0xdb),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_BAND, 0x18),
	QMP_PHY_INIT_CFG(QSERDES_RX_UCDR_SO_GAIN, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_RX_UCDR_SO_GAIN_HALF, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE, 0x4b),
	QMP_PHY_INIT_CFG(QSERDES_RX_SIGDET_DEGLITCH_CNTRL, 0x14),
	QMP_PHY_INIT_CFG(QSERDES_RX_SIGDET_LVL, 0x19),
};

static const struct qmp_phy_init_tbl pciephy_pcs_tbl[] = {
	QMP_PHY_INIT_CFG(QPHY_RX_IDLE_DTCT_CNTRL, 0x4c),
	QMP_PHY_INIT_CFG(QPHY_PWRUP_RESET_DLY_TIME_AUXCLK, 0x00),
	QMP_PHY_INIT_CFG(QPHY_LP_WAKEUP_DLY_TIME_AUXCLK, 0x01),

	QMP_PHY_INIT_CFG_L(QPHY_PLL_LOCK_CHK_DLY_TIME, 0x05),

	QMP_PHY_INIT_CFG(QPHY_ENDPOINT_REFCLK_DRIVE, 0x05),
	QMP_PHY_INIT_CFG(QPHY_POWER_DOWN_CONTROL, 0x02),
	QMP_PHY_INIT_CFG(QPHY_POWER_STATE_CONFIG4, 0x00),
	QMP_PHY_INIT_CFG(QPHY_POWER_STATE_CONFIG1, 0xa3),
	QMP_PHY_INIT_CFG(QPHY_TXDEEMPH_M3P5DB_V0, 0x0e),
};

static const struct qmp_phy_init_tbl usb3phy_serdes_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_COM_SYSCLK_EN_SEL, 0x14),
	QMP_PHY_INIT_CFG(QSERDES_COM_BIAS_EN_CLKBUFLR_EN, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_COM_CLK_SELECT, 0x30),
	QMP_PHY_INIT_CFG(QSERDES_COM_CMN_CONFIG, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_COM_SVS_MODE_CLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_HSCLK_SEL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_BG_TRIM, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_COM_PLL_IVCO, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_COM_SYS_CLK_CTRL, 0x04),
	/* PLL and Loop filter settings */
	QMP_PHY_INIT_CFG(QSERDES_COM_DEC_START_MODE0, 0x82),
	QMP_PHY_INIT_CFG(QSERDES_COM_DIV_FRAC_START1_MODE0, 0x55),
	QMP_PHY_INIT_CFG(QSERDES_COM_DIV_FRAC_START2_MODE0, 0x55),
	QMP_PHY_INIT_CFG(QSERDES_COM_DIV_FRAC_START3_MODE0, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_COM_CP_CTRL_MODE0, 0x0b),
	QMP_PHY_INIT_CFG(QSERDES_COM_PLL_RCTRL_MODE0, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_COM_PLL_CCTRL_MODE0, 0x28),
	QMP_PHY_INIT_CFG(QSERDES_COM_INTEGLOOP_GAIN0_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_COM_VCO_TUNE_CTRL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP1_MODE0, 0x15),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP2_MODE0, 0x34),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP3_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_CORE_CLK_EN, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_LOCK_CMP_CFG, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_VCO_TUNE_MAP, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_BG_TIMER, 0x0a),
	/* SSC settings */
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_EN_CENTER, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_PER1, 0x31),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_PER2, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_ADJ_PER1, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_ADJ_PER2, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_STEP_SIZE1, 0xde),
	QMP_PHY_INIT_CFG(QSERDES_COM_SSC_STEP_SIZE2, 0x07),
};

static const struct qmp_phy_init_tbl usb3phy_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_TX_HIGHZ_TRANSCEIVEREN_BIAS_DRVR_EN, 0x45),
	QMP_PHY_INIT_CFG(QSERDES_TX_RCV_DETECT_LVL_2, 0x12),
	QMP_PHY_INIT_CFG(QSERDES_TX_LANE_MODE, 0x06),
};

static const struct qmp_phy_init_tbl usb3phy_rx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_RX_UCDR_FASTLOCK_FO_GAIN, 0x0b),
	QMP_PHY_INIT_CFG(QSERDES_RX_UCDR_SO_GAIN, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQU_ADAPTOR_CNTRL3, 0x4c),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQU_ADAPTOR_CNTRL4, 0xbb),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1, 0x77),
	QMP_PHY_INIT_CFG(QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_RX_SIGDET_CNTRL, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_RX_SIGDET_LVL, 0x18),
	QMP_PHY_INIT_CFG(QSERDES_RX_SIGDET_DEGLITCH_CNTRL, 0x16),
};

static const struct qmp_phy_init_tbl usb3phy_pcs_tbl[] = {
	/* FLL settings */
	QMP_PHY_INIT_CFG_L(QPHY_FLL_CNTRL2, 0x03),
	QMP_PHY_INIT_CFG_L(QPHY_FLL_CNTRL1, 0x02),
	QMP_PHY_INIT_CFG_L(QPHY_FLL_CNT_VAL_L, 0x09),
	QMP_PHY_INIT_CFG_L(QPHY_FLL_CNT_VAL_H_TOL, 0x42),
	QMP_PHY_INIT_CFG_L(QPHY_FLL_MAN_CODE, 0x85),

	/* Lock Det settings */
	QMP_PHY_INIT_CFG(QPHY_LOCK_DETECT_CONFIG1, 0xd1),
	QMP_PHY_INIT_CFG(QPHY_LOCK_DETECT_CONFIG2, 0x1f),
	QMP_PHY_INIT_CFG(QPHY_LOCK_DETECT_CONFIG3, 0x47),
	QMP_PHY_INIT_CFG(QPHY_POWER_STATE_CONFIG2, 0x08),
};

/* struct qmp_phy_cfg - per-PHY initialization config */
struct qmp_phy_cfg {
	/* phy-type - PCIE/UFS/USB */
	unsigned int type;
	/* number of lanes provided by phy */
	int nlanes;

	/* Init sequence for PHY blocks - serdes, tx, rx, pcs */
	const struct qmp_phy_init_tbl *serdes_tbl;
	int serdes_tbl_num;
	const struct qmp_phy_init_tbl *tx_tbl;
	int tx_tbl_num;
	const struct qmp_phy_init_tbl *rx_tbl;
	int rx_tbl_num;
	const struct qmp_phy_init_tbl *pcs_tbl;
	int pcs_tbl_num;

	/* array of registers with different offsets */
	const unsigned int *regs;

	unsigned int start_ctrl;
	unsigned int pwrdn_ctrl;
	unsigned int mask_pcs_ready;
	unsigned int mask_com_pcs_ready;

	/* true, if PHY has a separate PHY_COM control block */
	bool has_phy_com_ctrl;
	/* true, if PHY has a reset for individual lanes */
	bool has_lane_rst;
	/* true, if PHY needs delay after POWER_DOWN */
	bool has_pwrdn_delay;
	/* power_down delay in usec */
	int pwrdn_delay_min;
	int pwrdn_delay_max;
};

/**
 * struct qmp_phy - per-lane phy descriptor
 *
 * @phy: generic phy
 * @tx: iomapped memory space for lane's tx
 * @rx: iomapped memory space for lane's rx
 * @pcs: iomapped memory space for lane's pcs
 * @pipe_clk: pipe lock
 * @index: lane index
 * @qmp: QMP phy to which this lane belongs
 * @lane_rst: lane's reset controller
 */
struct qmp_phy {
	struct phy *phy;
	void __iomem *tx;
	void __iomem *rx;
	void __iomem *pcs;
	struct clk *pipe_clk;
	unsigned int index;
	struct qcom_qmp *qmp;
	struct reset_control *lane_rst;
};

/**
 * struct qcom_qmp - structure holding QMP phy block attributes
 *
 * @dev: device
 * @serdes: iomapped memory space for phy's serdes
 *
 * @aux_clk: phy core clock
 * @cfg_ahb_clk: AHB2PHY interface clock
 * @ref_clk: reference clock
 *
 * @vdda_phy: vdd supply to the phy core block
 * @vdda_pll: 1.8V vdd supply to ref_clk block
 * @vddp_ref_clk: vdd supply to specific ref_clk block (Optional)
 *
 * @phy_rst: phy reset control
 * @phycom_rst: phy common reset control
 * @phycfg_rst: phy ahb cfg reset control (Optional)
 *
 * @cfg: phy specific configuration
 * @phys: array of per-lane phy descriptors
 * @phy_mutex: mutex lock for PHY common block initialization
 * @init_count: phy common block initialization count
 */
struct qcom_qmp {
	struct device *dev;
	void __iomem *serdes;

	struct clk *aux_clk;
	struct clk *cfg_ahb_clk;
	struct clk *ref_clk;

	struct regulator *vdda_phy;
	struct regulator *vdda_pll;
	struct regulator *vddp_ref_clk;

	struct reset_control *phy_rst;
	struct reset_control *phycom_rst;
	struct reset_control *phycfg_rst;

	const struct qmp_phy_cfg *cfg;
	struct qmp_phy **phys;

	struct mutex phy_mutex;
	int init_count;
};

static inline void qphy_setbits(void __iomem *base, u32 offset, u32 val)
{
	u32 reg;

	reg = readl_relaxed(base + offset);
	reg |= val;
	writel_relaxed(reg, base + offset);

	/* Make sure that above writes are completed */
	mb();
}

static inline void qphy_clrbits(void __iomem *base, u32 offset, u32 val)
{
	u32 reg;

	reg = readl_relaxed(base + offset);
	reg &= ~val;
	writel_relaxed(reg, base + offset);

	/* Make sure that above writes are completed */
	mb();
}

static const struct qmp_phy_cfg msm8996_pciephy_cfg = {
	.type			= PHY_TYPE_PCIE,
	.nlanes			= 3,

	.serdes_tbl		= pciephy_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(pciephy_serdes_tbl),
	.tx_tbl			= pciephy_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(pciephy_tx_tbl),
	.rx_tbl			= pciephy_rx_tbl,
	.rx_tbl_num		= ARRAY_SIZE(pciephy_rx_tbl),
	.pcs_tbl		= pciephy_pcs_tbl,
	.pcs_tbl_num		= ARRAY_SIZE(pciephy_pcs_tbl),
	.regs			= pciephy_regs_layout,
	.start_ctrl		= PCS_START | PLL_READY_GATE_EN,
	.pwrdn_ctrl		= SW_PWRDN | REFCLK_DRV_DSBL,
	.mask_com_pcs_ready	= PCS_READY,

	.has_phy_com_ctrl	= true,
	.has_lane_rst		= true,
	.has_pwrdn_delay	= true,
	.pwrdn_delay_min	= POWER_DOWN_DELAY_US_MIN,
	.pwrdn_delay_max	= POWER_DOWN_DELAY_US_MAX,
};

static const struct qmp_phy_cfg msm8996_usb3phy_cfg = {
	.type			= PHY_TYPE_USB3,
	.nlanes			= 1,

	.serdes_tbl		= usb3phy_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(usb3phy_serdes_tbl),
	.tx_tbl			= usb3phy_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(usb3phy_tx_tbl),
	.rx_tbl			= usb3phy_rx_tbl,
	.rx_tbl_num		= ARRAY_SIZE(usb3phy_rx_tbl),
	.pcs_tbl		= usb3phy_pcs_tbl,
	.pcs_tbl_num		= ARRAY_SIZE(usb3phy_pcs_tbl),
	.regs			= usb3phy_regs_layout,
	.start_ctrl		= SERDES_START | PCS_START,
	.pwrdn_ctrl		= SW_PWRDN,
	.mask_pcs_ready		= PHYSTATUS,
};

static void qcom_qmp_phy_configure(void __iomem *base,
				const unsigned int *regs,
				const struct qmp_phy_init_tbl tbl[],
				int num)
{
	int i;
	const struct qmp_phy_init_tbl *t = tbl;

	for (i = 0; i < num; i++, t++) {
		if (t->in_layout)
			writel_relaxed(t->val, base + regs[t->offset]);
		else
			writel_relaxed(t->val, base + t->offset);
	}

	/* flush buffered writes */
	mb();
}

static int qcom_qmp_phy_poweron(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	struct qcom_qmp *qmp = qphy->qmp;
	int ret;

	dev_vdbg(&phy->dev, "Powering on QMP phy\n");

	ret = regulator_enable(qmp->vdda_phy);
	if (ret) {
		dev_err(qmp->dev, "%s: vdda-phy enable failed, err=%d\n",
				__func__, ret);
		return ret;
	}

	ret = regulator_enable(qmp->vdda_pll);
	if (ret) {
		dev_err(qmp->dev, "%s: vdda-pll enable failed, err=%d\n",
				__func__, ret);
		goto disable_vdda_phy;
	}

	ret = regulator_enable(qmp->vddp_ref_clk);
	if (ret) {
		dev_err(qmp->dev,
			"%s: vdda-ref-clk enable failed, err=%d\n",
			__func__, ret);
		goto disable_vdda_pll;
	}

	ret = clk_prepare_enable(qmp->ref_clk);
	if (ret) {
		dev_err(qmp->dev, "%s: ref_clk enable failed, err=%d\n",
				__func__, ret);
		goto disable_vddp_ref_clk;
	}

	ret = clk_prepare_enable(qphy->pipe_clk);
	if (ret) {
		dev_err(qmp->dev, "%s: pipe_clk enable failed, err=%d\n",
				__func__, ret);
		goto disable_ref_clk;
	}

	return 0;

disable_ref_clk:
	clk_disable_unprepare(qmp->ref_clk);
disable_vddp_ref_clk:
	regulator_disable(qmp->vddp_ref_clk);
disable_vdda_pll:
	regulator_disable(qmp->vdda_pll);
disable_vdda_phy:
	regulator_disable(qmp->vdda_phy);
	return ret;
}

static int qcom_qmp_phy_poweroff(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	struct qcom_qmp *qmp = qphy->qmp;

	clk_disable_unprepare(qphy->pipe_clk);
	clk_disable_unprepare(qmp->ref_clk);

	regulator_disable(qmp->vddp_ref_clk);
	regulator_disable(qmp->vdda_pll);
	regulator_disable(qmp->vdda_phy);

	return 0;
}

static int qcom_qmp_phy_com_init(struct qcom_qmp *qmp)
{
	const struct qmp_phy_cfg *cfg = qmp->cfg;
	void __iomem *serdes = qmp->serdes;
	int ret;

	mutex_lock(&qmp->phy_mutex);
	if (qmp->init_count++) {
		mutex_unlock(&qmp->phy_mutex);
		return 0;
	}

	ret = reset_control_deassert(qmp->phy_rst);
	if (ret) {
		dev_err(qmp->dev, "phy reset deassert failed\n");
		goto err_rst;
	}

	ret = reset_control_deassert(qmp->phycom_rst);
	if (ret) {
		dev_err(qmp->dev, "common reset deassert failed\n");
		goto err_com_rst;
	}

	if (qmp->phycfg_rst) {
		ret = reset_control_deassert(qmp->phycfg_rst);
		if (ret) {
			dev_err(qmp->dev, "ahb cfg reset deassert failed\n");
			goto err_cfg_rst;
		}
	}

	if (cfg->has_phy_com_ctrl)
		qphy_setbits(serdes, cfg->regs[QPHY_COM_POWER_DOWN_CONTROL],
				SW_PWRDN);

	/* Serdes configuration */
	qcom_qmp_phy_configure(serdes, cfg->regs, cfg->serdes_tbl,
				cfg->serdes_tbl_num);

	if (cfg->has_phy_com_ctrl) {
		void __iomem *status;
		unsigned int mask, val;

		qphy_clrbits(serdes, cfg->regs[QPHY_COM_SW_RESET],
				SW_RESET);
		qphy_setbits(serdes, cfg->regs[QPHY_COM_START_CONTROL],
				SERDES_START | PCS_START);

		status = serdes + cfg->regs[QPHY_COM_PCS_READY_STATUS];
		mask = cfg->mask_com_pcs_ready;

		ret = readl_poll_timeout(status, val, (val & mask), 10,
						PHY_INIT_COMPLETE_TIMEOUT);
		if (ret) {
			dev_err(qmp->dev,
				"phy common block init timed-out\n");
			goto err_com_init;
		}
	}

	mutex_unlock(&qmp->phy_mutex);

	return 0;

err_com_init:
	if (qmp->phycfg_rst)
		reset_control_assert(qmp->phycfg_rst);
err_cfg_rst:
	reset_control_assert(qmp->phycom_rst);
err_com_rst:
	reset_control_assert(qmp->phy_rst);
err_rst:
	mutex_unlock(&qmp->phy_mutex);
	return ret;
}

static int qcom_qmp_phy_com_exit(struct qcom_qmp *qmp)
{
	const struct qmp_phy_cfg *cfg = qmp->cfg;
	void __iomem *serdes = qmp->serdes;

	mutex_lock(&qmp->phy_mutex);
	if (--qmp->init_count) {
		mutex_unlock(&qmp->phy_mutex);
		return 0;
	}

	if (cfg->has_phy_com_ctrl) {
		qphy_setbits(serdes, cfg->regs[QPHY_COM_START_CONTROL],
				SERDES_START | PCS_START);
		qphy_clrbits(serdes, cfg->regs[QPHY_COM_SW_RESET],
				SW_RESET);
		qphy_setbits(serdes, cfg->regs[QPHY_COM_POWER_DOWN_CONTROL],
				SW_PWRDN);
	}

	if (qmp->phycfg_rst)
		reset_control_assert(qmp->phycfg_rst);

	reset_control_assert(qmp->phycom_rst);
	reset_control_assert(qmp->phy_rst);

	mutex_unlock(&qmp->phy_mutex);

	return 0;
}

/* PHY Initialization */
static int qcom_qmp_phy_init(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	struct qcom_qmp *qmp = qphy->qmp;
	const struct qmp_phy_cfg *cfg = qmp->cfg;
	void __iomem *tx = qphy->tx;
	void __iomem *rx = qphy->rx;
	void __iomem *pcs = qphy->pcs;
	void __iomem *status;
	unsigned int mask, val;
	int ret;

	dev_vdbg(qmp->dev, "Initializing QMP phy\n");

	/* enable interface clocks to program phy */
	ret = clk_prepare_enable(qmp->aux_clk);
	if (ret) {
		dev_err(qmp->dev, "failed to enable aux clk, err=%d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(qmp->cfg_ahb_clk);
	if (ret) {
		dev_err(qmp->dev, "failed to enable cfg ahb clk, err=%d\n",
									ret);
		goto err;
	}

	ret = qcom_qmp_phy_com_init(qmp);
	if (ret)
		goto err_com_init;

	if (cfg->has_lane_rst) {
		ret = reset_control_deassert(qphy->lane_rst);
		if (ret) {
			dev_err(qmp->dev, "lane%d reset deassert failed\n",
					qphy->index);
			goto err_lane_rst;
		}
	}

	/* Tx, Rx, and PCS configurations */
	qcom_qmp_phy_configure(tx, cfg->regs, cfg->tx_tbl, cfg->tx_tbl_num);
	qcom_qmp_phy_configure(rx, cfg->regs, cfg->rx_tbl, cfg->rx_tbl_num);
	qcom_qmp_phy_configure(pcs, cfg->regs, cfg->pcs_tbl, cfg->pcs_tbl_num);

	/*
	 * Pull out PHY from POWER DOWN state.
	 * This is active low enable signal to power-down PHY.
	 */
	qphy_setbits(pcs, QPHY_POWER_DOWN_CONTROL, cfg->pwrdn_ctrl);

	if (cfg->has_pwrdn_delay)
		usleep_range(cfg->pwrdn_delay_min, cfg->pwrdn_delay_max);

	/* start SerDes and Phy-Coding-Sublayer */
	qphy_setbits(pcs, QPHY_START_CTRL, cfg->start_ctrl);

	/* Pull PHY out of reset state */
	qphy_clrbits(pcs, QPHY_SW_RESET, SW_RESET);

	status = pcs + cfg->regs[QPHY_PCS_READY_STATUS];
	mask = cfg->mask_pcs_ready;

	ret = readl_poll_timeout(status, val, !(val & mask), 1,
					PHY_INIT_COMPLETE_TIMEOUT);
	if (ret) {
		dev_err(qmp->dev, "phy initialization timed-out\n");
		goto err_pcs_ready;
	}

	return ret;

err_pcs_ready:
	if (cfg->has_lane_rst)
		reset_control_assert(qphy->lane_rst);
err_lane_rst:
	qcom_qmp_phy_com_exit(qmp);
err_com_init:
	clk_disable_unprepare(qmp->cfg_ahb_clk);
err:
	clk_disable_unprepare(qmp->aux_clk);
	return ret;
}

static int qcom_qmp_phy_exit(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	struct qcom_qmp *qmp = qphy->qmp;
	const struct qmp_phy_cfg *cfg = qmp->cfg;

	/* PHY reset */
	qphy_setbits(qphy->pcs, QPHY_SW_RESET, SW_RESET);

	/* stop SerDes and Phy-Coding-Sublayer */
	qphy_clrbits(qphy->pcs, QPHY_START_CTRL, cfg->start_ctrl);

	/* Put PHY into POWER DOWN state: active low */
	qphy_clrbits(qphy->pcs, QPHY_POWER_DOWN_CONTROL,
			cfg->pwrdn_ctrl);

	if (cfg->has_lane_rst)
		reset_control_assert(qphy->lane_rst);

	qcom_qmp_phy_com_exit(qmp);

	clk_disable_unprepare(qmp->aux_clk);
	clk_disable_unprepare(qmp->cfg_ahb_clk);

	return 0;
}

static int qcom_qmp_phy_regulator_init(struct device *dev)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	int ret;

	qmp->vdda_phy = devm_regulator_get(dev, "vdda-phy");
	if (IS_ERR(qmp->vdda_phy)) {
		ret = PTR_ERR(qmp->vdda_phy);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get vdda-phy, %d\n", ret);
		return ret;
	}

	qmp->vdda_pll = devm_regulator_get(dev, "vdda-pll");
	if (IS_ERR(qmp->vdda_pll)) {
		ret = PTR_ERR(qmp->vdda_pll);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get vdda-pll, %d\n", ret);
		return ret;
	}

	/* optional regulator */
	qmp->vddp_ref_clk = devm_regulator_get(dev, "vddp-ref-clk");
	if (IS_ERR(qmp->vddp_ref_clk)) {
		if (PTR_ERR(qmp->vddp_ref_clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_dbg(dev, "failed to get vddp-ref-clk, %d\n", ret);
	}

	return 0;
}

static int qcom_qmp_phy_clk_init(struct device *dev)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	int ret;

	qmp->aux_clk = devm_clk_get(dev, "aux");
	if (IS_ERR(qmp->aux_clk)) {
		ret = PTR_ERR(qmp->aux_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get aux_clk, %d\n", ret);
		return ret;
	}

	qmp->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb");
	if (IS_ERR(qmp->cfg_ahb_clk)) {
		ret = PTR_ERR(qmp->cfg_ahb_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get cfg_ahb_clk, %d\n", ret);
		return ret;
	}

	qmp->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(qmp->ref_clk)) {
		ret = PTR_ERR(qmp->ref_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get ref_clk, %d\n", ret);
		return ret;
	}

	return 0;
}

static struct phy *qcom_qmp_phy_xlate(struct device *dev,
					struct of_phandle_args *args)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	int i;

	if (WARN_ON(args->args_count <= 0))
		return ERR_PTR(-EINVAL);
	if (WARN_ON(args->args[0] >= qmp->cfg->nlanes))
		return ERR_PTR(-EINVAL);

	i = args->args[0];

	return qmp->phys[i]->phy;
}

/*
 * Register a fixed rate pipe clock.
 *
 * The <s>_pipe_clksrc generated by PHY goes to the GCC that gate
 * controls it. The <s>_pipe_clk coming out of the GCC is requested
 * by the PHY driver for its operations.
 * We register the <s>_pipe_clksrc here. The gcc driver takes care
 * of assigning this <s>_pipe_clksrc as parent to <s>_pipe_clk.
 * Below picture shows this relationship.
 *
 *         +---------------+
 *         |   PHY block   |<<---------------------------------------+
 *         |               |                                         |
 *         |   +-------+   |                   +-----+               |
 *   I/P---^-->|  PLL  |---^--->pipe_clksrc--->| GCC |--->pipe_clk---+
 *    clk  |   +-------+   |                   +-----+
 *         +---------------+
 */
static int phy_pipe_clk_register(struct qcom_qmp *qmp, int id)
{
	char name[24];
	struct clk_fixed_rate *fixed;
	struct clk_init_data init = { };
	int ret;

	switch (qmp->cfg->type) {
	case PHY_TYPE_USB3:
		snprintf(name, sizeof(name), "usb3_phy_pipe_clk_src");
		break;
	case PHY_TYPE_PCIE:
		snprintf(name, sizeof(name), "pcie_%d_pipe_clk_src", id);
		break;
	default:
		return -EINVAL;
	}

	fixed = devm_kzalloc(qmp->dev, sizeof(*fixed), GFP_KERNEL);
	if (!fixed)
		return -ENOMEM;

	init.name = name;
	init.ops = &clk_fixed_rate_ops;

	/* controllers using QMP phys use 125MHz pipe clock interface */
	fixed->fixed_rate = 125000000;
	fixed->hw.init = &init;

	ret = devm_clk_hw_register(qmp->dev, &fixed->hw);

	return ret;
}

static const struct phy_ops qcom_qmp_phy_gen_ops = {
	.init		= qcom_qmp_phy_init,
	.exit		= qcom_qmp_phy_exit,
	.power_on	= qcom_qmp_phy_poweron,
	.power_off	= qcom_qmp_phy_poweroff,
	.owner		= THIS_MODULE,
};

static
struct qmp_phy *qcom_qmp_phy_create(struct platform_device *pdev, int id)
{
	struct device *dev = &pdev->dev;
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	struct phy *generic_phy;
	struct qmp_phy *qphy;
	struct resource *res;
	void __iomem *base;
	char prop_name[MAX_PROP_NAME];
	unsigned int lane_offsets[3];
	int ret;

	/* mem resources from index 1 to N for N number of lanes */
	res = platform_get_resource(pdev, IORESOURCE_MEM, id + 1);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return ERR_CAST(base);

	qphy = devm_kzalloc(dev, sizeof(*qphy), GFP_KERNEL);
	if (!qphy)
		return ERR_PTR(-ENOMEM);

	/*
	 * read offsets to Tx, Rx, and PCS blocks into a u32 array:
	 * +------------------------------------+
	 * | tx offset | rx offset | pcs offset |
	 * +------------------------------------+
	 */
	ret = of_property_read_u32_array(dev->of_node, "lane-offsets",
						   lane_offsets, 3);
	if (ret) {
		dev_err(dev, "failed to get tx/rx/pcs offsets for lane%d\n",
			id);
		return ERR_PTR(ret);
	}

	qphy->tx = base + lane_offsets[0];
	qphy->rx = base + lane_offsets[1];
	qphy->pcs = base + lane_offsets[2];

	/*
	 * Get PHY's Pipe clock, if any; USB3 and PCIe are PIPE3
	 * based phys, so they essentially have pipe clock. So,
	 * we return error in case phy is USB3 or PIPE type.
	 * Otherwise, we initialize pipe clock to NULL for
	 * all phys that don't need this.
	 */
	snprintf(prop_name, sizeof(prop_name), "pipe%d", id);
	qphy->pipe_clk = devm_clk_get(dev, prop_name);
	if (IS_ERR(qphy->pipe_clk)) {
		if (qmp->cfg->type == PHY_TYPE_PCIE ||
		    qmp->cfg->type == PHY_TYPE_USB3) {
			ret = PTR_ERR(qphy->pipe_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
					"failed to get lane%d pipe_clk, %d\n",
					id, ret);
			return ERR_PTR(ret);
		}
		qphy->pipe_clk = NULL;
	}

	/* Get lane reset, if any */
	if (qmp->cfg->has_lane_rst) {
		snprintf(prop_name, sizeof(prop_name), "lane%d", id);
		qphy->lane_rst = devm_reset_control_get(dev,
							    prop_name);
		if (IS_ERR(qphy->lane_rst)) {
			dev_err(dev, "failed to get lane%d reset\n", id);
			return ERR_CAST(qphy->lane_rst);
		}
	}

	generic_phy = devm_phy_create(dev, NULL, &qcom_qmp_phy_gen_ops);
	if (IS_ERR(generic_phy)) {
		ret = PTR_ERR(generic_phy);
		dev_err(dev, "failed to create qphy %d\n", ret);
		return ERR_PTR(ret);
	}

	qphy->phy = generic_phy;
	qphy->index = id;
	qphy->qmp = qmp;
	phy_set_drvdata(generic_phy, qphy);

	return qphy;
}

static const struct of_device_id qcom_qmp_phy_of_match_table[] = {
	{
		.compatible = "qcom,msm8996-qmp-pcie-phy",
		.data = &msm8996_pciephy_cfg,
	}, {
		.compatible = "qcom,msm8996-qmp-usb3-phy",
		.data = &msm8996_usb3phy_cfg,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_qmp_phy_of_match_table);

static int qcom_qmp_phy_probe(struct platform_device *pdev)
{
	struct qcom_qmp *qmp;
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	struct resource *res;
	void __iomem *base;
	int ret;
	int id;

	qmp = devm_kzalloc(dev, sizeof(*qmp), GFP_KERNEL);
	if (!qmp)
		return -ENOMEM;

	qmp->dev = dev;
	dev_set_drvdata(dev, qmp);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* per PHY serdes; usually located at base address */
	qmp->serdes = base;

	mutex_init(&qmp->phy_mutex);

	/* Get the specific init parameters of QMP phy */
	qmp->cfg = of_device_get_match_data(dev);

	ret = qcom_qmp_phy_clk_init(dev);
	if (ret)
		return ret;

	ret = qcom_qmp_phy_regulator_init(dev);
	if (ret)
		return ret;

	qmp->phy_rst = devm_reset_control_get(dev, "phy");
	if (IS_ERR(qmp->phy_rst)) {
		dev_err(dev, "failed to get phy core reset\n");
		return PTR_ERR(qmp->phy_rst);
	}

	qmp->phycom_rst = devm_reset_control_get(dev, "common");
	if (IS_ERR(qmp->phycom_rst)) {
		dev_err(dev, "failed to get phy common reset\n");
		return PTR_ERR(qmp->phycom_rst);
	}

	qmp->phycfg_rst = devm_reset_control_get(dev, "cfg");
	if (IS_ERR(qmp->phycfg_rst)) {
		dev_dbg(dev, "failed to get phy ahb cfg reset\n");
		qmp->phycfg_rst = NULL;
	}


	qmp->phys = devm_kcalloc(dev, qmp->cfg->nlanes,
					sizeof(*qmp->phys), GFP_KERNEL);
	if (!qmp->phys)
		return -ENOMEM;

	for (id = 0; id < qmp->cfg->nlanes; id++) {
		/* Create per-lane phy */
		qmp->phys[id] = qcom_qmp_phy_create(pdev, id);
		if (IS_ERR(qmp->phys[id])) {
			dev_err(dev, "failed to create lane%d phy, %d\n",
				id, ret);
			return PTR_ERR(qmp->phys[id]);
		}

		/*
		 * Register the pipe clock provided by phy.
		 * See function description to see details of this pipe clock.
		 */
		ret = phy_pipe_clk_register(qmp, id);
		if (ret) {
			dev_err(qmp->dev,
				"failed to register pipe clock source\n");
			return ret;
		}
	}

	phy_provider = devm_of_phy_provider_register(dev, qcom_qmp_phy_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		dev_err(dev, "failed to register qphy, %d\n", ret);
	}

	return ret;
}

static struct platform_driver qcom_qmp_phy_driver = {
	.probe		= qcom_qmp_phy_probe,
	.driver = {
		.name	= "qcom-qmp-phy",
		.of_match_table = of_match_ptr(qcom_qmp_phy_of_match_table),
	},
};

module_platform_driver(qcom_qmp_phy_driver);

MODULE_AUTHOR("Vivek Gautam <vivek.gautam@codeaurora.org>");
MODULE_DESCRIPTION("Qualcomm QMP PHY driver");
MODULE_LICENSE("GPL v2");
