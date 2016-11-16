/*
 * Copyright (c) 2016, Linaro Ltd
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

#ifndef __QCOM_GLINK_NATIVE_H__
#define __QCOM_GLINK_NATIVE_H__

struct qcom_glink_native_pipe {
	size_t (*avail)(struct qcom_glink_native_pipe *pipe);

	void (*peak)(struct qcom_glink_native_pipe *pipe,
		     void *data, size_t count);
	void (*advance)(struct qcom_glink_native_pipe *pipe, size_t count);

	void (*write)(struct qcom_glink_native_pipe *pipe,
		      const void *data, size_t count);
};

struct qcom_glink;

struct qcom_glink *qcom_glink_native_probe(struct device *dev,
					   struct qcom_glink_native_pipe *rx,
					   struct qcom_glink_native_pipe *tx,
					   size_t max_tx_size);
void qcom_glink_native_remove(struct qcom_glink *glink);

#endif
