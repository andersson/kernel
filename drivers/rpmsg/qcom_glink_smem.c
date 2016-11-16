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

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>
#include <linux/rpmsg.h>
#include <linux/idr.h>
#include <linux/circ_buf.h>
#include <linux/soc/qcom/smem.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/list.h>

#include <linux/delay.h>
#include <linux/rpmsg.h>

#include "qcom_glink_native.h"

struct glink_smem_pipe {
	struct qcom_glink_native_pipe native;

	__le32 *tail;
	__le32 *head;

	void *fifo;
	size_t length;
};

#define to_smem_pipe(p) container_of(p, struct glink_smem_pipe, native)

static size_t glink_smem_rx_avail(struct qcom_glink_native_pipe *np)
{
	struct glink_smem_pipe *pipe = to_smem_pipe(np);
	u32 head;
	u32 tail;

	head = le32_to_cpu(*pipe->head);
	tail = le32_to_cpu(*pipe->tail);

	if (head < tail)
		return pipe->length - tail + head;
	else
		return head - tail;
}

static void glink_smem_rx_peak(struct qcom_glink_native_pipe *np,
			      void *data, size_t count)
{
	struct glink_smem_pipe *pipe = to_smem_pipe(np);
	size_t len;
	u32 tail;

	tail = le32_to_cpu(*pipe->tail);

	len = min_t(size_t, count, pipe->length - tail);
	if (len) {
		__ioread32_copy(data, pipe->fifo + tail,
				len / sizeof(u32));
	}

	if (len != count) {
		__ioread32_copy(data + len, pipe->fifo,
				(count - len) / sizeof(u32));
	}
}

static void glink_smem_rx_advance(struct qcom_glink_native_pipe *np,
				 size_t count)
{
	struct glink_smem_pipe *pipe = to_smem_pipe(np);
	u32 tail;

	tail = le32_to_cpu(*pipe->tail);

	tail += count;
	if (tail > pipe->length)
		tail -= pipe->length;

	*pipe->tail = cpu_to_le32(tail);
}

static size_t glink_smem_tx_avail(struct qcom_glink_native_pipe *np)
{
	struct glink_smem_pipe *pipe = to_smem_pipe(np);
	u32 head;
	u32 tail;

	head = le32_to_cpu(*pipe->head);
	tail = le32_to_cpu(*pipe->tail);

	if (tail <= head)
		return pipe->length - head + tail;
	else
		return tail - head;
}

static void glink_smem_tx_write(struct qcom_glink_native_pipe *np,
			       const void *data, size_t count)
{
	struct glink_smem_pipe *pipe = to_smem_pipe(np);
	size_t len;
	u32 head;

	head = le32_to_cpu(*pipe->head);

	len = min_t(size_t, count, pipe->length - head);
	if (len) {
		__iowrite32_copy(pipe->fifo + head, data,
				 len / sizeof(u32));
	}

	if (len != count) {
		__iowrite32_copy(pipe->fifo, data + len,
				 (count - len) / sizeof(u32));
	}

	head += count;
	if (head > pipe->length)
		head -= pipe->length;

	*pipe->head = cpu_to_le32(head);
}

#define SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR	478
#define SMEM_GLINK_NATIVE_XPRT_FIFO_0		479
#define SMEM_GLINK_NATIVE_XPRT_FIFO_1		480

static int qcom_glink_smem_probe(struct platform_device *pdev)
{
	struct glink_smem_pipe *rx_pipe;
	struct glink_smem_pipe *tx_pipe;
	struct qcom_glink *glink;
	u32 remote_pid;
	__le32 *descs;
	size_t size;
	int ret;
	
	ret = of_property_read_u32(pdev->dev.of_node, "qcom,remote-pid", &remote_pid);
	if (ret) {
		dev_err(&pdev->dev, "failed to parse qcom,remote-pid\n");
		return ret;
	}

	rx_pipe = devm_kzalloc(&pdev->dev, sizeof(*rx_pipe), GFP_KERNEL);
	tx_pipe = devm_kzalloc(&pdev->dev, sizeof(*tx_pipe), GFP_KERNEL);
	if (!rx_pipe || !tx_pipe)
		return -ENOMEM;


	descs = qcom_smem_get(remote_pid, SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR, &size);
	if (IS_ERR(descs)) {
		dev_err(&pdev->dev, "failed to acquire xprt descriptor\n");
		return PTR_ERR(descs);
	};

	tx_pipe->tail = &descs[0];
	tx_pipe->head = &descs[1];
	rx_pipe->tail = &descs[2];
	rx_pipe->head = &descs[3];

	ret = qcom_smem_alloc(remote_pid, SMEM_GLINK_NATIVE_XPRT_FIFO_0, SZ_16K);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate fifo 0\n");
		return ret;
	}

	tx_pipe->fifo = qcom_smem_get(remote_pid, SMEM_GLINK_NATIVE_XPRT_FIFO_0, &tx_pipe->length);
	if (IS_ERR(tx_pipe->fifo)) {
		dev_err(&pdev->dev, "failed to acquire fifo 0: %ld\n", PTR_ERR(tx_pipe->fifo));
		return PTR_ERR(tx_pipe->fifo);
	}
	
	rx_pipe->fifo = qcom_smem_get(remote_pid, SMEM_GLINK_NATIVE_XPRT_FIFO_1, &rx_pipe->length);
	if (IS_ERR(rx_pipe->fifo)) {
		dev_err(&pdev->dev, "failed to acquire fifo 1: %ld\n", PTR_ERR(rx_pipe->fifo));
		return PTR_ERR(rx_pipe->fifo);
	}

	rx_pipe->native.avail = glink_smem_rx_avail;
	rx_pipe->native.peak = glink_smem_rx_peak;
	rx_pipe->native.advance = glink_smem_rx_advance;

	tx_pipe->native.avail = glink_smem_tx_avail;
	tx_pipe->native.write = glink_smem_tx_write;

	*rx_pipe->tail = 0;
	*tx_pipe->head = 0;

	glink = qcom_glink_native_probe(&pdev->dev,
					&rx_pipe->native, &tx_pipe->native,
					tx_pipe->length);
	if (IS_ERR(glink))
		return PTR_ERR(glink);
	
	platform_set_drvdata(pdev, glink);

	return 0;
}

static int qcom_glink_smem_remove(struct platform_device *pdev)
{
	struct qcom_glink *glink = platform_get_drvdata(pdev);

	qcom_glink_native_remove(glink);

	return 0;
}

static const struct of_device_id qcom_glink_smem_of_match[] = {
	{ .compatible = "qcom,glink-smem" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_glink_smem_of_match);

static struct platform_driver qcom_glink_smem_driver = {
	.probe = qcom_glink_smem_probe,
	.remove = qcom_glink_smem_remove,
	.driver = {
		.name = "qcom-glink_smem",
		.of_match_table = qcom_glink_smem_of_match,
	},
};

static int __init qcom_glink_smem_init(void)
{
	return platform_driver_register(&qcom_glink_smem_driver);
}
subsys_initcall(qcom_glink_smem_init);

static void __exit qcom_glink_smem_exit(void)
{
	platform_driver_unregister(&qcom_glink_smem_driver);
}
module_exit(qcom_glink_smem_exit);

MODULE_AUTHOR("Bjorn Andersson <bjorn.andersson@linaro.org>");
MODULE_DESCRIPTION("Qualcomm GLINK SMEM driver");
MODULE_LICENSE("GPL v2");
