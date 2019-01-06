// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Ltd
 */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/aoss-qmp.h>

#define QMP_DESC_MAGIC			0x0
#define QMP_DESC_VERSION		0x4
#define QMP_DESC_FEATURES		0x8

/* AOP-side offsets */
#define QMP_DESC_UCORE_LINK_STATE	0xc
#define QMP_DESC_UCORE_LINK_STATE_ACK	0x10
#define QMP_DESC_UCORE_CH_STATE		0x14
#define QMP_DESC_UCORE_CH_STATE_ACK	0x18
#define QMP_DESC_UCORE_MBOX_SIZE	0x1c
#define QMP_DESC_UCORE_MBOX_OFFSET	0x20

/* Linux-side offsets */
#define QMP_DESC_MCORE_LINK_STATE	0x24
#define QMP_DESC_MCORE_LINK_STATE_ACK	0x28
#define QMP_DESC_MCORE_CH_STATE		0x2c
#define QMP_DESC_MCORE_CH_STATE_ACK	0x30
#define QMP_DESC_MCORE_MBOX_SIZE	0x34
#define QMP_DESC_MCORE_MBOX_OFFSET	0x38

#define QMP_STATE_UP	0x0000ffff
#define QMP_STATE_DOWN	0xffff0000

#define QMP_MAGIC	0x4d41494c
#define QMP_VERSION	1

/**
 * struct qmp - driver state for QMP implementation
 * @msgram: iomem referencing the message RAM used for communication
 * @dev: reference to QMP device
 * @mbox_client: mailbox client used to ring the doorbell on transmit
 * @mbox_chan: mailbox channel used to ring the doorbell on transmit
 * @offset: offset within @msgram where messages should be written
 * @size: maximum size of the messages to be transmitted
 * @event: wait_queue for synchronization with the IRQ
 * @tx_lock: provides syncrhonization between multiple callers of qmp_send()
 * @pd_pdev: platform device for the power-domain child device
 */
struct qmp {
	void __iomem *msgram;
	struct device *dev;

	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;

	size_t offset;
	size_t size;

	wait_queue_head_t event;

	struct mutex tx_lock;

	struct platform_device *pd_pdev;
};

static void qmp_kick(struct qmp *qmp)
{
	mbox_send_message(qmp->mbox_chan, NULL);
	mbox_client_txdone(qmp->mbox_chan, 0);
}

static bool qmp_magic_valid(struct qmp *qmp)
{
	return readl(qmp->msgram + QMP_DESC_MAGIC) == QMP_MAGIC;
}

static bool qmp_link_acked(struct qmp *qmp)
{
	return readl(qmp->msgram + QMP_DESC_MCORE_LINK_STATE_ACK) == QMP_STATE_UP;
}

static bool qmp_mcore_channel_acked(struct qmp *qmp)
{
	return readl(qmp->msgram + QMP_DESC_MCORE_CH_STATE_ACK) == QMP_STATE_UP;
}

static bool qmp_ucore_channel_up(struct qmp *qmp)
{
	return readl(qmp->msgram + QMP_DESC_UCORE_CH_STATE) == QMP_STATE_UP;
}

static int qmp_open(struct qmp *qmp)
{
	int ret;
	u32 val;

	if (!qmp_magic_valid(qmp)) {
		dev_err(qmp->dev, "QMP magic doesn't match\n");
		return -ETIMEDOUT;
	}

	val = readl(qmp->msgram + QMP_DESC_VERSION);
	if (val != QMP_VERSION) {
		dev_err(qmp->dev, "unsupported QMP version %d\n", val);
		return -EINVAL;
	}

	qmp->offset = readl(qmp->msgram + QMP_DESC_MCORE_MBOX_OFFSET);
	qmp->size = readl(qmp->msgram + QMP_DESC_MCORE_MBOX_SIZE);
	if (!qmp->size) {
		dev_err(qmp->dev, "invalid mailbox size\n");
		return -EINVAL;
	}

	/* Ack remote core's link state */
	val = readl(qmp->msgram + QMP_DESC_UCORE_LINK_STATE);
	writel(val, qmp->msgram + QMP_DESC_UCORE_LINK_STATE_ACK);

	/* Set local core's link state to up */
	writel(QMP_STATE_UP, qmp->msgram + QMP_DESC_MCORE_LINK_STATE);

	qmp_kick(qmp);

	ret = wait_event_timeout(qmp->event, qmp_link_acked(qmp), HZ);
	if (!ret) {
		dev_err(qmp->dev, "ucore didn't ack link\n");
		goto timeout_close_link;
	}

	writel(QMP_STATE_UP, qmp->msgram + QMP_DESC_MCORE_CH_STATE);

	qmp_kick(qmp);

	ret = wait_event_timeout(qmp->event, qmp_ucore_channel_up(qmp), HZ);
	if (!ret) {
		dev_err(qmp->dev, "ucore didn't open channel\n");
		goto timeout_close_channel;
	}

	/* Ack remote core's channel state */
	writel(QMP_STATE_UP, qmp->msgram + QMP_DESC_UCORE_CH_STATE_ACK);

	qmp_kick(qmp);

	ret = wait_event_timeout(qmp->event, qmp_mcore_channel_acked(qmp), HZ);
	if (!ret) {
		dev_err(qmp->dev, "ucore didn't ack channel\n");
		goto timeout_close_channel;
	}

	return 0;

timeout_close_channel:
	writel(QMP_STATE_DOWN, qmp->msgram + QMP_DESC_MCORE_CH_STATE);

timeout_close_link:
	writel(QMP_STATE_DOWN, qmp->msgram + QMP_DESC_MCORE_LINK_STATE);
	qmp_kick(qmp);

	return -ETIMEDOUT;
}

static void qmp_close(struct qmp *qmp)
{
	writel(QMP_STATE_DOWN, qmp->msgram + QMP_DESC_MCORE_CH_STATE);
	writel(QMP_STATE_DOWN, qmp->msgram + QMP_DESC_MCORE_LINK_STATE);
	qmp_kick(qmp);
}

static irqreturn_t qmp_intr(int irq, void *data)
{
	struct qmp *qmp = data;

	wake_up_interruptible_all(&qmp->event);

	return IRQ_HANDLED;
}

static bool qmp_message_empty(struct qmp *qmp)
{
	return readl(qmp->msgram + qmp->offset) == 0;
}

/**
 * qmp_send() - send a message to the AOSS
 * @qmp: qmp context
 * @data: message to be sent
 * @len: length of the message
 *
 * Transmit @data to AOSS and wait for the AOSS to acknowledge the message.
 * @len must be a multiple of 4 and not longer than the mailbox size. Access is
 * synchronized by this implementation.
 *
 * Return: 0 on success, negative errno on failure
 */
int qmp_send(struct qmp *qmp, const void *data, size_t len)
{
	int ret;

	if (WARN_ON(len + sizeof(u32) > qmp->size))
		return -EINVAL;

	if (WARN_ON(len % sizeof(u32)))
		return -EINVAL;

	mutex_lock(&qmp->tx_lock);

	/* The message RAM only implements 32-bit accesses */
	__iowrite32_copy(qmp->msgram + qmp->offset + sizeof(u32),
			 data, len / sizeof(u32));
	writel(len, qmp->msgram + qmp->offset);
	qmp_kick(qmp);

	ret = wait_event_interruptible_timeout(qmp->event,
					       qmp_message_empty(qmp), HZ);
	if (!ret) {
		dev_err(qmp->dev, "ucore did not ack channel\n");
		ret = -ETIMEDOUT;

		/* Clear message from buffer */
		writel(0, qmp->msgram + qmp->offset);
	} else {
		ret = 0;
	}

	mutex_unlock(&qmp->tx_lock);

	return ret;
}
EXPORT_SYMBOL(qmp_send);

static int qmp_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct qmp *qmp;
	int irq;
	int ret;

	qmp = devm_kzalloc(&pdev->dev, sizeof(*qmp), GFP_KERNEL);
	if (!qmp)
		return -ENOMEM;

	qmp->dev = &pdev->dev;
	init_waitqueue_head(&qmp->event);
	mutex_init(&qmp->tx_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	qmp->msgram = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(qmp->msgram))
		return PTR_ERR(qmp->msgram);

	qmp->mbox_client.dev = &pdev->dev;
	qmp->mbox_client.knows_txdone = true;
	qmp->mbox_chan = mbox_request_channel(&qmp->mbox_client, 0);
	if (IS_ERR(qmp->mbox_chan)) {
		dev_err(&pdev->dev, "failed to acquire ipc mailbox\n");
		return PTR_ERR(qmp->mbox_chan);
	}

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq, qmp_intr, IRQF_ONESHOT,
			       "aoss-qmp", qmp);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to request interrupt\n");
		goto err_close_qmp;
	}

	ret = qmp_open(qmp);
	if (ret < 0)
		goto err_free_mbox;

	platform_set_drvdata(pdev, qmp);

	if (of_property_read_bool(pdev->dev.of_node, "#power-domain-cells")) {
		qmp->pd_pdev = platform_device_register_data(&pdev->dev,
							     "aoss_qmp_pd",
							     PLATFORM_DEVID_NONE,
							     NULL, 0);
		if (IS_ERR(qmp->pd_pdev)) {
			dev_err(&pdev->dev, "failed to register AOSS PD\n");
			ret = PTR_ERR(qmp->pd_pdev);
			goto err_close_qmp;
		}
	}

	return 0;

err_close_qmp:
	qmp_close(qmp);
err_free_mbox:
	mbox_free_channel(qmp->mbox_chan);

	return ret;
}

static int qmp_remove(struct platform_device *pdev)
{
	struct qmp *qmp = platform_get_drvdata(pdev);

	platform_device_unregister(qmp->pd_pdev);

	qmp_close(qmp);
	mbox_free_channel(qmp->mbox_chan);

	return 0;
}

static const struct of_device_id qmp_dt_match[] = {
	{ .compatible = "qcom,sdm845-aoss-qmp", },
	{}
};
MODULE_DEVICE_TABLE(of, qmp_dt_match);

static struct platform_driver qmp_driver = {
	.driver = {
		.name		= "aoss_qmp",
		.of_match_table	= qmp_dt_match,
	},
	.probe = qmp_probe,
	.remove	= qmp_remove,
};
module_platform_driver(qmp_driver);

MODULE_DESCRIPTION("Qualcomm AOSS QMP driver");
MODULE_LICENSE("GPL v2");
