/*
 * Copyright (c) 2015, Linaro Limited
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smd-rpm.h>

#include "clk-smd-rpm.h"
#include <dt-bindings/clock/qcom,rpmcc.h>

#define to_clk_smd_rpm(_hw) container_of(_hw, struct clk_smd_rpm, hw)

static DEFINE_MUTEX(rpm_smd_clk_lock);

static int clk_smd_rpm_set_rate_active(struct clk_smd_rpm *r,
				       unsigned long rate)
{
	struct clk_smd_rpm_req req = {
		.key = cpu_to_le32(r->rpm_key),
		.nbytes = cpu_to_le32(sizeof(u32)),
		.value = cpu_to_le32(DIV_ROUND_UP(rate, 1000)), /* to kHz */
	};

	return qcom_rpm_smd_write(r->rpm, QCOM_SMD_RPM_ACTIVE_STATE,
				  r->rpm_res_type, r->rpm_clk_id, &req,
				  sizeof(req));
}

static int clk_smd_rpm_set_rate_sleep(struct clk_smd_rpm *r,
				      unsigned long rate)
{
	struct clk_smd_rpm_req req = {
		.key = cpu_to_le32(r->rpm_key),
		.nbytes = cpu_to_le32(sizeof(u32)),
		.value = cpu_to_le32(DIV_ROUND_UP(rate, 1000)), /* to kHz */
	};

	return qcom_rpm_smd_write(r->rpm, QCOM_SMD_RPM_SLEEP_STATE,
				  r->rpm_res_type, r->rpm_clk_id, &req,
				  sizeof(req));
}

static void to_active_sleep(struct clk_smd_rpm *r, unsigned long rate,
			    unsigned long *active, unsigned long *sleep)
{
	*active = rate;

	/*
	 * Active-only clocks don't care what the rate is during sleep. So,
	 * they vote for zero.
	 */
	if (r->active_only)
		*sleep = 0;
	else
		*sleep = *active;
}

static int clk_smd_rpm_prepare(struct clk_hw *hw)
{
	struct clk_smd_rpm *r = to_clk_smd_rpm(hw);
	struct clk_smd_rpm *peer = r->peer;
	unsigned long this_rate = 0, this_sleep_rate = 0;
	unsigned long peer_rate = 0, peer_sleep_rate = 0;
	unsigned long active_rate, sleep_rate;
	int ret = 0;

	mutex_lock(&rpm_smd_clk_lock);

	/* Don't send requests to the RPM if the rate has not been set. */
	if (!r->rate)
		goto out;

	to_active_sleep(r, r->rate, &this_rate, &this_sleep_rate);

	/* Take peer clock's rate into account only if it's enabled. */
	if (peer->enabled)
		to_active_sleep(peer, peer->rate,
				&peer_rate, &peer_sleep_rate);

	active_rate = max(this_rate, peer_rate);

	if (r->branch)
		active_rate = !!active_rate;

	ret = clk_smd_rpm_set_rate_active(r, active_rate);
	if (ret)
		goto out;

	sleep_rate = max(this_sleep_rate, peer_sleep_rate);
	if (r->branch)
		sleep_rate = !!sleep_rate;

	ret = clk_smd_rpm_set_rate_sleep(r, sleep_rate);
	if (ret)
		/* Undo the active set vote and restore it */
		ret = clk_smd_rpm_set_rate_active(r, peer_rate);

out:
	if (!ret)
		r->enabled = true;

	mutex_unlock(&rpm_smd_clk_lock);

	return ret;
}

static void clk_smd_rpm_unprepare(struct clk_hw *hw)
{
	struct clk_smd_rpm *r = to_clk_smd_rpm(hw);
	struct clk_smd_rpm *peer = r->peer;
	unsigned long peer_rate = 0, peer_sleep_rate = 0;
	unsigned long active_rate, sleep_rate;
	int ret;

	mutex_lock(&rpm_smd_clk_lock);

	if (!r->rate)
		goto out;

	/* Take peer clock's rate into account only if it's enabled. */
	if (peer->enabled)
		to_active_sleep(peer, peer->rate, &peer_rate,
				&peer_sleep_rate);

	active_rate = r->branch ? !!peer_rate : peer_rate;
	ret = clk_smd_rpm_set_rate_active(r, active_rate);
	if (ret)
		goto out;

	sleep_rate = r->branch ? !!peer_sleep_rate : peer_sleep_rate;
	ret = clk_smd_rpm_set_rate_sleep(r, sleep_rate);
	if (ret)
		goto out;

	r->enabled = false;

out:
	mutex_unlock(&rpm_smd_clk_lock);
}

static int clk_smd_rpm_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct clk_smd_rpm *r = to_clk_smd_rpm(hw);
	struct clk_smd_rpm *peer = r->peer;
	unsigned long active_rate, sleep_rate;
	unsigned long this_rate = 0, this_sleep_rate = 0;
	unsigned long peer_rate = 0, peer_sleep_rate = 0;
	int ret = 0;

	mutex_lock(&rpm_smd_clk_lock);

	if (!r->enabled)
		goto out;

	to_active_sleep(r, rate, &this_rate, &this_sleep_rate);

	/* Take peer clock's rate into account only if it's enabled. */
	if (peer->enabled)
		to_active_sleep(peer, peer->rate,
				&peer_rate, &peer_sleep_rate);

	active_rate = max(this_rate, peer_rate);
	ret = clk_smd_rpm_set_rate_active(r, active_rate);
	if (ret)
		goto out;

	sleep_rate = max(this_sleep_rate, peer_sleep_rate);
	ret = clk_smd_rpm_set_rate_sleep(r, sleep_rate);
	if (ret)
		goto out;

	r->rate = rate;

out:
	mutex_unlock(&rpm_smd_clk_lock);

	return ret;
}

static long clk_smd_rpm_round_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long *parent_rate)
{
	/*
	 * RPM handles rate rounding and we don't have a way to
	 * know what the rate will be, so just return whatever
	 * rate is requested.
	 */
	return rate;
}

static unsigned long clk_smd_rpm_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct clk_smd_rpm *r = to_clk_smd_rpm(hw);

	/*
	 * RPM handles rate rounding and we don't have a way to
	 * know what the rate will be, so just return whatever
	 * rate was set.
	 */
	return r->rate;
}

static int clk_smd_rpm_enable_scaling(struct qcom_smd_rpm *rpm)
{
	int ret;
	struct clk_smd_rpm_req req = {
		.key = cpu_to_le32(QCOM_RPM_SMD_KEY_ENABLE),
		.nbytes = cpu_to_le32(sizeof(u32)),
		.value = cpu_to_le32(1),
	};

	ret = qcom_rpm_smd_write(rpm, QCOM_SMD_RPM_SLEEP_STATE,
				 QCOM_SMD_RPM_MISC_CLK,
				 QCOM_RPM_SCALING_ENABLE_ID, &req, sizeof(req));
	if (ret) {
		pr_err("RPM clock scaling (sleep set) not enabled!\n");
		return ret;
	}

	ret = qcom_rpm_smd_write(rpm, QCOM_SMD_RPM_ACTIVE_STATE,
				 QCOM_SMD_RPM_MISC_CLK,
				 QCOM_RPM_SCALING_ENABLE_ID, &req, sizeof(req));
	if (ret) {
		pr_err("RPM clock scaling (active set) not enabled!\n");
		return ret;
	}

	pr_debug("%s: RPM clock scaling is enabled\n", __func__);
	return 0;
}

const struct clk_ops clk_smd_rpm_ops = {
	.prepare	= clk_smd_rpm_prepare,
	.unprepare	= clk_smd_rpm_unprepare,
	.set_rate	= clk_smd_rpm_set_rate,
	.round_rate	= clk_smd_rpm_round_rate,
	.recalc_rate	= clk_smd_rpm_recalc_rate,
};
EXPORT_SYMBOL_GPL(clk_smd_rpm_ops);

const struct clk_ops clk_smd_rpm_branch_ops = {
	.prepare	= clk_smd_rpm_prepare,
	.unprepare	= clk_smd_rpm_unprepare,
	.round_rate	= clk_smd_rpm_round_rate,
	.recalc_rate	= clk_smd_rpm_recalc_rate,
};
EXPORT_SYMBOL_GPL(clk_smd_rpm_branch_ops);

struct rpm_cc {
	struct qcom_rpm *rpm;
	struct clk_onecell_data data;
	struct clk *clks[];
};

struct rpm_smd_clk_desc {
	struct clk_smd_rpm **clks;
	size_t num_clks;
};

/* msm8916 */
DEFINE_CLK_SMD_RPM(msm8916, pcnoc_clk, pcnoc_a_clk, QCOM_SMD_RPM_BUS_CLK, 0);
DEFINE_CLK_SMD_RPM(msm8916, snoc_clk, snoc_a_clk, QCOM_SMD_RPM_BUS_CLK, 1);
DEFINE_CLK_SMD_RPM(msm8916, bimc_clk, bimc_a_clk, QCOM_SMD_RPM_MEM_CLK, 0);
DEFINE_CLK_SMD_RPM_BRANCH(msm8916, xo, xo_a, QCOM_SMD_RPM_MISC_CLK, 0, 19200000);
DEFINE_CLK_SMD_RPM_QDSS(msm8916, qdss_clk, qdss_a_clk, QCOM_SMD_RPM_MISC_CLK, 1);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8916, bb_clk1, bb_clk1_a, 1);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8916, bb_clk2, bb_clk2_a, 2);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8916, rf_clk1, rf_clk1_a, 4);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8916, rf_clk2, rf_clk2_a, 5);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8916, bb_clk1_pin, bb_clk1_a_pin, 1);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8916, bb_clk2_pin, bb_clk2_a_pin, 2);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8916, rf_clk1_pin, rf_clk1_a_pin, 4);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8916, rf_clk2_pin, rf_clk2_a_pin, 5);

static struct clk_smd_rpm *msm8916_clks[] = {
//	[RPM_XO_CLK_SRC]	= &msm8916_xo,
//	[RPM_XO_A_CLK_SRC]	= &msm8916_xo_a,
	[RPM_PCNOC_CLK]		= &msm8916_pcnoc_clk,
	[RPM_PCNOC_A_CLK]	= &msm8916_pcnoc_a_clk,
	[RPM_SNOC_CLK]		= &msm8916_snoc_clk,
	[RPM_SNOC_A_CLK]	= &msm8916_snoc_a_clk,
	[RPM_BIMC_CLK]		= &msm8916_bimc_clk,
	[RPM_BIMC_A_CLK]	= &msm8916_bimc_a_clk,
	[RPM_QDSS_CLK]		= &msm8916_qdss_clk,
	[RPM_QDSS_A_CLK]	= &msm8916_qdss_a_clk,
	[RPM_BB_CLK1]		= &msm8916_bb_clk1,
	[RPM_BB_CLK1_A]		= &msm8916_bb_clk1_a,
	[RPM_BB_CLK2]		= &msm8916_bb_clk2,
	[RPM_BB_CLK2_A]		= &msm8916_bb_clk2_a,
	[RPM_RF_CLK1]		= &msm8916_rf_clk1,
	[RPM_RF_CLK1_A]		= &msm8916_rf_clk1_a,
	[RPM_RF_CLK2]		= &msm8916_rf_clk2,
	[RPM_RF_CLK2_A]		= &msm8916_rf_clk2_a,
	[RPM_BB_CLK1_PIN]	= &msm8916_bb_clk1_pin,
	[RPM_BB_CLK1_A_PIN]	= &msm8916_bb_clk1_a_pin,
	[RPM_BB_CLK2_PIN]	= &msm8916_bb_clk2_pin,
	[RPM_BB_CLK2_A_PIN]	= &msm8916_bb_clk2_a_pin,
	[RPM_RF_CLK1_PIN]	= &msm8916_rf_clk1_pin,
	[RPM_RF_CLK1_A_PIN]	= &msm8916_rf_clk1_a_pin,
	[RPM_RF_CLK2_PIN]	= &msm8916_rf_clk2_pin,
	[RPM_RF_CLK2_A_PIN]	= &msm8916_rf_clk2_a_pin,
};

static const struct rpm_smd_clk_desc rpm_clk_msm8916 = {
	.clks = msm8916_clks,
	.num_clks = ARRAY_SIZE(msm8916_clks),
};

/* msm8974 */
DEFINE_CLK_SMD_RPM(msm8974, pnoc_clk, pnoc_a_clk, QCOM_SMD_RPM_BUS_CLK, 0);
DEFINE_CLK_SMD_RPM(msm8974, snoc_clk, snoc_a_clk, QCOM_SMD_RPM_BUS_CLK, 1);
DEFINE_CLK_SMD_RPM(msm8974, cnoc_clk, cnoc_a_clk, QCOM_SMD_RPM_BUS_CLK, 2);
DEFINE_CLK_SMD_RPM(msm8974, mmssnoc_ahb_clk, mmssnoc_ahb_a_clk, QCOM_SMD_RPM_BUS_CLK, 3);
DEFINE_CLK_SMD_RPM(msm8974, bimc_clk, bimc_a_clk, QCOM_SMD_RPM_MEM_CLK, 0);
DEFINE_CLK_SMD_RPM(msm8974, gfx3d_clk_src, gfx3d_a_clk_src, QCOM_SMD_RPM_MEM_CLK, 1);
DEFINE_CLK_SMD_RPM(msm8974, ocmemgx_clk, ocmemgx_a_clk, QCOM_SMD_RPM_MEM_CLK, 2);
DEFINE_CLK_SMD_RPM_BRANCH(msm8974, cxo, cxo_a_clk_src, QCOM_SMD_RPM_MISC_CLK, 0, 19200000);
DEFINE_CLK_SMD_RPM_QDSS(msm8974, qdss_clk, qdss_a_clk, QCOM_SMD_RPM_MISC_CLK, 1);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, cxo_d0, cxo_d0_a, 1);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, cxo_d1, cxo_d1_a, 2);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, cxo_a0, cxo_a0_a, 4);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, cxo_a1, cxo_a1_a, 5);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, cxo_a2, cxo_a2_a, 6);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, diff_clk, diff_a_clk, 7);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, div_clk1, div_a_clk1, 11);
DEFINE_CLK_SMD_RPM_XO_BUFFER(msm8974, div_clk2, div_a_clk2, 12);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8974, cxo_d0_pin, cxo_d0_a_pin, 1);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8974, cxo_d1_pin, cxo_d1_a_pin, 2);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8974, cxo_a0_pin, cxo_a0_a_pin, 4);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8974, cxo_a1_pin, cxo_a1_a_pin, 5);
DEFINE_CLK_SMD_RPM_XO_BUFFER_PINCTRL(msm8974, cxo_a2_pin, cxo_a2_a_pin, 6);

static struct clk_smd_rpm *msm8974_clks[] = {
       [RPM_PNOC_CLK] =		&msm8974_pnoc_clk,
       [RPM_PNOC_A_CLK] =	&msm8974_pnoc_a_clk,
       [RPM_SNOC_CLK] =		&msm8974_snoc_clk,
       [RPM_SNOC_A_CLK] =	&msm8974_snoc_a_clk,
       [RPM_CNOC_CLK] =		&msm8974_cnoc_clk,
       [RPM_CNOC_A_CLK] =	&msm8974_cnoc_a_clk,
       [RPM_MMSSNOC_AHB_CLK] =	&msm8974_mmssnoc_ahb_clk,
       [RPM_MMSSNOC_AHB_A_CLK] = &msm8974_mmssnoc_ahb_a_clk,
       [RPM_BIMC_CLK] =		&msm8974_bimc_clk,
       [RPM_BIMC_A_CLK] =	&msm8974_bimc_a_clk,
      // [RPM_GFX3D_CLK_SRC] =	&msm8974_gfx3d_clk_src,
       //[RPM_GFX3D_A_CLK_SRC] =	&msm8974_gfx3d_a_clk_src,
       [RPM_OCMEMGX_CLK] =	&msm8974_ocmemgx_clk,
       [RPM_OCMEMGX_A_CLK] =	&msm8974_ocmemgx_a_clk,
       [RPM_CXO_CLK_SRC] =	&msm8974_cxo,
       [RPM_CXO_A_CLK_SRC] =	&msm8974_cxo_a_clk_src,
       [RPM_QDSS_CLK] =		&msm8974_qdss_clk,
       [RPM_QDSS_A_CLK] =	&msm8974_qdss_a_clk,
       [RPM_CXO_D0] =		&msm8974_cxo_d0,
       [RPM_CXO_D0_A] =		&msm8974_cxo_d0_a,
       [RPM_CXO_D1] =		&msm8974_cxo_d1,
       [RPM_CXO_D1_A] =		&msm8974_cxo_d1_a,
       [RPM_CXO_A0] =		&msm8974_cxo_a0,
       [RPM_CXO_A0_A] =		&msm8974_cxo_a0_a,
       [RPM_CXO_A1] =		&msm8974_cxo_a1,
       [RPM_CXO_A1_A] =		&msm8974_cxo_a1_a,
       [RPM_CXO_A2] =		&msm8974_cxo_a2,
       [RPM_CXO_A2_A] =		&msm8974_cxo_a2_a,
       [RPM_DIFF_CLK] =		&msm8974_diff_clk,
       [RPM_DIFF_A_CLK] =	&msm8974_diff_a_clk,
       [RPM_DIV_CLK1] =		&msm8974_div_clk1,
       [RPM_DIV_A_CLK1] =	&msm8974_div_a_clk1,
       [RPM_DIV_CLK2] =		&msm8974_div_clk2,
       [RPM_DIV_A_CLK2] =	&msm8974_div_a_clk2,
       [RPM_CXO_D0_PIN] =	&msm8974_cxo_d0_pin,
       [RPM_CXO_D0_A_PIN] =	&msm8974_cxo_d0_a_pin,
       [RPM_CXO_D1_PIN] =	&msm8974_cxo_d1_pin,
       [RPM_CXO_D1_A_PIN] =	&msm8974_cxo_d1_a_pin,
       [RPM_CXO_A0_PIN] =	&msm8974_cxo_a0_pin,
       [RPM_CXO_A0_A_PIN] =	&msm8974_cxo_a0_a_pin,
       [RPM_CXO_A1_PIN] =	&msm8974_cxo_a1_pin,
       [RPM_CXO_A1_A_PIN] =	&msm8974_cxo_a1_a_pin,
       [RPM_CXO_A2_PIN] =	&msm8974_cxo_a2_pin,
       [RPM_CXO_A2_A_PIN] =	&msm8974_cxo_a2_a_pin,
};

static const struct rpm_smd_clk_desc rpm_clk_msm8974 = {
	.clks = msm8974_clks,
	.num_clks = ARRAY_SIZE(msm8974_clks),
};

static const struct of_device_id rpm_smd_clk_match_table[] = {
	{ .compatible = "qcom,rpmcc-msm8916", .data = &rpm_clk_msm8916},
	{ .compatible = "qcom,rpmcc-msm8974", .data = &rpm_clk_msm8974},
	{ }
};
MODULE_DEVICE_TABLE(of, rpm_smd_clk_match_table);

static int rpm_smd_clk_probe(struct platform_device *pdev)
{
	struct clk **clks;
	struct clk *clk;
	struct rpm_cc *rcc;
	struct clk_onecell_data *data;
	int ret;
	size_t num_clks, i;
	struct qcom_smd_rpm *rpm;
	struct clk_smd_rpm **rpm_smd_clks;
	const struct rpm_smd_clk_desc *desc;

	rpm = dev_get_drvdata(pdev->dev.parent);
	if (!rpm) {
		dev_err(&pdev->dev, "Unable to retrieve handle to RPM\n");
		return -ENODEV;
	}

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	rpm_smd_clks = desc->clks;
	num_clks = desc->num_clks;

	rcc = devm_kzalloc(&pdev->dev, sizeof(*rcc) + sizeof(*clks) * num_clks,
			   GFP_KERNEL);
	if (!rcc)
		return -ENOMEM;

	clks = rcc->clks;
	data = &rcc->data;
	data->clks = clks;
	data->clk_num = num_clks;

	for (i = 0; i < num_clks; i++) {
		if (!rpm_smd_clks[i]) {
			clks[i] = ERR_PTR(-ENOENT);
			continue;
		}

		rpm_smd_clks[i]->rpm = rpm;
		clk = devm_clk_register(&pdev->dev, &rpm_smd_clks[i]->hw);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			goto err;
		}

		clks[i] = clk;
	}

	ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get,
				  data);
	if (ret)
		goto err;

#if 0
	ret = clk_smd_rpm_enable_scaling(rpm);
	if (ret) {
		of_clk_del_provider(pdev->dev.of_node);
		goto err;
	}
#endif

	return 0;
err:
	dev_err(&pdev->dev, "Error registering SMD clock driver (%d)\n", ret);
	return ret;
}

static int rpm_smd_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	return 0;
}

static struct platform_driver rpm_smd_clk_driver = {
	.driver = {
		.name = "qcom-clk-smd-rpm",
		.of_match_table = rpm_smd_clk_match_table,
	},
	.probe = rpm_smd_clk_probe,
	.remove = rpm_smd_clk_remove,
};

static int __init rpm_smd_clk_init(void)
{
	return platform_driver_register(&rpm_smd_clk_driver);
}
core_initcall(rpm_smd_clk_init);

static void __exit rpm_smd_clk_exit(void)
{
	platform_driver_unregister(&rpm_smd_clk_driver);
}
module_exit(rpm_smd_clk_exit);

MODULE_DESCRIPTION("Qualcomm RPM over SMD Clock Controller Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:qcom-clk-smd-rpm");
