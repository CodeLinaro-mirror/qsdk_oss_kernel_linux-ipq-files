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
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sort.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>
#include <linux/dirent.h>
#include <linux/namei.h>

#include "tmelcom_qmi_internal.h"

/* Global client list */
LIST_HEAD(tmelcom_qmi_clients);
/* Protects tmelcom_qmi_clients list */
DEFINE_MUTEX(tmelcom_qmi_clients_lock);
static struct dentry *tmelcom_qmi_debugfs;
static struct kobject *tmelcomm_sysfs_root;
static struct kobject *qmi_sysfs_root;
atomic_t client_count = ATOMIC_INIT(0);
/* Sorted domain list: position in list = attach number */
static LIST_HEAD(domain_attach_list);
//EXPORT_SYMBOL_GPL(client_count);

/* Notifier chain for QMI client events */
static BLOCKING_NOTIFIER_HEAD(tmelcom_qmi_notifier_list);

/* Module parameters */
unsigned int qmi_timeout_ms = TME_QMI_TIMEOUT_MS;
module_param(qmi_timeout_ms, uint, 0644);
MODULE_PARM_DESC(qmi_timeout_ms, "QMI transaction timeout in milliseconds");
//EXPORT_SYMBOL_GPL(qmi_timeout_ms);

/* ===== PCIe Domain to Attach Number Mapping (Linked List) ===== */

/**
 * domain_to_attach_num() - Get attach number for a given domain
 * @domain: PCIe domain number
 *
 * Returns: Attach number (1-based, position in sorted list) or -1 if not found
 */
static int domain_to_attach_num(u32 domain)
{
	struct domain_attach_node *node;

	list_for_each_entry(node, &domain_attach_list, list) {
		if (node->domain_num == domain)
			return node->attach_num;
	}
	return -1;
}

/**
 * register_domain_attach_num() - Register domain and assign attach number
 * @new_domain: The new PCIe domain to add
 *
 * Registers the domain in sorted order in the linked list and reassigns attach numbers
 * for all nodes based on their position. Must be called with tmelcom_qmi_clients_lock held.
 */
static void register_domain_attach_num(u32 new_domain)
{
	struct domain_attach_node *new_node, *pos;
	int next_attach_num = 1;

	/* Check if domain already exists */
	list_for_each_entry(pos, &domain_attach_list, list) {
		if (pos->domain_num == new_domain)
			return;  /* Already exists */
	}

	/* Allocate new node */
	new_node = kzalloc(sizeof(*new_node), GFP_KERNEL);
	if (!new_node) {
		pr_err("tmelcom_qmi: Failed to allocate domain_attach_node\n");
		return;
	}

	new_node->domain_num = new_domain;
	INIT_LIST_HEAD(&new_node->list);

	/* Calculate next attach_num based on current list size (discovery order) */
	list_for_each_entry(pos, &domain_attach_list, list) {
		next_attach_num++;
	}
	new_node->attach_num = next_attach_num;

	/* Add at end to maintain discovery order */
	list_add_tail(&new_node->list, &domain_attach_list);
}

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

struct qmi_elem_info qmi_tme_arb_get_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_arb_get_req_msg_v01,
					   sw_id),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_arb_get_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   oem_version_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   oem_version),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   qti_version_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   qti_version),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   oem_is_valid_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   oem_is_valid),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x13,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   qti_is_valid_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x13,
		.offset		= offsetof(struct qmi_arb_get_resp_msg_v01,
					   qti_is_valid),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_arb_update_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_arb_update_req_msg_v01,
					   reserved),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_arb_update_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_arb_update_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_arb_update_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_arb_update_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_read_fuse_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_read_fuse_req_msg_v01,
					   fuse_addr),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_read_fuse_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   fuse_val_lsb_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   fuse_val_lsb),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   fuse_val_msb_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_read_fuse_resp_msg_v01,
					   fuse_val_msb),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_dpr_image_load_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_dpr_image_load_req_msg_v01,
					   image_data_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_DPR_IMAGE_BUFFER_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_dpr_image_load_req_msg_v01,
					   image_data),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_dpr_image_load_req_msg_v01,
					   dpr_image_data_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_dpr_image_load_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_dpr_image_load_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_dpr_image_load_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_dpr_image_load_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_dpr_image_load_resp_msg_v01,
					   entry_addr_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_dpr_image_load_resp_msg_v01,
					   entry_addr),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_tmel_version_read_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_req_msg_v01,
					   reserved),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_tmel_version_read_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   tmel_mode_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   tmel_mode),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   tmel_patch_status_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   tmel_patch_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   version_info_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   version_info_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_TMEL_VERSION_BUFFER_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   version_info),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x13,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   tmel_version_info_len_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x13,
		.offset		= offsetof(struct qmi_tme_tmel_version_read_resp_msg_v01,
					   tmel_version_info_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_qbec_key_read_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_qbec_key_read_req_msg_v01,
					   feature_id),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_qbec_key_read_req_msg_v01,
					   src_l1_key_id),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_qbec_key_read_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   public_key_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   public_key_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_QBEC_PUBLIC_KEY_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   public_key),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   qbec_public_key_len_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   qbec_public_key_len),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   sequencer_status_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x12,
		.offset		= offsetof(struct qmi_qbec_key_read_resp_msg_v01,
					   sequencer_status),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_fuse_blow_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_fuse_blow_req_msg_v01,
					   secdat_fuse_data_len),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= QMI_TME_SECDAT_BUFFER_SIZE_V01,
		.elem_size	= sizeof(u8),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct qmi_tme_fuse_blow_req_msg_v01,
					   secdat_fuse_data),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_fuse_blow_req_msg_v01,
					   secdat_data_len),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_tme_fuse_blow_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct qmi_tme_fuse_blow_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct qmi_tme_fuse_blow_resp_msg_v01,
					   status),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct qmi_tme_fuse_blow_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

/* ===== Response Handlers ===== */

struct qmi_elem_info qmi_get_chip_params_req_msg_v01_ei[] = {
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct
					   qmi_get_chip_params_req_msg_v01,
					   reserved),
	},
	{
		.data_type      = QMI_EOTI,
		.array_type       = NO_ARRAY,
		.tlv_type       = QMI_COMMON_TLV_TYPE,
	},
};

struct qmi_elem_info qmi_get_chip_params_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   resp),
		.ei_array      = qmi_response_type_v01_ei,
	},
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x03,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   status),
	},
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x04,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   ipc_status),
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   chip_id_valid),
	},
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   chip_id),
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x11,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   serial_number_valid),
	},
	{
		.data_type      = QMI_UNSIGNED_8_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u64),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x11,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   serial_number),
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x12,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   chip_id_verified_valid),
	},
	{
		.data_type      = QMI_UNSIGNED_1_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x12,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   chip_id_verified),
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x13,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   serial_verified_valid),
	},
	{
		.data_type      = QMI_UNSIGNED_1_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x13,
		.offset         = offsetof(struct
					   qmi_get_chip_params_resp_msg_v01,
					   serial_verified),
	},
	{
		.data_type      = QMI_EOTI,
		.array_type       = NO_ARRAY,
		.tlv_type       = QMI_COMMON_TLV_TYPE,
	},
};

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
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_ARB_GET_RESP_V01,
		.ei = qmi_tme_arb_get_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_arb_get_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_ARB_UPDATE_RESP_V01,
		.ei = qmi_tme_arb_update_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_arb_update_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_READ_FUSE_RESP_V01,
		.ei = qmi_tme_read_fuse_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_read_fuse_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_DPR_IMAGE_LOAD_RESP_V01,
		.ei = qmi_dpr_image_load_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_dpr_image_load_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_TMEL_VERSION_READ_RESP_V01,
		.ei = qmi_tme_tmel_version_read_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_tmel_version_read_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_QBEC_KEY_READ_RESP_V01,
		.ei = qmi_qbec_key_read_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_qbec_key_read_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_TME_FUSE_BLOW_RESP_V01,
		.ei = qmi_tme_fuse_blow_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_tme_fuse_blow_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{
		.type = QMI_RESPONSE,
		.msg_id = QMI_GET_CHIP_PARAMS_RESP_V01,
		.ei = qmi_get_chip_params_resp_msg_v01_ei,
		.decoded_size = sizeof(struct qmi_get_chip_params_resp_msg_v01),
		.fn = tmelcom_qmi_response_handler,
	},
	{} /* Sentinel */
};

/* ===== Client Management ===== */

/**
 * tmelcom_qmi_get_client() - Get QMI client for an attach number
 * @attach_num: Attach number (1, 2, 3, etc.) - assigned during probe
 *
 * Lookup: Find domain node with matching attach_num, then find client with that domain
 *
 * Return: Client pointer on success, NULL if not found
 */
struct tmelcom_qmi_client *tmelcom_qmi_get_client(int attach_num)
{
	struct tmelcom_qmi_client *client;
	struct domain_attach_node *node;
	u32 target_domain = 0;
	bool found = false;

	if (attach_num < 1) {
		pr_err("tmelcom_qmi: Invalid attach_num %d (must be >= 1)\n",
		       attach_num);
		return NULL;
	}

	mutex_lock(&tmelcom_qmi_clients_lock);

	/* Find domain with matching attach_num */
	list_for_each_entry(node, &domain_attach_list, list) {
		if (node->attach_num == attach_num) {
			target_domain = node->domain_num;
			found = true;
			break;
		}
	}

	if (!found) {
		pr_debug("tmelcom_qmi: attach_num %d not found in domain list\n",
			 attach_num);
		mutex_unlock(&tmelcom_qmi_clients_lock);
		return NULL;
	}

	/* Find client with matching domain */
	list_for_each_entry(client, &tmelcom_qmi_clients, node) {
		if (client->domain_num == target_domain && client->connected) {
			mutex_unlock(&tmelcom_qmi_clients_lock);
			return client;
		}
	}

	mutex_unlock(&tmelcom_qmi_clients_lock);
	return NULL;
}

/* ===== Sysfs Implementation ===== */

/* Helper to get client from kobject */
static struct tmelcom_qmi_client *kobj_to_client(struct kobject *kobj)
{
	struct tmelcom_qmi_client *client;

	mutex_lock(&tmelcom_qmi_clients_lock);
	list_for_each_entry(client, &tmelcom_qmi_clients, node) {
		if (client->sysfs_kobj == kobj) {
			mutex_unlock(&tmelcom_qmi_clients_lock);
			return client;
		}
	}
	mutex_unlock(&tmelcom_qmi_clients_lock);

	return NULL;
}

/* Basic info attributes */
static ssize_t instance_id_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);

	if (!client)
		return -ENODEV;

	return sysfs_emit(buf, "0x%x\n", client->instance_id);
}

/* attach_num attribute - shows the assigned attach number (1-based) */
static ssize_t attach_num_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	int attach_num;

	if (!client)
		return -ENODEV;

	attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0)
		return -ENOENT;

	return sysfs_emit(buf, "%d\n", attach_num);
}

/* ARB Version Get - write sw_id, read returns version info */
static ssize_t arb_version_get_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	u32 sw_id, version = 0;
	int ret;

	if (!client)
		return -ENODEV;

	ret = kstrtou32(buf, 0, &sw_id);
	if (ret)
		return ret;

	/* Calculate attach_num on-demand */
	int attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0)
		return -ENOENT;

	/* Call IPC directly */
	ret = tmelcom_qmi_secboot_get_arb_version(attach_num, sw_id,
						  &version);
	/*
	 * The API returns:
	 * - Negative values for QMI-level errors (timeout, no client, etc.)
	 * - resp->ipc_status when QMI fails (usually 0)
	 * - resp->status on success (can be non-zero error code from firmware)
	 *
	 * We should only print the version if ret == 0 (complete success).
	 * For any other return value, it's an error.
	 */
	if (ret != 0) {
		pr_err("ARB version query failed for sw_id 0x%x: ret=%d\n", sw_id, ret);
		return (ret < 0) ? ret : -EIO;
	}

	/* Only print version on success (ret == 0) */
	pr_info("ARB version for sw_id 0x%x: 0x%x\n", sw_id, version);

	return count;
}

static ssize_t arb_version_get_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "Write sw_id to query ARB version\n");
}

/* ARB Version Update - write triggers update */
static ssize_t arb_version_update_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	int ret;

	if (!client)
		return -ENODEV;

	/* Calculate attach_num on-demand */
	int attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0)
		return -ENOENT;

	/* Call IPC directly */
	ret = tmelcom_qmi_secboot_update_arb_version_list(attach_num);
	if (ret)
		return ret;

	pr_info("ARB version list updated successfully\n");

	return count;
}

/* ECC Public Key - write src_l1_key_id, read returns key */
static ssize_t ecc_public_key_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	u32 src_l1_key_id;
	u8 public_key[QMI_TME_QBEC_PUBLIC_KEY_SIZE_V01];
	u32 public_key_len;
	int ret, i;

	if (!client)
		return -ENODEV;

	ret = kstrtou32(buf, 0, &src_l1_key_id);
	if (ret)
		return ret;

	/* Calculate attach_num on-demand */
	int attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0)
		return -ENOENT;

	/* Call API with key_type (src_l1_key_id) */
	ret = tmelcom_qmi_get_ecc_public_key(attach_num, src_l1_key_id,
					     public_key, sizeof(public_key),
					     &public_key_len);
	if (ret)
		return ret;

	/* Print public key in hex format */
	if (public_key_len > 0) {
		int print_len = min_t(u32, public_key_len, 32);
		char hex_buf[96];  /* 32 bytes * 3 chars per byte */
		int pos = 0;

		for (i = 0; i < print_len && pos < sizeof(hex_buf) - 3; i++) {
			pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos,
					"%02x ", public_key[i]);
		}
		pr_info("ECC public key (len=%u): %s%s\n", public_key_len, hex_buf,
			public_key_len > 32 ? "..." : "");
	} else {
		pr_info("ECC public key: empty\n");
	}

	return count;
}

static ssize_t ecc_public_key_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "Write src_l1_key_id to get key\n");
}

/* Fuse Read - write fuse_addr, read returns fuse value */
static ssize_t fuse_read_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	u32 fuse_addr, fuse_val_lsb, fuse_val_msb;
	int ret;

	if (!client)
		return -ENODEV;

	ret = kstrtou32(buf, 0, &fuse_addr);
	if (ret)
		return ret;

	/* Calculate attach_num on-demand */
	int attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0)
		return -ENOENT;

	/* Call IPC directly */
	ret = tmelcom_qmi_read_fuse(attach_num, fuse_addr,
				    &fuse_val_lsb, &fuse_val_msb);
	if (ret)
		return ret;

	pr_info("Fuse[0x%x]: lsb=0x%08x msb=0x%08x\n",
		fuse_addr, fuse_val_lsb, fuse_val_msb);

	return count;
}

static ssize_t fuse_read_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "Write fuse_addr to read fuse value\n");
}

/* TMEL Version - read-only, returns version */
static ssize_t tmel_version_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	u8 version_info[QMI_TME_TMEL_VERSION_BUFFER_SIZE_V01];
	u32 version_info_len;
	int ret;

	if (!client)
		return -ENODEV;

	/* Calculate attach_num on-demand */
	int attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0)
		return -ENOENT;

	/* Call IPC directly */
	ret = tmelcom_qmi_tmel_version_read(attach_num, version_info,
					    sizeof(version_info),
					    &version_info_len);
	if (ret)
		return ret;

	/* Ensure null termination */
	if (version_info_len < sizeof(version_info))
		version_info[version_info_len] = '\0';
	else
		version_info[sizeof(version_info) - 1] = '\0';

	return sysfs_emit(buf, "%s\n", version_info);
}

/* Chip Parameters - read-only, returns chip ID and serial number */
static ssize_t chip_params_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	int ret;

	if (!client)
		return -ENODEV;

	ret = tmelcom_qmi_get_chip_params(client, &client->chip_id,
					  &client->serial_number);
	if (ret)
		return sysfs_emit(buf, "Error retrieving chip params: %d\n", ret);

	return sysfs_emit(buf, "chip_id: 0x%x\nserial_number: 0x%llx\n",
			  client->chip_id, client->serial_number);
}

/* Fuse Blow - write file path, read file and pass contents to QMI */
static ssize_t fuse_blow_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct tmelcom_qmi_client *client = kobj_to_client(kobj);
	struct file *fptr = NULL;
	struct kstat st;
	void *file_data = NULL;
	loff_t file_size;
	char *file_path;
	int ret;
	loff_t pos = 0;
	size_t original_count = count;
	size_t path_len;

	if (!client)
		return -ENODEV;

	/* Validate input size to prevent excessive allocations */
	if (count > PATH_MAX || count == 0)
		return -EINVAL;

	/* Allocate buffer for file path and remove trailing newline/whitespace */
	file_path = kzalloc(count + 1, GFP_KERNEL);
	if (!file_path)
		return -ENOMEM;

	memcpy(file_path, buf, count);
	file_path[count] = '\0';

	/* Remove trailing newline and whitespace if present */
	path_len = count;
	while (path_len > 0 && (file_path[path_len - 1] == '\n' ||
				file_path[path_len - 1] == '\r' ||
				file_path[path_len - 1] == ' ' ||
				file_path[path_len - 1] == '\t')) {
		file_path[path_len - 1] = '\0';
		path_len--;
	}

	if (path_len == 0) {
		pr_err("Empty file path provided\n");
		ret = -EINVAL;
		goto free_path;
	}

	/* Open file from path */
	fptr = filp_open(file_path, O_RDONLY, 0);
	if (IS_ERR(fptr)) {
		pr_err("File open failed: '%s' (error %ld)\n", file_path, PTR_ERR(fptr));
		pr_err("Make sure the file path is absolute (e.g., /tmp/sec.elf)\n");
		ret = PTR_ERR(fptr);
		goto free_path;
	}

	/* Get file size */
	ret = vfs_getattr(&fptr->f_path, &st, STATX_SIZE, AT_STATX_SYNC_AS_STAT);
	if (ret) {
		pr_err("Getting file attributes failed for '%s': %d\n", file_path, ret);
		goto file_close;
	}
	file_size = st.size;

	if (file_size == 0) {
		pr_err("File is empty: '%s'\n", file_path);
		ret = -EINVAL;
		goto file_close;
	}

	/* Allocate buffer for file contents */
	file_data = vzalloc(file_size);
	if (!file_data) {
		pr_err("Failed to allocate %lld bytes for file data\n", file_size);
		ret = -ENOMEM;
		goto file_close;
	}

	/* Read file contents */
	ret = kernel_read(fptr, file_data, file_size, &pos);
	if (ret != file_size) {
		pr_err("File read failed for '%s': expected %lld bytes, got %d\n",
		       file_path, file_size, ret);
		ret = (ret < 0) ? ret : -EIO;
		goto free_data;
	}

	/* Calculate attach_num on-demand */
	int attach_num = domain_to_attach_num(client->domain_num);
	if (attach_num < 0) {
		ret = -ENOENT;
		goto free_data;
	}

	/* Call QMI function with file buffer and size */
	ret = tmelcom_qmi_fuse_blow(attach_num, (u8 *)file_data, file_size);
	if (ret) {
		pr_err("Fuse blow failed for '%s': %d\n", file_path, ret);
		/* Return the actual error code */
		goto free_data;
	}

	pr_info("Fuse blow successful for '%s'\n", file_path);

	/* Return original count to indicate success */
	ret = original_count;

free_data:
	vfree(file_data);
file_close:
	filp_close(fptr, NULL);
free_path:
	kfree(file_path);
	return ret;
}

/* Attribute definitions */
static struct kobj_attribute instance_id_attr = __ATTR_RO(instance_id);
static struct kobj_attribute attach_num_attr = __ATTR_RO(attach_num);
static struct kobj_attribute arb_read_version_attr =
	__ATTR(arb_read_version, 0644, arb_version_get_show,
	       arb_version_get_store);
static struct kobj_attribute version_commit_attr =
	__ATTR(version_commit, 0200, NULL, arb_version_update_store);
static struct kobj_attribute get_ecc_public_key_attr =
	__ATTR(get_ecc_public_key, 0644, ecc_public_key_show,
	       ecc_public_key_store);
static struct kobj_attribute dump_fuse_attr =
	__ATTR(dump_fuse, 0644, fuse_read_show, fuse_read_store);
static struct kobj_attribute tmel_version_attr = __ATTR_RO(tmel_version);
static struct kobj_attribute chip_params_attr = __ATTR_RO(chip_params);
static struct kobj_attribute sec_elf_attr =
	__ATTR(sec_elf, 0200, NULL, fuse_blow_store);

static struct attribute *qmi_client_attrs[] = {
	&instance_id_attr.attr,
	&attach_num_attr.attr,
	&arb_read_version_attr.attr,
	&version_commit_attr.attr,
	&get_ecc_public_key_attr.attr,
	&dump_fuse_attr.attr,
	&tmel_version_attr.attr,
	&chip_params_attr.attr,
	&sec_elf_attr.attr,
	NULL,
};

static struct attribute_group qmi_client_attr_group = {
	.attrs = qmi_client_attrs,
};

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

	seq_printf(s, "%u\n", domain_num_to_qrtr_instance(client->domain_num));
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

/**
 * get_dpr_firmware_dir() - Get DPR firmware directory for chip ID
 * @chip_id: Chip ID to check
 *
 * Return: Firmware directory path, or NULL if chip ID not supported
 */
static const char *get_dpr_firmware_dir(u32 chip_id)
{
	switch (chip_id) {
	case CHIP_ID_QCN9625:
		return DPR_QCN9625_FIRMWARE_DIR;
	default:
		return NULL;
	}
}

/**
 * load_dpr_image_for_client() - Load DPR image for a client based on serial number
 * @client: QMI client structure
 *
 * Tries to load DPR images in priority order:
 * 1. First tries: DPR_0x<serial_number>.elf
 * 2. Fallback: DPR_COMMON.elf
 *
 * Files should be placed in /lib/firmware/qcn9625/
 *
 * This function is non-fatal - errors are logged but don't fail probe.
 *
 * Return: 0 on success, negative error code on failure (non-fatal)
 */
static int load_dpr_image_for_client(struct tmelcom_qmi_client *client)
{
	const struct firmware *fw = NULL;
	void *image_data = NULL;
	const char *firmware_dir;
	char *firmware_path = NULL;
	int ret, len;

	if (!client || client->serial_number == 0) {
		dev_info(&client->pdev->dev,
			 "Skipping DPR load: no valid serial number\n");
		return -EINVAL;
	}

	firmware_dir = get_dpr_firmware_dir(client->chip_id);
	if (!firmware_dir) {
		dev_info(&client->pdev->dev,
			 "DPR loading not supported for chip_id=0x%x\n",
			 client->chip_id);
		return -EOPNOTSUPP;
	}

	firmware_path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!firmware_path)
		return -ENOMEM;

	dev_dbg(&client->pdev->dev,
		"Searching for DPR image: chip_id=0x%x, serial=0x%llx\n",
		client->chip_id, client->serial_number);

	/* Try 1: Load serial-specific file (DPR_0x<serial>.elf) */
	len = snprintf(firmware_path, PATH_MAX,
		       "%s/DPR_0x%llx.elf",
		       firmware_dir, client->serial_number);
	if (len >= PATH_MAX) {
		dev_err(&client->pdev->dev,
			"DPR firmware path too long (would be %d chars)\n", len);
		ret = -ENAMETOOLONG;
		goto free_path;
	}

	ret = request_firmware_direct(&fw, firmware_path, &client->pdev->dev);
	if (ret == 0) {
		dev_info(&client->pdev->dev,
			 "Found serial-specific DPR: %s\n", firmware_path);
		goto load_image;
	} else if (ret == -ENOENT) {
		dev_dbg(&client->pdev->dev,
			"Serial-specific DPR not found: %s (trying common fallback)\n",
			firmware_path);
	} else {
		dev_err(&client->pdev->dev,
			"Failed to load serial-specific DPR: %s (error %d)\n",
			firmware_path, ret);
	}

	/* Try 2: Load common file (DPR_COMMON.elf) as fallback */
	len = snprintf(firmware_path, PATH_MAX,
		       "%s/DPR_COMMON.elf", firmware_dir);
	if (len >= PATH_MAX) {
		dev_err(&client->pdev->dev,
			"DPR firmware path too long (would be %d chars)\n", len);
		ret = -ENAMETOOLONG;
		goto free_path;
	}

	ret = request_firmware_direct(&fw, firmware_path, &client->pdev->dev);
	if (ret == 0) {
		dev_info(&client->pdev->dev,
			 "Found common DPR: %s\n", firmware_path);
	} else if (ret == -ENOENT) {
		dev_dbg(&client->pdev->dev,
			"Common DPR not found: %s (no DPR image available)\n",
			firmware_path);
	} else {
		dev_err(&client->pdev->dev,
			"Failed to load common DPR: %s (error %d)\n",
			firmware_path, ret);
		goto free_path;
	}

	if (ret == -ENOENT) {
		dev_warn(&client->pdev->dev,
			 "DPR not found: no DPR image available\n");
		goto free_path;
	}

load_image:
	image_data = vzalloc(fw->size);
	if (!image_data) {
		ret = -ENOMEM;
		goto cleanup;
	}

	memcpy(image_data, fw->data, fw->size);

	ret = tmelcom_qmi_dpr_image_load(client, (u8 *)image_data, fw->size);

	if (ret == 0) {
		dev_info(&client->pdev->dev,
			 "DPR image loaded successfully: %s (size=%zu bytes)\n",
			 firmware_path, fw->size);
	} else {
		dev_err(&client->pdev->dev,
			"DPR image load failed: %s (size=%zu bytes): error %d\n",
			firmware_path, fw->size, ret);
	}

cleanup:
	if (fw)
		release_firmware(fw);
	if (image_data)
		vfree(image_data);
free_path:
	kfree(firmware_path);

	return ret;
}

/* ===== Platform Driver ===== */

static int tmelcom_qmi_probe(struct platform_device *pdev)
{
	struct tmelcom_qmi_pdata *pdata;
	struct tmelcom_qmi_client *client;
	char dir_name[32];
	int attach_num;
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
	client->instance_id = pdata->instance_id;
	client->domain_num = qrtr_instance_to_domain_num(pdata->instance_id);

	if (client->domain_num < 0) {
		dev_err(&pdev->dev, "Invalid QRTR instance\n");
		return -EINVAL;
	}

	mutex_init(&client->lock);
	atomic_set(&client->stats.requests_sent, 0);
	atomic_set(&client->stats.responses_received, 0);
	atomic_set(&client->stats.errors, 0);
	atomic_set(&client->stats.timeouts, 0);

	/* Initialize QMI handle BEFORE adding to list */
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

	/* NOW add to global list with fully initialized client */
	mutex_lock(&tmelcom_qmi_clients_lock);
	list_add_tail(&client->node, &tmelcom_qmi_clients);

	/* Register domain and assign attach number */
	register_domain_attach_num(client->domain_num);

	mutex_unlock(&tmelcom_qmi_clients_lock);

	ret = tmelcom_qmi_get_chip_params(client, &client->chip_id,
					  &client->serial_number);
	if (ret == 0) {
		dev_dbg(&pdev->dev,
			"Chip params: chip_id=0x%x, serial_number=0x%llx\n",
			client->chip_id, client->serial_number);
	} else {
		dev_warn(&pdev->dev,
			 "Failed to retrieve chip params: %d\n", ret);
		client->chip_id = 0;
		client->serial_number = 0;
	}

	/* Create debugfs directory */
	snprintf(dir_name, sizeof(dir_name), "domain_%d", client->domain_num);
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

	/* Create sysfs directory for this instance using domain_num (unique, no conflicts) */
	if (qmi_sysfs_root) {
		char sysfs_name[16];

		snprintf(sysfs_name, sizeof(sysfs_name), "%d", client->domain_num);
		client->sysfs_kobj = kobject_create_and_add(sysfs_name,
							    qmi_sysfs_root);
		if (!client->sysfs_kobj) {
			dev_warn(&pdev->dev,
				 "Failed to create sysfs directory\n");
		} else {
			/* Create sysfs files */
			ret = sysfs_create_group(client->sysfs_kobj,
						 &qmi_client_attr_group);
			if (ret) {
				dev_warn(&pdev->dev,
					 "Failed to create sysfs attrs: %d\n",
					 ret);
				kobject_put(client->sysfs_kobj);
				client->sysfs_kobj = NULL;
				/* Don't fail probe for sysfs errors - just warn */
			} else {
				attach_num = domain_to_attach_num(client->domain_num);
				dev_dbg(&pdev->dev,
					"Sysfs: /sys/kernel/tmelcomm/qmi/%d/ (instance_id=0x%x, attach_num=%d)\n",
					client->domain_num, client->instance_id,
					attach_num);
			}
		}
	}

	atomic_inc(&client_count);
	platform_set_drvdata(pdev, client);

	dev_dbg(&pdev->dev,
		"TME QMI client registered for PCIe domain %d\n",
		client->domain_num);

	{
		attach_num = domain_to_attach_num(client->domain_num);
		dev_dbg(&pdev->dev,
			"node=0x%x port=%u instance_id=0x%x attach_num=%d\n",
			client->sq.sq_node, client->sq.sq_port,
			client->instance_id, attach_num);
	}

	/* Notify registered listeners about the new connection */
	{
		struct tmelcom_qmi_notify_data notify_data = {
			.domain_num = client->domain_num,
			.event = TMELCOM_QMI_CLIENT_CONNECTED,
		};
		blocking_notifier_call_chain(&tmelcom_qmi_notifier_list,
					     TMELCOM_QMI_CLIENT_CONNECTED,
					     &notify_data);
	}

	/* Load DPR image if chip params are valid */
	if (client->chip_id != 0 && client->serial_number != 0) {
		ret = load_dpr_image_for_client(client);
		if (ret) {
			dev_warn(&pdev->dev,
				 "DPR loading failed: %d (non-fatal)\n", ret);
		}
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
			.domain_num = client->domain_num,
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

	/* Remove sysfs */
	if (client->sysfs_kobj) {
		sysfs_remove_group(client->sysfs_kobj, &qmi_client_attr_group);
		kobject_put(client->sysfs_kobj);
	}

	/* Remove debugfs */
	debugfs_remove_recursive(client->debugfs_dir);

	/* Release QMI handle */
	qmi_handle_release(&client->qmi);

	dev_info(&pdev->dev, "TME QMI client removed for PCIe domain %d\n",
		 client->domain_num);

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

/**
 * qrtr_node_to_instance_id() - Calculate and validate instance ID from QRTR node
 * @node: QRTR node number
 *
 * Calculates the instance ID from QRTR node, matching MHI's calculation:
 * instance_id = node - QRTR_INSTANCE_BASE
 *
 * Also checks if a client with this instance ID already exists.
 *
 * Examples:
 *   node 0x18 - 0x7 = 0x11 (PCIe domain 1)
 *   node 0x28 - 0x7 = 0x21 (PCIe domain 2)
 *   node 0x38 - 0x7 = 0x31 (PCIe domain 3)
 *
 * Return: Instance ID on success, -EEXIST if already registered, negative error code on failure
 */
static int qrtr_node_to_instance_id(u32 node)
{
	struct tmelcom_qmi_client *client;
	u32 instance_id;
	bool already_exists = false;

	/* Validate QRTR node */
	if (node < QRTR_INSTANCE_BASE) {
		pr_err("tmelcom_qmi: Invalid QRTR node: 0x%x (must be >= 0x%x)\n",
		       node, QRTR_INSTANCE_BASE);
		return -EINVAL;
	}

	/* Calculate instance ID from QRTR node */
	instance_id = node - QRTR_INSTANCE_BASE;

	/* Check if a client with this instance ID already exists */
	mutex_lock(&tmelcom_qmi_clients_lock);
	list_for_each_entry(client, &tmelcom_qmi_clients, node) {
		if (client->instance_id == instance_id) {
			already_exists = true;
			pr_info("tmelcom_qmi: QMI server instance 0x%x already registered, ignoring duplicate\n",
				instance_id);
			break;
		}
	}
	mutex_unlock(&tmelcom_qmi_clients_lock);

	if (already_exists)
		return -EEXIST;

	return instance_id;
}

static int tmelcom_qmi_new_server(struct qmi_handle *qmi,
				   struct qmi_service *service)
{
	struct platform_device *pdev;
	struct tmelcom_qmi_pdata pdata;
	u32 calculated_instance_id;
	int domain_num;
	int ret;

	/* Calculate and validate instance ID from QRTR node */
	ret = qrtr_node_to_instance_id(service->node);
	if (ret == -EEXIST)
		return 0;  /* Already registered, not an error */
	if (ret < 0)
		return ret;  /* Validation error */

	calculated_instance_id = ret;

	/* Calculate domain number from instance ID */
	domain_num = qrtr_instance_to_domain_num(calculated_instance_id);
	if (domain_num < 0 || domain_num > 31) {  /* Reasonable upper bound */
		pr_err("tmelcom_qmi: Invalid PCIe domain %d from instance ID: 0x%x\n",
		       domain_num, calculated_instance_id);
		return -EINVAL;
	}

	pr_info("tmelcom_qmi: Discovered TME QMI server: node=0x%x, port=%u, calculated_instance=0x%x, PCIe domain=%d\n",
		service->node, service->port, calculated_instance_id, domain_num);

	/* Prepare platform data */
	pdata.sq.sq_family = AF_QIPCRTR;
	pdata.sq.sq_node = service->node;
	pdata.sq.sq_port = service->port;
	pdata.instance_id = calculated_instance_id;

	/* Create platform device with QRTR node as unique ID to avoid conflicts */
	pdev = platform_device_alloc("tmelcom_qmi_client", service->node);
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

	/* Create sysfs root directory at /sys/kernel/tmelcomm/qmi/ */
	tmelcomm_sysfs_root = kobject_create_and_add("tmelcomm", kernel_kobj);
	if (!tmelcomm_sysfs_root) {
		pr_warn("tmelcom_qmi: Failed to create tmelcomm sysfs root, continuing without sysfs\n");
	} else {
		qmi_sysfs_root = kobject_create_and_add("qmi", tmelcomm_sysfs_root);
		if (!qmi_sysfs_root) {
			pr_warn("tmelcom_qmi: Failed to create qmi sysfs root, continuing without sysfs\n");
			kobject_put(tmelcomm_sysfs_root);
			tmelcomm_sysfs_root = NULL;
		}
	}

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

	pr_info("tmelcom_qmi: Driver initialized (service 0x%x)\n",
		QMI_TME_SERVICE_ID_V01);
	return 0;

err_release_lookup:
	qmi_handle_release(&lookup_client);
err_unregister_driver:
	platform_driver_unregister(&tmelcom_qmi_driver);
err_remove_debugfs:
	debugfs_remove_recursive(tmelcom_qmi_debugfs);
	if (qmi_sysfs_root)
		kobject_put(qmi_sysfs_root);
	if (tmelcomm_sysfs_root)
		kobject_put(tmelcomm_sysfs_root);
	return ret;
}

static void __exit tmelcom_qmi_exit(void)
{
	pr_info("tmelcom_qmi: Exiting TME QMI client driver\n");

	qmi_handle_release(&lookup_client);
	platform_driver_unregister(&tmelcom_qmi_driver);
	debugfs_remove_recursive(tmelcom_qmi_debugfs);

	/* Remove sysfs root */
	if (qmi_sysfs_root)
		kobject_put(qmi_sysfs_root);
	if (tmelcomm_sysfs_root)
		kobject_put(tmelcomm_sysfs_root);
}

module_init(tmelcom_qmi_init);
module_exit(tmelcom_qmi_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TMEL Communication QMI Client Driver");
MODULE_AUTHOR("Qualcomm Innovation Center, Inc.");
