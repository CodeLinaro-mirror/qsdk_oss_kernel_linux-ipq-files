// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#ifndef __QCOM_SECCRYPT_OPTEE_H
#define __QCOM_SECCRYPT_OPTEE_H

#include <linux/types.h>

/* SecCrypt OP-TEE TA UUID - This should match the TA UUID in OP-TEE OS */
#define PTA_SEC_STORAGE_AES_UUID UUID_INIT(0x1e64fceb, 0x66b5, 0x41d5, 0xba, 0x0a, 0x1f, 0x48, 0x7a, 0x59, 0xa8, 0xa0)

/* SecCrypt OP-TEE Commands */
#define SECCRYPT_TA_CMD_SET_KEY		0x0
#define SECCRYPT_TA_CMD_CRYPT		0x1
#define SECCRYPT_TA_CMD_CLEAR_KEY	0x2

/* OP-TEE Success Code */
#define TEEC_SUCCESS	0x00000000

/* AES HUK seed types */
#define AES_CRBK		0
#define AES_OEM_PRODSEED	1

/* AES HUK algorithm modes (values are PTA ABI) */
enum aes_huk_mode {
	AES_MODE_ECB = 0,
	AES_MODE_CBC = 1,
	AES_MODE_CTR = 2,
	AES_MODE_XTS = 3,
	AES_MODE_MAX,		/* keep last */
};

/* Max length (bytes) for each SW context buffer */
#define AES_SW_CONTEXT_MAX_LEN	128

/* AES IV/tweak size in bytes for CBC/XTS crypt operations */
#define AES_HUK_IV_SIZE		16

/* CRYPT data length limits (bytes), must also be a multiple of 16 */
#define AES_HUK_DATA_MIN_LEN	16
#define AES_HUK_DATA_MAX_LEN	4096

/* SecCrypt Key Configuration Structure */
struct aes_set_key_req {
	u32 seed_type;
	u32 key_size;
	u32 algo_mode;
} __packed;

/* SecCrypt Command Structure for TEE backend*/
struct secure_nand_aes_cmd_tee {
	u64 direction;
	u64 mode;
	void *iv_buf_virt;      /* Virtual address for IV buffer */
	u64 *iv_buf;
	u64 iv_size;
	void *req_buf_virt;     /* Virtual address for request buffer */
	u64 *req_buf;
	u64 reqlen;
	void *rsp_buf_virt;     /* Virtual address for response buffer */
	u64 *rsp_buf;
	u64 rsplen;
};

/* SecCrypt Command Structure for SCM backend */
struct secure_nand_aes_cmd_scm {
	u64 direction;
	u64 mode;
	u64 *iv_buf;
	u64 iv_size;
	u64 *req_buf;
	u64 reqlen;
	u64 *rsp_buf;
	u64 rsplen;
};
#endif /* __QCOM_SECCRYPT_OPTEE_H */
