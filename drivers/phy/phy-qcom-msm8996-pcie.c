/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
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

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/phy/phy.h>

#define QSERDES_COM_BG_TIMER			0x00C
#define QSERDES_COM_SSC_EN_CENTER		0x010
#define QSERDES_COM_SSC_ADJ_PER1		0x014
#define QSERDES_COM_SSC_ADJ_PER2		0x018
#define QSERDES_COM_SSC_PER1			0x01C
#define QSERDES_COM_SSC_PER2			0x020
#define QSERDES_COM_SSC_STEP_SIZE1		0x024
#define QSERDES_COM_SSC_STEP_SIZE2		0x028
#define QSERDES_COM_BIAS_EN_CLKBUFLR_EN		0x034
#define QSERDES_COM_CLK_ENABLE1			0x038
#define QSERDES_COM_SYS_CLK_CTRL		0x03C
#define QSERDES_COM_SYSCLK_BUF_ENABLE		0x040
#define QSERDES_COM_PLL_IVCO			0x048
#define QSERDES_COM_LOCK_CMP1_MODE0		0x04C
#define QSERDES_COM_LOCK_CMP2_MODE0		0x050
#define QSERDES_COM_LOCK_CMP3_MODE0		0x054
#define QSERDES_COM_BG_TRIM			0x070
#define QSERDES_COM_CLK_EP_DIV			0x074
#define QSERDES_COM_CP_CTRL_MODE0		0x078
#define QSERDES_COM_PLL_RCTRL_MODE0		0x084
#define QSERDES_COM_PLL_CCTRL_MODE0		0x090
#define QSERDES_COM_SYSCLK_EN_SEL		0x0AC
#define QSERDES_COM_RESETSM_CNTRL		0x0B4
#define QSERDES_COM_RESTRIM_CTRL		0x0BC
#define QSERDES_COM_RESCODE_DIV_NUM		0x0C4
#define QSERDES_COM_LOCK_CMP_EN			0x0C8
#define QSERDES_COM_DEC_START_MODE0		0x0D0
#define QSERDES_COM_DIV_FRAC_START1_MODE0	0x0DC
#define QSERDES_COM_DIV_FRAC_START2_MODE0	0x0E0
#define QSERDES_COM_DIV_FRAC_START3_MODE0	0x0E4
#define QSERDES_COM_INTEGLOOP_GAIN0_MODE0	0x108
#define QSERDES_COM_INTEGLOOP_GAIN1_MODE0	0x10C
#define QSERDES_COM_VCO_TUNE_CTRL		0x124
#define QSERDES_COM_VCO_TUNE_MAP		0x128
#define QSERDES_COM_VCO_TUNE1_MODE0		0x12C
#define QSERDES_COM_VCO_TUNE2_MODE0		0x130
#define QSERDES_COM_VCO_TUNE_TIMER1		0x144
#define QSERDES_COM_VCO_TUNE_TIMER2		0x148
#define QSERDES_COM_BG_CTRL			0x170
#define QSERDES_COM_CLK_SELECT			0x174
#define QSERDES_COM_HSCLK_SEL			0x178
#define QSERDES_COM_CORECLK_DIV			0x184
#define QSERDES_COM_CORE_CLK_EN			0x18C
#define QSERDES_COM_C_READY_STATUS		0x190
#define QSERDES_COM_CMN_CONFIG			0x194
#define QSERDES_COM_SVS_MODE_CLK_SEL		0x19C
#define QSERDES_COM_DEBUG_BUS0			0x1A0
#define QSERDES_COM_DEBUG_BUS1			0x1A4
#define QSERDES_COM_DEBUG_BUS2			0x1A8
#define QSERDES_COM_DEBUG_BUS3			0x1AC
#define QSERDES_COM_DEBUG_BUS_SEL		0x1B0

#define PCIE_N_SW_RESET(n)			(PCS_PORT(n) + 0x00)
#define PCIE_N_POWER_DOWN_CONTROL(n)		(PCS_PORT(n) + 0x04)
#define PCIE_N_START_CONTROL(n)		(PCS_PORT(n) + 0x08)
#define PCIE_N_TXDEEMPH_M6DB_V0(n)		(PCS_PORT(n) + 0x24)
#define PCIE_N_TXDEEMPH_M3P5DB_V0(n)		(PCS_PORT(n) + 0x28)
#define PCIE_N_ENDPOINT_REFCLK_DRIVE(n)	(PCS_PORT(n) + 0x54)
#define PCIE_N_RX_IDLE_DTCT_CNTRL(n)		(PCS_PORT(n) + 0x58)
#define PCIE_N_POWER_STATE_CONFIG1(n)	(PCS_PORT(n) + 0x60)
#define PCIE_N_POWER_STATE_CONFIG4(n)	(PCS_PORT(n) + 0x6C)
#define PCIE_N_PWRUP_RESET_DLY_TIME_AUXCLK(n)	(PCS_PORT(n) + 0xA0)
#define PCIE_N_LP_WAKEUP_DLY_TIME_AUXCLK(n)	(PCS_PORT(n) + 0xA4)
#define PCIE_N_PLL_LOCK_CHK_DLY_TIME(n)	(PCS_PORT(n) + 0xA8)
#define PCIE_N_TEST_CONTROL4(n)		(PCS_PORT(n) + 0x11C)
#define PCIE_N_TEST_CONTROL5(n)		(PCS_PORT(n) + 0x120)
#define PCIE_N_TEST_CONTROL6(n)		(PCS_PORT(n) + 0x124)
#define PCIE_N_TEST_CONTROL7(n)		(PCS_PORT(n) + 0x128)
#define PCIE_N_PCS_STATUS(n)			(PCS_PORT(n) + 0x174)
#define PCIE_N_DEBUG_BUS_0_STATUS(n)		(PCS_PORT(n) + 0x198)
#define PCIE_N_DEBUG_BUS_1_STATUS(n)		(PCS_PORT(n) + 0x19C)
#define PCIE_N_DEBUG_BUS_2_STATUS(n)		(PCS_PORT(n) + 0x1A0)
#define PCIE_N_DEBUG_BUS_3_STATUS(n)		(PCS_PORT(n) + 0x1A4)
#define PCIE_N_LP_WAKEUP_DLY_TIME_AUXCLK_MSB(n)	(PCS_PORT(n) + 0x1A8)
#define PCIE_N_OSC_DTCT_ACTIONS(n)			(PCS_PORT(n) + 0x1AC)
#define PCIE_N_SIGDET_CNTRL(n)			(PCS_PORT(n) + 0x1B0)
#define PCIE_N_L1SS_WAKEUP_DLY_TIME_AUXCLK_LSB(n)	(PCS_PORT(n) + 0x1DC)
#define PCIE_N_L1SS_WAKEUP_DLY_TIME_AUXCLK_MSB(n)	(PCS_PORT(n) + 0x1E0)

#define PCIE_COM_SW_RESET		0x400
#define PCIE_COM_POWER_DOWN_CONTROL	0x404
#define PCIE_COM_START_CONTROL		0x408
#define PCIE_COM_DEBUG_BUS_BYTE0_INDEX	0x438
#define PCIE_COM_DEBUG_BUS_BYTE1_INDEX	0x43C
#define PCIE_COM_DEBUG_BUS_BYTE2_INDEX	0x440
#define PCIE_COM_DEBUG_BUS_BYTE3_INDEX	0x444
#define PCIE_COM_PCS_READY_STATUS	0x448
#define PCIE_COM_DEBUG_BUS_0_STATUS	0x45C
#define PCIE_COM_DEBUG_BUS_1_STATUS	0x460
#define PCIE_COM_DEBUG_BUS_2_STATUS	0x464
#define PCIE_COM_DEBUG_BUS_3_STATUS	0x468

#define TX_BASE 0x1000
#define RX_BASE 0x1200
#define PCS_BASE 0x1400

#define TX(n) (TX_BASE + n * 0x1000)
#define RX(n) (RX_BASE + n * 0x1000)
#define PCS_PORT(n) (PCS_BASE + n * 0x1000)

#define QSERDES_TX_N_RES_CODE_LANE_OFFSET(n)		(TX(n) + 0x4C)
#define QSERDES_TX_N_DEBUG_BUS_SEL(n)		(TX(n) + 0x64)
#define QSERDES_TX_N_HIGHZ_TRANSCEIVEREN_BIAS_DRVR_EN(n) (TX(n) + 0x68)
#define QSERDES_TX_N_LANE_MODE(n)			(TX(n) + 0x94)
#define QSERDES_TX_N_RCV_DETECT_LVL_2(n)		(TX(n) + 0xAC)

#define QSERDES_RX_N_UCDR_SO_GAIN_HALF(n)		(RX(n) + 0x010)
#define QSERDES_RX_N_UCDR_SO_GAIN(n)			(RX(n) + 0x01C)
#define QSERDES_RX_N_UCDR_SO_SATURATION_AND_ENABLE(n) (RX(n) + 0x048)
#define QSERDES_RX_N_RX_EQU_ADAPTOR_CNTRL2(n)	(RX(n) + 0x0D8)
#define QSERDES_RX_N_RX_EQU_ADAPTOR_CNTRL3(n)	(RX(n) + 0x0DC)
#define QSERDES_RX_N_RX_EQU_ADAPTOR_CNTRL4(n)	(RX(n) + 0x0E0)
#define QSERDES_RX_N_SIGDET_ENABLES(n)		(RX(n) + 0x110)
#define QSERDES_RX_N_SIGDET_DEGLITCH_CNTRL(n)	(RX(n) + 0x11C)
#define QSERDES_RX_N_SIGDET_LVL(n)			(RX(n) + 0x118)
#define QSERDES_RX_N_RX_BAND(n)			(RX(n) + 0x120)

#define REFCLK_STABILIZATION_DELAY_US_MIN     1000
#define REFCLK_STABILIZATION_DELAY_US_MAX     1005
#define PHY_READY_TIMEOUT_COUNT		   10
#define POWER_DOWN_DELAY_US_MIN		10
#define POWER_DOWN_DELAY_US_MAX		11

struct phy_msm8996_priv;

struct phy_msm8996_desc {
	struct phy	*phy;
	unsigned	index;
	struct	reset_control *phy_rstc;
	struct phy_msm8996_priv *priv;
};

struct phy_msm8996_priv {
	void __iomem *base;
	spinlock_t	lock;
	struct clk *cfg_clk;
	struct clk *aux_clk;
	struct	reset_control *phycom_rstc, *phycom2_rstc, *phycfg_rstc;
	struct device *dev;
	unsigned	nphys;
	int 		inited;
	struct phy_msm8996_desc	**phys;
};

static struct phy *phy_msm8996_pcie_phy_xlate(struct device *dev,
					     struct of_phandle_args *args)
{
	struct phy_msm8996_priv *priv = dev_get_drvdata(dev);
	int i;

	if (WARN_ON(args->args[0] >= priv->nphys))
		return ERR_PTR(-ENODEV);

	for (i = 0; i < priv->nphys; i++) {
		if (priv->phys[i]->index == args->args[0])
			break;
	}

	if (i == priv->nphys)
		return ERR_PTR(-ENODEV);

	return priv->phys[i]->phy;
}

static int pcie_phy_is_ready(struct phy *phy)
{
	struct phy_msm8996_desc *phydesc = phy_get_drvdata(phy);
	struct phy_msm8996_priv *priv = phydesc->priv;
	void __iomem *base = priv->base;
	int retries = 0;

	do {
//	if (!(readl_relaxed(dev->phy + PCIE_COM_PCS_READY_STATUS) & 0x1))
//		return false;
		if ((readl_relaxed(base + PCIE_COM_PCS_READY_STATUS) & 0x1))
			return 0;
		retries++;
		usleep_range(REFCLK_STABILIZATION_DELAY_US_MIN,
					 REFCLK_STABILIZATION_DELAY_US_MAX);
	} while (retries < PHY_READY_TIMEOUT_COUNT);

	return -EBUSY;
}

static int qcom_msm8996_phy_comm_init(struct phy *phy)
{
	struct phy_msm8996_desc *phydesc = phy_get_drvdata(phy);
	struct phy_msm8996_priv *priv = phydesc->priv;
	void __iomem *base = priv->base;

	if (priv->inited)
		return 0;

	pr_info("RC%d: Initializing 14nm QMP phy - 19.2MHz with Common Mode Clock (SSC ON)\n",
		phydesc->index);

	/* Prepare clocks */
	clk_prepare_enable(priv->cfg_clk);
	clk_prepare_enable(priv->aux_clk);

	reset_control_deassert(priv->phycom_rstc);
	reset_control_deassert(priv->phycom2_rstc);
	reset_control_deassert(priv->phycfg_rstc);

//	if (dev->common_phy)
	writel_relaxed(0x01, base + PCIE_COM_POWER_DOWN_CONTROL);

	writel_relaxed(0x1C, base + QSERDES_COM_BIAS_EN_CLKBUFLR_EN);
	writel_relaxed(0x10, base + QSERDES_COM_CLK_ENABLE1);
	writel_relaxed(0x33, base + QSERDES_COM_CLK_SELECT);
	writel_relaxed(0x06, base + QSERDES_COM_CMN_CONFIG);
	writel_relaxed(0x42, base + QSERDES_COM_LOCK_CMP_EN);
	writel_relaxed(0x00, base + QSERDES_COM_VCO_TUNE_MAP);
	writel_relaxed(0xFF, base + QSERDES_COM_VCO_TUNE_TIMER1);
	writel_relaxed(0x1F, base + QSERDES_COM_VCO_TUNE_TIMER2);
	writel_relaxed(0x01, base + QSERDES_COM_HSCLK_SEL);
	writel_relaxed(0x01, base + QSERDES_COM_SVS_MODE_CLK_SEL);
	writel_relaxed(0x00, base + QSERDES_COM_CORE_CLK_EN);
	writel_relaxed(0x0A, base + QSERDES_COM_CORECLK_DIV);
	writel_relaxed(0x09, base + QSERDES_COM_BG_TIMER);
	writel_relaxed(0x82, base + QSERDES_COM_DEC_START_MODE0);
	writel_relaxed(0x03, base + QSERDES_COM_DIV_FRAC_START3_MODE0);
	writel_relaxed(0x55, base + QSERDES_COM_DIV_FRAC_START2_MODE0);
	writel_relaxed(0x55, base + QSERDES_COM_DIV_FRAC_START1_MODE0);
	writel_relaxed(0x00, base + QSERDES_COM_LOCK_CMP3_MODE0);
	writel_relaxed(0x1A, base + QSERDES_COM_LOCK_CMP2_MODE0);
	writel_relaxed(0x0A, base + QSERDES_COM_LOCK_CMP1_MODE0);
	writel_relaxed(0x33, base + QSERDES_COM_CLK_SELECT);
	writel_relaxed(0x02, base + QSERDES_COM_SYS_CLK_CTRL);
	writel_relaxed(0x1F, base + QSERDES_COM_SYSCLK_BUF_ENABLE);
	writel_relaxed(0x04, base + QSERDES_COM_SYSCLK_EN_SEL);
	writel_relaxed(0x0B, base + QSERDES_COM_CP_CTRL_MODE0);
	writel_relaxed(0x16, base + QSERDES_COM_PLL_RCTRL_MODE0);
	writel_relaxed(0x28, base + QSERDES_COM_PLL_CCTRL_MODE0);
	writel_relaxed(0x00, base + QSERDES_COM_INTEGLOOP_GAIN1_MODE0);
	writel_relaxed(0x80, base + QSERDES_COM_INTEGLOOP_GAIN0_MODE0);
	writel_relaxed(0x01, base + QSERDES_COM_SSC_EN_CENTER);
	writel_relaxed(0x31, base + QSERDES_COM_SSC_PER1);
	writel_relaxed(0x01, base + QSERDES_COM_SSC_PER2);
	writel_relaxed(0x02, base + QSERDES_COM_SSC_ADJ_PER1);
	writel_relaxed(0x00, base + QSERDES_COM_SSC_ADJ_PER2);
	writel_relaxed(0x2f, base + QSERDES_COM_SSC_STEP_SIZE1);
	writel_relaxed(0x19, base + QSERDES_COM_SSC_STEP_SIZE2);
	writel_relaxed(0x15, base + QSERDES_COM_RESCODE_DIV_NUM);
	writel_relaxed(0x0F, base + QSERDES_COM_BG_TRIM);
	writel_relaxed(0x0F, base + QSERDES_COM_PLL_IVCO);
	writel_relaxed(0x19, base + QSERDES_COM_CLK_EP_DIV);
	writel_relaxed(0x10, base + QSERDES_COM_CLK_ENABLE1);

//	if (dev->phy_ver == 0x3) {
		writel_relaxed(0x00, base + QSERDES_COM_HSCLK_SEL);
		writel_relaxed(0x40, base + QSERDES_COM_RESCODE_DIV_NUM);
//	}

//	if (dev->common_phy) {
		writel_relaxed(0x00, base + PCIE_COM_SW_RESET);
		writel_relaxed(0x03, base + PCIE_COM_START_CONTROL);
//	}
//
	priv->inited = 1;
	return pcie_phy_is_ready(phy);
#if 0
	int retries;

	do {
//		if (pcie_phy_is_ready(dev))
		if ((readl_relaxed(base + PCIE_COM_PCS_READY_STATUS) & 0x1))
			break;
		retries++;
		usleep_range(REFCLK_STABILIZATION_DELAY_US_MIN,
					 REFCLK_STABILIZATION_DELAY_US_MAX);
	} while (retries < PHY_READY_TIMEOUT_COUNT);

	return 0;
#endif
}

static int qcom_msm8996_pcie_phy_init(struct phy *phy)
{
	struct phy_msm8996_desc *phydesc = phy_get_drvdata(phy);
	struct phy_msm8996_priv *priv = phydesc->priv;
	void __iomem *base = priv->base;
	int id = phydesc->index;
	int err;

	pr_info("Initializing PCIe PHY Port%d\n", id);

	err = qcom_msm8996_phy_comm_init(phy);
	if (err) {
		pr_err("PCIE phy init failed\n");
		return err;
	}

	//Reset Control
	reset_control_deassert(phydesc->phy_rstc);
	
	writel_relaxed(0x45, base + QSERDES_TX_N_HIGHZ_TRANSCEIVEREN_BIAS_DRVR_EN(id));
	writel_relaxed(0x06, base + QSERDES_TX_N_LANE_MODE(id));
	writel_relaxed(0x1c, base + QSERDES_RX_N_SIGDET_ENABLES(id));
	writel_relaxed(0x17, base + QSERDES_RX_N_SIGDET_LVL(id));
	writel_relaxed(0x01, base + QSERDES_RX_N_RX_EQU_ADAPTOR_CNTRL2(id));
	writel_relaxed(0x00, base + QSERDES_RX_N_RX_EQU_ADAPTOR_CNTRL3(id));
	writel_relaxed(0xdb, base + QSERDES_RX_N_RX_EQU_ADAPTOR_CNTRL4(id));
	writel_relaxed(0x18, base + QSERDES_RX_N_RX_BAND(id));
	writel_relaxed(0x04, base + QSERDES_RX_N_UCDR_SO_GAIN(id));
	writel_relaxed(0x04, base + QSERDES_RX_N_UCDR_SO_GAIN_HALF(id));
	writel_relaxed(0x4c, base + PCIE_N_RX_IDLE_DTCT_CNTRL(id));
	writel_relaxed(0x00, base + PCIE_N_PWRUP_RESET_DLY_TIME_AUXCLK(id));
	writel_relaxed(0x01, base + PCIE_N_LP_WAKEUP_DLY_TIME_AUXCLK(id));
	writel_relaxed(0x05, base + PCIE_N_PLL_LOCK_CHK_DLY_TIME(id));
	writel_relaxed(0x4b, base + QSERDES_RX_N_UCDR_SO_SATURATION_AND_ENABLE(id));
	writel_relaxed(0x14, base + QSERDES_RX_N_SIGDET_DEGLITCH_CNTRL(id));
	writel_relaxed(0x05, base + PCIE_N_ENDPOINT_REFCLK_DRIVE(id));
	writel_relaxed(0x02, base + PCIE_N_POWER_DOWN_CONTROL(id));
	writel_relaxed(0x00, base + PCIE_N_POWER_STATE_CONFIG4(id));
	writel_relaxed(0xa3, base + PCIE_N_POWER_STATE_CONFIG1(id));

//	if (dev->phy_ver == 0x3) {
	writel_relaxed(0x19, base + QSERDES_RX_N_SIGDET_LVL(id));
	writel_relaxed(0x0e, base + PCIE_N_TXDEEMPH_M3P5DB_V0(id));
//	}

	writel_relaxed(0x03, base + PCIE_N_POWER_DOWN_CONTROL(id));
	usleep_range(POWER_DOWN_DELAY_US_MIN, POWER_DOWN_DELAY_US_MAX);

	writel_relaxed(0x00, base + PCIE_N_SW_RESET(id));
	writel_relaxed(0x0a, base + PCIE_N_START_CONTROL(id));

	return 0;
}

static int qcom_msm8996_pcie_phy_exit(struct phy *phy)
{
#if 0
//FIXME.. 
	msm_pcie_write_reg(dev->phy,
		PCIE_N_SW_RESET(dev->rc_idx, dev->common_phy), 0x1);
	msm_pcie_write_reg(dev->phy,
		PCIE_N_POWER_DOWN_CONTROL(dev->rc_idx, dev->common_phy), 0);
			dev->rc_idx);
		msm_pcie_write_reg(dev->phy, PCIE_COM_SW_RESET, 0x1);
		msm_pcie_write_reg(dev->phy, PCIE_COM_POWER_DOWN_CONTROL, 0);
#endif
	return 0;
}

static const struct phy_ops qcom_msm8996_pcie_phy_ops = {
	.power_on	= qcom_msm8996_pcie_phy_init,
	.power_off	= qcom_msm8996_pcie_phy_exit,
	.owner		= THIS_MODULE,
};

static int qcom_msm8996_pcie_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *child;
	struct phy *phy;
	struct phy_provider *phy_provider;
	struct phy_msm8996_priv *priv;
	struct resource *res;
	int ret;
	u32 phy_id;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	priv->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!priv->base)
		return -ENOMEM;

	priv->cfg_clk = devm_clk_get(dev, "cfg");
	if (IS_ERR(priv->cfg_clk))
		return PTR_ERR(priv->cfg_clk);

	priv->aux_clk = devm_clk_get(dev, "aux");
	if (IS_ERR(priv->aux_clk))
		return PTR_ERR(priv->aux_clk);

	priv->nphys = of_get_child_count(dev->of_node);
	if (priv->nphys == 0)
		return -ENODEV;

	priv->phys = devm_kcalloc(dev, priv->nphys, sizeof(*priv->phys),
				  GFP_KERNEL);
	if (!priv->phys)
		return -ENOMEM;

	priv->phycom_rstc = reset_control_get(dev, "phy_com");
	if (IS_ERR(priv->phycom_rstc))
		return PTR_ERR(priv->phycom_rstc);

	priv->phycom2_rstc = reset_control_get(dev, "phy_com2");
	if (IS_ERR(priv->phycom2_rstc))
		return PTR_ERR(priv->phycom2_rstc);

	priv->phycfg_rstc = reset_control_get(dev, "phy_cfg");
	if (IS_ERR(priv->phycfg_rstc))
		return PTR_ERR(priv->phycfg_rstc);

	dev_set_drvdata(dev, priv);
	spin_lock_init(&priv->lock);

	for_each_available_child_of_node(dev->of_node, child) {
		struct phy_msm8996_desc *phy_desc;

		if (of_property_read_u32(child, "reg", &phy_id)) {
			dev_err(dev, "missing reg property in node %s\n",
				child->name);
			ret = -EINVAL;
			goto put_child;
		}

		phy_desc = devm_kzalloc(dev, sizeof(*phy_desc), GFP_KERNEL);
		if (!phy_desc) {
			ret = -ENOMEM;
			goto put_child;
		}

		phy = devm_phy_create(dev, NULL, &qcom_msm8996_pcie_phy_ops);
		if (IS_ERR(phy)) {
			dev_err(dev, "failed to create PHY %d\n", phy_id);
			ret = PTR_ERR(phy);
			goto put_child;
		}

		phy_desc->phy_rstc = of_reset_control_get(child, "phy");
		if (IS_ERR(phy_desc->phy_rstc)) {
			ret = PTR_ERR(phy_desc->phy_rstc);
			goto put_child;
		}

		phy_desc->phy = phy;
		phy_desc->priv = priv;
		phy_desc->index = phy_id;
		phy_set_drvdata(phy, phy_desc);
		priv->phys[phy_id] = phy_desc;
	}

	phy_provider =
		devm_of_phy_provider_register(dev, phy_msm8996_pcie_phy_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
put_child:
	of_node_put(child);
	return ret;
}
static int qcom_msm8996_pcie_phy_remove(struct platform_device *pdev)
{
//FIXME

	return 0;
}

static const struct of_device_id qcom_msm8996_pcie_phy_of_match[] = {
	{ .compatible = "qcom,msm8996-pcie-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_msm8996_pcie_phy_of_match);

static struct platform_driver qcom_msm8996_pcie_phy_driver = {
	.probe	= qcom_msm8996_pcie_phy_probe,
	.remove	= qcom_msm8996_pcie_phy_remove,
	.driver = {
		.name	= "qcom-msm8996-pcie-phy",
		.of_match_table	= qcom_msm8996_pcie_phy_of_match,
	}
};
module_platform_driver(qcom_msm8996_pcie_phy_driver);

MODULE_DESCRIPTION("QCOM msm8996 pcie PHY driver");
MODULE_LICENSE("GPL v2");
