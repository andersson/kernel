// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
// Copyright (c) 2021, Linaro Ltd

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <linux/soc/qcom/pmic_glink.h>

#define MSG_OWNER_BC		32778
#define MSG_OWNER_UC            32779
#define MSG_OWNER_USBC_PAN      32780


#define MSG_TYPE_REQ_RESP	1
#define USBC_CMD_WRITE_REQ      0x15
#define USBC_NOTIFY_IND		0x16

enum altmode_send_msg_type {
	ALTMODE_PAN_EN = 0x10,
	ALTMODE_PAN_ACK,
};

struct usbc_write_req {
	struct pmic_glink_hdr   hdr;
	u32 cmd;
	u32 arg;
	u32                     reserved;
};

#define NOTIFY_PAYLOAD_SIZE 16
struct usbc_notify_ind_msg {
	struct pmic_glink_hdr hdr;
	char payload[NOTIFY_PAYLOAD_SIZE];
	u32 reserved;
};

struct pmic_glink_altmode {
	struct device *dev;
	struct pmic_glink *pmic;

	struct completion pan_ack;
	struct pmic_glink_owner *owner;
};

static int pmic_glink_altmode_write(struct pmic_glink_altmode *altmode, u32 cmd, u32 arg)
{
	struct usbc_write_req req = {};

	req.hdr.owner = MSG_OWNER_USBC_PAN;
	req.hdr.type = MSG_TYPE_REQ_RESP;
	req.hdr.opcode = USBC_CMD_WRITE_REQ;
	req.cmd = cmd;
	req.arg = arg;

	return pmic_glink_send(altmode->pmic, &req, sizeof(req));
}

static int pmic_glink_altmode_enable(struct pmic_glink_altmode *altmode)
{
	unsigned long left;
	int ret;

	ret = pmic_glink_altmode_write(altmode, ALTMODE_PAN_EN, 0);
	if (ret)
		return ret;

	left = wait_for_completion_timeout(&altmode->pan_ack, 5 * HZ);
	if (!left) {
		dev_err(altmode->dev, "timeout waiting for pan enable ack\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void pmic_glink_altmode_callback(const void *data, size_t len, void *priv)
{
	struct pmic_glink_altmode *altmode = priv;
	const struct usbc_notify_ind_msg *notify;
	const struct pmic_glink_hdr *hdr = data;
	unsigned int opcode;

	dev_err(altmode->dev, "owner: %d, type: %d opcode: %#x\n", hdr->owner, hdr->type, hdr->opcode);

	opcode = le32_to_cpu(hdr->opcode) & 0xff;
	switch (opcode) {
	case USBC_CMD_WRITE_REQ:
		complete(&altmode->pan_ack);
		break;
	case USBC_NOTIFY_IND:
		notify = data;

		print_hex_dump(KERN_ERR, "ALTMODE NOTIFY ", DUMP_PREFIX_OFFSET, 16, 1, notify->payload, NOTIFY_PAYLOAD_SIZE, true);
		break;
	}
}

static int pmic_glink_altmode_probe(struct auxiliary_device *adev,
			       const struct auxiliary_device_id *id)
{
	struct pmic_glink_altmode *altmode;

	altmode = devm_kzalloc(&adev->dev, sizeof(*altmode), GFP_KERNEL);
	if (!altmode)
		return -ENOMEM;

	altmode->dev = &adev->dev;
	altmode->pmic = dev_get_drvdata(adev->dev.parent);

	init_completion(&altmode->pan_ack);

	altmode->owner = pmic_glink_register_callback(altmode->pmic, MSG_OWNER_USBC_PAN,
						      pmic_glink_altmode_callback, altmode);
	if (IS_ERR(altmode->owner))
		return PTR_ERR(altmode->owner);
	
	pmic_glink_altmode_enable(altmode);

	return 0;
}

static const struct auxiliary_device_id pmic_glink_altmode_id_table[] = {
	{ .name = "pmic_glink.altmode", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, pmic_glink_altmode_id_table);

static struct auxiliary_driver pmic_glink_altmode_driver = {
	.name = "pmic_glink_altmode",
	.probe = pmic_glink_altmode_probe,
	.id_table = pmic_glink_altmode_id_table,
};

module_auxiliary_driver(pmic_glink_altmode_driver);

MODULE_DESCRIPTION("Qualcomm PMIC GLINK Altmode driver");
MODULE_LICENSE("GPL v2");
