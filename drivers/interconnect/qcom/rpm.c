// SPDX-License-Identifier: GPL-2.0
/*
 * RPM communication wrapper for interconects
 *
 * Copyright (C) 2017 Linaro Ltd
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smd-rpm.h>
#include "rpm.h"

#define	RPM_KEY_BW	0x00007762

struct qcom_interconnect_rpm interconnect_rpm;

struct interconnect_rpm_req {
	__le32 key;
	__le32 nbytes;
	__le32 value;
};

int qcom_interconnect_rpm_send(int ctx, int rsc_type, int id, u32 val)
{
	struct interconnect_rpm_req req = {
		.key = cpu_to_le32(RPM_KEY_BW),
		.nbytes = cpu_to_le32(sizeof(u32)),
		.value = cpu_to_le32(val),
	};

	if (!interconnect_rpm.rpm) {
		pr_err("%s: RPM not ready yet\n", __func__);
		return -EBUSY;
	}

	return qcom_rpm_smd_write(interconnect_rpm.rpm, ctx, rsc_type, id, &req, sizeof(req));
}
EXPORT_SYMBOL(qcom_interconnect_rpm_send);

static int qcom_interconnect_rpm_probe(struct platform_device *pdev)
{
	interconnect_rpm.rpm = dev_get_drvdata(pdev->dev.parent);
	if (!interconnect_rpm.rpm) {
		dev_err(&pdev->dev, "unable to retrieve handle to rpm\n");
		return -ENODEV;
	}

	pr_info("interconnect: initialized RPM communication channel\n");

	return 0;
}

static const struct of_device_id qcom_interconnect_rpm_dt_match[] = {
	{ .compatible = "qcom,interconnect-rpm", },
	{ },
};

MODULE_DEVICE_TABLE(of, qcom_interconnect_rpm_dt_match);

static struct platform_driver qcom_interconnect_rpm_driver = {
	.driver = {
		.name		= "qcom-interconnect-rpm",
		.of_match_table	= qcom_interconnect_rpm_dt_match,
	},
	.probe = qcom_interconnect_rpm_probe,
};

module_platform_driver(qcom_interconnect_rpm_driver);
