// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, Linaro Ltd
 */

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <drm/drm_connector.h>

#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>

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

enum dp_altmode_pin_assignment {
	DPAM_HPD_OUT,
	DPAM_HPD_A,
	DPAM_HPD_B,
	DPAM_HPD_C,
	DPAM_HPD_D,
	DPAM_HPD_E,
	DPAM_HPD_F,
};

struct pmic_glink_altmode_port {
	unsigned int index;

	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;
	struct typec_mux_state state;
	struct typec_altmode dp_alt;
	struct fwnode_handle *dp_fwnode;

	enum typec_orientation orientation;
	u16 svid;
	u8 dp_data;
};

struct pmic_glink_altmode {
	struct device *dev;
	struct pmic_glink *pmic;

	struct completion pan_ack;
	struct pmic_glink_owner *owner;

	struct work_struct work;

	struct pmic_glink_altmode_port ports[2];
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

static int pmic_glink_altmode_ack(struct pmic_glink_altmode *altmode, int port)
{
	unsigned long left;
	int ret;

	ret = pmic_glink_altmode_write(altmode, ALTMODE_PAN_ACK, port);
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
	struct pmic_glink_altmode_port *alt_port;
	struct pmic_glink_altmode *altmode = priv;
	const struct usbc_notify_ind_msg *notify;
	const struct pmic_glink_hdr *hdr = data;
	u16 opcode;
	u16 svid;

	opcode = le32_to_cpu(hdr->opcode) & 0xff;
	svid = le32_to_cpu(hdr->opcode) >> 16;
	switch (opcode) {
	case USBC_CMD_WRITE_REQ:
		complete(&altmode->pan_ack);
		break;
	case USBC_NOTIFY_IND:
		notify = data;

		alt_port = &altmode->ports[0];

		switch (notify->payload[1]) {
		case 0:
			alt_port->orientation = TYPEC_ORIENTATION_NORMAL;
			break;
		case 1:
			alt_port->orientation = TYPEC_ORIENTATION_REVERSE;
			break;
		case 2:
		default:
			alt_port->orientation = TYPEC_ORIENTATION_NONE;
			break;
		}

		alt_port->svid = svid;
		alt_port->dp_data = notify->payload[8];
		schedule_work(&altmode->work);

		break;
	}
}

static void pmic_glink_altmode_enable_dp(struct pmic_glink_altmode *altmode,
					 struct pmic_glink_altmode_port *port,
					 u8 dpam_state, bool hpd_state,
					 bool hpd_irq)
{
	struct typec_displayport_data dp_data = {};
	unsigned int mode;
	int ret;

	mode = dpam_state - DPAM_HPD_A;

	dp_data.status = DP_STATUS_ENABLED;
	if (hpd_state)
		dp_data.status |= DP_STATUS_HPD_STATE;
	if (hpd_irq)
		dp_data.status |= DP_STATUS_IRQ_HPD;
	dp_data.conf = DP_CONF_SET_PIN_ASSIGN(mode);

	port->state.alt = &port->dp_alt;
	port->state.data = &dp_data;
	port->state.mode = TYPEC_MODAL_STATE(mode);

	ret = typec_mux_set(port->typec_mux, &port->state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to DP\n");
}

static void pmic_glink_altmode_enable_usb(struct pmic_glink_altmode *altmode,
					  struct pmic_glink_altmode_port *port)
{
	int ret;

	port->state.alt = NULL;
	port->state.data = NULL;
	port->state.mode = TYPEC_STATE_USB;

	ret = typec_mux_set(port->typec_mux, &port->state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to USB\n");
}

static void pmic_glink_altmode_worker(struct work_struct *work)
{
	struct pmic_glink_altmode_port *alt_port;
	struct pmic_glink_altmode *altmode = container_of(work, struct pmic_glink_altmode, work);
	unsigned int num_lanes;
	u8 dpam_state;
	bool hpd_state;
	bool hpd_irq;

	alt_port = &altmode->ports[0];

	dpam_state = alt_port->dp_data & 0x3f;
	hpd_state = alt_port->dp_data & BIT(6);
	hpd_irq = alt_port->dp_data & BIT(7);

	switch (dpam_state) {
	case DPAM_HPD_A:
	case DPAM_HPD_C:
	case DPAM_HPD_E:
		num_lanes = 4;
		break;
	case DPAM_HPD_B:
	case DPAM_HPD_D:
	case DPAM_HPD_F:
		num_lanes = 2;
		break;
	case DPAM_HPD_OUT:
	default:
		num_lanes = 0;
		break;
	}

	typec_switch_set(alt_port->typec_switch, alt_port->orientation);

	if (alt_port->svid == USB_TYPEC_DP_SID) {
		pmic_glink_altmode_enable_dp(altmode, alt_port, dpam_state,
					     hpd_state, hpd_irq);
	} else {
		pmic_glink_altmode_enable_usb(altmode, alt_port);
	}

	drm_connector_oob_hotplug_event(alt_port->dp_fwnode, num_lanes);

	pmic_glink_altmode_ack(altmode, alt_port->index);
};

static int pmic_glink_altmode_probe(struct auxiliary_device *adev,
				    const struct auxiliary_device_id *id)
{
	struct pmic_glink_altmode_port *alt_port;
	struct pmic_glink_altmode *altmode;
	struct typec_altmode_desc mux_desc = {};
	struct fwnode_handle *fwnode;
	unsigned int port = 0;
	int ret;

	altmode = devm_kzalloc(&adev->dev, sizeof(*altmode), GFP_KERNEL);
	if (!altmode)
		return -ENOMEM;

	INIT_WORK(&altmode->work, pmic_glink_altmode_worker);

	altmode->dev = &adev->dev;
	altmode->pmic = dev_get_drvdata(adev->dev.parent);

	init_completion(&altmode->pan_ack);

	device_for_each_child_node(&adev->dev, fwnode) {
		if (port >= ARRAY_SIZE(altmode->ports)) {
			dev_err(&adev->dev, "too many connectors, ignoring\n");
			continue;
		}

		alt_port = &altmode->ports[port];
		alt_port->index = port;

		alt_port->dp_fwnode = fwnode_graph_get_remote_node(fwnode, 0, 0);
		if (!alt_port->dp_fwnode)
			dev_dbg(&adev->dev, "no displayport reference\n");

		alt_port->dp_alt.svid = USB_TYPEC_DP_SID;
		alt_port->dp_alt.mode = USB_TYPEC_DP_MODE;
		alt_port->dp_alt.active = 1;

		alt_port->state.alt = NULL;
		alt_port->state.mode = TYPEC_STATE_USB;
		alt_port->state.data = NULL;

		mux_desc.svid = USB_TYPEC_DP_SID;
		mux_desc.mode = USB_TYPEC_DP_MODE;
		alt_port->typec_mux = fwnode_typec_mux_get(fwnode, &mux_desc);
		if (IS_ERR(alt_port->typec_mux)) {
			ret = dev_err_probe(&adev->dev, PTR_ERR(alt_port->typec_mux),
					    "failed to acquire mode-switch for port: %d\n",
					    port);
			goto err_rollback_current;
		}

		alt_port->typec_switch = fwnode_typec_switch_get(fwnode);
		if (IS_ERR(alt_port->typec_switch)) {
			ret = dev_err_probe(&adev->dev, PTR_ERR(alt_port->typec_switch),
					    "failed to acquire orientation-switch for port: %d\n",
					    port);
			goto err_rollback_current;
		}

		port++;
	}

	altmode->owner = pmic_glink_register_callback(altmode->pmic, MSG_OWNER_USBC_PAN,
						      pmic_glink_altmode_callback, altmode);
	if (IS_ERR(altmode->owner)) {
		ret = PTR_ERR(altmode->owner);
		goto err_put_all;
	}

	pmic_glink_altmode_enable(altmode);

	return 0;

err_put_all:
	port--;
err_rollback_current:
	for (; port >= 0; port--) {
		alt_port = &altmode->ports[port];

		fwnode_handle_put(alt_port->dp_fwnode);
		typec_mux_put(alt_port->typec_mux);
		typec_switch_put(alt_port->typec_switch);
	}

	return ret;
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
