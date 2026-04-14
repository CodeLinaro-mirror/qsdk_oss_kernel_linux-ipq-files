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
#include <linux/workqueue.h>

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
#define QMI_TME_ARB_GET_REQ_V01 0x000A
#define QMI_TME_ARB_GET_RESP_V01 0x000A
#define QMI_TME_ARB_UPDATE_REQ_V01 0x000B
#define QMI_TME_ARB_UPDATE_RESP_V01 0x000B
#define QMI_TME_READ_FUSE_REQ_V01 0x000C
#define QMI_TME_READ_FUSE_RESP_V01 0x000C
#define QMI_DPR_IMAGE_LOAD_REQ_V01 0x000D
#define QMI_DPR_IMAGE_LOAD_RESP_V01 0x000D
#define QMI_TME_TMEL_VERSION_READ_REQ_V01 0x000E
#define QMI_TME_TMEL_VERSION_READ_RESP_V01 0x000E
#define QMI_TME_QBEC_KEY_READ_REQ_V01 0x000F
#define QMI_TME_QBEC_KEY_READ_RESP_V01 0x000F
#define QMI_TME_FUSE_BLOW_REQ_V01 0x0009
#define QMI_TME_FUSE_BLOW_RESP_V01 0x0009
#define QMI_GET_CHIP_PARAMS_REQ_V01 0x0010
#define QMI_GET_CHIP_PARAMS_RESP_V01 0x0010

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
#define QMI_TME_DPR_IMAGE_BUFFER_SIZE_V01 32768
#define QMI_TME_TMEL_VERSION_BUFFER_SIZE_V01 64
#define QMI_TME_QBEC_PUBLIC_KEY_SIZE_V01 128
#define QMI_TME_SECDAT_BUFFER_SIZE_V01 32768

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
#define QMI_TME_ARB_GET_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_ARB_GET_RESP_MSG_V01_MAX_MSG_LEN 37
#define QMI_TME_ARB_UPDATE_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_ARB_UPDATE_RESP_MSG_V01_MAX_MSG_LEN 21
#define QMI_TME_READ_FUSE_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_READ_FUSE_RESP_MSG_V01_MAX_MSG_LEN 35
#define QMI_DPR_IMAGE_LOAD_REQ_MSG_V01_MAX_MSG_LEN 32780
#define QMI_DPR_IMAGE_LOAD_RESP_MSG_V01_MAX_MSG_LEN 28
#define QMI_TME_TMEL_VERSION_READ_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_TME_TMEL_VERSION_READ_RESP_MSG_V01_MAX_MSG_LEN 110
#define QMI_TME_QBEC_KEY_READ_REQ_MSG_V01_MAX_MSG_LEN 14
#define QMI_TME_QBEC_KEY_READ_RESP_MSG_V01_MAX_MSG_LEN 167
#define QMI_TME_FUSE_BLOW_REQ_MSG_V01_MAX_MSG_LEN 32780
#define QMI_TME_FUSE_BLOW_RESP_MSG_V01_MAX_MSG_LEN 21
#define QMI_GET_CHIP_PARAMS_REQ_MSG_V01_MAX_MSG_LEN 7
#define QMI_GET_CHIP_PARAMS_RESP_MSG_V01_MAX_MSG_LEN 47

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

struct qmi_arb_get_req_msg_v01 {
	u32 sw_id;
};

struct qmi_arb_get_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 oem_version_valid;
	u8 oem_version;
	u8 qti_version_valid;
	u8 qti_version;
	u8 oem_is_valid_valid;
	u8 oem_is_valid;
	u8 qti_is_valid_valid;
	u8 qti_is_valid;
};

struct qmi_tme_arb_update_req_msg_v01 {
	u32 reserved;
};

struct qmi_tme_arb_update_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
};

struct qmi_tme_read_fuse_req_msg_v01 {
	u32 fuse_addr;
};

struct qmi_tme_read_fuse_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 fuse_val_lsb_valid;
	u32 fuse_val_lsb;
	u8 fuse_val_msb_valid;
	u32 fuse_val_msb;
};

struct qmi_dpr_image_load_req_msg_v01 {
	u32 image_data_len;
	u8 image_data[QMI_TME_DPR_IMAGE_BUFFER_SIZE_V01];
	u32 dpr_image_data_len;
};

struct qmi_dpr_image_load_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 entry_addr_valid;
	u32 entry_addr;
};

struct qmi_tme_tmel_version_read_req_msg_v01 {
	u32 reserved;
};

struct qmi_tme_tmel_version_read_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 tmel_mode_valid;
	u32 tmel_mode;
	u8 tmel_patch_status_valid;
	u32 tmel_patch_status;
	u8 version_info_valid;
	u32 version_info_len;
	u8 version_info[QMI_TME_TMEL_VERSION_BUFFER_SIZE_V01];
	u8 tmel_version_info_len_valid;
	u32 tmel_version_info_len;
};

struct qmi_qbec_key_read_req_msg_v01 {
	u32 feature_id;
	u32 src_l1_key_id;
};

struct qmi_qbec_key_read_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 public_key_valid;
	u32 public_key_len;
	u8 public_key[QMI_TME_QBEC_PUBLIC_KEY_SIZE_V01];
	u8 qbec_public_key_len_valid;
	u32 qbec_public_key_len;
	u8 sequencer_status_valid;
	u32 sequencer_status;
};

struct qmi_tme_fuse_blow_req_msg_v01 {
	u32 secdat_fuse_data_len;
	u8 secdat_fuse_data[QMI_TME_SECDAT_BUFFER_SIZE_V01];
	u32 secdat_data_len;
};

struct qmi_tme_fuse_blow_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
};

struct qmi_get_chip_params_req_msg_v01 {
	u32 reserved;
};

struct qmi_get_chip_params_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 status;
	u32 ipc_status;
	u8 chip_id_valid;
	u32 chip_id;
	u8 serial_number_valid;
	u64 serial_number;
	u8 chip_id_verified_valid;
	u8 chip_id_verified;
	u8 serial_verified_valid;
	u8 serial_verified;
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
extern struct qmi_elem_info qmi_tme_arb_get_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_arb_get_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_arb_update_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_arb_update_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_read_fuse_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_read_fuse_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_dpr_image_load_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_dpr_image_load_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_tmel_version_read_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_tmel_version_read_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_qbec_key_read_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_qbec_key_read_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_fuse_blow_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_tme_fuse_blow_resp_msg_v01_ei[];
extern struct qmi_elem_info qmi_get_chip_params_req_msg_v01_ei[];
extern struct qmi_elem_info qmi_get_chip_params_resp_msg_v01_ei[];

/* ===== Driver Internal Definitions ===== */

#define QRTR_INSTANCE_BASE 7
#define TME_QMI_TIMEOUT_MS 5000
#define TME_QMI_MAX_MSG_LEN 4122  /* Largest message size */

#define DPR_QCN9625_FIRMWARE_DIR "qcn9625"
#define CHIP_ID_QCN9625 0x111317cb

/* Domain to attach number mapping node */
struct domain_attach_node {
	u32 domain_num;           /* PCIe domain number */
	int attach_num;           /* Attach number (1-based, position in sorted list) */
	struct list_head list;    /* Kernel list linkage */
};

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
	int domain_num;
	u32 instance_id;
	struct sockaddr_qrtr sq;
	struct list_head node;
	bool connected;
	struct mutex lock;
	struct tmelcom_qmi_stats stats;
	struct dentry *debugfs_dir;  /* For debugfs */
	struct kobject *sysfs_kobj;  /* For sysfs */
	u32 chip_id;
	u64 serial_number;
};

/* Internal functions */
struct tmelcom_qmi_client *tmelcom_qmi_get_client(int domain_num);

/* Shared variables across compilation units */
extern unsigned int qmi_timeout_ms;
extern atomic_t client_count;
extern struct list_head tmelcom_qmi_clients;
extern struct mutex tmelcom_qmi_clients_lock;

/* Conversion helpers */
static inline int qrtr_instance_to_domain_num(u32 instance_id)
{
	/* The instance_id encodes the PCIe domain in the upper nibble.
	 * The lower nibble is always 0x1 (as per hardware spec).
	 * Shift right by 4 bits to obtain the domain number.
	 */
	return (int)(instance_id >> 4);
}

static inline u32 domain_num_to_qrtr_instance(int domain_num)
{
	/* Reverse of qrtr_instance_to_domain_num:
	 *   instance_id = (domain_num << 4) + 0x1
	 * This yields the correct QRTR node values
	 */
	return (u32)((domain_num << 4) + 0x1);
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
	pr_err("tmelcom_qmi[domain=%d]: " fmt, (client)->domain_num, ##__VA_ARGS__)

#define tmelcom_qmi_info(client, fmt, ...) \
	pr_info("tmelcom_qmi[domain=%d]: " fmt, (client)->domain_num, ##__VA_ARGS__)

#define tmelcom_qmi_dbg(client, fmt, ...) \
	pr_debug("tmelcom_qmi[domain=%d]: " fmt, (client)->domain_num, ##__VA_ARGS__)

#endif /* _TMELCOM_QMI_INTERNAL_H */
