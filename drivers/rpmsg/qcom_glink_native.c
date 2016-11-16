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
#include <linux/of_irq.h>
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
#include "qcom_glink_native.h"

struct glink_cmd {
	__le16 cmd;
	__le16 param1;
	__le32 param2;
	u8 data[];
} __packed;

struct glink_defer_cmd {
	struct list_head node;

	struct glink_cmd cmd;
};

struct qcom_glink {
	struct device *dev;

	void __iomem *msg_ram;
	size_t msg_ram_size;

	struct regmap *ipc_regmap;
	unsigned int ipc_offset;
	unsigned int ipc_bit;

	struct qcom_glink_native_pipe *rx_pipe;
	struct qcom_glink_native_pipe *tx_pipe;
	size_t max_tx_size;

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
	struct rpmsg_device rpdev;

	struct list_head node;

	struct qcom_glink *glink;
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
	struct qcom_glink *glink;
};

#define to_glink_channel(_rpdev) container_of(_rpdev, struct glink_channel, rpdev)
#define to_glink_endpoint(ept) container_of(ept, struct glink_endpoint, ept)


#define GLINK_NAME_SIZE 32

static const struct rpmsg_endpoint_ops glink_endpoint_ops;

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

#define GLINK_FEATURE_SIGNALS            BIT(0)
#define GLINK_FEATURE_INTENTLESS         BIT(1)
#define GLINK_FEATURE_TRACER_PKT         BIT(2)
#define GLINK_FEATURE_AUTO_QUEUE_RX_INT  BIT(3)

static void qcom_glink_rpm_kick(struct qcom_glink *glink)
{
	wmb();
	regmap_write(glink->ipc_regmap, glink->ipc_offset, BIT(glink->ipc_bit));
}

static size_t qcom_glink_rpm_rx_avail(struct qcom_glink *glink)
{
	return glink->rx_pipe->avail(glink->rx_pipe);
}

static size_t qcom_glink_rpm_tx_avail(struct qcom_glink *glink)
{
	return glink->tx_pipe->avail(glink->tx_pipe);
}

static void qcom_glink_rpm_peak_fifo(struct qcom_glink *glink,
				     void *data, size_t count)
{
	glink->rx_pipe->peak(glink->rx_pipe, data, count);
}

static void qcom_glink_rpm_rx_advance(struct qcom_glink *glink, size_t count)
{
	glink->rx_pipe->advance(glink->rx_pipe, count);
	qcom_glink_rpm_kick(glink);
}

static void qcom_glink_rpm_write_fifo(struct qcom_glink *glink,
				      const void *data, size_t count)
{
	glink->tx_pipe->write(glink->tx_pipe, data, count);
}

static int qcom_glink_rpm_tx(struct qcom_glink *glink, const void *data,
			     size_t len, bool wait)
{
	int ret;

	/* Reject packets that are too big */
	if (len >= glink->max_tx_size)
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

static int qcom_glink_rpm_send_version(struct qcom_glink *glink)
{
	struct glink_cmd cmd;

	dev_dbg(glink->dev, "%s()\n", __func__);

	cmd.cmd = cpu_to_le16(RPM_CMD_VERSION);
	cmd.param1 = cpu_to_le16(1);
	cmd.param2 = cpu_to_le32(GLINK_FEATURE_INTENTLESS);

	return qcom_glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void qcom_glink_rpm_send_version_ack(struct qcom_glink *glink)
{
	struct glink_cmd cmd;

	dev_dbg(glink->dev, "%s(%d)\n", __func__, 1);

	cmd.cmd = cpu_to_le16(RPM_CMD_VERSION_ACK);
	cmd.param1 = cpu_to_le16(1);
	cmd.param2 = cpu_to_le32(0);

	qcom_glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void qcom_glink_rpm_send_open_ack(struct qcom_glink *glink,
					 struct glink_channel *channel)
{
	struct glink_cmd cmd;

	dev_dbg(glink->dev, "%s(%s)\n", __func__, channel->name);

	cmd.cmd = cpu_to_le16(RPM_CMD_OPEN_ACK);
	cmd.param1 = cpu_to_le16(channel->rcid);
	cmd.param2 = cpu_to_le32(0);

	qcom_glink_rpm_tx(glink, &cmd, sizeof(cmd), true);
}

static void qcom_glink_rpm_send_open_req(struct qcom_glink *glink,
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

	qcom_glink_rpm_tx(glink, &req, req_len, true);
}

static void qcom_glink_rpm_send_close_req(struct qcom_glink *glink,
					  struct glink_channel *channel)
{
	struct glink_cmd req;

	req.cmd = cpu_to_le16(RPM_CMD_OPEN);
	req.param1 = cpu_to_le16(channel->lcid);
	req.param2 = 0;

	qcom_glink_rpm_tx(glink, &req, sizeof(req), true);
}

static int qcom_glink_rpm_rx_defer(struct qcom_glink *glink, size_t extra)
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

static int qcom_glink_rpm_rx_data(struct qcom_glink *glink, size_t avail)
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
	unsigned int chunk_size;
	unsigned int left_size;
	int req_len;
	u16 rcid;
	int ret;

	if (avail < sizeof(hdr)) {
		dev_err(glink->dev, "not enough data in fifo\n");
		return -EAGAIN;
	}

	qcom_glink_rpm_peak_fifo(glink, &hdr, sizeof(hdr));
	chunk_size = le32_to_cpu(hdr.chunk_size);
	left_size = le32_to_cpu(hdr.left_size);

	if (avail < sizeof(hdr) + chunk_size) {
		dev_err(glink->dev, "payload not yet in fifo\n");
		return -EAGAIN;
	}

	rcid = le16_to_cpu(hdr.cmd.param1);

	dev_dbg(glink->dev, "%s(rcid: %d, chunk: %d, left: %d)\n",
		 __func__, rcid, chunk_size, left_size);

	req_len = sizeof(hdr) + chunk_size;
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
			ret = ept->cb(ept->rpdev, req->data, hdr.chunk_size,
				      ept->priv, RPMSG_ADDR_ANY);
		}
		spin_unlock(&channel->recv_lock);
	}
	spin_unlock_irqrestore(&glink->channels_lock, flags);

	qcom_glink_rpm_rx_advance(glink, ALIGN(req_len, 8));

	kfree(req);
	return 0;
}

static irqreturn_t glink_native_intr(int irq, void *data)
{
	struct qcom_glink *glink = data;
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
qcom_glink_find_channel(struct qcom_glink *glink, const char *name)
{
	struct glink_channel *channel;
	struct glink_channel *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&glink->channels_lock, flags);
	list_for_each_entry(channel, &glink->channels, node) {
		if (!strcmp(channel->name, name)) {
			ret = channel;
			break;
		}
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
	struct glink_channel *parent = to_glink_channel(rpdev);
	struct glink_channel *channel;
	struct qcom_glink *glink = parent->glink;
	struct rpmsg_endpoint *ept;
	const char *name = chinfo.name;
	int ret;

	/* Wait up to HZ for the channel to appear */
	ret = wait_event_interruptible_timeout(glink->new_channel_event,
			(channel = qcom_glink_find_channel(glink, name)) != NULL,
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

	qcom_glink_rpm_send_open_ack(glink, channel);

	qcom_glink_rpm_send_open_req(glink, channel);
	channel->state = GLINK_STATE_OPENING;

	return ept;
}

static void qcom_glink_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);
	struct glink_channel *channel = glink_ept->channel;
	struct qcom_glink *glink = glink_ept->glink;
	unsigned long flags;

	spin_lock_irqsave(&channel->recv_lock, flags);
	glink_ept->ept.cb = NULL;
	spin_unlock_irqrestore(&channel->recv_lock, flags);

	qcom_glink_rpm_send_close_req(glink, channel);

	channel->state = GLINK_STATE_CLOSING;
	channel->glink_ept = NULL;
	kref_put(&ept->refcount, __ept_release);
}

static int __qcom_glink_send(struct glink_endpoint *glink_ept,
			     void *data, int len, bool wait)
{
	struct glink_channel *channel = glink_ept->channel;
	struct qcom_glink *glink = glink_ept->glink;
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

	req->cmd.cmd = cpu_to_le16(RPM_CMD_TX_DATA);
	req->cmd.param1 = cpu_to_le16(channel->lcid);
	req->cmd.param2 = cpu_to_le32(channel->rcid);
	req->chunk_size = cpu_to_le32(len);
	req->left_size = cpu_to_le32(0);

	memcpy(req->data, data, len);

	ret = qcom_glink_rpm_tx(glink, req, req_len, wait);
	kfree(req);

	return ret;
}

static int qcom_glink_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);

	dev_dbg(glink_ept->glink->dev, "%s(%d)\n", __func__, len);

	return __qcom_glink_send(glink_ept, data, len, false);
}

static int qcom_glink_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct glink_endpoint *glink_ept = to_glink_endpoint(ept);

	dev_dbg(glink_ept->glink->dev, "%s(%d)\n", __func__, len);

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

static const struct rpmsg_device_ops glink_device_ops = {
	.create_ept = qcom_glink_create_ept,
};

static const struct rpmsg_endpoint_ops glink_endpoint_ops = {
	.destroy_ept = qcom_glink_destroy_ept,
	.send = qcom_glink_send,
	.trysend = qcom_glink_trysend,
};

static int qcom_glink_rpm_open(struct qcom_glink *glink, u16 rcid, char *name)
{
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
	ch->state = GLINK_STATE_CLOSED;

	/* Assign public information to the rpmsg_device */
	rpdev = &ch->rpdev;
	strncpy(rpdev->id.name, name, RPMSG_NAME_SIZE);
	rpdev->src = RPMSG_ADDR_ANY;
	rpdev->dst = RPMSG_ADDR_ANY;
	rpdev->ops = &glink_device_ops;

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

static int qcom_glink_rpm_open_ack(struct qcom_glink *glink, u16 lcid)
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

	if (!channel || channel->state != GLINK_STATE_OPENING) {
		dev_err(glink->dev, "invalid open ack packet\n");
		return -EINVAL;
	}

	channel->state = GLINK_STATE_OPEN;
	dev_dbg(glink->dev, "%s is now fully open\n", channel->name);

	return 0;
}

static void qcom_glink_rpm_work(struct work_struct *work)
{
	struct qcom_glink *glink = container_of(work, struct qcom_glink, rx_work);
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

struct qcom_glink *qcom_glink_native_probe(struct device *dev,
					   struct qcom_glink_native_pipe *rx,
					   struct qcom_glink_native_pipe *tx,
					   size_t max_tx_size)
{
	struct of_phandle_args args;
	struct qcom_glink *glink;
	int irq;
	int ret;

	glink = devm_kzalloc(dev, sizeof(*glink), GFP_KERNEL);
	if (!glink)
		return ERR_PTR(-ENOMEM);

	glink->dev = dev;
	glink->rx_pipe = rx;
	glink->tx_pipe = tx;
	glink->max_tx_size = max_tx_size;

	mutex_init(&glink->tx_lock);
	spin_lock_init(&glink->rx_lock);
	INIT_LIST_HEAD(&glink->rx_queue);
	INIT_WORK(&glink->rx_work, qcom_glink_rpm_work);

	spin_lock_init(&glink->channels_lock);
	INIT_LIST_HEAD(&glink->channels);
	init_waitqueue_head(&glink->new_channel_event);

	ret = of_parse_phandle_with_fixed_args(dev->of_node,
					       "qcom,ipc", 2, 0,
					       &args);
	if (ret < 0) {
		dev_err(dev, "failed to parse qcom,ipc\n");
		return ERR_PTR(ret);
	}

	glink->ipc_regmap = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(glink->ipc_regmap))
		return ERR_CAST(glink->ipc_regmap);

	glink->ipc_offset = args.args[0];
	glink->ipc_bit = args.args[1];
	
	irq = of_irq_get(dev->of_node, 0);
	ret = devm_request_irq(dev, irq,
			       glink_native_intr,
			       IRQF_NO_SUSPEND |IRQF_SHARED,
			       "glink-native", glink);
	if (ret) {
		dev_err(dev, "failed to request IRQ\n");
		return ERR_PTR(ret);
	}
	
	ret = qcom_glink_rpm_send_version(glink);
	if (ret)
		return ERR_PTR(ret);

	return glink;
}

void qcom_glink_native_remove(struct qcom_glink *glink)
{

}
EXPORT_SYMBOL_GPL(qcom_glink_native_remove);

