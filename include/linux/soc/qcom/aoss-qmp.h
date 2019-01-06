/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018, Linaro Ltd
 */
#ifndef __AOP_QMP_H__
#define __AOP_QMP_H__

#include <linux/types.h>

struct qmp;

int qmp_send(struct qmp *qmp, const void *data, size_t len);

#endif
