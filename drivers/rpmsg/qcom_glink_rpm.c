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

struct glink_channel_pipe {
	__le32 tail;
	__le32 head;
	u8 fifo[];
};

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

	void __iomem *out_irq_reg;

	struct glink_channel_pipe *rx;
	struct glink_channel_pipe *tx;
	size_t rx_size;
	size_t tx_size;

	struct work_struct rx_work;
	spinlock_t rx_lock;
	struct list_head rx_queue;

	struct mutex tx_lock;

	spinlock_t channels_lock;
	struct list_head channels;

	wait_queue_head_t new_channel_event;
};

enum {
	GLINK_STATE_CLOSED,
	GLINK_STATE_OPENING,
	GLINK_STATE_OPEN,
	GLINK_STATE_CLOSING,
};

struct glink_endpoint;

struct glink_channel {
	struct list_head node;

	struct rpmsg_channel rpch;

	struct glink_rpm *glink;
	struct glink_endpoint *glink_ept;

	int remote_state;
	int local_state;

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

static inline struct glink_channel *to_glink_channel(struct device *d)
{
	struct rpmsg_channel *rpch = to_rpmsg_channel(d);
	return container_of(rpch, struct glink_channel, rpch);
}

#define to_glink_endpoint(ept) container_of(ept, struct glink_endpoint, ept)


#define RPM_TOC_SIZE 256
#define RPM_TOC_MAGIC 0x67727430 /* grt0 */
#define RPM_TOC_MAX_ENTRIES ((RPM_TOC_SIZE - sizeof(struct rpm_toc)) / \
			     sizeof(struct rpm_toc_entry))

#define RPM_TX_FIFO_ID 0x61703272 /* ap2r */
#define RPM_RX_FIFO_ID 0x72326170 /* r2ap */

/**
 * enum command_types - definition of the types of commands sent/received
 * @VERSION_CMD:                Version and feature set supported
 * @VERSION_ACK_CMD:            Response for @VERSION_CMD
 * @OPEN_CMD:                   Open a channel
 * @CLOSE_CMD:                  Close a channel
 * @OPEN_ACK_CMD:               Response to @OPEN_CMD
 * @RX_INTENT_CMD:              RX intent for a channel was queued
 * @RX_DONE_CMD:                Use of RX intent for a channel is complete
 * @RX_INTENT_REQ_CMD:          Request to have RX intent queued
 * @RX_INTENT_REQ_ACK_CMD:      Response for @RX_INTENT_REQ_CMD
 * @TX_DATA_CMD:                Start of a data transfer
 * @ZERO_COPY_TX_DATA_CMD:      Start of a data transfer with zero copy
 * @CLOSE_ACK_CMD:              Response for @CLOSE_CMD
 * @TX_DATA_CONT_CMD:           Continuation or end of a data transfer
 * @READ_NOTIF_CMD:             Request for a notification when this cmd is read
 * @RX_DONE_W_REUSE_CMD:        Same as @RX_DONE but also reuse the used intent
 * @SIGNALS_CMD:                Sideband signals
 * @TRACER_PKT_CMD:             Start of a Tracer Packet Command
 * @TRACER_PKT_CONT_CMD:        Continuation or end of a Tracer Packet Command
 */
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

static void qcom_glink_rpm_kick(struct glink_rpm *glink)
{
	wmb();
	regmap_write(glink->ipc_regmap, glink->ipc_offset, BIT(glink->ipc_bit));
}

static size_t qcom_glink_rpm_rx_avail(struct glink_rpm *glink)
{
	u32 head = cpu_to_le32(glink->rx->head);
	u32 tail = cpu_to_le32(glink->rx->tail);

	if (head < tail)
		return glink->rx_size - tail + head;
	else
		return head - tail;
}

static size_t qcom_glink_rpm_tx_avail(struct glink_rpm *glink)
{
	u32 head = cpu_to_le32(glink->tx->head);
	u32 tail = cpu_to_le32(glink->tx->tail);

	if (tail <= head)
		return glink->tx_size - head + tail;
	else
		return tail - head;
}

static void qcom_glink_rpm_peak_fifo(struct glink_rpm *glink,
				     void *data, size_t count)
{
	//u32 head = glink->rx->head;
	u32 tail = glink->rx->tail;
	size_t len;

	len = min_t(size_t, count, glink->rx_size - tail);
	if (len) {
		__ioread32_copy(data, glink->rx->fifo + tail,
				len / sizeof(u32));
	}

	if (len != count) {
		__ioread32_copy(data + len, glink->rx->fifo,
				(count - len) / sizeof(u32));
	}
}

static void qcom_glink_rpm_rx_advance(struct glink_rpm *glink, size_t count)
{
	u32 tail = glink->rx->tail;

	tail += count;
	if (tail > glink->rx_size)
		tail -= glink->rx_size;

	glink->rx->tail = tail;

	qcom_glink_rpm_kick(glink);
}

static void qcom_glink_rpm_write_fifo(struct glink_rpm *glink,
				      const void *data, size_t count)
{
	u32 head = glink->tx->head;
	size_t len;

	len = min_t(size_t, count, glink->tx_size - head);
	if (len) {
		__iowrite32_copy(glink->tx->fifo + head, data,
				 len / sizeof(u32));
	}

	if (len != count) {
		__iowrite32_copy(glink->tx->fifo, data + len,
				 (count - len) / sizeof(u32));
	}

	head += count;
	if (head > glink->tx_size)
		head -= glink->tx_size;

	glink->tx->head = head;
}

static int qcom_glink_rpm_tx(struct glink_rpm *glink, const void *data,
			     size_t len, bool wait)
{
	int ret;

	/* Reject packets that are too big */
	if (len >= glink->tx_size)
		return -EINVAL;

	ret = mutex_lock_interruptible(&glink->tx_lock);
	if (ret)
		return ret;

	while (qcom_glink_rpm_tx_avail(glink) < len) {
		if (!wait) {
			ret = -ENOMEM;
			goto out;
		}

		/* TODO: use wait_event_interruptible() */
		msleep(10);
	}

	qcom_glink_rpm_write_fifo(glink, data, len);
	qcom_glink_rpm_kick(glink);

out:
	mutex_unlock(&glink->tx_lock);

	return ret;
}

static void qcom_glink_rpm_send_version(struct glink_rpm *glink)
{
	struct {
		__le16 id;
		__le16 version;
		__le32 features;
	} __packed cmd;

	cmd.id = cpu_to_le16(RPM_CMD_VERSION);
	cmd.version = cpu_to_le16(1);
	cmd.features = cpu_to_le32(BIT(2));

	qcom_glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void qcom_glink_rpm_send_version_ack(struct glink_rpm *glink)
{
	struct {
		__le16 id;
		__le16 version;
		__le32 features;
	} __packed cmd;

	dev_dbg(glink->dev, "%s(%d)\n", __func__, 1);

	cmd.id = cpu_to_le16(RPM_CMD_VERSION_ACK);
	cmd.version = cpu_to_le16(1);
	cmd.features = 0;

	qcom_glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void qcom_glink_rpm_send_open_ack(struct glink_rpm *glink,
					 struct glink_channel *channel)
{
	struct {
		__le16 id;
		__le16 rcid;
		__le32 reserved;
	} __packed cmd;

	dev_dbg(glink->dev, "%s(%s)\n", __func__, channel->name);

	cmd.id = cpu_to_le16(RPM_CMD_OPEN_ACK);
	cmd.rcid = cpu_to_le16(channel->rcid);
	cmd.reserved = 0;

	qcom_glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void qcom_glink_rpm_send_open_req(struct glink_rpm *glink,
					 struct glink_channel *channel)
{
	struct {
		struct glink_cmd cmd;
		u8 name[32];
	} __packed req;

	int name_len = strlen(channel->name) + 1;
	int req_len = ALIGN(sizeof(req.cmd) + name_len, 8);

	dev_dbg(glink->dev, "%s(%s)\n", __func__, channel->name);

	req.cmd.cmd = cpu_to_le16(RPM_CMD_OPEN);
	req.cmd.param1 = cpu_to_le16(channel->lcid);
	req.cmd.param2 = cpu_to_le32(name_len);
	strcpy(req.name, channel->name);

	qcom_glink_rpm_tx(glink, &req, req_len, true);
}

static void qcom_glink_rpm_send_close_req(struct glink_rpm *glink,
					  struct glink_channel *channel)
{
	struct glink_cmd req;

	req.cmd = cpu_to_le16(RPM_CMD_OPEN);
	req.param1 = cpu_to_le16(channel->lcid);
	req.param2 = 0;

	qcom_glink_rpm_tx(glink, &req, sizeof(req), true);
}

static int qcom_glink_rpm_rx_defer(struct glink_rpm *glink, size_t extra)
{
	struct glink_defer_cmd *dcmd;

	extra = ALIGN(extra, 8);
	dcmd = kzalloc(sizeof(*dcmd) + extra, GFP_ATOMIC);
	if (!dcmd)
		return -ENOMEM;

	INIT_LIST_HEAD(&dcmd->node);

	qcom_glink_rpm_peak_fifo(glink, &dcmd->cmd, sizeof(dcmd->cmd) + extra);

	spin_lock(&glink->rx_lock);
	list_add_tail(&dcmd->node, &glink->rx_queue);
	schedule_work(&glink->rx_work);
	spin_unlock(&glink->rx_lock);

	qcom_glink_rpm_rx_advance(glink, sizeof(dcmd->cmd) + extra);

	return 0;
}

static int qcom_glink_rpm_rx_data(struct glink_rpm *glink, size_t avail)
{
	struct glink_channel *channel = NULL;
	struct rpmsg_endpoint *ept;
	struct glink_channel *tmp;
	struct {
		struct glink_cmd cmd;
		u32 chunk_size;
		u32 left_size;
		u8 data[];
	} hdr, *req;
	unsigned long flags;
	u16 rcid;
	int ret;
	int req_len;

	if (avail < sizeof(hdr)) {
		dev_err(glink->dev, "not enough data in fifo\n");
		return -EAGAIN;
	}

	qcom_glink_rpm_peak_fifo(glink, &hdr, sizeof(hdr));
	if (avail < sizeof(hdr) + hdr.chunk_size) {
		dev_err(glink->dev, "payload not yet in fifo\n");
		return -EAGAIN;
	}

	rcid = hdr.cmd.param1;

	dev_dbg(glink->dev, "%s(rcid: %d, chunk: %d, left: %d)\n",
		 __func__, rcid, hdr.chunk_size, hdr.left_size);

	req_len = sizeof(hdr) + hdr.chunk_size;
	req = kmalloc(req_len, GFP_ATOMIC);
	if (!req)
		return -ENOMEM;

	qcom_glink_rpm_peak_fifo(glink, req, req_len);

	spin_lock_irqsave(&glink->channels_lock, flags);
	list_for_each_entry(tmp, &glink->channels, node) {
		if (tmp->rcid == rcid) {
			channel = tmp;
			break;
		}
	}

	if (channel) {
		spin_lock(&channel->recv_lock);
		if (channel->glink_ept) {
			ept = &channel->glink_ept->ept;
			ret = ept->cb(ept->rpdev, req->data, hdr.chunk_size, ept->priv, RPMSG_ADDR_ANY);
		}
		spin_unlock(&channel->recv_lock);
	}
	spin_unlock_irqrestore(&glink->channels_lock, flags);

	qcom_glink_rpm_rx_advance(glink, ALIGN(req_len, 8));

	kfree(req);
	return 0;
}

static irqreturn_t qcom_glink_rpm_intr(int irq, void *data)
{
	struct glink_rpm *glink = data;
	struct glink_cmd cmd;
	u32 avail;

	for (;;) {
		avail = qcom_glink_rpm_rx_avail(glink);
		if (avail < sizeof(cmd))
			break;

		qcom_glink_rpm_peak_fifo(glink, &cmd, sizeof(cmd));

		switch (cmd.cmd) {
		case RPM_CMD_VERSION:
		case RPM_CMD_VERSION_ACK:
		case RPM_CMD_OPEN_ACK:
			qcom_glink_rpm_rx_defer(glink, 0);
			break;
		case RPM_CMD_OPEN:
			qcom_glink_rpm_rx_defer(glink, cmd.param2);
			break;
		case RPM_CMD_TX_DATA:
		case RPM_CMD_TX_DATA_CONT:
			qcom_glink_rpm_rx_data(glink, avail);
			break;
		default:
			print_hex_dump(KERN_ERR, "UNKNOWN RX: ", DUMP_PREFIX_OFFSET, 16, 1, &cmd, sizeof(cmd), true);
			goto out;
		}
	}

out:
	return IRQ_HANDLED;
}

static struct glink_channel *
qcom_glink_find_channel(struct glink_rpm *glink, const char *name)
{
	struct glink_channel *channel;
	struct glink_channel *ret = NULL;
	unsigned long flags;
	unsigned state;

	spin_lock_irqsave(&glink->channels_lock, flags);
	list_for_each_entry(channel, &glink->channels, node) {
		if (strcmp(channel->name, name))
			continue;

		state = channel->remote_state;
		if (state != GLINK_STATE_OPENING &&
		    state != GLINK_STATE_OPEN)
			continue;

		ret = channel;
		break;
	}
	spin_unlock_irqrestore(&glink->channels_lock, flags);

	return ret;
}

static void __ept_release(struct kref *kref)
{
	struct rpmsg_endpoint *ept = container_of(kref, struct rpmsg_endpoint,
						  refcount);
	kfree(to_glink_endpoint(ept));
}

static struct rpmsg_endpoint *qcom_glink_create_ept(struct rpmsg_device *rpdev,
						  rpmsg_rx_cb_t cb, void *priv,
						  struct rpmsg_channel_info chinfo)
{
	struct glink_endpoint *glink_ept;
	struct glink_channel *parent;
	struct glink_channel *channel;
	struct glink_rpm *glink;
	struct rpmsg_endpoint *ept;
	const char *name = chinfo.name;
	int ret;

	parent = to_glink_channel(&rpdev->dev);
	glink = parent->glink;

	/* Wait up to HZ for the channel to appear */
	ret = wait_event_interruptible_timeout(glink->new_channel_event,
			(channel = qcom_glink_find_channel(glink, name)) != NULL,
			HZ);
	if (!ret)
		return ERR_PTR(-ETIMEDOUT);

	if (channel->local_state != GLINK_STATE_CLOSED) {
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

	channel->glink_ept = glink_ept;
	glink_ept->channel = channel;
	glink_ept->glink = glink;

	qcom_glink_rpm_send_open_ack(glink, channel);

	qcom_glink_rpm_send_open_req(glink, channel);
	channel->local_state = GLINK_STATE_OPENING;

	return ept;
}

static void qcom_glink_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);
	struct glink_channel *channel = glink_ept->channel;
	struct glink_rpm *glink = glink_ept->glink;
	unsigned long flags;

	spin_lock_irqsave(&channel->recv_lock, flags);
	glink_ept->ept.cb = NULL;
	spin_unlock_irqrestore(&channel->recv_lock, flags);

	qcom_glink_rpm_send_close_req(glink, channel);

	channel->local_state = GLINK_STATE_CLOSING;
	channel->glink_ept = NULL;
	kref_put(&ept->refcount, __ept_release);
}

static int __qcom_glink_send(struct glink_endpoint *glink_ept,
			     void *data, int len, bool wait)
{
	struct glink_channel *channel = glink_ept->channel;
	struct glink_rpm *glink = glink_ept->glink;
	struct {
		struct glink_cmd cmd;
		u32 chunk_size;
		u32 left_size;
		u8 data[];
	} *req;
	int req_len = ALIGN(sizeof(*req) + len, 8);
	int ret;

	req = kzalloc(req_len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->cmd.cmd = RPM_CMD_TX_DATA;
	req->cmd.param1 = channel->lcid;
	req->cmd.param2 = channel->rcid;
	req->chunk_size = len;
	req->left_size = 0;

	memcpy(req->data, data, len);

	ret = qcom_glink_rpm_tx(glink, req, req_len, wait);
	kfree(req);

	return ret;
}

static int qcom_glink_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);
	return __qcom_glink_send(glink_ept, data, len, false);
}

static int qcom_glink_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);
	return __qcom_glink_send(glink_ept, data, len, false);
}

/*
 * Finds the device_node for the glink child interested in this channel.
 */
static struct device_node *qcom_glink_match_channel(struct device_node *node,
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

static const struct rpmsg_channel glink_rpmsg_ops = {
	.create_ept = qcom_glink_create_ept,
	.destroy_ept = qcom_glink_destroy_ept,
	.send = qcom_glink_send,
	.trysend = qcom_glink_trysend,
};

static int qcom_glink_rpm_open(struct glink_rpm *glink, u16 rcid, char *name)
{
	struct rpmsg_channel *rpch;
	struct rpmsg_device *rpdev;
	struct glink_channel *ch;
	unsigned long flags;
	int ret;
	static int current_lcid = 0;

	dev_dbg(glink->dev, "%s(%d, %s)\n", __func__, rcid, name);

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return -ENOMEM;

	/* Setup glink internal glink_channel data */
	spin_lock_init(&ch->recv_lock);
	ch->glink = glink;
	ch->lcid = current_lcid++;
	ch->rcid = rcid;
	ch->name = kstrdup(name, GFP_KERNEL);
	ch->local_state = GLINK_STATE_CLOSED;
	ch->remote_state = GLINK_STATE_OPENING;

	/* Assign callbacks for rpmsg_channel */
	ch->rpch = glink_rpmsg_ops;

	/* Assign public information to the rpmsg_device */
	rpdev = &ch->rpch.rpdev;
	strncpy(rpdev->id.name, name, RPMSG_NAME_SIZE);
	rpdev->src = RPMSG_ADDR_ANY;
	rpdev->dst = RPMSG_ADDR_ANY;

	rpdev->dev.of_node = qcom_glink_match_channel(glink->dev->of_node, name);
	rpdev->dev.parent = glink->dev;

	spin_lock_irqsave(&glink->channels_lock, flags);
	list_add(&ch->node, &glink->channels);
	spin_unlock_irqrestore(&glink->channels_lock, flags);

	wake_up_interruptible(&glink->new_channel_event);

	ret = rpmsg_register_device(rpdev);
	if (ret)
		goto err_cleanup;

	return 0;

err_cleanup:
	spin_lock_irqsave(&glink->channels_lock, flags);
	list_del(&ch->node);
	spin_unlock_irqrestore(&glink->channels_lock, flags);

	kfree(ch);
	return ret;
}

static int qcom_glink_rpm_open_ack(struct glink_rpm *glink, u16 lcid)
{
	struct glink_channel *channel = NULL;
	struct glink_channel *tmp;
	unsigned long flags;

	spin_lock_irqsave(&glink->channels_lock, flags);
	list_for_each_entry(tmp, &glink->channels, node) {
		if (tmp->lcid == lcid) {
			channel = tmp;
			break;
		}
	}
	spin_unlock_irqrestore(&glink->channels_lock, flags);

	if (!channel || channel->local_state != GLINK_STATE_OPENING) {
		dev_err(glink->dev, "invalid open ack packet\n");
		return -EINVAL;
	}

	channel->local_state = GLINK_STATE_OPEN;
	dev_dbg(glink->dev, "%s is now fully open\n", channel->name);
	return 0;
}

static void qcom_glink_rpm_work(struct work_struct *work)
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
			qcom_glink_rpm_send_version_ack(glink);
			break;
		case RPM_CMD_VERSION_ACK:
			break;
		case RPM_CMD_OPEN:
			qcom_glink_rpm_open(glink, cmd->param1, cmd->data);
			break;
		case RPM_CMD_OPEN_ACK:
			qcom_glink_rpm_open_ack(glink, cmd->param1);
			break;
		default:
			dev_err(glink->dev, "unknown defer: %d!\n", cmd->cmd);
			break;
		}

		kfree(dcmd);
	}
}

static int qcom_glink_rpm_parse_toc(struct glink_rpm *glink)
{
	struct rpm_toc *toc;
	int num_entries;
	u32 offset;
	void *buf;
	u32 size;
	u32 id;
	int i;

	buf = devm_kzalloc(glink->dev, RPM_TOC_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	__ioread32_copy(buf,
			glink->msg_ram + glink->msg_ram_size - RPM_TOC_SIZE,
			RPM_TOC_SIZE / sizeof(u32));

	toc = buf;

	if (le32_to_cpu(toc->magic) != RPM_TOC_MAGIC) {
		dev_err(glink->dev, "rpm toc has invalid magic\n");
		return -EINVAL;
	}

	num_entries = le32_to_cpu(toc->count);
	if (num_entries > RPM_TOC_MAX_ENTRIES) {
		dev_err(glink->dev, "invalid number of toc entries\n");
		return -EINVAL;
	}

	dev_dbg(glink->dev, "id         offset   sizex\n");
	for (i = 0; i < num_entries; i++) {
		id = le32_to_cpu(toc->entries[i].id);
		offset = le32_to_cpu(toc->entries[i].offset);
		size = le32_to_cpu(toc->entries[i].size);

		dev_dbg(glink->dev, "0x%08x 0x%5x 0x%5x\n", id, offset, size);

		if (offset > glink->msg_ram_size ||
		    offset + size > glink->msg_ram_size) {
			dev_err(glink->dev, "toc entry with invalid dimensions\n");
			continue;
		}

		switch (id) {
		case RPM_TX_FIFO_ID:
			glink->tx = glink->msg_ram + offset;
			glink->tx_size = size;
			break;
		case RPM_RX_FIFO_ID:
			glink->rx = glink->msg_ram + offset;
			glink->rx_size = size;
			break;
		};
	}

	if (!glink->rx || !glink->tx) {
		dev_err(glink->dev, "unable to find rx and tx descriptors\n");
		return -EINVAL;
	}

	devm_kfree(glink->dev, buf);
	return 0;
}

static int qcom_glink_rpm_probe(struct platform_device *pdev)
{
	struct of_phandle_args args;
	struct device_node *np;
	struct glink_rpm *glink;
	struct resource r;
	int irq;
	int ret;

	glink = devm_kzalloc(&pdev->dev, sizeof(*glink), GFP_KERNEL);
	if (!glink)
		return -ENOMEM;

	glink->dev = &pdev->dev;

	mutex_init(&glink->tx_lock);
	spin_lock_init(&glink->rx_lock);
	INIT_LIST_HEAD(&glink->rx_queue);
	INIT_WORK(&glink->rx_work, qcom_glink_rpm_work);

	spin_lock_init(&glink->channels_lock);
	INIT_LIST_HEAD(&glink->channels);
	init_waitqueue_head(&glink->new_channel_event);

	ret = of_parse_phandle_with_fixed_args(pdev->dev.of_node,
					       "qcom,ipc", 2, 0,
					       &args);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to parse qcom,ipc\n");
		return ret;
	}

	glink->ipc_regmap = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(glink->ipc_regmap))
		return PTR_ERR(glink->ipc_regmap);

	glink->ipc_offset = args.args[0];
	glink->ipc_bit = args.args[1];

	glink->out_irq_reg = devm_ioremap(&pdev->dev, 0x09820010, 4);

	np = of_parse_phandle(pdev->dev.of_node, "qcom,rpm-msg-ram", 0);
	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return ret;

	glink->msg_ram = devm_ioremap(&pdev->dev, r.start, resource_size(&r));
	glink->msg_ram_size = resource_size(&r);
	if (!glink->msg_ram)
		return -ENOMEM;

	ret = qcom_glink_rpm_parse_toc(glink);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq,
			       qcom_glink_rpm_intr,
			       IRQF_NO_SUSPEND |IRQF_SHARED,
			       "test", glink);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		return ret;
	}

	glink->tx->head = 0;
	glink->rx->tail = 0;

	qcom_glink_rpm_send_version(glink);

	return 0;
}

static int qcom_glink_rpm_remove(struct platform_device *pdev)
{
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
MODULE_DESCRIPTION("Qualcomm Glink RPM driver");
MODULE_LICENSE("GPL v2");
