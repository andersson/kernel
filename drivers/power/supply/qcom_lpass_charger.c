// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/regulator/driver.h>
#include <linux/rpmsg.h>

#define MSG_OWNER_BC		32778
#define MSG_TYPE_NOTIFY		2
#define MSG_TYPE_REQ_RESP	1

#define BC_SET_NOTIFY_REQ	0x4
#define BC_NOTIFY_IND		0x7
#define BC_BATTERY_STATUS_GET	0x30
#define BC_BATTERY_STATUS_SET	0x31
#define BC_USB_STATUS_GET	0x32
#define BC_USB_STATUS_SET	0x33
#define BC_WLS_STATUS_GET	0x34
#define BC_WLS_STATUS_SET	0x35

#define MODEL_NAME_LEN		128

struct lpass_charger {
	struct rpmsg_endpoint *ept;
	struct device *dev;

	struct power_supply *bat_psy;
	struct power_supply *usb_psy;
	struct power_supply *wls_psy;

	char bat_model[MODEL_NAME_LEN];
	u32 value;
	int error;
	struct completion ack;

	struct mutex lock;
};

struct lpass_charger_hdr {
	__le32 owner;
	__le32 type;
	__le32 opcode;
};

struct lpass_charger_enable_notifications {
	struct lpass_charger_hdr hdr;
	__le32 battery_id;
	__le32 power_state;
	__le32 low_capacity;
	__le32 high_capacity;
};

struct lpass_charger_req {
	struct lpass_charger_hdr hdr;
	__le32 battery;
	__le32 property;
	__le32 value;
};

struct lpass_charger_resp {
	struct lpass_charger_hdr hdr;
	__le32 property;
	union {
		struct {
			__le32 value;
			__le32 result;
		} intval;
		struct {
			char model[MODEL_NAME_LEN];
		} strval;
	};
};

struct lpass_charger_notification {
	struct lpass_charger_hdr hdr;
	__le32 notification;
};

enum {
	BATT_STATUS,
	BATT_HEALTH,
	BATT_PRESENT,
	BATT_CHG_TYPE,
	BATT_CAPACITY,
	BATT_SOH,
	BATT_VOLT_OCV,
	BATT_VOLT_NOW,
	BATT_VOLT_MAX,
	BATT_CURR_NOW,
	BATT_CHG_CTRL_LIM,
	BATT_CHG_CTRL_LIM_MAX,
	BATT_TEMP,
	BATT_TECHNOLOGY,
	BATT_CHG_COUNTER,
	BATT_CYCLE_COUNT,
	BATT_CHG_FULL_DESIGN,
	BATT_CHG_FULL,
	BATT_MODEL_NAME,
	BATT_TTF_AVG,
	BATT_TTE_AVG,
	BATT_RESISTANCE,
	BATT_POWER_NOW,
	BATT_POWER_AVG,
};

enum {
	USB_ONLINE,
	USB_VOLT_NOW,
	USB_VOLT_MAX,
	USB_CURR_NOW,
	USB_CURR_MAX,
	USB_INPUT_CURR_LIMIT,
	USB_TYPE,
	USB_ADAP_TYPE,
	USB_MOISTURE_DET_EN,
	USB_MOISTURE_DET_STS,
};

enum {
	WLS_ONLINE,
	WLS_VOLT_NOW,
	WLS_VOLT_MAX,
	WLS_CURR_NOW,
	WLS_CURR_MAX,
	WLS_TYPE,
	WLS_BOOST_EN,
};

static int lpass_charger_send(struct lpass_charger *lpc, void *data, size_t len)
{
	unsigned long left;
	int ret;

	reinit_completion(&lpc->ack);

	print_hex_dump(KERN_ERR, "> LPASS ", DUMP_PREFIX_OFFSET, 16, 1, data, len, true);
	ret = rpmsg_trysend(lpc->ept, data, len);
	if (ret < 0)
		return ret;

	left = wait_for_completion_timeout(&lpc->ack, HZ);
	if (!left)
		return -ETIMEDOUT;

	return 0;
}

static int lpass_charger_enable_notifications(struct lpass_charger *lpc)
{
	struct lpass_charger_enable_notifications req = {
		.hdr.owner = MSG_OWNER_BC,
		.hdr.type = MSG_TYPE_NOTIFY,
		.hdr.opcode = BC_SET_NOTIFY_REQ,
	};
	int ret;

	mutex_lock(&lpc->lock);
	ret = lpass_charger_send(lpc, &req, sizeof(req));
	mutex_unlock(&lpc->lock);

	return ret;
}

static int lpass_charger_request(struct lpass_charger *lpc, int opcode,
				 int property, u32 value)
{
	struct {
		struct lpass_charger_hdr hdr;
		__le32 battery;
		__le32 property;
		__le32 value;
	} request = {
		.hdr.owner = cpu_to_le32(MSG_OWNER_BC),
		.hdr.type = cpu_to_le32(MSG_TYPE_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.battery = cpu_to_le32(0),
		.property = cpu_to_le32(property),
		.value = cpu_to_le32(value),
	};

	return lpass_charger_send(lpc, &request, sizeof(request));
}

static void lpass_charger_notification(struct lpass_charger *lpc,
				       const struct lpass_charger_notification *msg,
				       int len)
{
	if (len != sizeof(*msg)) {
		dev_warn(lpc->dev, "ignoring notification with invalid length\n");
		return;
	}

	switch (msg->notification) {
	case BC_BATTERY_STATUS_GET:
		power_supply_changed(lpc->bat_psy);
		break;
	case BC_USB_STATUS_GET:
		power_supply_changed(lpc->usb_psy);
		break;
	case BC_WLS_STATUS_GET:
		power_supply_changed(lpc->wls_psy);
		break;
	}
}

static void lpass_charger_response(struct lpass_charger *lpc,
				   struct lpass_charger_resp *resp, int len)
{
	unsigned int property;
	int payload_len = len - sizeof(struct lpass_charger_hdr);
	int opcode;

	if (payload_len < sizeof(__le32)) {
		dev_warn(lpc->dev, "ignoring response %d of invalid size %d\n",
			 resp->hdr.opcode, len);
		return;
	}

	opcode = le32_to_cpu(resp->hdr.opcode);
	property = le32_to_cpu(resp->property);

	if (opcode == BC_BATTERY_STATUS_GET && property == BATT_MODEL_NAME) {
		if (payload_len != sizeof(__le32) + MODEL_NAME_LEN) {
			dev_warn(lpc->dev, "received short model response\n");
			lpc->bat_model[0] = '\0';
			lpc->error = -ENODATA;
		} else {
			strlcpy(lpc->bat_model, resp->strval.model, sizeof(lpc->bat_model));
		}
	} else if (opcode == BC_SET_NOTIFY_REQ) {
		lpc->value = 0;
		lpc->error = 0;
	} else {
		if (payload_len != 3 * sizeof(__le32)) {
			dev_warn(lpc->dev,
				 "received response of invalid length %d\n",
				 len);
			lpc->error = -ENODATA;
		} else {
			lpc->value = le32_to_cpu(resp->intval.value);
			lpc->error = le32_to_cpu(resp->intval.result);
		}
	}

	complete(&lpc->ack);
}

static int lpass_charger_callback(struct rpmsg_device *rpdev, void *data,
				  int len, void *priv, u32 addr)
{
	struct lpass_charger *lpc = dev_get_drvdata(&rpdev->dev);
	struct lpass_charger_hdr *hdr = data;

	print_hex_dump(KERN_ERR, "< LPASS ", DUMP_PREFIX_OFFSET, 16, 1, data, len, true);
	if (len < sizeof(*hdr)) {
		dev_warn(lpc->dev, "ignoring truncated message\n");
		return 0;
	}

	if (le32_to_cpu(hdr->opcode) == BC_NOTIFY_IND)
		lpass_charger_notification(lpc, data, len);
	else
		lpass_charger_response(lpc, data, len);

	return 0;
}

static int lpass_charger_bat_get(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct lpass_charger *lpc = power_supply_get_drvdata(psy);
	int prop;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		prop = BATT_STATUS;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		prop = BATT_HEALTH;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		prop = BATT_PRESENT;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		prop = BATT_CHG_TYPE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		prop = BATT_CAPACITY;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		prop = BATT_VOLT_OCV;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		prop = BATT_VOLT_NOW;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		prop = BATT_VOLT_MAX;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		prop = BATT_CURR_NOW;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		prop = BATT_CHG_CTRL_LIM;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		prop = BATT_CHG_CTRL_LIM_MAX;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		prop = BATT_TEMP;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		prop = BATT_TECHNOLOGY;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		prop = BATT_CHG_COUNTER;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		prop = BATT_CYCLE_COUNT;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		prop = BATT_CHG_FULL_DESIGN;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		prop = BATT_CHG_FULL;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		prop = BATT_MODEL_NAME;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		prop = BATT_TTF_AVG;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		prop = BATT_TTE_AVG;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		prop = BATT_POWER_NOW;
		break;
	case POWER_SUPPLY_PROP_POWER_AVG:
		prop = BATT_POWER_AVG;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&lpc->lock);
	ret = lpass_charger_request(lpc, BC_BATTERY_STATUS_GET, prop, 0);
	if (ret < 0)
		goto out_unlock;

	if (lpc->error) {
		ret = lpc->error;
		goto out_unlock;
	}

	if (psp == POWER_SUPPLY_PROP_MODEL_NAME)
		val->strval = lpc->bat_model;
	else
		val->intval = lpc->value;

out_unlock:
	mutex_unlock(&lpc->lock);
	return ret;
}

static int lpass_charger_bat_set(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	return 0;
}

static int lpass_charger_bat_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	return 0;
}

static const enum power_supply_property bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
};

static const struct power_supply_desc bat_psy_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = bat_props,
	.num_properties = ARRAY_SIZE(bat_props),
	.get_property = lpass_charger_bat_get,
	.set_property = lpass_charger_bat_set,
	.property_is_writeable = lpass_charger_bat_is_writeable,
};

static int lpass_charger_usb_get(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct lpass_charger *lpc = power_supply_get_drvdata(psy);
	int prop;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		prop = USB_ONLINE;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		prop = USB_VOLT_NOW;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		prop = USB_VOLT_MAX;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		prop = USB_CURR_NOW;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		prop = USB_CURR_MAX;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		prop = USB_INPUT_CURR_LIMIT;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		prop = USB_TYPE;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&lpc->lock);
	ret = lpass_charger_request(lpc, BC_USB_STATUS_GET, prop, 0);
	if (ret < 0)
		goto out_unlock;

	if (lpc->error)
		ret = lpc->error;
	else
		val->intval = lpc->value;

out_unlock:
	mutex_unlock(&lpc->lock);

	return ret;
}

static int lpass_charger_usb_set(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	return 0;
}

static int lpass_charger_usb_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	return 0;
}

static const enum power_supply_property usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static const enum power_supply_usb_type usb_psy_supported_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_ACA,
	POWER_SUPPLY_USB_TYPE_C,
	POWER_SUPPLY_USB_TYPE_PD,
	POWER_SUPPLY_USB_TYPE_PD_DRP,
	POWER_SUPPLY_USB_TYPE_PD_PPS,
	POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
};

static const struct power_supply_desc usb_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = usb_props,
	.num_properties = ARRAY_SIZE(usb_props),
	.get_property = lpass_charger_usb_get,
	.set_property = lpass_charger_usb_set,
	.usb_types = usb_psy_supported_types,
	.num_usb_types = ARRAY_SIZE(usb_psy_supported_types),
	.property_is_writeable = lpass_charger_usb_is_writeable,
};

static int lpass_charger_wls_get(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct lpass_charger *lpc = power_supply_get_drvdata(psy);
	int prop;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		prop = WLS_ONLINE;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		prop = WLS_VOLT_NOW;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		prop = WLS_VOLT_MAX;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		prop = WLS_CURR_NOW;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		prop = WLS_CURR_MAX;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&lpc->lock);
	ret = lpass_charger_request(lpc, BC_WLS_STATUS_GET, prop, 0);
	if (ret < 0)
		goto out_unlock;

	if (lpc->error)
		ret = lpc->error;
	else
		val->intval = lpc->value;

out_unlock:
	mutex_unlock(&lpc->lock);

	return ret;
}

static int lpass_charger_wls_set(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	return 0;
}

static int lpass_charger_wls_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static const enum power_supply_property wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const struct power_supply_desc wls_psy_desc = {
	.name = "wireless",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = wls_props,
	.num_properties = ARRAY_SIZE(wls_props),
	.get_property = lpass_charger_wls_get,
	.set_property = lpass_charger_wls_set,
	.property_is_writeable = lpass_charger_wls_is_writeable,
};

static int lpass_charger_probe(struct rpmsg_device *rpdev)
{
	struct power_supply_config psy_cfg = {};
	struct lpass_charger *lpc;

	lpc = devm_kzalloc(&rpdev->dev, sizeof(*lpc), GFP_KERNEL);
	if (!lpc)
		return -ENOMEM;

	init_completion(&lpc->ack);
	mutex_init(&lpc->lock);

	lpc->dev = &rpdev->dev;
	lpc->ept = rpdev->ept;

	dev_set_drvdata(&rpdev->dev, lpc);

	psy_cfg.drv_data = lpc;
	psy_cfg.of_node = rpdev->dev.of_node;

	lpc->bat_psy = devm_power_supply_register(&rpdev->dev, &bat_psy_desc, &psy_cfg);
	if (IS_ERR(lpc->bat_psy)) {
		dev_err(&rpdev->dev,
			"failed to register battery power supply: %ld\n",
			PTR_ERR(lpc->bat_psy));
		return PTR_ERR(lpc->bat_psy);
	}

	lpc->usb_psy = devm_power_supply_register(&rpdev->dev, &usb_psy_desc, &psy_cfg);
	if (IS_ERR(lpc->usb_psy)) {
		dev_err(&rpdev->dev,
			"failed to register usb power supply: %ld\n",
			PTR_ERR(lpc->usb_psy));
		return PTR_ERR(lpc->usb_psy);
	}

	lpc->wls_psy = devm_power_supply_register(&rpdev->dev, &wls_psy_desc, &psy_cfg);
	if (IS_ERR(lpc->wls_psy)) {
		dev_err(&rpdev->dev,
			"failed to register wireless charging power supply: %ld\n",
			PTR_ERR(lpc->wls_psy));
		return PTR_ERR(lpc->wls_psy);
	}

	return lpass_charger_enable_notifications(lpc);
}

static const struct of_device_id lpass_charger_of_match[] = {
	{ .compatible = "qcom,lpass-charger", },
	{}
};
MODULE_DEVICE_TABLE(of, lpass_charger_of_match);

static const struct rpmsg_device_id lpass_charger_id_match[] = {
	{ "PMIC_RTR_ADSP_APPS" },
	{}
};

static struct rpmsg_driver lpass_charger_driver = {
	.probe = lpass_charger_probe,
	.callback = lpass_charger_callback,
	.id_table = lpass_charger_id_match,
	.drv  = {
		.name  = "qcom_lpass_charger",
	},
};

module_rpmsg_driver(lpass_charger_driver);

MODULE_DESCRIPTION("Qualcomm LPASS charger driver");
MODULE_LICENSE("GPL v2");
