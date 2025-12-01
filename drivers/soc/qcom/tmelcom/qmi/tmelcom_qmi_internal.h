/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _TMELCOM_QMI_INTERNAL_H
#define _TMELCOM_QMI_INTERNAL_H

#include <linux/soc/qcom/qmi.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>

/* ===== QMI TME Service Definitions ===== */
/* Copied from qmi_tme_service_v01.h */

#define QMI_TME_SERVICE_ID_V01 0x5F
#define QMI_TME_SERVICE_VERS_V01 0x01

/* Message IDs */
#define QMI_TME_INIT_ATTESTATION_REQ_V01 0x0001
#define QMI_TME_INIT_ATTESTATION_RESP_V01 0x0001
#define QMI_TME_DEV_ATTESTATION_REQ_V01 0x0002
#define QMI_TME_DEV_ATTESTATION_RESP_V01 0x0002
#define QMI_TME_DEV_PROVISION_REQ_V01 0x0003
#define QMI_TME_DEV_PROVISION_RESP_V01 0x0003
#define QMI_TME_LIC_INSTALL_REQ_V01 0x0004
#define QMI_TME_LIC_INSTALL_RESP_V01 0x0004
#define QMI_TME_LIC_FEATURE_STATUS_REQ_V01 0x0005
#define QMI_TME_LIC_FEATURE_STATUS_RESP_V01 0x0005
#define QMI_TME_TTIME_GET_PARAMS_REQ_V01 0x0006
#define QMI_TME_TTIME_GET_PARAMS_RESP_V01 0x0006
#define QMI_TME_TTIME_SET_REQ_V01 0x0007
#define QMI_TME_TTIME_SET_RESP_V01 0x0007
#define QMI_TME_LIC_CLEAN_REQ_V01 0x0008
#define QMI_TME_LIC_CLEAN_RESP_V01 0x0008

/* Max sizes */
#define QMI_TME_MAX_KEY_SIZE_V01 128
#define QMI_TME_MAX_RESPONSE_SIZE_V01 2048
#define QMI_TME_MAX_LICENSE_SIZE_V01 4096
#define QMI_TME_MAX_CHIPINFO_ID_LEN_V01 32
#define QMI_TME_MAX_FEATURE_LIST_V01 10
#define QMI_TME_MAX_EXT_CLAIMS_SIZE_V01 200
#define QMI_TME_MAX_PROV_REQ_SIZE_V01 2048
#define QMI_TME_MAX_ATT_REQ_SIZE_V01 2048
#define QMI_TME_MAX_LIC_ID_SIZE_V01 256
#define QMI_TME_MAX_HW_FEATURE_SIZE_V01 1024
#define QMI_TME_MAX_INIT_ATT_RESPONSE_SIZE_V01 100
#define QMI_TME_MAX_PROV_RESPONSE_SIZE_V01 100
#define QMI_TME_MAX_LIC_CLEAN_COUNT_V01 30

/* Max message lengths */
#define QMI_TME_INIT_ATTESTATION_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_INIT_ATTESTATION_RESP_MSG_V01_MAX_MSG_LEN 125
#define QMI_TME_DEV_ATTESTATION_REQ_MSG_V01_MAX_MSG_LEN 2271
#define QMI_TME_DEV_ATTESTATION_RESP_MSG_V01_MAX_MSG_LEN 2074
#define QMI_TME_DEV_PROVISION_REQ_MSG_V01_MAX_MSG_LEN 2060
#define QMI_TME_DEV_PROVISION_RESP_MSG_V01_MAX_MSG_LEN 125
#define QMI_TME_LIC_INSTALL_REQ_MSG_V01_MAX_MSG_LEN 4122
#define QMI_TME_LIC_INSTALL_RESP_MSG_V01_MAX_MSG_LEN 282
#define QMI_TME_LIC_FEATURE_STATUS_REQ_MSG_V01_MAX_MSG_LEN 4108
#define QMI_TME_LIC_FEATURE_STATUS_RESP_MSG_V01_MAX_MSG_LEN 2074
#define QMI_TME_TTIME_GET_PARAMS_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_TTIME_GET_PARAMS_RESP_MSG_V01_MAX_MSG_LEN 2074
#define QMI_TME_TTIME_SET_REQ_MSG_V01_MAX_MSG_LEN 2060
#define QMI_TME_TTIME_SET_RESP_MSG_V01_MAX_MSG_LEN 14
#define QMI_TME_LIC_CLEAN_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_LIC_CLEAN_RESP_MSG_V01_MAX_MSG_LEN 265

/* Message structures */
struct qmi_tme_init_attestation_req_msg_v01 {
	u32 reserved;
};

struct qmi_tme_init_attestation_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 init_att_rsp_valid;
	u32 init_att_rsp_len;
	u8 init_att_rsp[QMI_TME_MAX_INIT_ATT_RESPONSE_SIZE_V01];
	u8 rsp_len_used_valid;
	u32 rsp_len_used;
};

struct qmi_tme_dev_attestation_req_msg_v01 {
	u32 att_req_len;
	u8 att_req[QMI_TME_MAX_ATT_REQ_SIZE_V01];
	u32 attest_req_len;
	u8 ext_claims_valid;
	u32 ext_claims_len;
	u8 ext_claims[QMI_TME_MAX_EXT_CLAIMS_SIZE_V01];
	u8 external_claims_len_valid;
	u32 external_claims_len;
};

struct qmi_tme_dev_attestation_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 att_rsp_valid;
	u32 att_rsp_len;
	u8 att_rsp[QMI_TME_MAX_RESPONSE_SIZE_V01];
	u8 rsp_len_used_valid;
	u32 rsp_len_used;
};

struct qmi_tme_dev_provision_req_msg_v01 {
	u32 prov_req_len;
	u8 prov_req[QMI_TME_MAX_PROV_REQ_SIZE_V01];
	u32 provision_req_len;
};

struct qmi_tme_dev_provision_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 prov_rsp_valid;
	u32 prov_rsp_len;
	u8 prov_rsp[QMI_TME_MAX_PROV_RESPONSE_SIZE_V01];
	u8 rsp_len_used_valid;
	u32 rsp_len_used;
};

struct qmi_tme_lic_install_req_msg_v01 {
	u32 license_len;
	u8 license[QMI_TME_MAX_LICENSE_SIZE_V01];
	u32 license_data_len;
	u32 license_operation;
};

struct qmi_tme_lic_install_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 identifier_valid;
	u32 identifier_len;
	u8 identifier[QMI_TME_MAX_LIC_ID_SIZE_V01];
	u8 id_len_used_valid;
	u32 id_len_used;
	u8 flags_valid;
	u32 flags;
};

struct qmi_tme_lic_feature_status_req_msg_v01 {
	u32 request_len;
	u8 request[QMI_TME_MAX_LICENSE_SIZE_V01];
	u32 feat_req_len;
};

struct qmi_tme_lic_feature_status_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 response_valid;
	u32 response_len;
	u8 response[QMI_TME_MAX_RESPONSE_SIZE_V01];
	u8 rsp_len_used_valid;
	u32 rsp_len_used;
};

struct qmi_tme_ttime_get_params_req_msg_v01 {
	u32 reserved;
};

struct qmi_tme_ttime_get_params_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 request_packet_valid;
	u32 request_packet_len;
	u8 request_packet[QMI_TME_MAX_RESPONSE_SIZE_V01];
	u8 packet_len_used_valid;
	u32 packet_len_used;
};

struct qmi_tme_ttime_set_req_msg_v01 {
	u32 response_packet_len;
	u8 response_packet[QMI_TME_MAX_RESPONSE_SIZE_V01];
	u32 packet_len;
};

struct qmi_tme_ttime_set_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
};

struct qmi_tme_lic_clean_req_msg_v01 {
	u32 reserved;
};

struct qmi_tme_lic_clean_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 to_be_deleted_identifiers_valid;
	u32 to_be_deleted_identifiers_len;
	u64 to_be_deleted_identifiers[QMI_TME_MAX_LIC_CLEAN_COUNT_V01];
	u8 to_be_deleted_count_valid;
	u32 to_be_deleted_count;
};

/* Element info array declarations */
extern struct qmi_elem_info qmi_tme_init_attestation_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_init_attestation_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_dev_attestation_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_dev_attestation_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_dev_provision_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_dev_provision_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_lic_install_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_lic_install_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_lic_feature_status_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_lic_feature_status_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_ttime_get_params_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_ttime_get_params_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_ttime_set_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_ttime_set_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_lic_clean_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_lic_clean_resp_msg_v01_ei[];

/* ===== Driver Internal Definitions ===== */

#define QRTR_INSTANCE_BASE 7
#define TME_QMI_TIMEOUT_MS 5000
#define TME_QMI_MAX_MSG_LEN 4122  /* Largest message size */

/* Statistics structure */
struct tmelcom_qmi_stats {
	atomic_t requests_sent;
	atomic_t responses_received;
	atomic_t errors;
	atomic_t timeouts;
	u32 last_error_code;
	ktime_t last_request_time;
	ktime_t last_response_time;
};

/* Platform data structure */
struct tmelcom_qmi_pdata {
	struct sockaddr_qrtr sq;
	u32 instance_id;
};

/* QMI client structure per PCIe domain */
struct tmelcom_qmi_client {
	struct qmi_handle qmi;
	struct platform_device *pdev;
	int pcie_domain;
	struct sockaddr_qrtr sq;
	struct list_head node;
	bool connected;
	struct mutex lock;
	struct tmelcom_qmi_stats stats;
	struct dentry *debugfs_dir;  /* For debugfs */
};

/* Internal functions */
struct tmelcom_qmi_client *tmelcom_qmi_get_client(int pcie_domain);

/* Shared variables across compilation units */
extern unsigned int qmi_timeout_ms;
extern atomic_t client_count;

/* Conversion helpers */
static inline int qrtr_instance_to_pcie_domain(u32 instance_id)
{
	if (instance_id < QRTR_INSTANCE_BASE)
		return -EINVAL;
	return instance_id - QRTR_INSTANCE_BASE;
}

static inline u32 pcie_domain_to_qrtr_instance(int pcie_domain)
{
	return QRTR_INSTANCE_BASE + pcie_domain;
}

/* Error codes */
#define TMELCOM_QMI_SUCCESS		0
#define TMELCOM_QMI_ERR_NO_CLIENT	-ENODEV
#define TMELCOM_QMI_ERR_NOT_CONNECTED	-ENOTCONN
#define TMELCOM_QMI_ERR_TIMEOUT		-ETIMEDOUT
#define TMELCOM_QMI_ERR_QMI_FAILURE	-EIO
#define TMELCOM_QMI_ERR_INVALID_DOMAIN	-EINVAL
#define TMELCOM_QMI_ERR_INVALID_PARAM	-EINVAL
#define TMELCOM_QMI_ERR_NO_MEMORY	-ENOMEM

/* Logging macros */
#define tmelcom_qmi_err(client, fmt, ...) \
	pr_err("tmelcom_qmi[domain=%d]: " fmt, (client)->pcie_domain, ##__VA_ARGS__)

#define tmelcom_qmi_info(client, fmt, ...) \
	pr_info("tmelcom_qmi[domain=%d]: " fmt, (client)->pcie_domain, ##__VA_ARGS__)

#define tmelcom_qmi_dbg(client, fmt, ...) \
	pr_debug("tmelcom_qmi[domain=%d]: " fmt, (client)->pcie_domain, ##__VA_ARGS__)

#endif /* _TMELCOM_QMI_INTERNAL_H */
