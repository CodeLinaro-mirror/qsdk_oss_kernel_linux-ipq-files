// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * TMEL Communication QMI Client Driver
 * Provides QMI-based communication infrastructure for TME-L services
 * over PCIe-attached Trestles devices.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/qrtr.h>
#include <linux/net.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/tmelcom_qmi.h>

#include "tmelcom_qmi_internal.h"

/* Global client list */
static LIST_HEAD(tmelcom_qmi_clients);
static DEFINE_MUTEX(tmelcom_qmi_clients_lock);
static struct dentry *tmelcom_qmi_debugfs;
atomic_t client_count = ATOMIC_INIT(0);
//EXPORT_SYMBOL_GPL(client_count);

/* Notifier chain for QMI client events */
static BLOCKING_NOTIFIER_HEAD(tmelcom_qmi_notifier_list);

/* Module parameters */
unsigned int qmi_timeout_ms = TME_QMI_TIMEOUT_MS;
module_param(qmi_timeout_ms, uint, 0644);
MODULE_PARM_DESC(qmi_timeout_ms, "QMI transaction timeout in milliseconds");
//EXPORT_SYMBOL_GPL(qmi_timeout_ms);

/* ===== QMI Element Info Arrays ===== */
/* Copied from qmi_tme_service_v01.c */

struct qmi_elem_info qmi_tme_init_attestation_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_init_attestation_req_msg_v01,
					   reserved),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_init_attestation_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   init_att_rsp_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   init_att_rsp_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_INIT_ATT_RESPONSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   init_att_rsp),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   rsp_len_used_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_init_attestation_resp_msg_v01,
					   rsp_len_used),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_dev_attestation_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   att_req_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_ATT_REQ_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   att_req),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   attest_req_len),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   ext_claims_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   ext_claims_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_EXT_CLAIMS_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   ext_claims),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   external_claims_len_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_dev_attestation_req_msg_v01,
					   external_claims_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_dev_attestation_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   att_rsp_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   att_rsp_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_RESPONSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   att_rsp),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   rsp_len_used_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_dev_attestation_resp_msg_v01,
					   rsp_len_used),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_dev_provision_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_dev_provision_req_msg_v01,
					   prov_req_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_PROV_REQ_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_dev_provision_req_msg_v01,
					   prov_req),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_dev_provision_req_msg_v01,
					   provision_req_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_dev_provision_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   prov_rsp_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   prov_rsp_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_PROV_RESPONSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   prov_rsp),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   rsp_len_used_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_dev_provision_resp_msg_v01,
					   rsp_len_used),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_lic_install_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_lic_install_req_msg_v01,
					   license_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_LICENSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_lic_install_req_msg_v01,
					   license),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_lic_install_req_msg_v01,
					   license_data_len),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_lic_install_req_msg_v01,
					   license_operation),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_lic_install_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   identifier_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   identifier_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_LIC_ID_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   identifier),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   id_len_used_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   id_len_used),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   flags_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_tme_lic_install_resp_msg_v01,
					   flags),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_lic_feature_status_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_req_msg_v01,
					   request_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_LICENSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_req_msg_v01,
					   request),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_req_msg_v01,
					   feat_req_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_lic_feature_status_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   response_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   response_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_RESPONSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   response),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   rsp_len_used_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_lic_feature_status_resp_msg_v01,
					   rsp_len_used),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_ttime_get_params_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_req_msg_v01,
					   reserved),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_ttime_get_params_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   request_packet_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   request_packet_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_RESPONSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   request_packet),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   packet_len_used_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_ttime_get_params_resp_msg_v01,
					   packet_len_used),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_ttime_set_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_ttime_set_req_msg_v01,
					   response_packet_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_MAX_RESPONSE_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_ttime_set_req_msg_v01,
					   response_packet),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_ttime_set_req_msg_v01,
					   packet_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_ttime_set_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_ttime_set_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_ttime_set_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_ttime_set_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_lic_clean_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_lic_clean_req_msg_v01,
					   reserved),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_lic_clean_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   to_be_deleted_identifiers_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   to_be_deleted_identifiers_len),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= QMI_TME_MAX_LIC_CLEAN_COUNT_V01,
		.elem_size	= sizeof(u64),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   to_be_deleted_identifiers),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   to_be_deleted_count_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_lic_clean_resp_msg_v01,
					   to_be_deleted_count),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

/* ===== Response Handlers ===== */

static void tmelcom_qmi_response_handler(struct qmi_handle *qmi,
					 struct sockaddr_qrtr *sq,
					 struct qmi_txn *txn,
					 const void *data)
{
	/* Generic response handler - just complete the transaction */
	if (!txn) {
		pr_err("tmelcom_qmi: Spurious response received\n");
		return;
	}

	complete(&txn->completion);
}

/* ===== QMI Message Handlers ===== */

static const struct qmi_msg_handler tme_qmi_msg_handlers[] = {
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_INIT_ATTESTATION_RESP_V01,
		.ei = qmi_tme_init_attestation_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_init_attestation_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_DEV_ATTESTATION_RESP_V01,
		.ei = qmi_tme_dev_attestation_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_dev_attestation_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_DEV_PROVISION_RESP_V01,
		.ei = qmi_tme_dev_provision_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_dev_provision_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_LIC_INSTALL_RESP_V01,
		.ei = qmi_tme_lic_install_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_lic_install_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_LIC_FEATURE_STATUS_RESP_V01,
		.ei = qmi_tme_lic_feature_status_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_lic_feature_status_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_TTIME_GET_PARAMS_RESP_V01,
		.ei = qmi_tme_ttime_get_params_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_ttime_get_params_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_TTIME_SET_RESP_V01,
		.ei = qmi_tme_ttime_set_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_ttime_set_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_LIC_CLEAN_RESP_V01,
		.ei = qmi_tme_lic_clean_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_lic_clean_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{} /* Sentinel */
};

/* ===== Client Management ===== */

/**
 * tmelcom_qmi_get_client() - Get QMI client for a PCIe domain
 * @pcie_domain: PCIe domain number
 *
 * Return: Client pointer on success, NULL if not found
 */
struct tmelcom_qmi_client *tmelcom_qmi_get_client(int pcie_domain)
{
	struct tmelcom_qmi_client *client;

	mutex_lock(&tmelcom_qmi_clients_lock);
	list_for_each_entry(client, &tmelcom_qmi_clients, node) {
		if (client->pcie_domain == pcie_domain && client->connected) {
			mutex_unlock(&tmelcom_qmi_clients_lock);
			return client;
		}
	}
	mutex_unlock(&tmelcom_qmi_clients_lock);

	return NULL;
}

/* ===== Debugfs Implementation ===== */

static int status_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%s\n", client->connected ? "connected" : "disconnected");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int qrtr_node_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "0x%x\n", client->sq.sq_node);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qrtr_node);

static int qrtr_port_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%u\n", client->sq.sq_port);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qrtr_port);

static int qrtr_instance_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%u\n", pcie_domain_to_qrtr_instance(client->pcie_domain));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qrtr_instance);

static int service_id_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "0x%x\n", QMI_TME_SERVICE_ID_V01);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(service_id);

static int service_version_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "0x%x\n", QMI_TME_SERVICE_VERS_V01);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(service_version);

static int requests_sent_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%d\n", atomic_read(&client->stats.requests_sent));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(requests_sent);

static int responses_received_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%d\n", atomic_read(&client->stats.responses_received));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(responses_received);

static int errors_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%d\n", atomic_read(&client->stats.errors));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(errors);

static int timeouts_show(struct seq_file *s, void *unused)
{
	struct tmelcom_qmi_client *client = s->private;

	seq_printf(s, "%d\n", atomic_read(&client->stats.timeouts));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(timeouts);

static int client_count_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "%d\n", atomic_read(&client_count));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(client_count);

/* ===== Platform Driver ===== */

static int tmelcom_qmi_probe(struct platform_device *pdev)
{
	struct tmelcom_qmi_pdata *pdata;
	struct tmelcom_qmi_client *client;
	char dir_name[32];
	int ret;

	client = devm_kzalloc(&pdev->dev, sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data\n");
		return -EINVAL;
	}

	client->pdev = pdev;
	client->sq = pdata->sq;
	client->pcie_domain = qrtr_instance_to_pcie_domain(pdata->instance_id);

	if (client->pcie_domain < 0) {
		dev_err(&pdev->dev, "Invalid QRTR instance\n");
		return -EINVAL;
	}

	mutex_init(&client->lock);
	atomic_set(&client->stats.requests_sent, 0);
	atomic_set(&client->stats.responses_received, 0);
	atomic_set(&client->stats.errors, 0);
	atomic_set(&client->stats.timeouts, 0);

	/* Initialize QMI handle */
	ret = qmi_handle_init(&client->qmi, TME_QMI_MAX_MSG_LEN, NULL,
			      tme_qmi_msg_handlers);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to initialize QMI handle: %d\n", ret);
		return ret;
	}

	/* Connect to the server */
	ret = kernel_connect(client->qmi.sock, (struct sockaddr *)&client->sq,
			     sizeof(client->sq), 0);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to connect to QMI server: %d\n", ret);
		goto err_release_qmi;
	}

	client->connected = true;

	/* Create debugfs directory */
	snprintf(dir_name, sizeof(dir_name), "domain_%d", client->pcie_domain);
	client->debugfs_dir = debugfs_create_dir(dir_name, tmelcom_qmi_debugfs);
	if (client->debugfs_dir) {
		debugfs_create_file("status", 0400, client->debugfs_dir,
				    client, &status_fops);
		debugfs_create_file("qrtr_node", 0400, client->debugfs_dir,
				    client, &qrtr_node_fops);
		debugfs_create_file("qrtr_port", 0400, client->debugfs_dir,
				    client, &qrtr_port_fops);
		debugfs_create_file("qrtr_instance", 0400, client->debugfs_dir,
				    client, &qrtr_instance_fops);
		debugfs_create_file("service_id", 0400, client->debugfs_dir,
				    client, &service_id_fops);
		debugfs_create_file("service_version", 0400, client->debugfs_dir,
				    client, &service_version_fops);
		debugfs_create_file("requests_sent", 0400, client->debugfs_dir,
				    client, &requests_sent_fops);
		debugfs_create_file("responses_received", 0400, client->debugfs_dir,
				    client, &responses_received_fops);
		debugfs_create_file("errors", 0400, client->debugfs_dir,
				    client, &errors_fops);
		debugfs_create_file("timeouts", 0400, client->debugfs_dir,
				    client, &timeouts_fops);
	}

	/* Add to global list */
	mutex_lock(&tmelcom_qmi_clients_lock);
	list_add_tail(&client->node, &tmelcom_qmi_clients);
	mutex_unlock(&tmelcom_qmi_clients_lock);

	atomic_inc(&client_count);
	platform_set_drvdata(pdev, client);

	dev_info(&pdev->dev, "TME QMI client registered for PCIe domain %d (node=0x%x, port=%u)\n",
		 client->pcie_domain, client->sq.sq_node, client->sq.sq_port);

	/* Notify registered listeners about the new connection */
	{
		struct tmelcom_qmi_notify_data notify_data = {
			.pcie_domain = client->pcie_domain,
			.event = TMELCOM_QMI_CLIENT_CONNECTED,
		};
		blocking_notifier_call_chain(&tmelcom_qmi_notifier_list,
					     TMELCOM_QMI_CLIENT_CONNECTED,
					     &notify_data);
	}

	return 0;

err_release_qmi:
	qmi_handle_release(&client->qmi);
	return ret;
}

static int tmelcom_qmi_remove(struct platform_device *pdev)
{
	struct tmelcom_qmi_client *client = platform_get_drvdata(pdev);

	if (!client)
		return 0;

	/* Notify registered listeners about disconnection */
	{
		struct tmelcom_qmi_notify_data notify_data = {
			.pcie_domain = client->pcie_domain,
			.event = TMELCOM_QMI_CLIENT_DISCONNECTED,
		};
		blocking_notifier_call_chain(&tmelcom_qmi_notifier_list,
					     TMELCOM_QMI_CLIENT_DISCONNECTED,
					     &notify_data);
	}

	client->connected = false;

	/* Remove from global list */
	mutex_lock(&tmelcom_qmi_clients_lock);
	list_del(&client->node);
	mutex_unlock(&tmelcom_qmi_clients_lock);

	atomic_dec(&client_count);

	/* Remove debugfs */
	debugfs_remove_recursive(client->debugfs_dir);

	/* Release QMI handle */
	qmi_handle_release(&client->qmi);

	dev_info(&pdev->dev, "TME QMI client removed for PCIe domain %d\n",
		 client->pcie_domain);

	return 0;
}

static struct platform_driver tmelcom_qmi_driver = {
	.probe = tmelcom_qmi_probe,
	.remove = tmelcom_qmi_remove,
	.driver = {
		.name = "tmelcom_qmi_client",
	},
};

/* ===== QMI Service Discovery ===== */

static int tmelcom_qmi_new_server(struct qmi_handle *qmi,
				   struct qmi_service *service)
{
	struct platform_device *pdev;
	struct tmelcom_qmi_pdata pdata;
	int pcie_domain;
	int ret;

	/* Extract PCIe domain from QRTR instance ID */
	pcie_domain = qrtr_instance_to_pcie_domain(service->instance);
	if (pcie_domain < 0) {
		pr_err("tmelcom_qmi: Invalid QRTR instance ID: %u\n",
		       service->instance);
		return -EINVAL;
	}

	pr_debug("tmelcom_qmi: Discovered TME QMI server: node=0x%x, port=%u, instance=%u, PCIe domain=%d\n",
		service->node, service->port, service->instance, pcie_domain);

	/* Prepare platform data */
	pdata.sq.sq_family = AF_QIPCRTR;
	pdata.sq.sq_node = service->node;
	pdata.sq.sq_port = service->port;
	pdata.instance_id = service->instance;

	/* Create platform device */
	pdev = platform_device_alloc("tmelcom_qmi_client", pcie_domain);
	if (!pdev)
		return -ENOMEM;

	ret = platform_device_add_data(pdev, &pdata, sizeof(pdata));
	if (ret)
		goto err_put_device;

	ret = platform_device_add(pdev);
	if (ret)
		goto err_put_device;

	service->priv = pdev;

	return 0;

err_put_device:
	platform_device_put(pdev);
	return ret;
}

static void tmelcom_qmi_del_server(struct qmi_handle *qmi,
				    struct qmi_service *service)
{
	struct platform_device *pdev = service->priv;

	if (pdev) {
		pr_info("tmelcom_qmi: TME QMI server disconnected: node=0x%x, port=%u\n",
			service->node, service->port);
		platform_device_unregister(pdev);
	}
}

static struct qmi_handle lookup_client;

static const struct qmi_ops lookup_ops = {
	.new_server = tmelcom_qmi_new_server,
	.del_server = tmelcom_qmi_del_server,
};

/* ===== Notifier Registration API ===== */

/**
 * tmelcom_qmi_register_notifier() - Register for QMI client notifications
 * @nb: Notifier block to register
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&tmelcom_qmi_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_register_notifier);

/**
 * tmelcom_qmi_unregister_notifier() - Unregister QMI client notifier
 * @nb: Notifier block to unregister
 *
 * Return: 0 on success, negative error code on failure
 */
int tmelcom_qmi_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&tmelcom_qmi_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(tmelcom_qmi_unregister_notifier);

/* ===== Module Init/Exit ===== */

static int __init tmelcom_qmi_init(void)
{
	int ret;

	/* Create debugfs directory */
	tmelcom_qmi_debugfs = debugfs_create_dir("tmelcom_qmi", NULL);
	if (IS_ERR_OR_NULL(tmelcom_qmi_debugfs)) {
		pr_warn("tmelcom_qmi: Failed to create debugfs directory, continuing without debugfs\n");
		tmelcom_qmi_debugfs = NULL;
	} else {
		/* Create client_count debugfs entry */
		debugfs_create_file("client_count", 0400, tmelcom_qmi_debugfs,
				    NULL, &client_count_fops);
	}

	/* Register platform driver */
	ret = platform_driver_register(&tmelcom_qmi_driver);
	if (ret) {
		pr_err("tmelcom_qmi: Failed to register platform driver: %d\n", ret);
		goto err_remove_debugfs;
	}

	/* Initialize QMI lookup handle */
	ret = qmi_handle_init(&lookup_client, 0, &lookup_ops, NULL);
	if (ret < 0) {
		pr_err("tmelcom_qmi: Failed to initialize lookup handle: %d\n", ret);
		goto err_unregister_driver;
	}

	/* Add service lookup */
	ret = qmi_add_lookup(&lookup_client, QMI_TME_SERVICE_ID_V01, 0, 0);
	if (ret < 0) {
		pr_err("tmelcom_qmi: Failed to add service lookup: %d\n", ret);
		goto err_release_lookup;
	}

	pr_info("tmelcom_qmi: Driver initialized successfully (looking for service 0x%x\n", QMI_TME_SERVICE_ID_V01);
	return 0;

err_release_lookup:
	qmi_handle_release(&lookup_client);
err_unregister_driver:
	platform_driver_unregister(&tmelcom_qmi_driver);
err_remove_debugfs:
	debugfs_remove_recursive(tmelcom_qmi_debugfs);
	return ret;
}

static void __exit tmelcom_qmi_exit(void)
{
	pr_info("tmelcom_qmi: Exiting TME QMI client driver\n");

	qmi_handle_release(&lookup_client);
	platform_driver_unregister(&tmelcom_qmi_driver);
	debugfs_remove_recursive(tmelcom_qmi_debugfs);
}

module_init(tmelcom_qmi_init);
module_exit(tmelcom_qmi_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TMEL Communication QMI Client Driver");
MODULE_AUTHOR("Qualcomm Innovation Center, Inc.");
