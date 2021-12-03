// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
// Copyright (c) 2021, Linaro Ltd

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/soc/qcom/pdr.h>

#define UC_UCSI_READ_BUF_REQ            0x11
#define UC_UCSI_WRITE_BUF_REQ           0x12
#define UC_UCSI_USBC_NOTIFY_IND         0x13

struct pmic_glink_hdr {
	u32 owner;
	u32 type;
	u32 opcode;
};

struct pmic_glink {
	struct device *dev;
	struct pdr_handle *pdr;

	struct rpmsg_endpoint *ept;

	struct auxiliary_device altmode_aux;
	struct auxiliary_device ps_aux;
	struct auxiliary_device ucsi_aux;

	struct mutex lock;
	struct list_head owners;
};

struct pmic_glink_owner {
	struct list_head node;

	unsigned int id;

	void (*cb)(const void *, size_t, void *);
	void *priv;
};

struct pmic_glink_owner *pmic_glink_register_callback(struct pmic_glink *pg,
						      unsigned int id,
						      void (*cb)(const void *, size_t, void *),
						      void *priv)
{
	struct pmic_glink_owner *owner;

	owner = kzalloc(sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return ERR_PTR(-ENOMEM);

	owner->id = id;
	owner->cb = cb;
	owner->priv = priv;

	mutex_lock(&pg->lock);
	list_add(&owner->node, &pg->owners);
	mutex_unlock(&pg->lock);

	return owner;
}
EXPORT_SYMBOL_GPL(pmic_glink_register_callback);

void pmic_glink_unregister_callback(struct pmic_glink *pg,
				    struct pmic_glink_owner *owner)
{
	mutex_lock(&pg->lock);
	list_del(&owner->node);
	mutex_unlock(&pg->lock);
	kfree(owner);
}

int pmic_glink_send(struct pmic_glink *pg, void *data, size_t len)
{
	//print_hex_dump(KERN_ERR, "> LPASS ", DUMP_PREFIX_OFFSET, 16, 1, data, len, true);
	return rpmsg_trysend(pg->ept, data, len);
}
EXPORT_SYMBOL_GPL(pmic_glink_send);

static int pmic_glink_callback(struct rpmsg_device *rpdev, void *data,
			       int len, void *priv, u32 addr)
{
	struct pmic_glink_owner *owner;
	struct pmic_glink_hdr *hdr;
	struct pmic_glink *pg = dev_get_drvdata(&rpdev->dev);

	if (len < sizeof(*hdr)) {
		dev_warn(pg->dev, "ignoring truncated message\n");
		return 0;
	}

	hdr = data;

	list_for_each_entry(owner, &pg->owners, node) {
		if (owner->id == le32_to_cpu(hdr->owner))
			owner->cb(data, len, owner->priv);
	}

	return 0;
}

static void pmic_glink_aux_release(struct device *dev) {}

static int pmic_glink_add_aux_device(struct pmic_glink *pg,
				     struct auxiliary_device *aux,
				     const char *name)
{
	struct device *parent = pg->dev;
	int ret;

	aux->name = name;
	aux->dev.parent = parent;
	aux->dev.release = pmic_glink_aux_release;
	device_set_of_node_from_dev(&aux->dev, parent);
	ret = auxiliary_device_init(aux);
	if (ret)
		return ret;

	ret = auxiliary_device_add(aux);
	if (ret)
		auxiliary_device_uninit(aux);

	return ret;
}

static void pmic_glink_del_aux_device(struct pmic_glink *pg,
				      struct auxiliary_device *aux)
{
	auxiliary_device_delete(aux);
	auxiliary_device_uninit(aux);
}

static void pmic_glink_service_up(struct pmic_glink *pg)
{
	pmic_glink_add_aux_device(pg, &pg->altmode_aux, "altmode");
	pmic_glink_add_aux_device(pg, &pg->ps_aux, "power-supply");
	pmic_glink_add_aux_device(pg, &pg->ucsi_aux, "ucsi");
}

static void pmic_glink_service_down(struct pmic_glink *pg)
{
	pmic_glink_del_aux_device(pg, &pg->altmode_aux);
	pmic_glink_del_aux_device(pg, &pg->ps_aux);
	pmic_glink_del_aux_device(pg, &pg->ucsi_aux);
}

static void pmic_glink_pdr_callback(int state, char *svc_path, void *priv)
{
	struct pmic_glink *pg = priv;

	// printk(KERN_ERR "  %s(%d)\n", __func__, state);

	switch (state) {
	case SERVREG_SERVICE_STATE_UP:
		pmic_glink_service_up(pg);
		break;
	case SERVREG_SERVICE_STATE_DOWN:
		pmic_glink_service_down(pg);
		break;
	}
}

static int pmic_glink_probe(struct rpmsg_device *rpdev)
{
	struct pmic_glink *pg;

	pg = devm_kzalloc(&rpdev->dev, sizeof(*pg), GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	dev_set_drvdata(&rpdev->dev, pg);

	pg->dev = &rpdev->dev;
	pg->ept = rpdev->ept;

	INIT_LIST_HEAD(&pg->owners);
	mutex_init(&pg->lock);

	pg->pdr = pdr_handle_alloc(pmic_glink_pdr_callback, pg);
	if (IS_ERR(pg->pdr)) {
		dev_err(&rpdev->dev, "failed to initalize pdr\n");
		return PTR_ERR(pg->pdr);
	}

	pdr_add_lookup(pg->pdr, "tms/servreg", "msm/adsp/charger_pd");

	return 0;
}

static void pmic_glink_remove(struct rpmsg_device *rpdev)
{
	struct pmic_glink *pg = dev_get_drvdata(&rpdev->dev);

	pdr_handle_release(pg->pdr);
}

static const struct of_device_id pmic_glink_of_match[] = {
	{ .compatible = "qcom,pmic-glink", },
	{}
};
MODULE_DEVICE_TABLE(of, pmic_glink_of_match);

static const struct rpmsg_device_id pmic_glink_id_match[] = {
	{ "PMIC_RTR_ADSP_APPS" },
	{}
};

static struct rpmsg_driver pmic_glink_driver = {
	.probe = pmic_glink_probe,
	.remove = pmic_glink_remove,
	.callback = pmic_glink_callback,
	.id_table = pmic_glink_id_match,
	.drv  = {
		.name  = "qcom_pmic_glink",
	},
};
module_rpmsg_driver(pmic_glink_driver);

MODULE_DESCRIPTION("Qualcomm PMIC GLINK driver");
MODULE_LICENSE("GPL v2");
