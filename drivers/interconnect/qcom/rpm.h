// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#ifndef __LINUX_INTERCONNECT_QCOM_RPM_H
#define __LINUX_INTERCONNECT_QCOM_RPM_H

#include <linux/soc/qcom/smd-rpm.h>

struct qcom_interconnect_rpm {
	struct qcom_smd_rpm *rpm;
};

extern struct qcom_interconnect_rpm interconnect_rpm;

int qcom_interconnect_rpm_send(int ctx, int rsc_type, int id, u32 val);

#endif
