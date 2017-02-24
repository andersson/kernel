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

#include "rpmsg_internal.h"

#define RPM_TOC_SIZE		256
#define RPM_TOC_MAGIC		0x67727430 /* grt0 */
#define RPM_TOC_MAX_ENTRIES	((RPM_TOC_SIZE - sizeof(struct rpm_toc)) / \
				 sizeof(struct rpm_toc_entry))

#define RPM_TX_FIFO_ID		0x61703272 /* ap2r */
#define RPM_RX_FIFO_ID		0x72326170 /* r2ap */

#define GLINK_NAME_SIZE		32

#define RPM_GLINK_CID_MIN	1
#define RPM_GLINK_CID_MAX	65536

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
	void __iomem *tail;
	void __iomem *head;

	void __iomem *fifo;

	size_t length;
};

struct glink_defer_cmd {
	struct list_head node;

	struct glink_cmd cmd;
};

struct glink_rpm {
	struct device *dev;

	void __iomem *msg_ram;
	size_t msg_ram_size;

	struct regmap *ipc_regmap;
	unsigned int ipc_offset;
	unsigned int ipc_bit;

	struct glink_rpm_pipe rx_pipe;
	struct glink_rpm_pipe tx_pipe;

	int irq;

	struct work_struct rx_work;
	spinlock_t rx_lock;
	struct list_head rx_queue;

	struct mutex tx_lock;

	struct mutex idr_lock;
	struct idr lcids;
	struct idr rcids;
};

enum {
	GLINK_STATE_CLOSED,
	GLINK_STATE_OPENING,
	GLINK_STATE_OPEN,
	GLINK_STATE_CLOSING,
};

struct glink_channel {
	struct rpmsg_device rpdev;

	struct rpmsg_endpoint ept;

	struct glink_rpm *glink;

	spinlock_t recv_lock;

	char *name;
	u16 lcid;
	u16 rcid;

	void *buf;
	int buf_offset;
	int buf_size;

	struct completion open_ack;
	struct completion open_req;
};

#define to_glink_channel(_rpdev) container_of(_rpdev, struct glink_channel, rpdev)

static const struct rpmsg_endpoint_ops glink_endpoint_ops;

enum command_types {
	RPM_CMD_VERSION,
	RPM_CMD_VERSION_ACK,
	RPM_CMD_OPEN,
	RPM_CMD_CLOSE,
	RPM_CMD_OPEN_ACK,
	RPM_CMD_RX_INTENT,
	RPM_CMD_RX_DONE,
	RPM_CMD_RX_INTENT_REQ,
	RPM_CMD_RX_INTENT_REQ_ACK,
	RPM_CMD_TX_DATA,
	RPM_CMD_ZERO_COPY_TX_DATA,
	RPM_CMD_CLOSE_ACK,
	RPM_CMD_TX_DATA_CONT,
	RPM_CMD_READ_NOTIF,
	RPM_CMD_RX_DONE_W_REUSE,
	RPM_CMD_SIGNALS,
	RPM_CMD_TRACER_PKT,
	RPM_CMD_TRACER_PKT_CONT,
};

#define GLINK_FEATURE_SIGNALS            BIT(0)
#define GLINK_FEATURE_INTENTLESS         BIT(1)
#define GLINK_FEATURE_TRACER_PKT         BIT(2)
#define GLINK_FEATURE_AUTO_QUEUE_RX_INT  BIT(3)

static int glink_rpm_open_ack(struct glink_rpm *glink, u16 lcid);
static struct glink_channel *glink_rpm_alloc_channel(struct glink_rpm *glink,
						     const char *name);

static size_t glink_rpm_rx_avail(struct glink_rpm *glink)
{
	struct glink_rpm_pipe *pipe = &glink->rx_pipe;
	u32 head;
	u32 tail;

	head = readl(pipe->head);
	tail = readl(pipe->tail);

	if (head < tail)
		return pipe->length - tail + head;
	else
		return head - tail;
}

static void glink_rpm_rx_peak(struct glink_rpm *glink,
			      void *data, size_t count)
{
	struct glink_rpm_pipe *pipe = &glink->rx_pipe;
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

static void glink_rpm_rx_advance(struct glink_rpm *glink,
				 size_t count)
{
	struct glink_rpm_pipe *pipe = &glink->rx_pipe;
	u32 tail;

	tail = readl(pipe->tail);

	tail += count;
	if (tail > pipe->length)
		tail -= pipe->length;

	writel(tail, pipe->tail);
}

static size_t glink_rpm_tx_avail(struct glink_rpm *glink)
{
	struct glink_rpm_pipe *pipe = &glink->tx_pipe;
	u32 head;
	u32 tail;

	head = readl(pipe->head);
	tail = readl(pipe->tail);

	if (tail <= head)
		return pipe->length - head + tail;
	else
		return tail - head;
}

static void glink_rpm_tx_write(struct glink_rpm *glink,
			       const void *data, size_t count)
{
	struct glink_rpm_pipe *pipe = &glink->tx_pipe;
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

static void glink_rpm_kick(struct glink_rpm *glink)
{
	wmb();
	regmap_write(glink->ipc_regmap, glink->ipc_offset, BIT(glink->ipc_bit));
}

static int glink_rpm_tx(struct glink_rpm *glink, const void *data,
			     size_t len, bool wait)
{
	int ret;

	/* Reject packets that are too big */
	if (len >= glink->tx_pipe.length)
		return -EINVAL;

	ret = mutex_lock_interruptible(&glink->tx_lock);
	if (ret)
		return ret;

	while (glink_rpm_tx_avail(glink) < len) {
		if (!wait) {
			ret = -ENOMEM;
			goto out;
		}

		msleep(10);
	}

	glink_rpm_tx_write(glink, data, len);
	glink_rpm_kick(glink);

out:
	mutex_unlock(&glink->tx_lock);

	return ret;
}

static int glink_rpm_send_version(struct glink_rpm *glink)
{
	struct glink_cmd cmd;

	dev_dbg(glink->dev, "%s()\n", __func__);

	cmd.cmd = cpu_to_le16(RPM_CMD_VERSION);
	cmd.param1 = cpu_to_le16(1);
	cmd.param2 = cpu_to_le32(GLINK_FEATURE_INTENTLESS);

	return glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void glink_rpm_send_version_ack(struct glink_rpm *glink)
{
	struct glink_cmd cmd;

	dev_dbg(glink->dev, "%s(%d)\n", __func__, 1);

	cmd.cmd = cpu_to_le16(RPM_CMD_VERSION_ACK);
	cmd.param1 = cpu_to_le16(1);
	cmd.param2 = cpu_to_le32(0);

	glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void glink_rpm_send_open_ack(struct glink_rpm *glink,
					 struct glink_channel *channel)
{
	struct glink_cmd cmd;

	dev_dbg(glink->dev, "%s(%s)\n", __func__, channel->name);

	cmd.cmd = cpu_to_le16(RPM_CMD_OPEN_ACK);
	cmd.param1 = cpu_to_le16(channel->rcid);
	cmd.param2 = cpu_to_le32(0);

	glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static int glink_rpm_send_open_req(struct glink_rpm *glink,
					 struct glink_channel *channel)
{
	struct {
		struct glink_cmd cmd;
		u8 name[GLINK_NAME_SIZE];
	} __packed req;
	int name_len = strlen(channel->name) + 1;
	int req_len = ALIGN(sizeof(req.cmd) + name_len, 8);
	int ret;

	dev_dbg(glink->dev, "%s(%s)\n", __func__, channel->name);

	mutex_lock(&glink->idr_lock);
	ret = idr_alloc_cyclic(&glink->lcids, channel,
			       RPM_GLINK_CID_MIN, RPM_GLINK_CID_MAX, GFP_KERNEL);
	mutex_unlock(&glink->idr_lock);
	if (ret < 0)
		return ret;

	channel->lcid = ret;

	req.cmd.cmd = cpu_to_le16(RPM_CMD_OPEN);
	req.cmd.param1 = cpu_to_le16(channel->lcid);
	req.cmd.param2 = cpu_to_le32(name_len);
	strcpy(req.name, channel->name);

	return glink_rpm_tx(glink, &req, req_len, true);
}

static void glink_rpm_send_close_req(struct glink_rpm *glink,
					  struct glink_channel *channel)
{
	struct glink_cmd req;

	req.cmd = cpu_to_le16(RPM_CMD_CLOSE);
	req.param1 = cpu_to_le16(channel->lcid);
	req.param2 = 0;

	glink_rpm_tx(glink, &req, sizeof(req), true);
}

static int glink_rpm_rx_defer(struct glink_rpm *glink, size_t extra)
{
	struct glink_defer_cmd *dcmd;

	extra = ALIGN(extra, 8);

	if (glink_rpm_rx_avail(glink) < sizeof(struct glink_cmd) + extra) {
		dev_dbg(glink->dev, "insufficient data in rx fifo");
		return -ENXIO;
	}

	dcmd = kzalloc(sizeof(*dcmd) + extra, GFP_ATOMIC);
	if (!dcmd)
		return -ENOMEM;

	INIT_LIST_HEAD(&dcmd->node);

	glink_rpm_rx_peak(glink, &dcmd->cmd, sizeof(dcmd->cmd) + extra);

	spin_lock(&glink->rx_lock);
	list_add_tail(&dcmd->node, &glink->rx_queue);
	spin_unlock(&glink->rx_lock);

	schedule_work(&glink->rx_work);
	glink_rpm_rx_advance(glink, sizeof(dcmd->cmd) + extra);

	return 0;
}

static int glink_rpm_rx_data(struct glink_rpm *glink, size_t avail)
{
	struct glink_channel *channel;
	struct {
		struct glink_cmd cmd;
		u32 chunk_size;
		u32 left_size;
		u8 data[];
	} __packed hdr;
	unsigned int chunk_size;
	unsigned int left_size;
	u16 rcid;
	int ret;

	if (avail < sizeof(hdr)) {
		dev_dbg(glink->dev, "not enough data in fifo\n");
		return -EAGAIN;
	}

	glink_rpm_rx_peak(glink, &hdr, sizeof(hdr));
	chunk_size = le32_to_cpu(hdr.chunk_size);
	left_size = le32_to_cpu(hdr.left_size);

	if (avail < sizeof(hdr) + chunk_size) {
		dev_dbg(glink->dev, "payload not yet in fifo\n");
		return -EAGAIN;
	}

	rcid = le16_to_cpu(hdr.cmd.param1);
	channel = idr_find(&glink->rcids, rcid);
	if (!channel) {
		dev_dbg(glink->dev, "data on non-existing channel\n");
		return -EINVAL;
	}

	if (!channel->buf) {
		channel->buf = kmalloc(chunk_size + left_size, GFP_ATOMIC);
		if (!channel->buf)
			return -ENOMEM;

		channel->buf_size = chunk_size + left_size;
		channel->buf_offset = 0;
	}

	glink_rpm_rx_advance(glink, ALIGN(sizeof(hdr), 8));

	if (channel->buf_size - channel->buf_offset < chunk_size) {
		dev_err(glink->dev, "insufficient space in input buffer\n");
		glink_rpm_rx_advance(glink, ALIGN(chunk_size, 8));
		return -ENOMEM;
	}

	glink_rpm_rx_peak(glink, channel->buf + channel->buf_offset, chunk_size);
	channel->buf_offset += chunk_size;

	if (!left_size) {
		spin_lock(&channel->recv_lock);
		if (channel->ept.cb) {
			ret = channel->ept.cb(&channel->rpdev,
					      channel->buf,
					      channel->buf_offset,
					      channel->ept.priv,
					      RPMSG_ADDR_ANY);
		}
		spin_unlock(&channel->recv_lock);

		kfree(channel->buf);
		channel->buf = NULL;
		channel->buf_size = 0;
	}

	glink_rpm_rx_advance(glink, ALIGN(chunk_size, 8));

	return 0;
}

static irqreturn_t glink_rpm_intr(int irq, void *data)
{
	struct glink_rpm *glink = data;
	struct glink_cmd cmd;
	u32 avail;
	int ret;

	for (;;) {
		avail = glink_rpm_rx_avail(glink);
		if (avail < sizeof(cmd))
			break;

		glink_rpm_rx_peak(glink, &cmd, sizeof(cmd));

		switch (cmd.cmd) {
		case RPM_CMD_VERSION:
		case RPM_CMD_VERSION_ACK:
			ret = glink_rpm_rx_defer(glink, 0);
			break;
		case RPM_CMD_OPEN_ACK:
			ret = glink_rpm_open_ack(glink, cmd.param1);
			glink_rpm_rx_advance(glink, ALIGN(sizeof(cmd), 8));
			break;
		case RPM_CMD_OPEN:
			ret = glink_rpm_rx_defer(glink, cmd.param2);
			break;
		case RPM_CMD_TX_DATA:
		case RPM_CMD_TX_DATA_CONT:
			ret = glink_rpm_rx_data(glink, avail);
			if (ret)
				dev_err(glink->dev, "rx data failed\n");
			break;
		case RPM_CMD_READ_NOTIF:
			glink_rpm_rx_advance(glink, ALIGN(sizeof(cmd), 8));
			glink_rpm_kick(glink);

			ret = 0;
			break;
		default:
			dev_err(glink->dev, "unhandled rx cmd: %d\n", cmd.cmd);
			ret = -EINVAL;
			break;
		}

		if (ret)
			break;
	}

	return IRQ_HANDLED;
}

static struct rpmsg_endpoint *glink_rpm_create_ept(struct rpmsg_device *rpdev,
						  rpmsg_rx_cb_t cb, void *priv,
						  struct rpmsg_channel_info chinfo)
{
	struct glink_channel *parent = to_glink_channel(rpdev);
	struct glink_channel *channel;
	struct glink_rpm *glink = parent->glink;
	struct rpmsg_endpoint *ept;
	const char *name = chinfo.name;
	int cid;
	int ret;

	idr_for_each_entry(&glink->rcids, channel, cid) {
		if (!strcmp(channel->name, name))
			break;
	}

	if (!channel) {
		channel = glink_rpm_alloc_channel(glink, name);
		if (!channel)
			return NULL;

		ret = glink_rpm_send_open_req(glink, channel);
		if (ret)
			return NULL;

		ret = wait_for_completion_timeout(&channel->open_ack, 5 * HZ);
		if (!ret) {
			dev_err(glink->dev, "TIMEOUT!\n");
			return NULL;
		}

		ret = wait_for_completion_timeout(&channel->open_req, 5 * HZ);
		if (!ret) {
			dev_err(glink->dev, "TIMEOUT!\n");
			return NULL;
		}

		glink_rpm_send_open_ack(glink, channel);
	} else {
		glink_rpm_send_open_ack(glink, channel);

		ret = glink_rpm_send_open_req(glink, channel);
		if (ret)
			return NULL;

		dev_err(glink->dev, "waiting for ack on %d\n", channel->lcid);
		ret = wait_for_completion_timeout(&channel->open_ack, 5 * HZ);
		if (!ret) {
			dev_err(glink->dev, "TIMEOUT!\n");
			return NULL;
		}
	}

	ept = &channel->ept;
	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;
	ept->ops = &glink_endpoint_ops;

	return ept;
}

static void glink_rpm_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct glink_channel *channel = to_glink_channel(ept->rpdev);
	struct glink_rpm *glink = channel->glink;
	unsigned long flags;

	spin_lock_irqsave(&channel->recv_lock, flags);
	channel->ept.cb = NULL;
	spin_unlock_irqrestore(&channel->recv_lock, flags);

	glink_rpm_send_close_req(glink, channel);
}

static int __glink_rpm_send(struct glink_channel *channel,
			     void *data, int len, bool wait)
{
	struct glink_rpm *glink = channel->glink;
	struct {
		struct glink_cmd cmd;
		u32 chunk_size;
		u32 left_size;
		u8 data[];
	} __packed *req;
	int req_len = ALIGN(sizeof(*req) + len, 8);
	int ret;

	dev_err(glink->dev, "%s(%d)\n", __func__, len);

	req = kzalloc(req_len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->cmd.cmd = cpu_to_le16(RPM_CMD_TX_DATA);
	req->cmd.param1 = cpu_to_le16(channel->lcid);
	req->cmd.param2 = cpu_to_le32(channel->rcid);
	req->chunk_size = cpu_to_le32(len);
	req->left_size = cpu_to_le32(0);

	memcpy(req->data, data, len);

	ret = glink_rpm_tx(glink, req, req_len, wait);
	kfree(req);

	return ret;
}

static int glink_rpm_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_channel *channel = to_glink_channel(ept->rpdev);

	return __glink_rpm_send(channel, data, len, true);
}

static int glink_rpm_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_channel *channel = to_glink_channel(ept->rpdev);

	return __glink_rpm_send(channel, data, len, false);
}

/*
 * Finds the device_node for the glink child interested in this channel.
 */
static struct device_node *glink_rpm_match_channel(struct device_node *node,
						    const char *channel)
{
	struct device_node *child;
	const char *name;
	const char *key;
	int ret;

	for_each_available_child_of_node(node, child) {
		key = "qcom,glink-channels";
		ret = of_property_read_string(child, key, &name);
		if (ret)
			continue;

		if (strcmp(name, channel) == 0)
			return child;
	}

	return NULL;
}

static const struct rpmsg_device_ops glink_device_ops = {
	.create_ept = glink_rpm_create_ept,
};

static const struct rpmsg_endpoint_ops glink_endpoint_ops = {
	.destroy_ept = glink_rpm_destroy_ept,
	.send = glink_rpm_send,
	.trysend = glink_rpm_trysend,
};

static void glink_rpm_release_device(struct device *dev)
{
	struct rpmsg_device *rpdev = to_rpmsg_device(dev);
	struct glink_channel *channel = to_glink_channel(rpdev);
	struct glink_rpm *glink = channel->glink;

	idr_remove(&glink->lcids, channel->lcid);
	idr_remove(&glink->rcids, channel->rcid);

	kfree(channel->name);
	kfree(channel);
}

static struct glink_channel *glink_rpm_alloc_channel(struct glink_rpm *glink,
						     const char *name)
{
	struct rpmsg_device *rpdev;
	struct glink_channel *channel;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return ERR_PTR(-ENOMEM);

	/* Setup glink internal glink_channel data */
	spin_lock_init(&channel->recv_lock);
	channel->glink = glink;
	channel->name = kstrdup(name, GFP_KERNEL);

	init_completion(&channel->open_req);
	init_completion(&channel->open_ack);

	/* Assign public information to the rpmsg_device */
	rpdev = &channel->rpdev;
	strncpy(rpdev->id.name, name, RPMSG_NAME_SIZE);
	rpdev->src = RPMSG_ADDR_ANY;
	rpdev->dst = RPMSG_ADDR_ANY;
	rpdev->ops = &glink_device_ops;

	rpdev->dev.of_node = glink_rpm_match_channel(glink->dev->of_node, name);
	rpdev->dev.parent = glink->dev;
	rpdev->dev.release = glink_rpm_release_device;

	return channel;
}

static int glink_rpm_open(struct glink_rpm *glink, u16 rcid, char *name)
{
	struct glink_channel *channel;
	bool create_device = false;
	int lcid;
	int ret;

	dev_dbg(glink->dev, "%s(%d, %s)\n", __func__, rcid, name);

	idr_for_each_entry(&glink->lcids, channel, lcid) {
		if (!strcmp(channel->name, name))
			break;
	}

	if (!channel) {
		channel = glink_rpm_alloc_channel(glink, name);
		if (IS_ERR(channel))
			return PTR_ERR(channel);

		create_device = true;
	}

	mutex_lock(&glink->idr_lock);
	ret = idr_alloc(&glink->rcids, channel, rcid, rcid + 1, GFP_KERNEL);
	if (ret < 0) {
		dev_err(glink->dev, "unable to insert channel into rcid list\n");
		mutex_unlock(&glink->idr_lock);
		goto free_channel;
	}
	channel->rcid = ret;
	mutex_unlock(&glink->idr_lock);

	complete(&channel->open_req);

	if (create_device) {
		ret = rpmsg_register_device(&channel->rpdev);
		if (ret)
			goto rcid_remove;
	}

	return 0;

rcid_remove:
	idr_remove(&glink->rcids, channel->rcid);
free_channel:
	kfree(channel);

	return ret;
}

static int glink_rpm_open_ack(struct glink_rpm *glink, u16 lcid)
{
	struct glink_channel *channel;

	channel = idr_find(&glink->lcids, lcid);
	if (!channel) {
		dev_err(glink->dev, "invalid open ack packet\n");
		return -EINVAL;
	}

	complete(&channel->open_ack);

	dev_dbg(glink->dev, "%s (%d) is now fully open\n", channel->name, channel->lcid);

	return 0;
}

static void glink_rpm_work(struct work_struct *work)
{
	struct glink_rpm *glink = container_of(work, struct glink_rpm, rx_work);
	struct glink_defer_cmd *dcmd;
	struct glink_cmd *cmd;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&glink->rx_lock, flags);
		if (list_empty(&glink->rx_queue)) {
			spin_unlock_irqrestore(&glink->rx_lock, flags);
			break;
		}
		dcmd = list_first_entry(&glink->rx_queue, struct glink_defer_cmd, node);
		list_del(&dcmd->node);
		spin_unlock_irqrestore(&glink->rx_lock, flags);

		cmd = &dcmd->cmd;
		switch (cmd->cmd) {
		case RPM_CMD_VERSION:
			glink_rpm_send_version_ack(glink);
			break;
		case RPM_CMD_VERSION_ACK:
			break;
		case RPM_CMD_OPEN:
			glink_rpm_open(glink, cmd->param1, cmd->data);
			break;
		default:
			dev_err(glink->dev, "unknown defer: %d!\n", cmd->cmd);
			break;
		}

		kfree(dcmd);
	}
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
		}
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

static int glink_rpm_probe(struct platform_device *pdev)
{
	struct of_phandle_args args;
	struct glink_rpm *glink;
	struct device_node *np;
	void __iomem *msg_ram;
	size_t msg_ram_size;
	struct device *dev = &pdev->dev;
	struct resource r;
	int irq;
	int ret;

	glink = devm_kzalloc(dev, sizeof(*glink), GFP_KERNEL);
	if (!glink)
		return -ENOMEM;

	glink->dev = dev;

	mutex_init(&glink->tx_lock);
	spin_lock_init(&glink->rx_lock);
	INIT_LIST_HEAD(&glink->rx_queue);
	INIT_WORK(&glink->rx_work, glink_rpm_work);

	mutex_init(&glink->idr_lock);
	idr_init(&glink->lcids);
	idr_init(&glink->rcids);

	ret = of_parse_phandle_with_fixed_args(dev->of_node,
					       "qcom,ipc", 2, 0,
					       &args);
	if (ret < 0) {
		dev_err(dev, "failed to parse qcom,ipc\n");
		return ret;
	}

	glink->ipc_offset = args.args[0];
	glink->ipc_bit = args.args[1];

	glink->ipc_regmap = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(glink->ipc_regmap))
		return PTR_ERR(glink->ipc_regmap);

	np = of_parse_phandle(dev->of_node, "qcom,rpm-msg-ram", 0);
	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return ret;

	msg_ram = devm_ioremap(dev, r.start, resource_size(&r));
	msg_ram_size = resource_size(&r);
	if (!msg_ram)
		return -ENOMEM;

	ret = glink_rpm_parse_toc(dev, msg_ram, msg_ram_size,
				  &glink->rx_pipe, &glink->tx_pipe);
	if (ret)
		return ret;

	writel(0, glink->tx_pipe.head);
	writel(0, glink->rx_pipe.tail);

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, irq,
			       glink_rpm_intr,
			       IRQF_NO_SUSPEND |IRQF_SHARED,
			       "glink-rpm", glink);
	if (ret) {
		dev_err(dev, "failed to request IRQ\n");
		return ret;
	}

	glink->irq = irq;

	ret = glink_rpm_send_version(glink);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, glink);

	return 0;
}

static int glink_rpm_remove_device(struct device *dev, void *data)
{
	device_unregister(dev);

	return 0;
}

static int glink_rpm_remove(struct platform_device *pdev)
{
	struct glink_rpm *glink = platform_get_drvdata(pdev);
	int ret;

	disable_irq(glink->irq);
	cancel_work_sync(&glink->rx_work);

	ret = device_for_each_child(glink->dev, NULL, glink_rpm_remove_device);
	if (ret)
		dev_warn(glink->dev, "can't remove glink devices: %d\n", ret);

	idr_destroy(&glink->lcids);
	idr_destroy(&glink->rcids);

	return 0;
}

static const struct of_device_id glink_rpm_of_match[] = {
	{ .compatible = "qcom,glink-rpm" },
	{}
};
MODULE_DEVICE_TABLE(of, glink_rpm_of_match);

static struct platform_driver glink_rpm_driver = {
	.probe = glink_rpm_probe,
	.remove = glink_rpm_remove,
	.driver = {
		.name = "qcom_glink_rpm",
		.of_match_table = glink_rpm_of_match,
	},
};

static int __init glink_rpm_init(void)
{
	return platform_driver_register(&glink_rpm_driver);
}
subsys_initcall(glink_rpm_init);

static void __exit glink_rpm_exit(void)
{
	platform_driver_unregister(&glink_rpm_driver);
}
module_exit(glink_rpm_exit);

MODULE_AUTHOR("Bjorn Andersson <bjorn.andersson@linaro.org>");
MODULE_DESCRIPTION("Qualcomm GLINK RPM driver");
MODULE_LICENSE("GPL v2");
