// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/of_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/delay.h>
#include <linux/rpmsg.h>
#include <linux/of.h>

#define SERVER_TIMEOUT					500
#define SERVREG_NOTIF_SERVICE				0x42
#define SERVREG_NOTIF_STATE_UPDATED_IND_MSG		0x22
#define SERVREG_NOTIF_REGISTER_LISTENER_REQ		0x20
#define QMI_SERVREG_NOTIF_NAME_LENGTH_V01		64
#define SERVREG_NOTIF_REGISTER_LISTENER_REQ_MSG_LEN	71
#define QMI_SERVREG_NOTIF_SET_ACK_RESP_MSG_V01_MAX_MSG_LEN 7
#define SERVREG_NOTIF_SET_ACK_REQ_MSG_LEN 72
#define SERVREG_NOTIF_SET_ACK_REQ 0x23

struct apr {
	struct rpmsg_endpoint *ch;
	struct device *dev;
	spinlock_t svcs_lock;
	spinlock_t rx_lock;
	struct idr svcs_idr;
	int dest_domain_id;
	struct workqueue_struct *rxwq;
	struct work_struct rx_work;
        struct workqueue_struct *indackwq;
	struct list_head rx_list;
	struct completion comp;
	struct completion ind_comp;
	struct qmi_handle qmi;
	struct sockaddr_qrtr servreg_sock;
};

struct apr_rx_buf {
	struct list_head node;
	int len;
	uint8_t buf[];
};

struct ind_req_resp {
	int transaction_id;
	struct work_struct ind_ack;
	struct apr *client_data;
};

struct qmi_servreg_notif_state_updated_ind_msg_v01 {
	u32 curr_state;
	char service_name[QMI_SERVREG_NOTIF_NAME_LENGTH_V01 + 1];
	uint16_t transaction_id;
};

struct qmi_elem_info qmi_servreg_notif_state_updated_ind_msg_v01_ei[] = {
	{
		.data_type      = QMI_SIGNED_4_BYTE_ENUM,
		.elem_len       = 1,
		.elem_size      = sizeof(uint32_t),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct qmi_servreg_notif_state_updated_ind_msg_v01, curr_state),
	},
	{
		.data_type      = QMI_STRING,
		.elem_len       = QMI_SERVREG_NOTIF_NAME_LENGTH_V01 + 1,
		.elem_size      = sizeof(char),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct qmi_servreg_notif_state_updated_ind_msg_v01, service_name),
	},
	{
		.data_type      = QMI_UNSIGNED_2_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(uint16_t),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x03,
		.offset         = offsetof(struct qmi_servreg_notif_state_updated_ind_msg_v01, transaction_id),
	},
	{}
};

struct qmi_servreg_notif_register_listener_req_msg_v01 {
	uint8_t enable;
	char service_name[QMI_SERVREG_NOTIF_NAME_LENGTH_V01 + 1];
};

struct qmi_elem_info qmi_servreg_notif_register_listener_req_msg_v01_ei[] = {
	{
		.data_type      = QMI_UNSIGNED_1_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(uint8_t),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct qmi_servreg_notif_register_listener_req_msg_v01, enable),
	},
	{
		.data_type      = QMI_STRING,
		.elem_len       = QMI_SERVREG_NOTIF_NAME_LENGTH_V01 + 1,
		.elem_size      = sizeof(char),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct qmi_servreg_notif_register_listener_req_msg_v01, service_name),
	},
	{}
};

struct qmi_servreg_notif_register_listener_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint8_t curr_state_valid;
	u32 curr_state;
};

struct qmi_elem_info qmi_servreg_notif_register_listener_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct qmi_servreg_notif_register_listener_resp_msg_v01, resp),
		.ei_array      = qmi_response_type_v01_ei,
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(uint8_t),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct qmi_servreg_notif_register_listener_resp_msg_v01, curr_state_valid),
	},
	{
		.data_type      = QMI_SIGNED_4_BYTE_ENUM,
		.elem_len       = 1,
		.elem_size      = sizeof(uint32_t),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct qmi_servreg_notif_register_listener_resp_msg_v01, curr_state),
	},
	{}
};

struct qmi_servreg_notif_set_ack_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct qmi_elem_info qmi_servreg_notif_set_ack_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct qmi_servreg_notif_set_ack_resp_msg_v01, resp),
		.ei_array       = qmi_response_type_v01_ei,
	},
	{}
};

struct qmi_servreg_notif_set_ack_req_msg_v01 {
	char service_name[QMI_SERVREG_NOTIF_NAME_LENGTH_V01 + 1];
	uint16_t transaction_id;
};

struct qmi_elem_info qmi_servreg_notif_set_ack_req_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRING,
		.elem_len       = QMI_SERVREG_NOTIF_NAME_LENGTH_V01 + 1,
		.elem_size      = sizeof(char),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct qmi_servreg_notif_set_ack_req_msg_v01, service_name),
	},
	{
		.data_type      = QMI_UNSIGNED_2_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(uint16_t),
		.array_type	= NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct qmi_servreg_notif_set_ack_req_msg_v01, transaction_id),
	},
	{}
};

static void servreg_ind_ack(struct work_struct *work) {
	struct ind_req_resp *ind_info = container_of(work, struct ind_req_resp, ind_ack);
	struct qmi_servreg_notif_set_ack_resp_msg_v01 resp = { { 0, 0 } };
	struct qmi_servreg_notif_set_ack_req_msg_v01 req;
	struct apr *apr = ind_info->client_data;
	struct qmi_txn txn;
	int ret;

	ret = qmi_txn_init(&apr->qmi, &txn, qmi_servreg_notif_set_ack_resp_msg_v01_ei,
			   &resp);
	if (ret < 0) {
		pr_err("QMI tx init failed , ret - %d\n", ret);
		return;
	}

	req.transaction_id = ind_info->transaction_id;
	snprintf(req.service_name, ARRAY_SIZE(req.service_name), "%s",
		 "msm/adsp/audio_pd");

	ret = qmi_send_request(&apr->qmi, &apr->servreg_sock,
			       &txn, SERVREG_NOTIF_SET_ACK_REQ,
			       SERVREG_NOTIF_SET_ACK_REQ_MSG_LEN,
			       qmi_servreg_notif_set_ack_req_msg_v01_ei,
			       &req);
	if (ret < 0) {
		pr_err("QMI send ACK failed, ret - %d\n", ret);
		qmi_txn_cancel(&txn);
		return;
	}

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(SERVER_TIMEOUT));
	if (ret < 0) {
		pr_err("QMI qmi txn wait failed, ret - %d\n", ret);
		return;
	}

	/* Check the response */
	if (resp.resp.result != QMI_RESULT_SUCCESS_V01)
		pr_err("QMI request failed 0x%x\n", resp.resp.error);
	else
		pr_err("QMI ack success\n");
}

static void servreg_ind_cb(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			   struct qmi_txn *back_txn, const void *data)
{
	struct qmi_servreg_notif_state_updated_ind_msg_v01 ind_msg = *((struct qmi_servreg_notif_state_updated_ind_msg_v01 *)data);
	struct apr *apr = container_of(qmi, struct apr, qmi);
	struct ind_req_resp *ind_info = kmalloc(sizeof(*ind_info), GFP_KERNEL);

	pr_err("Indication received from %s, state: 0x%x, trans-id: %d\n",
	       ind_msg.service_name, ind_msg.curr_state, ind_msg.transaction_id);

	if (ind_msg.curr_state == 0x1FFFFFFF)
		complete(&apr->ind_comp);

	ind_info->transaction_id = ind_msg.transaction_id;
	ind_info->client_data = apr;

        INIT_WORK(&ind_info->ind_ack, servreg_ind_ack);
	queue_work(apr->indackwq, &ind_info->ind_ack);
}

static struct qmi_msg_handler qmi_indication_handler[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = SERVREG_NOTIF_STATE_UPDATED_IND_MSG,
		.ei = qmi_servreg_notif_state_updated_ind_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_servreg_notif_state_updated_ind_msg_v01),
		.fn = servreg_ind_cb,
	},
	{}
};

static int servreg_new_server(struct qmi_handle *qmi, struct qmi_service *serv)
{
	struct apr *apr = container_of(qmi, struct apr, qmi);

	pr_err("servreg_new_server\n");
	apr->servreg_sock.sq_family = AF_QIPCRTR;
	apr->servreg_sock.sq_node = serv->node;
	apr->servreg_sock.sq_port = serv->port;
	complete(&apr->comp);

	return 0;
};

static void servreg_del_server(struct qmi_handle *qmi, struct qmi_service *serv)
{
	struct apr *apr = container_of(qmi, struct apr, qmi);
	reinit_completion(&apr->comp);
	reinit_completion(&apr->ind_comp);
	pr_err("servreg_del_server\n");
};

static const struct qmi_ops servreg_ops = {
	.new_server = servreg_new_server,
	.del_server = servreg_del_server,
};

/**
 * apr_send_pkt() - Send a apr message from apr device
 *
 * @adev: Pointer to previously registered apr device.
 * @pkt: Pointer to apr packet to send
 *
 * Return: Will be an negative on packet size on success.
 */
int apr_send_pkt(struct apr_device *adev, struct apr_pkt *pkt)
{
	struct apr *apr = dev_get_drvdata(adev->dev.parent);
	struct apr_hdr *hdr;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&adev->lock, flags);

	hdr = &pkt->hdr;
	hdr->src_domain = APR_DOMAIN_APPS;
	hdr->src_svc = adev->svc_id;
	hdr->dest_domain = adev->domain_id;
	hdr->dest_svc = adev->svc_id;

	ret = rpmsg_trysend(apr->ch, pkt, hdr->pkt_size);
	spin_unlock_irqrestore(&adev->lock, flags);

	return ret ? ret : hdr->pkt_size;
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
	struct apr_rx_buf *abuf;
	unsigned long flags;

	if (len <= APR_HDR_SIZE) {
		dev_err(apr->dev, "APR: Improper apr pkt received:%p %d\n",
			buf, len);
		return -EINVAL;
	}

	abuf = kzalloc(sizeof(*abuf) + len, GFP_ATOMIC);
	if (!abuf)
		return -ENOMEM;

	abuf->len = len;
	memcpy(abuf->buf, buf, len);

	spin_lock_irqsave(&apr->rx_lock, flags);
	list_add_tail(&abuf->node, &apr->rx_list);
	spin_unlock_irqrestore(&apr->rx_lock, flags);

	queue_work(apr->rxwq, &apr->rx_work);

	return 0;
}


static int apr_do_rx_callback(struct apr *apr, struct apr_rx_buf *abuf)
{
	uint16_t hdr_size, msg_type, ver, svc_id;
	struct apr_device *svc = NULL;
	struct apr_driver *adrv = NULL;
	struct apr_resp_pkt resp;
	struct apr_hdr *hdr;
	unsigned long flags;
	void *buf = abuf->buf;
	int len = abuf->len;

	hdr = buf;
	ver = APR_HDR_FIELD_VER(hdr->hdr_field);
	if (ver > APR_PKT_VER + 1)
		return -EINVAL;

	hdr_size = APR_HDR_FIELD_SIZE_BYTES(hdr->hdr_field);
	if (hdr_size < APR_HDR_SIZE) {
		dev_err(apr->dev, "APR: Wrong hdr size:%d\n", hdr_size);
		return -EINVAL;
	}

	if (hdr->pkt_size < APR_HDR_SIZE || hdr->pkt_size != len) {
		dev_err(apr->dev, "APR: Wrong packet size\n");
		return -EINVAL;
	}

	msg_type = APR_HDR_FIELD_MT(hdr->hdr_field);
	if (msg_type >= APR_MSG_TYPE_MAX) {
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

	svc_id = hdr->dest_svc;
	spin_lock_irqsave(&apr->svcs_lock, flags);
	svc = idr_find(&apr->svcs_idr, svc_id);
	if (svc && svc->dev.driver)
		adrv = to_apr_driver(svc->dev.driver);
	spin_unlock_irqrestore(&apr->svcs_lock, flags);

	if (!adrv) {
		dev_err(apr->dev, "APR: service is not registered\n");
		return -EINVAL;
	}

	resp.hdr = *hdr;
	resp.payload_size = hdr->pkt_size - hdr_size;

	/*
	 * NOTE: hdr_size is not same as APR_HDR_SIZE as remote can include
	 * optional headers in to apr_hdr which should be ignored
	 */
	if (resp.payload_size > 0)
		resp.payload = buf + hdr_size;

	adrv->callback(svc, &resp);

	return 0;
}

static void apr_rxwq(struct work_struct *work)
{
	struct apr *apr = container_of(work, struct apr, rx_work);
	struct apr_rx_buf *abuf, *b;
	unsigned long flags;

	if (!list_empty(&apr->rx_list)) {
		list_for_each_entry_safe(abuf, b, &apr->rx_list, node) {
			apr_do_rx_callback(apr, abuf);
			spin_lock_irqsave(&apr->rx_lock, flags);
			list_del(&abuf->node);
			spin_unlock_irqrestore(&apr->rx_lock, flags);
			kfree(abuf);
		}
	}
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
		idr_remove(&apr->svcs_idr, adev->svc_id);
		spin_unlock(&apr->svcs_lock);
	}

	return 0;
}

static int apr_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct apr_device *adev = to_apr_device(dev);
	int ret;

	ret = of_device_uevent_modalias(dev, env);
	if (ret != -ENODEV)
		return ret;

	return add_uevent_var(env, "MODALIAS=apr:%s", adev->name);
}

struct bus_type aprbus = {
	.name		= "aprbus",
	.match		= apr_device_match,
	.probe		= apr_device_probe,
	.uevent		= apr_uevent,
	.remove		= apr_device_remove,
};
EXPORT_SYMBOL_GPL(aprbus);

static int apr_add_device(struct device *dev, struct device_node *np,
			  const struct apr_device_id *id)
{
	struct apr *apr = dev_get_drvdata(dev);
	struct apr_device *adev = NULL;
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	spin_lock_init(&adev->lock);

	adev->svc_id = id->svc_id;
	adev->domain_id = id->domain_id;
	adev->version = id->svc_version;
	if (np)
		snprintf(adev->name, APR_NAME_SIZE, "%pOFn", np);
	else
		strscpy(adev->name, id->name, APR_NAME_SIZE);

	dev_set_name(&adev->dev, "aprsvc:%s:%x:%x", adev->name,
		     id->domain_id, id->svc_id);

	adev->dev.bus = &aprbus;
	adev->dev.parent = dev;
	adev->dev.of_node = np;
	adev->dev.release = apr_dev_release;
	adev->dev.driver = NULL;

	spin_lock(&apr->svcs_lock);
	idr_alloc(&apr->svcs_idr, adev, id->svc_id,
		  id->svc_id + 1, GFP_ATOMIC);
	spin_unlock(&apr->svcs_lock);

	dev_info(dev, "Adding APR dev: %s\n", dev_name(&adev->dev));

	ret = device_register(&adev->dev);
	if (ret) {
		dev_err(dev, "device_register failed: %d\n", ret);
		put_device(&adev->dev);
	}

	return ret;
}

static void of_register_apr_devices(struct device *dev)
{
	struct apr *apr = dev_get_drvdata(dev);
	struct device_node *node;

	for_each_child_of_node(dev->of_node, node) {
		struct apr_device_id id = { {0} };

		if (of_property_read_u32(node, "reg", &id.svc_id))
			continue;

		id.domain_id = apr->dest_domain_id;

		if (apr_add_device(dev, node, &id))
			dev_err(dev, "Failed to add apr %d svc\n", id.svc_id);
	}
}

static int apr_probe(struct rpmsg_device *rpdev)
{
	struct qmi_servreg_notif_register_listener_resp_msg_v01 resp = { { 0, 0 } };
	struct qmi_servreg_notif_register_listener_req_msg_v01 req;
	struct device *dev = &rpdev->dev;
	struct qmi_txn txn;
	struct apr *apr;
	int ret;

	apr = devm_kzalloc(dev, sizeof(*apr), GFP_KERNEL);
	if (!apr)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, "qcom,apr-domain", &apr->dest_domain_id);
	if (ret) {
		dev_err(dev, "APR Domain ID not specified in DT\n");
		return ret;
	}
	init_completion(&apr->comp);
	init_completion(&apr->ind_comp);

	dev_set_drvdata(dev, apr);
	apr->ch = rpdev->ept;
	apr->dev = dev;
	apr->rxwq = create_singlethread_workqueue("qcom_apr_rx");
	if (!apr->rxwq) {
		dev_err(apr->dev, "Failed to start Rx WQ\n");
		return -ENOMEM;
	}
	INIT_WORK(&apr->rx_work, apr_rxwq);

	apr->indackwq = create_singlethread_workqueue("qcom_apr_indack");
        if (!apr->indackwq) {
                dev_err(apr->dev, "Failed to start IndAck WQ\n");
                return -ENOMEM;
        }

	INIT_LIST_HEAD(&apr->rx_list);
	spin_lock_init(&apr->rx_lock);
	spin_lock_init(&apr->svcs_lock);

	/* Wait for PD UP notification */
	ret = qmi_handle_init(&apr->qmi, 79, &servreg_ops, qmi_indication_handler);
	if (ret < 0) {
		pr_err("apr_probe qmi handle init failed\n");
	}

	qmi_add_lookup(&apr->qmi, SERVREG_NOTIF_SERVICE, 1, 74);

	ret = wait_for_completion_timeout(&apr->comp, 10 * HZ);
	if (!ret)
		pr_err("timed out waiting for new server\n");

	ret = qmi_txn_init(&apr->qmi, &txn,
			   qmi_servreg_notif_register_listener_resp_msg_v01_ei,
			   &resp);
	if (ret < 0) {
		pr_err("QMI tx init failed , ret - %d\n", ret);
		qmi_handle_release(&apr->qmi);
		return ret;
	}

	req.enable = true;
	snprintf(req.service_name, ARRAY_SIZE(req.service_name), "%s",
		 "msm/adsp/audio_pd");

	ret = qmi_send_request(&apr->qmi, &apr->servreg_sock,
			       &txn, SERVREG_NOTIF_REGISTER_LISTENER_REQ,
			       SERVREG_NOTIF_REGISTER_LISTENER_REQ_MSG_LEN,
			       qmi_servreg_notif_register_listener_req_msg_v01_ei,
			       &req);
	if (ret < 0) {
		pr_err("QMI send req failed, ret - %d\n", ret);
		qmi_txn_cancel(&txn);
		qmi_handle_release(&apr->qmi);
		return ret;
	}

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(SERVER_TIMEOUT));
	if (ret < 0) {
		pr_err("QMI qmi txn wait failed, ret - %d\n", ret);
		qmi_handle_release(&apr->qmi);
		return ret;
	}

	/* Check the response */
	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		pr_err("QMI request failed 0x%x\n", resp.resp.error);
		qmi_handle_release(&apr->qmi);
		return -EREMOTEIO;
	} else {
		pr_err("QMI request succeeded 0x%x\n", resp.resp.error);
		pr_err("Serv Reg current state : 0x%x\n", resp.curr_state);
	}

	/* HACK */
	if (resp.curr_state != 0x1FFFFFFF) {
		ret = wait_for_completion_timeout(&apr->ind_comp, 10 * HZ);
		if (!ret) {
			pr_err("timed out waiting for PD UP\n");
			qmi_handle_release(&apr->qmi);
			return ret;
		}
	}

	idr_init(&apr->svcs_idr);
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
	struct apr *apr = dev_get_drvdata(&rpdev->dev);

	qmi_handle_release(&apr->qmi);
	device_for_each_child(&rpdev->dev, NULL, apr_remove_device);
	flush_workqueue(apr->rxwq);
	destroy_workqueue(apr->rxwq);
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
	drv->driver.bus = &aprbus;
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
MODULE_DEVICE_TABLE(of, apr_of_match);

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

	ret = bus_register(&aprbus);
	if (!ret)
		ret = register_rpmsg_driver(&apr_driver);
	else
		bus_unregister(&aprbus);

	return ret;
}

static void __exit apr_exit(void)
{
	bus_unregister(&aprbus);
	unregister_rpmsg_driver(&apr_driver);
}

subsys_initcall(apr_init);
module_exit(apr_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm APR Bus");
