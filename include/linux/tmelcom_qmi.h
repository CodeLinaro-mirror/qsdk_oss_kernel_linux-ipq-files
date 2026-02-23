/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _LINUX_TMELCOM_QMI_H
#define _LINUX_TMELCOM_QMI_H

#include <linux/types.h>
#include <linux/notifier.h>
#include <linux/errno.h>

/* ===== License Operation Types ===== */

/* License operation types for QMI */
#define QMI_TME_LIC_OPERATION_DOWNLOAD_V01 0
#define QMI_TME_LIC_OPERATION_IMPORT_V01 1

/* ===== QMI Client Notification Events ===== */

/**
 * enum tmelcom_qmi_event - QMI client notification events
 * @TMELCOM_QMI_CLIENT_CONNECTED: QMI client successfully connected
 * @TMELCOM_QMI_CLIENT_DISCONNECTED: QMI client disconnected
 */
enum tmelcom_qmi_event {
	TMELCOM_QMI_CLIENT_CONNECTED,
	TMELCOM_QMI_CLIENT_DISCONNECTED,
};

/**
 * struct tmelcom_qmi_notify_data - Notification data structure
 * @pcie_domain: PCIe domain number of the client
 * @event: Event type (connected/disconnected)
 */
struct tmelcom_qmi_notify_data {
	int pcie_domain;
	enum tmelcom_qmi_event event;
};

#if IS_ENABLED(CONFIG_QCOM_TMELCOM_QMI)

/**
 * tmelcom_qmi_init_attestation() - Initialize attestation
 * @pcie_domain: PCIe domain number (0, 1, 2, etc.)
 * @rsp_buf: Response buffer
 * @rsp_buf_len: Response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_init_attestation(int pcie_domain, u8 *rsp_buf,
				  u32 rsp_buf_len, u32 *rsp_len_used);

/**
 * tmelcom_qmi_dev_attestation() - Device attestation
 * @pcie_domain: PCIe domain number
 * @att_req: Attestation request buffer
 * @att_req_len: Attestation request length
 * @ext_claims: External claims buffer (optional, can be NULL)
 * @ext_claims_len: External claims length
 * @att_rsp: Attestation response buffer
 * @att_rsp_len: Attestation response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_dev_attestation(int pcie_domain, u8 *att_req, u32 att_req_len,
				 u8 *ext_claims, u32 ext_claims_len,
				 u8 *att_rsp, u32 att_rsp_len, u32 *rsp_len_used);

/**
 * tmelcom_qmi_dev_provision() - Device provisioning
 * @pcie_domain: PCIe domain number
 * @prov_req: Provisioning request buffer
 * @prov_req_len: Provisioning request length
 * @prov_rsp: Provisioning response buffer
 * @prov_rsp_len: Provisioning response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_dev_provision(int pcie_domain, u8 *prov_req, u32 prov_req_len,
			       u8 *prov_rsp, u32 prov_rsp_len, u32 *rsp_len_used);

/**
 * tmelcom_qmi_lic_install() - Install license
 * @pcie_domain: PCIe domain number
 * @license: License data buffer
 * @license_len: License data length
 * @operation: License operation (download or import)
 * @identifier: License identifier buffer (output)
 * @id_len: Identifier buffer length
 * @id_len_used: Actual identifier length used (output)
 * @flags: License flags returned from TME-L (output)
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_install(int pcie_domain, u8 *license, u32 license_len,
			     u32 operation, u8 *identifier,
			     u32 id_len, u32 *id_len_used, u32 *flags);

/**
 * tmelcom_qmi_lic_feature_status() - Get license feature status
 * @pcie_domain: PCIe domain number
 * @request: Request buffer
 * @req_len: Request length
 * @response: Response buffer
 * @rsp_len: Response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_feature_status(int pcie_domain, u8 *request, u32 req_len,
				    u8 *response, u32 rsp_len, u32 *rsp_len_used);

/**
 * tmelcom_qmi_ttime_get_params() - Get TTIME parameters
 * @pcie_domain: PCIe domain number
 * @params: Parameters buffer
 * @buf_len: Buffer length
 * @used_len: Actual length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_ttime_get_params(int pcie_domain, u8 *params, u32 buf_len,
				  u32 *used_len);

/**
 * tmelcom_qmi_ttime_set() - Set TTIME
 * @pcie_domain: PCIe domain number
 * @ttime_data: TTIME data buffer
 * @buf_len: Buffer length
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_ttime_set(int pcie_domain, u8 *ttime_data, u32 buf_len);

/**
 * tmelcom_qmi_lic_clean() - Get licenses to be deleted/cleaned
 * @pcie_domain: PCIe domain number
 * @identifiers: Buffer to receive license identifiers (u64 array) to be deleted
 * @id_buf_len: Buffer length in bytes
 * @id_len_used: Actual length used in bytes (output)
 * @delete_count: Number of licenses to be deleted (output)
 *
 * This function sends a request to TME-L to get the list of license
 * identifiers that should be deleted/cleaned up. The identifiers are
 * returned as an array of u64 values in the identifiers buffer, and
 * the count is returned in delete_count.
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_clean(int pcie_domain, u64 *identifiers, u32 id_buf_len,
			   u32 *id_len_used, u32 *delete_count);

/**
 * tmelcom_qmi_is_client_ready() - Check if QMI client is ready
 * @pcie_domain: PCIe domain number
 *
 * Return: true if client is ready, false otherwise
 */
bool tmelcom_qmi_is_client_ready(int pcie_domain);

/**
 * tmelcom_qmi_get_client_count() - Get number of connected clients
 *
 * Return: Number of connected QMI clients
 */
int tmelcom_qmi_get_client_count(void);

/**
 * tmelcom_qmi_get_service_info() - Get service information for a domain
 * @pcie_domain: PCIe domain number
 * @service_id: Service ID (output, can be NULL)
 * @service_version: Service version (output, can be NULL)
 * @qrtr_node: QRTR node ID (output, can be NULL)
 * @qrtr_port: QRTR port (output, can be NULL)
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_get_service_info(int pcie_domain, u32 *service_id,
				  u32 *service_version, u32 *qrtr_node,
				  u32 *qrtr_port);

/**
 * tmelcom_qmi_register_notifier() - Register for QMI client notifications
 * @nb: Notifier block to register
 *
 * Register a notifier to receive notifications when QMI clients connect
 * or disconnect. The notifier callback will be called with:
 * - action: TMELCOM_QMI_CLIENT_CONNECTED or TMELCOM_QMI_CLIENT_DISCONNECTED
 * - data: struct tmelcom_qmi_notify_data containing pcie_domain and event
 *
 * Example usage:
 *   static int my_qmi_notify(struct notifier_block *nb,
 *                            unsigned long action, void *data)
 *   {
 *       struct tmelcom_qmi_notify_data *notify = data;
 *       if (action == TMELCOM_QMI_CLIENT_CONNECTED) {
 *           // Handle connection for notify->pcie_domain
 *       }
 *       return NOTIFY_OK;
 *   }
 *
 *   static struct notifier_block my_nb = {
 *       .notifier_call = my_qmi_notify,
 *   };
 *
 *   tmelcom_qmi_register_notifier(&my_nb);
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_register_notifier(struct notifier_block *nb);

/**
 * tmelcom_qmi_unregister_notifier() - Unregister QMI client notifier
 * @nb: Notifier block to unregister
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_unregister_notifier(struct notifier_block *nb);

#else /* !CONFIG_QCOM_TMELCOM_QMI */

/* Stub functions when QCOM_TMELCOM_QMI is not enabled */

static inline int tmelcom_qmi_init_attestation(int pcie_domain, u8 *rsp_buf,
						u32 rsp_buf_len, u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_dev_attestation(int pcie_domain, u8 *att_req, u32 att_req_len,
					       u8 *ext_claims, u32 ext_claims_len,
					       u8 *att_rsp, u32 att_rsp_len, u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_dev_provision(int pcie_domain, u8 *prov_req, u32 prov_req_len,
					     u8 *prov_rsp, u32 prov_rsp_len, u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_lic_install(int pcie_domain, u8 *license, u32 license_len,
					   u32 operation, u8 *identifier,
					   u32 id_len, u32 *id_len_used, u32 *flags)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_lic_feature_status(int pcie_domain, u8 *request, u32 req_len,
						  u8 *response, u32 rsp_len, u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_ttime_get_params(int pcie_domain, u8 *params, u32 buf_len,
						u32 *used_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_ttime_set(int pcie_domain, u8 *ttime_data, u32 buf_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_lic_clean(int pcie_domain, u64 *identifiers, u32 id_buf_len,
					 u32 *id_len_used, u32 *delete_count)
{
	return -ENODEV;
}

static inline bool tmelcom_qmi_is_client_ready(int pcie_domain)
{
	return false;
}

static inline int tmelcom_qmi_get_client_count(void)
{
	return 0;
}

static inline int tmelcom_qmi_get_service_info(int pcie_domain, u32 *service_id,
						u32 *service_version, u32 *qrtr_node,
						u32 *qrtr_port)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_register_notifier(struct notifier_block *nb)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_unregister_notifier(struct notifier_block *nb)
{
	return -ENODEV;
}

#endif /* CONFIG_QCOM_TMELCOM_QMI */

#endif /* _LINUX_TMELCOM_QMI_H */
