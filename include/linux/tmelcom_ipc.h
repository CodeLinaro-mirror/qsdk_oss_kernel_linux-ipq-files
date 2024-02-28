/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_IPC_H_
#define _TMELCOM_IPC_H_

#define TMEL_MAX_FUSE_ADDR_SIZE 8
#define SECBOOT_SW_ID_ROOTPD 0xD

struct tmel_msg_param_type_buf_in {
	u32 buf;
	u32 buf_len;
};

struct tmel_msg_param_type_buf_out {
	u32 buf;
	u32 buf_len;
	u32 out_buf_len;
};

struct tmel_msg_param_type_buf_in_out {
	u32 buf;
	u32 buf_len;
	u32 out_buf_len;
};

struct tmel_fuse_payload {
	u32 fuse_addr;
	u32 lsb_val;
	u32 msb_val;
} __packed;

struct tmel_fuse_read_multiple_msg {
	u32 status;
	struct tmel_msg_param_type_buf_in_out fuse_read_data;
} __packed;

struct tmel_qwes_init_att_msg {
	u32 status;
	struct tmel_msg_param_type_buf_out rsp;
} __packed;

struct tmel_qwes_device_att_msg {
	u32 status;
	struct tmel_msg_param_type_buf_in req;
	struct tmel_msg_param_type_buf_in ext_claim;
	struct tmel_msg_param_type_buf_out rsp;
} __packed;

struct tmel_qwes_device_prov_msg {
	u32 status;
	struct tmel_msg_param_type_buf_in req;
	struct tmel_msg_param_type_buf_out rsp;
} __packed;

struct tmel_secboot_sec_auth_req {
	u32 sw_id;
	struct tmel_msg_param_type_buf_in elf_buf;
	struct tmel_msg_param_type_buf_in region_list;
	u32 relocate;
} __packed;

struct tmel_secboot_sec_auth_resp {
	u32 first_seg_addr;
	u32 first_seg_len;
	u32 entry_addr;
	u32 extended_error;
	u32 status;
} __packed;

struct tmel_secboot_sec_auth {
	struct tmel_secboot_sec_auth_req req;
	struct tmel_secboot_sec_auth_resp resp;
} __packed;

struct tmel_secboot_teardown_req {
	u32 sw_id;
	u32 secondary_sw_id;
} __packed;

struct tmel_secboot_teardown_resp {
	u32 status;
} __packed;

struct tmel_secboot_teardown {
	struct tmel_secboot_teardown_req req;
	struct tmel_secboot_teardown_resp resp;
} __packed;

#ifdef CONFIG_QCOM_TMELCOM
int tmelcom_init_attestation(u32 *key_buf, u32 key_buf_len, u32 *key_buf_size);
int tmelcom_qwes_getattestation_report(u32 *req_buf, u32 req_buf_len,
				       u32 *extclaim_buf, u32 extclaim_buf_len,
				       u32 *resp_buf, u32 resp_buf_len,
				       u32 *resp_buf_size);
int tmelcom_qwes_device_provision(u32 *req_buf, u32 req_buf_len, u32 *resp_buf,
				  u32 resp_buf_len, u32 *resp_buf_size);
int tmelcom_fuse_list_read(struct tmel_fuse_payload *fuse, size_t size);
int tmelcom_secboot_sec_auth(u32 sw_id, void *metadata, size_t size);
int tmelcom_secboot_teardown(u32 sw_id, u32 secondary_sw_id);
#else
static inline int tmelcom_fuse_list_read(struct tmel_fuse_payload *fuse,
					 size_t size)
{
	return 0;
}

static inline int tmelcom_secboot_sec_auth(u32 sw_id, void *metadata,
					   size_t size)
{
	return 0;
}

static inline int tmelcom_secboot_teardown(u32 sw_id, u32 secondary_sw_id)
{
	return 0;
}

static inline int tmelcom_init_attestation(u32 *key_buf, u32 key_buf_len,
					   u32 *key_buf_size)
{
	return 0;
}
static inline int tmelcom_qwes_getattestation_report(u32 *req_buf,
						     u32 req_buf_len,
						     u32 *extclaim_buf,
						     u32 extclaim_buf_len,
						     u32 *resp_buf,
						     u32 resp_buf_len,
						     u32 *resp_buf_size)
{
	return 0;
}
static inline int tmelcom_qwes_device_provision(u32 *req_buf, u32 req_buf_len,
						u32 *resp_buf, u32 resp_buf_len,
						u32 *resp_buf_size)
{
	return 0;
}
#endif /* CONFIG_QCOM_TMELCOM */
#endif /* _TMELCOM_IPC_H_ */
