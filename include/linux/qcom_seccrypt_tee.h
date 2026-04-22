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

/* SecCrypt Key Configuration Structure */
struct sec_config_key_sec {
	u32 keylen;
} __attribute__((packed));

/* SecCrypt Command Structure */
struct secure_nand_aes_cmd {
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

#endif /* __QCOM_SECCRYPT_OPTEE_H */
