// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2017, The Linux Foundation
 * Copyright (c) 2018, Linaro Limited
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/rpmsg.h>
#include <linux/of.h>

struct apr {
	struct rpmsg_endpoint *ch;
	struct device *dev;
	spinlock_t svcs_lock;
	struct list_head svcs;
	int dest_domain_id;
};

/**
 * apr_send_pkt() - Send a apr message from apr device
 *
 * @adev: Pointer to previously registered apr device.
 * @buf: Pointer to buffer to send
 *
 * Return: Will be an negative on packet size on success.
 */
int apr_send_pkt(struct apr_device *adev, void *buf)
{
	struct apr *apr = dev_get_drvdata(adev->dev.parent);
	struct apr_hdr *hdr;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&adev->lock, flags);

	hdr = (struct apr_hdr *)buf;
	hdr->src_domain = APR_DOMAIN_APPS;
	hdr->src_svc = adev->svc_id;
	hdr->dest_domain = adev->domain_id;
	hdr->dest_svc = adev->svc_id;

	ret = rpmsg_send(apr->ch, buf, hdr->pkt_size);
	if (ret) {
		dev_err(&adev->dev, "Unable to send APR pkt %d\n",
			hdr->pkt_size);
	} else {
		ret = hdr->pkt_size;
	}

	spin_unlock_irqrestore(&adev->lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(apr_send_pkt);

static void apr_dev_release(struct device *dev)
{
	struct apr_device *adev = to_apr_device(dev);

	kfree(adev);
}

static int apr_callback(struct rpmsg_device *rpdev, void *buf,
				  int len, void *priv, u32 addr)
{
	struct apr *apr = dev_get_drvdata(&rpdev->dev);
	struct apr_client_message data;
	struct apr_device *p, *c_svc = NULL;
	struct apr_driver *adrv = NULL;
	struct apr_hdr *hdr;
	uint16_t hdr_size;
	uint16_t msg_type;
	uint16_t ver;
	uint16_t svc;

	if (len <= APR_HDR_SIZE) {
		dev_err(apr->dev, "APR: Improper apr pkt received:%p %d\n",
			buf, len);
		return -EINVAL;
	}

	hdr = buf;
	ver = APR_HDR_FIELD_VER(hdr->hdr_field);
	if (ver > APR_PKT_VER + 1)
		return -EINVAL;

	hdr_size = APR_HDR_FIELD_SIZE_BYTES(hdr->hdr_field);
	if (hdr_size < APR_HDR_SIZE) {
		dev_err(apr->dev, "APR: Wrong hdr size:%d\n", hdr_size);
		return -EINVAL;
	}

	if (hdr->pkt_size < APR_HDR_SIZE) {
		dev_err(apr->dev, "APR: Wrong paket size\n");
		return -EINVAL;
	}

	msg_type = APR_HDR_FIELD_MT(hdr->hdr_field);
	if (msg_type >= APR_MSG_TYPE_MAX && msg_type != APR_BASIC_RSP_RESULT) {
		dev_err(apr->dev, "APR: Wrong message type: %d\n", msg_type);
		return -EINVAL;
	}

	if (hdr->src_domain >= APR_DOMAIN_MAX ||
			hdr->dest_domain >= APR_DOMAIN_MAX ||
			hdr->src_svc >= APR_SVC_MAX ||
			hdr->dest_svc >= APR_SVC_MAX) {
		dev_err(apr->dev, "APR: Wrong APR header\n");
		return -EINVAL;
	}

	svc = hdr->dest_svc;
	spin_lock(&apr->svcs_lock);
	list_for_each_entry(p, &apr->svcs, node) {
		if (svc == p->svc_id) {
			c_svc = p;
			if (c_svc->dev.driver)
				adrv = to_apr_driver(c_svc->dev.driver);
			break;
		}
	}
	spin_unlock(&apr->svcs_lock);

	if (!adrv) {
		dev_err(apr->dev, "APR: service is not registered\n");
		return -EINVAL;
	}

	data.payload_size = hdr->pkt_size - hdr_size;
	data.opcode = hdr->opcode;
	data.src_port = hdr->src_port;
	data.dest_port = hdr->dest_port;
	data.token = hdr->token;
	data.msg_type = msg_type;

	if (data.payload_size > 0)
		data.payload = buf + hdr_size;

	adrv->callback(c_svc, &data);

	return 0;
}

static int apr_device_match(struct device *dev, struct device_driver *drv)
{
	struct apr_device *adev = to_apr_device(dev);
	struct apr_driver *adrv = to_apr_driver(drv);
	const struct apr_device_id *id = adrv->id_table;

	/* Attempt an OF style match first */
	if (of_driver_match_device(dev, drv))
		return 1;

	if (!id)
		return 0;

	while (id->domain_id != 0 || id->svc_id != 0) {
		if (id->domain_id == adev->domain_id &&
		    id->svc_id == adev->svc_id)
			return 1;
		id++;
	}

	return 0;
}

static int apr_device_probe(struct device *dev)
{
	struct apr_device *adev = to_apr_device(dev);
	struct apr_driver *adrv = to_apr_driver(dev->driver);

	return adrv->probe(adev);
}

static int apr_device_remove(struct device *dev)
{
	struct apr_device *adev = to_apr_device(dev);
	struct apr_driver *adrv;
	struct apr *apr = dev_get_drvdata(adev->dev.parent);

	if (dev->driver) {
		adrv = to_apr_driver(dev->driver);
		if (adrv->remove)
			adrv->remove(adev);
		spin_lock(&apr->svcs_lock);
		list_del(&adev->node);
		spin_unlock(&apr->svcs_lock);
	}

	return 0;
}

struct bus_type aprbus_type = {
	.name		= "aprbus",
	.match		= apr_device_match,
	.probe		= apr_device_probe,
	.remove		= apr_device_remove,
	.force_dma	= true,
};
EXPORT_SYMBOL_GPL(aprbus_type);

static int apr_add_device(struct device *dev, struct device_node *np,
			  const struct apr_device_id *id, bool is_svc)
{
	struct apr *apr = dev_get_drvdata(dev);
	struct apr_device *adev = NULL;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	spin_lock_init(&adev->lock);

	if (is_svc) {
		adev->svc_id = id->svc_id;
		adev->domain_id = id->domain_id;
		adev->version = id->svc_version;
		dev_set_name(&adev->dev, "aprsvc:%s:%x:%x", id->name,
			     id->domain_id, id->svc_id);
	} else  {
		dev_set_name(&adev->dev, "%s:%s", dev_name(dev), np->name);
	}

	adev->dev.bus = &aprbus_type;
	adev->dev.parent = dev;
	adev->dev.of_node = np;
	adev->dev.release = apr_dev_release;
	adev->dev.driver = NULL;

	spin_lock(&apr->svcs_lock);
	list_add_tail(&adev->node, &apr->svcs);
	spin_unlock(&apr->svcs_lock);

	dev_info(dev, "Adding APR dev: %s\n", dev_name(&adev->dev));

	return device_register(&adev->dev);
}

static void of_register_apr_devices(struct device *dev)
{
	struct apr *apr = dev_get_drvdata(dev);
	struct device_node *node;

	for_each_child_of_node(dev->of_node, node) {
		struct apr_device_id id = {0};
		const char *svc_name;
		bool is_svc = false;

		if (of_find_property(node, "qcom,apr-svc-id", NULL) &&
		    of_find_property(node, "qcom,apr-svc-name", NULL)) {
			/* svc node */
			of_property_read_u32(node, "qcom,apr-svc-id",
					     &id.svc_id);
			of_property_read_string(node, "qcom,apr-svc-name",
						&svc_name);
			id.domain_id = apr->dest_domain_id;

			memcpy(id.name, svc_name, strlen(svc_name) + 1);
			is_svc = true;
		}

		if (apr_add_device(dev, node, &id, is_svc))
			dev_err(dev, "Failed to add arp %s svc\n", svc_name);
	}
}

static int apr_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct apr *apr;
	int ret;

	apr = devm_kzalloc(dev, sizeof(*apr), GFP_KERNEL);
	if (!apr)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, "qcom,apr-dest-domain-id",
				   &apr->dest_domain_id);
	if (ret) {
		dev_err(dev, "APR Domain ID not specified in DT\n");
		return ret;
	}

	dev_set_drvdata(dev, apr);
	apr->ch = rpdev->ept;
	apr->dev = dev;
	INIT_LIST_HEAD(&apr->svcs);

	of_register_apr_devices(dev);

	return 0;
}

static int apr_remove_device(struct device *dev, void *null)
{
	struct apr_device *adev = to_apr_device(dev);

	device_unregister(&adev->dev);

	return 0;
}

static void apr_remove(struct rpmsg_device *rpdev)
{
	device_for_each_child(&rpdev->dev, NULL, apr_remove_device);
}

/*
 * __apr_driver_register() - Client driver registration with aprbus
 *
 * @drv:Client driver to be associated with client-device.
 * @owner: owning module/driver
 *
 * This API will register the client driver with the aprbus
 * It is called from the driver's module-init function.
 */
int __apr_driver_register(struct apr_driver *drv, struct module *owner)
{
	drv->driver.bus = &aprbus_type;
	drv->driver.owner = owner;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__apr_driver_register);

/*
 * apr_driver_unregister() - Undo effect of apr_driver_register
 *
 * @drv: Client driver to be unregistered
 */
void apr_driver_unregister(struct apr_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(apr_driver_unregister);

static const struct of_device_id apr_of_match[] = {
	{ .compatible = "qcom,apr"},
	{ .compatible = "qcom,apr-v2"},
	{}
};

static struct rpmsg_driver apr_driver = {
	.probe = apr_probe,
	.remove = apr_remove,
	.callback = apr_callback,
	.drv = {
		.name = "qcom,apr",
		.of_match_table = apr_of_match,
	},
};

static int __init apr_init(void)
{
	int ret;

	ret = bus_register(&aprbus_type);
	if (!ret)
		ret = register_rpmsg_driver(&apr_driver);

	return ret;
}

static void __exit apr_exit(void)
{
	bus_unregister(&aprbus_type);
	unregister_rpmsg_driver(&apr_driver);
}

subsys_initcall(apr_init);
module_exit(apr_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm APR Bus");
