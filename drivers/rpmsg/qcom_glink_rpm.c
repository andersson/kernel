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

	wait_queue_head_t new_channel_event;

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

struct glink_endpoint;

struct glink_channel {
	struct rpmsg_device rpdev;

	struct list_head node;

	struct glink_rpm *glink;
	struct glink_endpoint *glink_ept;

	int state;

	spinlock_t recv_lock;

	char *name;
	u16 lcid;
	u16 rcid;
};

struct glink_endpoint {
	struct rpmsg_endpoint ept;
	struct glink_channel *channel;
	struct glink_rpm *glink;
};

#define to_glink_channel(_rpdev) container_of(_rpdev, struct glink_channel, rpdev)
#define to_glink_endpoint(ept) container_of(ept, struct glink_endpoint, ept)

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

static void glink_rpm_send_open_req(struct glink_rpm *glink,
					 struct glink_channel *channel)
{
	struct {
		struct glink_cmd cmd;
		u8 name[GLINK_NAME_SIZE];
	} __packed req;

	int name_len = strlen(channel->name) + 1;
	int req_len = ALIGN(sizeof(req.cmd) + name_len, 8);

	dev_dbg(glink->dev, "%s(%s)\n", __func__, channel->name);

	req.cmd.cmd = cpu_to_le16(RPM_CMD_OPEN);
	req.cmd.param1 = cpu_to_le16(channel->lcid);
	req.cmd.param2 = cpu_to_le32(name_len);
	strcpy(req.name, channel->name);

	glink_rpm_tx(glink, &req, req_len, true);
}

static void glink_rpm_send_close_req(struct glink_rpm *glink,
					  struct glink_channel *channel)
{
	struct glink_cmd req;

	req.cmd = cpu_to_le16(RPM_CMD_OPEN);
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
	struct rpmsg_endpoint *ept;
	struct {
		struct glink_cmd cmd;
		u32 chunk_size;
		u32 left_size;
		u8 data[];
	} __packed hdr, *req;
	unsigned int chunk_size;
	unsigned int left_size;
	int req_len;
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

	dev_dbg(glink->dev, "%s(rcid: %d, chunk: %d, left: %d)\n",
		 __func__, rcid, chunk_size, left_size);

	req_len = sizeof(hdr) + chunk_size;
	req = kmalloc(req_len, GFP_ATOMIC);
	if (!req)
		return -ENOMEM;

	glink_rpm_rx_peak(glink, req, req_len);

	spin_lock(&channel->recv_lock);
	if (channel->glink_ept) {
		ept = &channel->glink_ept->ept;
		ret = ept->cb(ept->rpdev, req->data, chunk_size,
			      ept->priv, RPMSG_ADDR_ANY);
	}
	spin_unlock(&channel->recv_lock);

	glink_rpm_rx_advance(glink, ALIGN(req_len, 8));

	kfree(req);
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
		case RPM_CMD_OPEN_ACK:
			ret = glink_rpm_rx_defer(glink, 0);
			break;
		case RPM_CMD_OPEN:
			ret = glink_rpm_rx_defer(glink, cmd.param2);
			break;
		case RPM_CMD_TX_DATA:
		case RPM_CMD_TX_DATA_CONT:
			ret = glink_rpm_rx_data(glink, avail);
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

static struct glink_channel *
glink_find_channel(struct glink_rpm *glink, const char *name)
{
	struct glink_channel *channel;
	int lcid;

	idr_for_each_entry(&glink->lcids, channel, lcid) {
		if (!strcmp(channel->name, name))
			break;
	}

	return channel;
}

static void __ept_release(struct kref *kref)
{
	struct rpmsg_endpoint *ept = container_of(kref, struct rpmsg_endpoint,
						  refcount);
	kfree(to_glink_endpoint(ept));
}

static struct rpmsg_endpoint *glink_rpm_create_ept(struct rpmsg_device *rpdev,
						  rpmsg_rx_cb_t cb, void *priv,
						  struct rpmsg_channel_info chinfo)
{
	struct glink_endpoint *glink_ept;
	struct glink_channel *parent = to_glink_channel(rpdev);
	struct glink_channel *channel;
	struct glink_rpm *glink = parent->glink;
	struct rpmsg_endpoint *ept;
	const char *name = chinfo.name;
	int ret;

	/* Wait up to HZ for the channel to appear */
	ret = wait_event_interruptible_timeout(glink->new_channel_event,
			(channel = glink_find_channel(glink, name)) != NULL,
			HZ);
	if (!ret)
		return ERR_PTR(-ETIMEDOUT);

	if (channel->state != GLINK_STATE_CLOSED) {
		dev_err(&rpdev->dev, "channel %s is busy\n", channel->name);
		return ERR_PTR(-EBUSY);
	}

	glink_ept = kzalloc(sizeof(*glink_ept), GFP_KERNEL);
	if (!glink_ept)
		return ERR_PTR(-ENOMEM);

	ept = &glink_ept->ept;

	kref_init(&ept->refcount);

	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;
	ept->ops = &glink_endpoint_ops;

	channel->glink_ept = glink_ept;
	glink_ept->channel = channel;
	glink_ept->glink = glink;

	glink_rpm_send_open_ack(glink, channel);

	glink_rpm_send_open_req(glink, channel);
	channel->state = GLINK_STATE_OPENING;

	return ept;
}

static void glink_rpm_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);
	struct glink_channel *channel = glink_ept->channel;
	struct glink_rpm *glink = glink_ept->glink;
	unsigned long flags;

	spin_lock_irqsave(&channel->recv_lock, flags);
	glink_ept->ept.cb = NULL;
	spin_unlock_irqrestore(&channel->recv_lock, flags);

	glink_rpm_send_close_req(glink, channel);

	channel->state = GLINK_STATE_CLOSING;
	channel->glink_ept = NULL;
	kref_put(&ept->refcount, __ept_release);
}

static int __glink_rpm_send(struct glink_endpoint *glink_ept,
			     void *data, int len, bool wait)
{
	struct glink_channel *channel = glink_ept->channel;
	struct glink_rpm *glink = glink_ept->glink;
	struct {
		struct glink_cmd cmd;
		u32 chunk_size;
		u32 left_size;
		u8 data[];
	} __packed *req;
	int req_len = ALIGN(sizeof(*req) + len, 8);
	int ret;

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
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);

	dev_dbg(glink_ept->glink->dev, "%s(%d)\n", __func__, len);

	return __glink_rpm_send(glink_ept, data, len, true);
}

static int glink_rpm_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);

	dev_dbg(glink_ept->glink->dev, "%s(%d)\n", __func__, len);

	return __glink_rpm_send(glink_ept, data, len, false);
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

static int glink_rpm_open(struct glink_rpm *glink, u16 rcid, char *name)
{
	struct rpmsg_device *rpdev;
	struct glink_channel *ch;
	int ret;

	dev_dbg(glink->dev, "%s(%d, %s)\n", __func__, rcid, name);

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return -ENOMEM;

	/* Setup glink internal glink_channel data */
	spin_lock_init(&ch->recv_lock);
	ch->glink = glink;
	ch->name = kstrdup(name, GFP_KERNEL);
	ch->state = GLINK_STATE_CLOSED;

	/* Assign public information to the rpmsg_device */
	rpdev = &ch->rpdev;
	strncpy(rpdev->id.name, name, RPMSG_NAME_SIZE);
	rpdev->src = RPMSG_ADDR_ANY;
	rpdev->dst = RPMSG_ADDR_ANY;
	rpdev->ops = &glink_device_ops;

	rpdev->dev.of_node = glink_rpm_match_channel(glink->dev->of_node, name);
	rpdev->dev.parent = glink->dev;

	mutex_lock(&glink->idr_lock);

	ret = idr_alloc_cyclic(&glink->lcids, ch, RPM_GLINK_CID_MIN, RPM_GLINK_CID_MAX, GFP_KERNEL);
	if (ret < 0) {
		dev_err(glink->dev, "failed to allocate local channel id\n");
		goto free_channel;
	}
	ch->lcid = ret;

	ret = idr_alloc(&glink->rcids, ch, rcid, rcid + 1, GFP_KERNEL);
	if (ret < 0) {
		dev_err(glink->dev, "unable to insert channel into rcid list\n");
		goto lcid_remove;
	}
	ch->rcid = ret;

	mutex_unlock(&glink->idr_lock);

	wake_up_interruptible(&glink->new_channel_event);

	ret = rpmsg_register_device(rpdev);
	if (ret)
		goto rcid_remove;

	return 0;

rcid_remove:
	idr_remove(&glink->rcids, ch->rcid);

lcid_remove:
	idr_remove(&glink->lcids, ch->lcid);

	mutex_unlock(&glink->idr_lock);

free_channel:
	kfree(ch);
	return ret;
}

static int glink_rpm_open_ack(struct glink_rpm *glink, u16 lcid)
{
	struct glink_channel *channel;

	channel = idr_find(&glink->lcids, lcid);
	if (!channel || channel->state != GLINK_STATE_OPENING) {
		dev_err(glink->dev, "invalid open ack packet\n");
		return -EINVAL;
	}

	channel->state = GLINK_STATE_OPEN;
	dev_dbg(glink->dev, "%s is now fully open\n", channel->name);

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
		case RPM_CMD_OPEN_ACK:
			glink_rpm_open_ack(glink, cmd->param1);
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

	init_waitqueue_head(&glink->new_channel_event);

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
