// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * TMEL Communication QMI API Wrappers
 * Provides wrapper functions for TME-L QMI services
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tmelcom_qmi.h>

#include "tmelcom_qmi_internal.h"

/* External timeout parameter */
extern unsigned int qmi_timeout_ms;

extern atomic_t client_count;
/**
 * tmelcom_qmi_init_attestation() - Initialize attestation
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @rsp_buf: Buffer to store response
 * @rsp_buf_len: Size of response buffer
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_init_attestation(int attach_num, u8 *rsp_buf, u32 rsp_buf_len,
				 u32 *rsp_len_used)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_init_attestation_req_msg_v01 req = {0};
	struct qmi_tme_init_attestation_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!rsp_buf || !rsp_buf_len || !rsp_len_used)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_init_attestation_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_INIT_ATTESTATION_REQ_V01,
			       QMI_TME_INIT_ATTESTATION_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_init_attestation_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Copy response data */
	if (resp->init_att_rsp_valid && resp->init_att_rsp_len > 0) {
		u32 copy_len = min(resp->init_att_rsp_len, rsp_buf_len);
		memcpy(rsp_buf, resp->init_att_rsp, copy_len);
		*rsp_len_used = resp->rsp_len_used_valid ? resp->rsp_len_used : copy_len;
	} else {
		*rsp_len_used = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_init_attestation);

/**
 * tmelcom_qmi_dev_attestation() - Device attestation
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @att_req: Attestation request buffer
 * @att_req_len: Length of attestation request
 * @ext_claims: External claims buffer (optional)
 * @ext_claims_len: Length of external claims
 * @att_rsp: Buffer to store attestation response
 * @att_rsp_len: Size of response buffer
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_dev_attestation(int attach_num, u8 *att_req, u32 att_req_len,
				u8 *ext_claims, u32 ext_claims_len,
				u8 *att_rsp, u32 att_rsp_len,
				u32 *rsp_len_used)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_dev_attestation_req_msg_v01 *req;
	struct qmi_tme_dev_attestation_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!att_req || !att_req_len || !att_rsp || !att_rsp_len || !rsp_len_used)
		return -EINVAL;

	if (att_req_len > QMI_TME_MAX_ATT_REQ_SIZE_V01)
		return -EINVAL;

	if (ext_claims && ext_claims_len > QMI_TME_MAX_EXT_CLAIMS_SIZE_V01)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	/* Prepare request */
	req->att_req_len = att_req_len;
	memcpy(req->att_req, att_req, att_req_len);
	req->attest_req_len = att_req_len;

	if (ext_claims && ext_claims_len > 0) {
		req->ext_claims_valid = 1;
		req->ext_claims_len = ext_claims_len;
		memcpy(req->ext_claims, ext_claims, ext_claims_len);
		req->external_claims_len_valid = 1;
		req->external_claims_len = ext_claims_len;
	}

	mutex_lock(&client->lock);

	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_dev_attestation_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_DEV_ATTESTATION_REQ_V01,
			       QMI_TME_DEV_ATTESTATION_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_dev_attestation_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Copy response */
	if (resp->att_rsp_valid && resp->att_rsp_len > 0) {
		u32 copy_len = min(resp->att_rsp_len, att_rsp_len);
		memcpy(att_rsp, resp->att_rsp, copy_len);
		*rsp_len_used = resp->rsp_len_used_valid ? resp->rsp_len_used : copy_len;
	} else {
		*rsp_len_used = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_dev_attestation);

/**
 * tmelcom_qmi_dev_provision() - Device provisioning
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @prov_req: Provisioning request buffer
 * @prov_req_len: Length of provisioning request
 * @prov_rsp: Buffer to store provisioning response
 * @prov_rsp_len: Size of response buffer
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_dev_provision(int attach_num, u8 *prov_req, u32 prov_req_len,
			      u8 *prov_rsp, u32 prov_rsp_len,
			      u32 *rsp_len_used)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_dev_provision_req_msg_v01 *req;
	struct qmi_tme_dev_provision_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!prov_req || !prov_req_len || !prov_rsp || !prov_rsp_len || !rsp_len_used)
		return -EINVAL;

	if (prov_req_len > QMI_TME_MAX_PROV_REQ_SIZE_V01)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	req->prov_req_len = prov_req_len;
	memcpy(req->prov_req, prov_req, prov_req_len);
	req->provision_req_len = prov_req_len;

	mutex_lock(&client->lock);

	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_dev_provision_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_DEV_PROVISION_REQ_V01,
			       QMI_TME_DEV_PROVISION_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_dev_provision_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	if (resp->prov_rsp_valid && resp->prov_rsp_len > 0) {
		u32 copy_len = min(resp->prov_rsp_len, prov_rsp_len);
		memcpy(prov_rsp, resp->prov_rsp, copy_len);
		*rsp_len_used = resp->rsp_len_used_valid ? resp->rsp_len_used : copy_len;
	} else {
		*rsp_len_used = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_dev_provision);

/**
 * tmelcom_qmi_lic_install() - Install license
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @license: License data buffer
 * @license_len: Length of license data
 * @operation: License operation type
 * @identifier: Buffer to store license identifier
 * @id_len: Size of identifier buffer
 * @id_len_used: Actual identifier length used
 * @flags: Pointer to store flags
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_install(int attach_num, u8 *license, u32 license_len,
			    u32 operation, u8 *identifier, u32 id_len,
			    u32 *id_len_used, u32 *flags)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_lic_install_req_msg_v01 *req;
	struct qmi_tme_lic_install_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!license || !license_len || !identifier || !id_len || !id_len_used || !flags)
		return -EINVAL;

	if (license_len > QMI_TME_MAX_LICENSE_SIZE_V01)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	req->license_len = license_len;
	memcpy(req->license, license, license_len);
	req->license_data_len = license_len;
	req->license_operation = operation;

	mutex_lock(&client->lock);

	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_lic_install_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_LIC_INSTALL_REQ_V01,
			       QMI_TME_LIC_INSTALL_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_lic_install_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	if (resp->identifier_valid && resp->identifier_len > 0) {
		u32 copy_len = min(resp->identifier_len, id_len);
		memcpy(identifier, resp->identifier, copy_len);
		*id_len_used = resp->id_len_used_valid ? resp->id_len_used : copy_len;
	} else {
		*id_len_used = 0;
	}

	/* Copy flags from response */
	if (resp->flags_valid) {
		*flags = resp->flags;
	} else {
		*flags = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_lic_install);

/**
 * tmelcom_qmi_lic_feature_status() - Get license feature status
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @request: Request buffer
 * @req_len: Length of request
 * @response: Buffer to store response
 * @rsp_len: Size of response buffer
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_feature_status(int attach_num, u8 *request, u32 req_len,
				   u8 *response, u32 rsp_len,
				   u32 *rsp_len_used)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_lic_feature_status_req_msg_v01 *req;
	struct qmi_tme_lic_feature_status_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!request || !req_len || !response || !rsp_len || !rsp_len_used)
		return -EINVAL;

	if (req_len > QMI_TME_MAX_LICENSE_SIZE_V01)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	req->request_len = req_len;
	memcpy(req->request, request, req_len);
	req->feat_req_len = req_len;

	mutex_lock(&client->lock);

	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_lic_feature_status_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_LIC_FEATURE_STATUS_REQ_V01,
			       QMI_TME_LIC_FEATURE_STATUS_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_lic_feature_status_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	if (resp->response_valid && resp->response_len > 0) {
		u32 copy_len = min(resp->response_len, rsp_len);
		memcpy(response, resp->response, copy_len);
		*rsp_len_used = resp->rsp_len_used_valid ? resp->rsp_len_used : copy_len;
	} else {
		*rsp_len_used = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_lic_feature_status);

/**
 * tmelcom_qmi_ttime_get_params() - Get TTIME parameters
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @params: Buffer to store parameters
 * @buf_len: Size of buffer
 * @used_len: Actual length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_ttime_get_params(int attach_num, u8 *params, u32 buf_len,
				 u32 *used_len)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_ttime_get_params_req_msg_v01 req = {0};
	struct qmi_tme_ttime_get_params_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!params || !buf_len || !used_len)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	mutex_lock(&client->lock);

	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_ttime_get_params_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_TTIME_GET_PARAMS_REQ_V01,
			       QMI_TME_TTIME_GET_PARAMS_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_ttime_get_params_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	if (resp->request_packet_valid && resp->request_packet_len > 0) {
		u32 copy_len = min(resp->request_packet_len, buf_len);
		memcpy(params, resp->request_packet, copy_len);
		*used_len = resp->packet_len_used_valid ? resp->packet_len_used : copy_len;
	} else {
		*used_len = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_ttime_get_params);

/**
 * tmelcom_qmi_ttime_set() - Set TTIME
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @ttime_data: TTIME data buffer
 * @buf_len: Length of data
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_ttime_set(int attach_num, u8 *ttime_data, u32 buf_len)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_ttime_set_req_msg_v01 *req;
	struct qmi_tme_ttime_set_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!ttime_data || !buf_len)
		return -EINVAL;

	if (buf_len > QMI_TME_MAX_RESPONSE_SIZE_V01)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	req->response_packet_len = buf_len;
	memcpy(req->response_packet, ttime_data, buf_len);
	req->packet_len = buf_len;

	mutex_lock(&client->lock);

	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_ttime_set_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_TTIME_SET_REQ_V01,
			       QMI_TME_TTIME_SET_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_ttime_set_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_ttime_set);

/**
 * tmelcom_qmi_lic_clean() - Get licenses to be deleted/cleaned
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @identifiers: Buffer to store identifiers
 * @id_buf_len: Size of buffer
 * @id_len_used: Actual length used
 * @delete_count: Number of licenses to delete
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_clean(int attach_num, u64 *identifiers, u32 id_buf_len,
			  u32 *id_len_used, u32 *delete_count)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_lic_clean_req_msg_v01 req = {0};
	struct qmi_tme_lic_clean_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!identifiers || !id_buf_len || !id_len_used || !delete_count)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_lic_clean_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_LIC_CLEAN_REQ_V01,
			       QMI_TME_LIC_CLEAN_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_lic_clean_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Copy identifier data from response */
	if (resp->to_be_deleted_identifiers_valid && resp->to_be_deleted_identifiers_len > 0) {
		memcpy(identifiers, resp->to_be_deleted_identifiers,
		       resp->to_be_deleted_identifiers_len);
		*id_len_used = resp->to_be_deleted_identifiers_len;
	} else {
		*id_len_used = 0;
	}

	/* Copy delete count */
	if (resp->to_be_deleted_count_valid) {
		*delete_count = resp->to_be_deleted_count;
	} else {
		*delete_count = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_lic_clean);

/**
 * tmelcom_qmi_is_client_ready() - Check if QMI client is ready
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 *
 * Return: true if client is ready, false otherwise
 */
bool tmelcom_qmi_is_client_ready(int attach_num)
{
	struct tmelcom_qmi_client *client;

	client = tmelcom_qmi_get_client(attach_num);
	return (client != NULL && client->connected);
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_is_client_ready);

/**
 * tmelcom_qmi_get_client_count() - Get number of connected clients
 */
int tmelcom_qmi_get_client_count(void)
{
	return atomic_read(&client_count);
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_get_client_count);

/**
 * tmelcom_qmi_get_service_info() - Get service information
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @service_id: Pointer to store service ID
 * @service_version: Pointer to store service version
 * @qrtr_node: Pointer to store QRTR node
 * @qrtr_port: Pointer to store QRTR port
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_get_service_info(int attach_num, u32 *service_id,
				 u32 *service_version, u32 *qrtr_node,
				 u32 *qrtr_port)
{
	struct tmelcom_qmi_client *client;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client)
		return TMELCOM_QMI_ERR_NO_CLIENT;

	if (service_id)
		*service_id = QMI_TME_SERVICE_ID_V01;
	if (service_version)
		*service_version = QMI_TME_SERVICE_VERS_V01;
	if (qrtr_node)
		*qrtr_node = client->sq.sq_node;
	if (qrtr_port)
		*qrtr_port = client->sq.sq_port;

	return 0;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_get_service_info);

/**
 * tmelcom_qmi_secboot_get_arb_version() - Get ARB version for a software ID
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @sw_id: Software ID to query
 * @version: Pointer to store the version
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_secboot_get_arb_version(int attach_num, u32 sw_id, u32 *version)
{
	struct tmelcom_qmi_client *client;
	struct qmi_arb_get_req_msg_v01 req = {0};
	struct qmi_arb_get_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!version)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	/* Prepare request */
	req.sw_id = sw_id;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_arb_get_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_ARB_GET_REQ_V01,
			       QMI_TME_ARB_GET_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_arb_get_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		/* Ensure negative error code for kernel convention */
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Extract OEM version from response (matching IPC behavior) */
	if (resp->oem_version_valid) {
		*version = resp->oem_version;
	} else {
		*version = 0;
	}
	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_secboot_get_arb_version);

/**
 * tmelcom_qmi_secboot_update_arb_version_list() - Update ARB version list
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_secboot_update_arb_version_list(int attach_num)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_arb_update_req_msg_v01 req = {0};
	struct qmi_tme_arb_update_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_arb_update_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_ARB_UPDATE_REQ_V01,
			       QMI_TME_ARB_UPDATE_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_arb_update_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_secboot_update_arb_version_list);

/**
 * tmelcom_qmi_get_ecc_public_key() - Get ECC public key (QBEC key read)
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @key_type: Key type/feature ID
 * @buf: Buffer to store the public key
 * @size: Size of the buffer
 * @rsp_len: Pointer to store the actual response length
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_get_ecc_public_key(int attach_num, u32 key_type, void *buf,
				   u32 size, u32 *rsp_len)
{
	struct tmelcom_qmi_client *client;
	struct qmi_qbec_key_read_req_msg_v01 req = {0};
	struct qmi_qbec_key_read_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!buf || !size || !rsp_len)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	/* Prepare request */
	req.feature_id = 0;   /* Default value, matching IPC behavior */
	req.src_l1_key_id = key_type;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_qbec_key_read_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_QBEC_KEY_READ_REQ_V01,
			       QMI_TME_QBEC_KEY_READ_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_qbec_key_read_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Copy public key data from response */
	if (resp->public_key_valid && resp->public_key_len > 0) {
		u32 copy_len = min(resp->public_key_len, size);
		memcpy(buf, resp->public_key, copy_len);
		*rsp_len = resp->qbec_public_key_len_valid ? resp->qbec_public_key_len : copy_len;
	} else {
		*rsp_len = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_get_ecc_public_key);

/**
 * tmelcom_qmi_read_fuse() - Read a single fuse value
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @fuse_addr: Fuse address to read
 * @fuse_val_lsb: Pointer to store LSB of fuse value (lower 32 bits)
 * @fuse_val_msb: Pointer to store MSB of fuse value (upper 32 bits)
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_read_fuse(int attach_num, u32 fuse_addr, u32 *fuse_val_lsb,
			  u32 *fuse_val_msb)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_read_fuse_req_msg_v01 req = {0};
	struct qmi_tme_read_fuse_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!fuse_val_lsb || !fuse_val_msb)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	/* Prepare request */
	req.fuse_addr = fuse_addr;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_read_fuse_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_READ_FUSE_REQ_V01,
			       QMI_TME_READ_FUSE_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_read_fuse_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Extract fuse values from response */
	if (resp->fuse_val_lsb_valid) {
		*fuse_val_lsb = resp->fuse_val_lsb;
	} else {
		*fuse_val_lsb = 0;
	}

	if (resp->fuse_val_msb_valid) {
		*fuse_val_msb = resp->fuse_val_msb;
	} else {
		*fuse_val_msb = 0;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_read_fuse);

/**
 * tmelcom_qmi_tmel_version_read() - Read TMEL version information
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @version_info: Buffer to store version information
 * @buf_len: Size of the buffer
 * @version_info_len: Pointer to store actual version info length
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_tmel_version_read(int attach_num, u8 *version_info, u32 buf_len,
				  u32 *version_info_len)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_tmel_version_read_req_msg_v01 req = {0};
	struct qmi_tme_tmel_version_read_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!version_info || !buf_len || !version_info_len)
		return -EINVAL;

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_tmel_version_read_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_TMEL_VERSION_READ_REQ_V01,
			       QMI_TME_TMEL_VERSION_READ_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_tmel_version_read_req_msg_v01_ei, &req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client,
				"QMI request failed: result=%u, error=%u, status=0x%x, ipc_status=%u\n",
				resp->resp.result, resp->resp.error,
				resp->status, resp->ipc_status);
		ret = -EIO;
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	/* Extract version info from response */
	if (resp->version_info_valid && resp->version_info_len > 0) {
		u32 copy_len = min(resp->version_info_len, buf_len);
		memcpy(version_info, resp->version_info, copy_len);
		*version_info_len = resp->tmel_version_info_len_valid ?
				    resp->tmel_version_info_len : copy_len;
	} else {
		*version_info_len = 0;
	}

	ret = 0;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_tmel_version_read);

/**
 * tmelcom_qmi_dpr_image_load() - Load DPR image
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @image_data: Pointer to the image data (already loaded by caller)
 * @image_size: Size of the image data
 *
 * The caller is responsible for loading the image into memory at a particular
 * address. This function takes that address and size and sends it via QMI.
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_dpr_image_load(int attach_num, u8 *image_data, u32 image_size)
{
	struct tmelcom_qmi_client *client;
	struct qmi_dpr_image_load_req_msg_v01 *req;
	struct qmi_dpr_image_load_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!image_data || !image_size)
		return -EINVAL;

	if (image_size > QMI_TME_DPR_IMAGE_BUFFER_SIZE_V01) {
		pr_err("tmelcom_qmi: Image size %u exceeds maximum %u\n",
		       image_size, QMI_TME_DPR_IMAGE_BUFFER_SIZE_V01);
		return -EINVAL;
	}

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	/* Prepare request - copy image data from caller's buffer */
	req->image_data_len = image_size;
	memcpy(req->image_data, image_data, image_size);
	req->dpr_image_data_len = image_size;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_dpr_image_load_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_DPR_IMAGE_LOAD_REQ_V01,
			       QMI_DPR_IMAGE_LOAD_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_dpr_image_load_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: result=%u, error=%u\n",
				resp->resp.result, resp->resp.error);
		ret = -EIO;
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	ret = 0;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_dpr_image_load);

/**
 * tmelcom_qmi_fuse_blow() - Blow fuses with SECDAT data
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @fuse_data: Pointer to the fuse data (already loaded by caller)
 * @fuse_data_size: Size of the fuse data
 *
 * The caller is responsible for loading the fuse data into memory.
 * This function takes that address and size and sends it via QMI.
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_fuse_blow(int attach_num, u8 *fuse_data, u32 fuse_data_size)
{
	struct tmelcom_qmi_client *client;
	struct qmi_tme_fuse_blow_req_msg_v01 *req;
	struct qmi_tme_fuse_blow_resp_msg_v01 *resp;
	struct qmi_txn txn;
	int ret;

	if (!fuse_data || !fuse_data_size)
		return -EINVAL;

	if (fuse_data_size > QMI_TME_SECDAT_BUFFER_SIZE_V01) {
		pr_err("tmelcom_qmi: Fuse data size %u exceeds maximum %u\n",
		       fuse_data_size, QMI_TME_SECDAT_BUFFER_SIZE_V01);
		return -EINVAL;
	}

	client = tmelcom_qmi_get_client(attach_num);
	if (!client) {
		pr_err("tmelcom_qmi: No client for attach number %d\n", attach_num);
		return TMELCOM_QMI_ERR_NO_CLIENT;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	/* Prepare request - copy fuse data from caller's buffer */
	req->secdat_fuse_data_len = fuse_data_size;
	memcpy(req->secdat_fuse_data, fuse_data, fuse_data_size);
	req->secdat_data_len = fuse_data_size;

	mutex_lock(&client->lock);

	/* Initialize transaction */
	ret = qmi_txn_init(&client->qmi, &txn,
			   qmi_tme_fuse_blow_resp_msg_v01_ei, resp);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to init transaction: %d\n", ret);
		goto out_unlock;
	}

	/* Send request */
	ret = qmi_send_request(&client->qmi, &client->sq, &txn,
			       QMI_TME_FUSE_BLOW_REQ_V01,
			       QMI_TME_FUSE_BLOW_REQ_MSG_V01_MAX_MSG_LEN,
			       qmi_tme_fuse_blow_req_msg_v01_ei, req);
	if (ret < 0) {
		tmelcom_qmi_err(client, "Failed to send request: %d\n", ret);
		qmi_txn_cancel(&txn);
		atomic_inc(&client->stats.errors);
		goto out_unlock;
	}

	atomic_inc(&client->stats.requests_sent);
	client->stats.last_request_time = ktime_get();

	/* Wait for response */
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(qmi_timeout_ms));
	if (ret < 0) {
		tmelcom_qmi_err(client, "Transaction timeout: %d\n", ret);
		atomic_inc(&client->stats.timeouts);
		goto out_unlock;
	}

	atomic_inc(&client->stats.responses_received);
	client->stats.last_response_time = ktime_get();

	/* Check response */
	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		tmelcom_qmi_err(client, "QMI request failed: ipc_status=%u, status=0x%x\n",
				resp->ipc_status, resp->status);
		ret = resp->ipc_status ? -EIO : (resp->status ? resp->status : -EIO);
		atomic_inc(&client->stats.errors);
		client->stats.last_error_code = resp->status;
		goto out_unlock;
	}

	ret = resp->status;

out_unlock:
	mutex_unlock(&client->lock);
	kfree(resp);
	kfree(req);
	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_fuse_blow);
