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
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/list.h>

#include <linux/delay.h>
#include <linux/rpmsg.h>

#include "qcom_glink_native.h"

#define RPM_TOC_SIZE 256
#define RPM_TOC_MAGIC 0x67727430 /* grt0 */
#define RPM_TOC_MAX_ENTRIES ((RPM_TOC_SIZE - sizeof(struct rpm_toc)) / \
			     sizeof(struct rpm_toc_entry))

#define RPM_TX_FIFO_ID 0x61703272 /* ap2r */
#define RPM_RX_FIFO_ID 0x72326170 /* r2ap */

struct rpm_toc_entry {
	__le32 id;
	__le32 offset;
	__le32 size;
} __packed;

struct rpm_toc {
	__le32 magic;
	__le32 count;

	struct rpm_toc_entry entries[];
} __packed;

struct glink_cmd {
	__le16 cmd;
	__le16 param1;
	__le32 param2;
	u8 data[];
} __packed;

struct glink_rpm_pipe {
	struct qcom_glink_native_pipe native;

	size_t length;

	void __iomem *tail;
	void __iomem *head;

	void __iomem *fifo;
};

#define to_rpm_pipe(p) container_of(p, struct glink_rpm_pipe, native)

static size_t glink_rpm_rx_avail(struct qcom_glink_native_pipe *np)
{
	struct glink_rpm_pipe *pipe = to_rpm_pipe(np);
	u32 head;
	u32 tail;

	head = readl(pipe->head);
	tail = readl(pipe->tail);

	if (head < tail)
		return pipe->length - tail + head;
	else
		return head - tail;
}

static void glink_rpm_rx_peak(struct qcom_glink_native_pipe *np,
			      void *data, size_t count)
{
	struct glink_rpm_pipe *pipe = to_rpm_pipe(np);
	size_t len;
	u32 tail;

	tail = readl(pipe->tail);

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

static void glink_rpm_rx_advance(struct qcom_glink_native_pipe *np,
				 size_t count)
{
	struct glink_rpm_pipe *pipe = to_rpm_pipe(np);
	u32 tail;

	tail = readl(pipe->tail);

	tail += count;
	if (tail > pipe->length)
		tail -= pipe->length;

	writel(tail, pipe->tail);
}

static size_t glink_rpm_tx_avail(struct qcom_glink_native_pipe *np)
{
	struct glink_rpm_pipe *pipe = to_rpm_pipe(np);
	u32 head;
	u32 tail;

	head = readl(pipe->head);
	tail = readl(pipe->tail);

	if (tail <= head)
		return pipe->length - head + tail;
	else
		return tail - head;
}

static void glink_rpm_tx_write(struct qcom_glink_native_pipe *np,
			       const void *data, size_t count)
{
	struct glink_rpm_pipe *pipe = to_rpm_pipe(np);
	size_t len;
	u32 head;

	head = readl(pipe->head);

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

	writel(head, pipe->head);
}

static int glink_rpm_parse_toc(struct device *dev,
			       void __iomem *msg_ram,
			       size_t msg_ram_size,
			       struct glink_rpm_pipe *rx,
			       struct glink_rpm_pipe *tx)
{
	struct rpm_toc *toc;
	int num_entries;
	u32 offset;
	void *buf;
	u32 size;
	u32 id;
	int i;

	buf = kzalloc(RPM_TOC_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	__ioread32_copy(buf, msg_ram + msg_ram_size - RPM_TOC_SIZE,
			RPM_TOC_SIZE / sizeof(u32));

	toc = buf;

	if (le32_to_cpu(toc->magic) != RPM_TOC_MAGIC) {
		dev_err(dev, "rpm toc has invalid magic\n");
		goto err_inval;
	}

	num_entries = le32_to_cpu(toc->count);
	if (num_entries > RPM_TOC_MAX_ENTRIES) {
		dev_err(dev, "invalid number of toc entries\n");
		goto err_inval;
	}

	dev_dbg(dev, "id         offset   sizex\n");
	for (i = 0; i < num_entries; i++) {
		id = le32_to_cpu(toc->entries[i].id);
		offset = le32_to_cpu(toc->entries[i].offset);
		size = le32_to_cpu(toc->entries[i].size);

		dev_dbg(dev, "0x%08x 0x%5x 0x%5x\n", id, offset, size);

		if (offset > msg_ram_size || offset + size > msg_ram_size) {
			dev_err(dev, "toc entry with invalid size\n");
			continue;
		}

		switch (id) {
		case RPM_RX_FIFO_ID:
			rx->length = size;

			rx->tail = msg_ram + offset;
			rx->head = msg_ram + offset + sizeof(u32);
			rx->fifo = msg_ram + offset + 2 * sizeof(u32);
			break;
		case RPM_TX_FIFO_ID:
			tx->length = size;

			tx->tail = msg_ram + offset;
			tx->head = msg_ram + offset + sizeof(u32);
			tx->fifo = msg_ram + offset + 2 * sizeof(u32);
			break;
		};
	}

	if (!rx->fifo || !tx->fifo) {
		dev_err(dev, "unable to find rx and tx descriptors\n");
		goto err_inval;
	}

	kfree(buf);
	return 0;

err_inval:
	kfree(buf);
	return -EINVAL;
}

static int qcom_glink_rpm_probe(struct platform_device *pdev)
{
	struct glink_rpm_pipe *rx_pipe;
	struct glink_rpm_pipe *tx_pipe;
	struct qcom_glink *glink;
	struct device_node *np;
	void __iomem *msg_ram;
	size_t msg_ram_size;
	struct resource r;
	int ret;

	rx_pipe = devm_kzalloc(&pdev->dev, sizeof(*rx_pipe), GFP_KERNEL);
	tx_pipe = devm_kzalloc(&pdev->dev, sizeof(*tx_pipe), GFP_KERNEL);
	if (!rx_pipe || !tx_pipe)
		return -ENOMEM;

	np = of_parse_phandle(pdev->dev.of_node, "qcom,rpm-msg-ram", 0);
	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return ret;

	msg_ram = devm_ioremap(&pdev->dev, r.start, resource_size(&r));
	msg_ram_size = resource_size(&r);
	if (!msg_ram)
		return -ENOMEM;

	ret = glink_rpm_parse_toc(&pdev->dev, msg_ram, msg_ram_size,
				  rx_pipe, tx_pipe);
	if (ret)
		return ret;

	rx_pipe->native.avail = glink_rpm_rx_avail;
	rx_pipe->native.peak = glink_rpm_rx_peak;
	rx_pipe->native.advance = glink_rpm_rx_advance;

	tx_pipe->native.avail = glink_rpm_tx_avail;
	tx_pipe->native.write = glink_rpm_tx_write;

	writel(0, tx_pipe->head);
	writel(0, rx_pipe->tail);

	glink = qcom_glink_native_probe(&pdev->dev,
					&rx_pipe->native, &tx_pipe->native,
					tx_pipe->length);
	if (IS_ERR(glink))
		return PTR_ERR(glink);

	platform_set_drvdata(pdev, glink);

	return 0;
}

static int qcom_glink_rpm_remove(struct platform_device *pdev)
{
	struct qcom_glink *glink = platform_get_drvdata(pdev);

	qcom_glink_native_remove(glink);

	return 0;
}

static const struct of_device_id qcom_glink_rpm_of_match[] = {
	{ .compatible = "qcom,glink-rpm" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_glink_rpm_of_match);

static struct platform_driver qcom_glink_rpm_driver = {
	.probe = qcom_glink_rpm_probe,
	.remove = qcom_glink_rpm_remove,
	.driver = {
		.name = "qcom-glink_rpm",
		.of_match_table = qcom_glink_rpm_of_match,
	},
};

static int __init qcom_glink_rpm_init(void)
{
	return platform_driver_register(&qcom_glink_rpm_driver);
}
subsys_initcall(qcom_glink_rpm_init);

static void __exit qcom_glink_rpm_exit(void)
{
	platform_driver_unregister(&qcom_glink_rpm_driver);
}
module_exit(qcom_glink_rpm_exit);

MODULE_AUTHOR("Bjorn Andersson <bjorn.andersson@linaro.org>");
MODULE_DESCRIPTION("Qualcomm GLINK RPM driver");
MODULE_LICENSE("GPL v2");
