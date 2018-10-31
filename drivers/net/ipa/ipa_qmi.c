// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2019 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/qrtr.h>
#include <linux/soc/qcom/qmi.h>

#include "ipa.h"
#include "ipa_endpoint.h"
#include "ipa_mem.h"
#include "ipa_qmi_msg.h"

#define QMI_INIT_DRIVER_TIMEOUT	60000	/* A minute in milliseconds */

/**
 * DOC: AP/Modem QMI Handshake
 *
 * The AP and modem perform a "handshake" at initialization time to ensure
 * each side knows the other side is ready.  Two QMI handles (endpoints) are
 * used for this; one provides service on the modem for AP requests, and the
 * other is on the AP to service modem requests (and to supply an indication
 * from the AP).
 *
 * The QMI service on the modem expects to receive an INIT_DRIVER request from
 * the AP, which contains parameters used by the modem during initialization.
 * The AP sends this request using the client handle as soon as it is knows
 * the modem side service is available.  The modem responds to this request
 * immediately.
 *
 * When the modem learns the AP service is available, it is able to
 * communicate its status to the AP.  The modem uses this to tell
 * the AP when it is ready to receive an indication, sending an
 * INDICATION_REGISTER request to the handle served by the AP.  This
 * is independent of the modem's initialization of its driver.
 *
 * When the modem has completed the driver initialization requested by the
 * AP, it sends a DRIVER_INIT_COMPLETE request to the AP.   This request
 * could arrive at the AP either before or after the INDICATION_REGISTER
 * request.
 *
 * The final step in the handshake occurs after the AP has received both
 * requests from the modem.  The AP completes the handshake by sending an
 * INIT_COMPLETE_IND indication message to the modem.
 */

#define IPA_HOST_SERVICE_SVC_ID		0x31
#define IPA_HOST_SVC_VERS		1
#define IPA_HOST_SERVICE_INS_ID		1

#define IPA_MODEM_SERVICE_SVC_ID	0x31
#define IPA_MODEM_SERVICE_INS_ID	2
#define IPA_MODEM_SVC_VERS		1

/* Send an INIT_COMPLETE_IND indication message to the modem */
static int ipa_send_master_driver_init_complete_ind(struct qmi_handle *qmi,
						    struct sockaddr_qrtr *sq)
{
	struct ipa_init_complete_ind ind = { };

	ind.status.result = QMI_RESULT_SUCCESS_V01;
	ind.status.error = QMI_ERR_NONE_V01;

	return qmi_send_indication(qmi, sq, IPA_QMI_INIT_COMPLETE_IND,
				   IPA_QMI_INIT_COMPLETE_IND_SZ,
				   ipa_init_complete_ind_ei, &ind);
}

/* This function is called to determine whether to complete the handshake by
 * sending an INIT_COMPLETE_IND indication message to the modem.  It's called
 * after responding to both the the INDICATION_REGISTER and INIT_DRIVER
 * requests from the modem; @init_driver parameter is false for the former
 * case and true for the latter.  The INIT_COMPLETE indication is sent by
 * ipa_send_master_driver_init_complete_ind(), and only occurs after we have
 * received (and responded to) both requests.
 */
static void ipa_handshake_complete(struct qmi_handle *qmi,
				   struct sockaddr_qrtr *sq, bool init_driver)
{
	struct ipa *ipa = container_of(qmi, struct ipa, qmi.server_handle);
	bool send_it;
	int ret;

	if (init_driver) {
		ipa->qmi.init_driver_response_received = 1;
		send_it = !!ipa->qmi.indication_register_received;
	} else {
		ipa->qmi.indication_register_received = 1;
		send_it = !!ipa->qmi.init_driver_response_received;
	}
	if (!send_it)
		return;

	ret = ipa_send_master_driver_init_complete_ind(qmi, sq);
	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d sending init complete indication\n", ret);
}

/* Callback function to handle an INDICATION_REGISTER request message from the
 * modem.  This informs the AP that the modem is now ready to receive the
 * INIT_COMPLETE_IND indication message.
 */
static void ipa_indication_register_fn(struct qmi_handle *qmi,
				       struct sockaddr_qrtr *sq,
				       struct qmi_txn *txn,
				       const void *decoded)
{
	struct ipa_indication_register_rsp rsp = { };
	int ret;

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
				IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				ipa_indication_register_rsp_ei, &rsp);
	if (ret) {
		struct device *dev;
		struct ipa *ipa;

		ipa = container_of(qmi, struct ipa, qmi.server_handle);
		dev = &ipa->pdev->dev;
		dev_err(dev, "error %d sending register indication response\n",
			ret);
	} else {
		ipa_handshake_complete(qmi, sq, false);
	}
}

/* Callback function to handle a DRIVER_INIT_COMPLETE request message from the
 * modem.  This informs the AP that the modem has completed the initializion
 * of its driver.
 */
static void ipa_driver_init_complete_fn(struct qmi_handle *qmi,
					struct sockaddr_qrtr *sq,
					struct qmi_txn *txn,
					const void *decoded)
{
	struct ipa_driver_init_complete_rsp rsp = { };
	int ret;

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
				IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ,
				ipa_driver_init_complete_rsp_ei, &rsp);
	if (ret) {
		struct device *dev;
		struct ipa *ipa;

		ipa = container_of(qmi, struct ipa, qmi.server_handle);
		dev = &ipa->pdev->dev;
		dev_err(dev, "error %d sending init complete response\n", ret);
	} else {
		ipa_handshake_complete(qmi, sq, true);
	}
}

/* The server handles two request message types sent by the modem. */
static struct qmi_msg_handler ipa_server_msg_handlers[] = {
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INDICATION_REGISTER,
		.ei		= ipa_indication_register_req_ei,
		.decoded_size	= IPA_QMI_INDICATION_REGISTER_REQ_SZ,
		.fn		= ipa_indication_register_fn,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_DRIVER_INIT_COMPLETE,
		.ei		= ipa_driver_init_complete_req_ei,
		.decoded_size	= IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ,
		.fn		= ipa_driver_init_complete_fn,
	},
};

/* Callback function to handle an IPA_QMI_INIT_DRIVER response message from
 * the modem.  This only acknowledges that the modem received the request.
 * The modem will eventually report that it has completed its modem
 * initialization by sending a IPA_QMI_DRIVER_INIT_COMPLETE request.
 */
static void ipa_init_driver_rsp_fn(struct qmi_handle *qmi,
				   struct sockaddr_qrtr *sq,
				   struct qmi_txn *txn,
				   const void *decoded)
{
	txn->result = 0;	/* IPA_QMI_INIT_DRIVER request was successful */
	complete(&txn->completion);
}

/* The client handles one response message type sent by the modem. */
static struct qmi_msg_handler ipa_client_msg_handlers[] = {
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_INIT_DRIVER,
		.ei		= ipa_init_modem_driver_rsp_ei,
		.decoded_size	= IPA_QMI_INIT_DRIVER_RSP_SZ,
		.fn		= ipa_init_driver_rsp_fn,
	},
};

/* Return a pointer to an init modem driver request structure, which contains
 * configuration parameters for the modem.  The modem may be started multiple
 * times, but generally these parameters don't change so we can reuse the
 * request structure once it's initialized.  The only exception is the
 * skip_uc_load field, which will be set only after the microcontroller has
 * reported it has completed its initialization.
 */
static const struct ipa_init_modem_driver_req *
init_modem_driver_req(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	static struct ipa_init_modem_driver_req req;
	u32 size;

	/* The microcontroller is initialized on the first boot */
	req.skip_uc_load_valid = 1;
	req.skip_uc_load = ipa->uc_loaded;

	/* We only have to initialize most of it once */
	if (req.platform_type_valid)
		return &req;

	req.platform_type_valid = 1;
	req.platform_type = IPA_QMI_PLATFORM_TYPE_MSM_ANDROID;

	size = ipa->mem[IPA_MEM_MODEM_HEADER].size;
	if (size) {
		req.hdr_tbl_info_valid = 1;
		req.hdr_tbl_info.start = ipa->mem_offset +
				ipa->mem[IPA_MEM_MODEM_HEADER].offset;
		req.hdr_tbl_info.end = req.hdr_tbl_info.start + size - 1;
	}

	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = ipa->mem_offset +
			ipa->mem[IPA_MEM_V4_ROUTE].offset;
	req.v4_route_tbl_info.count = IPA_MEM_MODEM_RT_COUNT;

	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = ipa->mem_offset +
			ipa->mem[IPA_MEM_V6_ROUTE].offset;
	req.v6_route_tbl_info.count = IPA_MEM_MODEM_RT_COUNT;

	req.v4_filter_tbl_start_valid = 1;
	req.v4_filter_tbl_start = ipa->mem_offset +
			ipa->mem[IPA_MEM_V4_FILTER].offset;

	req.v6_filter_tbl_start_valid = 1;
	req.v6_filter_tbl_start = ipa->mem_offset +
			ipa->mem[IPA_MEM_V6_FILTER].offset;

	size = ipa->mem[IPA_MEM_MODEM].size;
	if (size) {
		req.modem_mem_info_valid = 1;
		req.modem_mem_info.start = ipa->mem_offset +
				ipa->mem[IPA_MEM_MODEM].offset;
		req.modem_mem_info.size = size;
	}

	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]->endpoint_id;

	/* skip_uc_load_valid and skip_uc_load are set above */

	size = ipa->mem[IPA_MEM_MODEM_PROC_CTX].size;
	if (size) {
		req.hdr_proc_ctx_tbl_info_valid = 1;
		req.hdr_proc_ctx_tbl_info.start = ipa->mem_offset +
				ipa->mem[IPA_MEM_MODEM_PROC_CTX].offset;
		req.hdr_proc_ctx_tbl_info.end =
			req.hdr_proc_ctx_tbl_info.start + size - 1;
	}

	/* Nothing to report for the compression table (zip_tbl_info) */

	size = ipa->mem[IPA_MEM_V4_ROUTE_HASHED].size;
	if (size) {
		req.v4_hash_route_tbl_info_valid = 1;
		req.v4_hash_route_tbl_info.start = ipa->mem_offset +
				ipa->mem[IPA_MEM_V4_ROUTE_HASHED].offset;
		req.v4_hash_route_tbl_info.count = IPA_MEM_MODEM_RT_COUNT;
	}

	size = ipa->mem[IPA_MEM_V6_ROUTE_HASHED].size;
	if (size) {
		req.v6_hash_route_tbl_info_valid = 1;
		req.v6_hash_route_tbl_info.start = ipa->mem_offset +
				ipa->mem[IPA_MEM_V6_ROUTE_HASHED].offset;
		req.v6_hash_route_tbl_info.count = IPA_MEM_MODEM_RT_COUNT;
	}

	size = ipa->mem[IPA_MEM_V4_FILTER_HASHED].size;
	if (size) {
		req.v4_hash_filter_tbl_start_valid = 1;
		req.v4_hash_filter_tbl_start = ipa->mem_offset +
				ipa->mem[IPA_MEM_V4_FILTER_HASHED].offset;
	}

	size = ipa->mem[IPA_MEM_V6_FILTER_HASHED].size;
	if (size) {
		req.v6_hash_filter_tbl_start_valid = 1;
		req.v6_hash_filter_tbl_start = ipa->mem_offset +
				ipa->mem[IPA_MEM_V6_FILTER_HASHED].offset;
	}

	/* None of the stats fields are valid (IPA v4.0 and above) */

	if (ipa->version != IPA_VERSION_3_5_1) {
		size = ipa->mem[IPA_MEM_STATS_QUOTA].size;
		if (size) {
			req.hw_stats_quota_base_addr_valid = 1;
			req.hw_stats_quota_base_addr = ipa->mem_offset +
				ipa->mem[IPA_MEM_STATS_QUOTA].offset;
			req.hw_stats_quota_size_valid = 1;
			req.hw_stats_quota_size = ipa->mem_offset + size;
		}

		size = ipa->mem[IPA_MEM_STATS_DROP].size;
		if (size) {
			req.hw_stats_drop_base_addr_valid = 1;
			req.hw_stats_drop_base_addr = ipa->mem_offset +
				ipa->mem[IPA_MEM_STATS_DROP].offset;
			req.hw_stats_drop_size_valid = 1;
			req.hw_stats_drop_size = ipa->mem_offset + size;
		}
	}

	return &req;
}

/* Start the handshake by sending an INIT_DRIVER request to the modem on
 * the client handle, and wait for it to complete.  We don't do anything
 * special after this completes, we just ensure we get the response.
 * The modem will follow up with a request that tells us it finished
 * its driver initialization.
 */
static void ipa_client_init_driver_work(struct work_struct *work)
{
	const struct ipa_init_modem_driver_req *req;
	struct ipa_qmi *ipa_qmi;
	struct qmi_handle *qmi;
	struct qmi_txn txn;
	struct device *dev;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(work, struct ipa_qmi, init_driver_work);
	qmi = &ipa_qmi->client_handle,

	ipa = container_of(ipa_qmi, struct ipa, qmi);
	dev = &ipa->pdev->dev;

	ret = qmi_txn_init(qmi, &txn, NULL, NULL);
	if (ret < 0) {
		dev_err(dev, "error %d preparing init driver request\n", ret);
		return;
	}

	req = init_modem_driver_req(ipa_qmi);
	ret = qmi_send_request(qmi, &ipa_qmi->modem_sq, &txn,
			       IPA_QMI_INIT_DRIVER, IPA_QMI_INIT_DRIVER_REQ_SZ,
			       ipa_init_modem_driver_req_ei, req);
	if (!ret) {
		ret = qmi_txn_wait(&txn, QMI_INIT_DRIVER_TIMEOUT);
		if (ret)
			dev_err(dev, "error %d awaiting init driver response\n",
				ret);
	} else {
		dev_err(dev, "error %d sending init driver request\n", ret);
	}

	/* If any error occurs we need to cancel the transaction */
	if (ret)
		qmi_txn_cancel(&txn);
}

/* The modem service we requested is now available via the client handle.
 * We want to send an INIT_DRIVER request to the modem and wait for it to
 * complete, but we can't wait in the new_server callback, so we schedule
 * a worker on the global workqueue to do that for us.
 */
static int
ipa_client_new_server(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, client_handle);

	ipa_qmi->modem_sq.sq_family = AF_QIPCRTR;
	ipa_qmi->modem_sq.sq_node = svc->node;
	ipa_qmi->modem_sq.sq_port = svc->port;

	schedule_work(&ipa_qmi->init_driver_work);

	return 0;
}

/* The only callback we supply for the client handle is notification that the
 * service on the modem has become available.
 */
static struct qmi_ops ipa_client_ops = {
	.new_server	= ipa_client_new_server,
};

/* This is called by ipa_netdev_setup().  We can be informed via remoteproc
 * that the modem has shut down, in which case this function will be called
 * again to prepare for it coming back up again.
 */
int ipa_qmi_setup(struct ipa *ipa)
{
	struct ipa_qmi *ipa_qmi = &ipa->qmi;
	int ret;

	ipa_qmi->init_driver_response_received = 0;
	ipa_qmi->indication_register_received = 0;

	if (ipa_qmi->initialized)
		return 0;

	/* The only handle operation that might be interesting for the server
	 * would be del_client, to find out when the modem side client has
	 * disappeared.  But other than reporting the event, we wouldn't do
	 * anything about that.  So we just pass a null pointer for its handle
	 * operations.  All the real work is done by the message handlers.
	 */
	ret = qmi_handle_init(&ipa_qmi->server_handle,
			      IPA_QMI_SERVER_MAX_RCV_SZ, NULL,
			      ipa_server_msg_handlers);
	if (ret)
		return ret;

	ret = qmi_add_server(&ipa_qmi->server_handle, IPA_HOST_SERVICE_SVC_ID,
			     IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret)
		goto err_release_server_handle;

	/* The client handle is only used for sending an INIT_DRIVER request
	 * to the modem, and receiving its response message.
	 */
	ret = qmi_handle_init(&ipa_qmi->client_handle,
			      IPA_QMI_CLIENT_MAX_RCV_SZ, &ipa_client_ops,
			      ipa_client_msg_handlers);
	if (ret)
		goto err_release_server_handle;

	/* We need this ready before the service lookup is added */
	INIT_WORK(&ipa_qmi->init_driver_work, ipa_client_init_driver_work);

	ret = qmi_add_lookup(&ipa_qmi->client_handle, IPA_MODEM_SERVICE_SVC_ID,
			     IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret)
		goto err_release_client_handle;

	ipa_qmi->initialized = 1;

	return 0;

err_release_client_handle:
	/* Releasing the handle also removes registered lookups */
	qmi_handle_release(&ipa_qmi->client_handle);
	memset(&ipa_qmi->client_handle, 0, sizeof(ipa_qmi->client_handle));
err_release_server_handle:
	/* Releasing the handle also removes registered services */
	qmi_handle_release(&ipa_qmi->server_handle);
	memset(&ipa_qmi->server_handle, 0, sizeof(ipa_qmi->server_handle));

	return ret;
}

void ipa_qmi_teardown(struct ipa *ipa)
{
	if (!ipa->qmi.initialized)
		return;

	cancel_work_sync(&ipa->qmi.init_driver_work);

	qmi_handle_release(&ipa->qmi.client_handle);
	memset(&ipa->qmi.client_handle, 0, sizeof(ipa->qmi.client_handle));

	qmi_handle_release(&ipa->qmi.server_handle);
	memset(&ipa->qmi.server_handle, 0, sizeof(ipa->qmi.server_handle));

	ipa->qmi.initialized = 0;
}
