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
 * @domain_num: Domain number of the client
 * @event: Event type (connected/disconnected)
 */
struct tmelcom_qmi_notify_data {
	int domain_num;
	enum tmelcom_qmi_event event;
};

#if IS_ENABLED(CONFIG_QCOM_TMELCOM_QMI)

/**
 * tmelcom_qmi_init_attestation() - Initialize attestation
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @rsp_buf: Response buffer
 * @rsp_buf_len: Response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_init_attestation(int attach_num, u8 *rsp_buf, u32 rsp_buf_len,
				 u32 *rsp_len_used);

/**
 * tmelcom_qmi_dev_attestation() - Device attestation
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
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
int tmelcom_qmi_dev_attestation(int attach_num, u8 *att_req, u32 att_req_len,
				u8 *ext_claims, u32 ext_claims_len,
				u8 *att_rsp, u32 att_rsp_len,
				u32 *rsp_len_used);

/**
 * tmelcom_qmi_dev_provision() - Device provisioning
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @prov_req: Provisioning request buffer
 * @prov_req_len: Provisioning request length
 * @prov_rsp: Provisioning response buffer
 * @prov_rsp_len: Provisioning response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_dev_provision(int attach_num, u8 *prov_req, u32 prov_req_len,
			      u8 *prov_rsp, u32 prov_rsp_len,
			      u32 *rsp_len_used);

/**
 * tmelcom_qmi_lic_install() - Install license
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
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
int tmelcom_qmi_lic_install(int attach_num, u8 *license, u32 license_len,
			    u32 operation, u8 *identifier, u32 id_len,
			    u32 *id_len_used, u32 *flags);

/**
 * tmelcom_qmi_lic_feature_status() - Get license feature status
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @request: Request buffer
 * @req_len: Request length
 * @response: Response buffer
 * @rsp_len: Response buffer length
 * @rsp_len_used: Actual response length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_lic_feature_status(int attach_num, u8 *request, u32 req_len,
				   u8 *response, u32 rsp_len,
				   u32 *rsp_len_used);

/**
 * tmelcom_qmi_ttime_get_params() - Get TTIME parameters
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @params: Parameters buffer
 * @buf_len: Buffer length
 * @used_len: Actual length used
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_ttime_get_params(int attach_num, u8 *params, u32 buf_len,
				 u32 *used_len);

/**
 * tmelcom_qmi_ttime_set() - Set TTIME
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @ttime_data: TTIME data buffer
 * @buf_len: Buffer length
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_ttime_set(int attach_num, u8 *ttime_data, u32 buf_len);

/**
 * tmelcom_qmi_lic_clean() - Get licenses to be deleted/cleaned
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
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
int tmelcom_qmi_lic_clean(int attach_num, u64 *identifiers, u32 id_buf_len,
			  u32 *id_len_used, u32 *delete_count);

/**
 * tmelcom_qmi_secboot_get_arb_version() - Get ARB version
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @sw_id: Software ID
 * @version: Version output pointer
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_secboot_get_arb_version(int attach_num, u32 sw_id, u32 *version);

/**
 * tmelcom_qmi_secboot_update_arb_version_list() - Update ARB version list
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_secboot_update_arb_version_list(int attach_num);

/**
 * tmelcom_qmi_get_ecc_public_key() - Get ECC public key
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @key_type: Key type/source L1 key ID
 * @buf: Buffer to store the public key
 * @size: Size of the buffer
 * @rsp_len: Pointer to store the actual response length
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_get_ecc_public_key(int attach_num, u32 key_type, void *buf,
				   u32 size, u32 *rsp_len);

/**
 * tmelcom_qmi_read_fuse() - Read fuse value
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @fuse_addr: Fuse address
 * @fuse_val_lsb: Fuse value LSB (output)
 * @fuse_val_msb: Fuse value MSB (output)
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_read_fuse(int attach_num, u32 fuse_addr, u32 *fuse_val_lsb,
			  u32 *fuse_val_msb);

/**
 * tmelcom_qmi_tmel_version_read() - Read TMEL version
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @version_info: Buffer to store version information
 * @buf_len: Buffer length
 * @version_info_len: Actual version info length (output)
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_tmel_version_read(int attach_num, u8 *version_info, u32 buf_len,
				  u32 *version_info_len);

/**
 * tmelcom_qmi_fuse_blow() - Blow fuse with secdat data
 * @attach_num: Attach number - dynamically mapped based on sorted instance IDs
 * @secdat_data: Secdat fuse data buffer
 * @data_len: Data length
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_fuse_blow(int attach_num, u8 *secdat_data, u32 data_len);

/**
 * tmelcom_qmi_get_slot_for_instance_id() - Get slot number for instance ID
 * @instance_id: Instance ID
 *
 * Return: Slot number (0, 1, 2, ...) or -1 if not found
 */
int tmelcom_qmi_get_slot_for_instance_id(u32 instance_id);

/**
 * tmelcom_qmi_is_client_ready() - Check if QMI client is ready
 * @domain_num: Domain number
 *
 * Return: true if client is ready, false otherwise
 */
bool tmelcom_qmi_is_client_ready(int domain_num);

/**
 * tmelcom_qmi_get_client_count() - Get number of connected clients
 *
 * Return: Number of connected QMI clients
 */
int tmelcom_qmi_get_client_count(void);

/**
 * tmelcom_qmi_get_service_info() - Get service information for a domain
 * @domain_num: Domain number
 * @service_id: Service ID (output, can be NULL)
 * @service_version: Service version (output, can be NULL)
 * @qrtr_node: QRTR node ID (output, can be NULL)
 * @qrtr_port: QRTR port (output, can be NULL)
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_get_service_info(int domain_num, u32 *service_id,
				 u32 *service_version, u32 *qrtr_node,
				 u32 *qrtr_port);

/**
 * tmelcom_qmi_register_notifier() - Register for QMI client notifications
 * @nb: Notifier block to register
 *
 * Register a notifier to receive notifications when QMI clients connect
 * or disconnect. The notifier callback will be called with:
 * - action: TMELCOM_QMI_CLIENT_CONNECTED or TMELCOM_QMI_CLIENT_DISCONNECTED
 * - data: struct tmelcom_qmi_notify_data containing domain_num and event
 *
 * Example usage:
 *   static int my_qmi_notify(struct notifier_block *nb,
 *                            unsigned long action, void *data)
 *   {
 *       struct tmelcom_qmi_notify_data *notify = data;
 *       if (action == TMELCOM_QMI_CLIENT_CONNECTED) {
 *           // Handle connection for notify->domain_num
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

static inline int tmelcom_qmi_init_attestation(int attach_num, u8 *rsp_buf,
					       u32 rsp_buf_len,
					       u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_dev_attestation(int attach_num, u8 *att_req,
					      u32 att_req_len,
					      u8 *ext_claims,
					      u32 ext_claims_len,
					      u8 *att_rsp, u32 att_rsp_len,
					      u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_dev_provision(int attach_num, u8 *prov_req,
					    u32 prov_req_len, u8 *prov_rsp,
					    u32 prov_rsp_len,
					    u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_lic_install(int attach_num, u8 *license,
					  u32 license_len, u32 operation,
					  u8 *identifier, u32 id_len,
					  u32 *id_len_used, u32 *flags)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_lic_feature_status(int attach_num, u8 *request,
						 u32 req_len, u8 *response,
						 u32 rsp_len,
						 u32 *rsp_len_used)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_ttime_get_params(int attach_num, u8 *params,
					       u32 buf_len, u32 *used_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_ttime_set(int attach_num, u8 *ttime_data, u32 buf_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_lic_clean(int attach_num, u64 *identifiers,
					u32 id_buf_len, u32 *id_len_used,
					u32 *delete_count)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_secboot_get_arb_version(int attach_num, u32 sw_id,
						      u32 *version)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_secboot_update_arb_version_list(int attach_num)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_get_ecc_public_key(int attach_num, u32 key_type,
						 void *buf, u32 size,
						 u32 *rsp_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_read_fuse(int attach_num, u32 fuse_addr,
					u32 *fuse_val_lsb,
					u32 *fuse_val_msb)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_tmel_version_read(int attach_num, u8 *version_info,
						u32 buf_len,
						u32 *version_info_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_fuse_blow(int attach_num, u8 *secdat_data, u32 data_len)
{
	return -ENODEV;
}

static inline int tmelcom_qmi_get_slot_for_instance_id(u32 instance_id)
{
	return -1;
}

static inline bool tmelcom_qmi_is_client_ready(int domain_num)
{
	return false;
}

static inline int tmelcom_qmi_get_client_count(void)
{
	return 0;
}

static inline int tmelcom_qmi_get_service_info(int domain_num, u32 *service_id,
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
