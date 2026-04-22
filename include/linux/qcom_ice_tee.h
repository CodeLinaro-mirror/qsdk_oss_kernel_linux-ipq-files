// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#ifndef __QCOM_ICE_OPTEE_H
#define __QCOM_ICE_OPTEE_H

#include <linux/types.h>

/* ICE OP-TEE TA UUID - This should match the TA UUID in OP-TEE OS */
#define ICE_TA_UUID UUID_INIT(0x29e87b9e, 0x012a, 0x4878, 0xa1, 0xe1, 0xa1, 0xb9, 0x0a, 0x21, 0x5b, 0x16)

/* ICE OP-TEE Commands */
#define ICE_TA_CMD_SET_KEY		0x3
#define ICE_TA_CMD_INVALIDATE_KEY	0x2
#define ICE_TA_CMD_CONFIG_HW_KEY	0x1
#define ICE_TA_CMD_SET_CONTEXT		0x0

#define QCOM_OPTEE_ICE_CIPHER_AES_128_XTS	0
#define QCOM_OPTEE_ICE_CIPHER_AES_128_CBC	1
#define QCOM_OPTEE_ICE_CIPHER_AES_128_ECB 2
#define QCOM_OPTEE_ICE_CIPHER_AES_256_XTS 3
#define QCOM_OPTEE_ICE_CIPHER_AES_256_CBC 4
#define QCOM_OPTEE_ICE_CIPHER_AES_256_ECB 5


/* Hardware Key Configuration Constants */

#define ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_XTS	0x3
#define ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_ECB	0x0
#define ICE_OPTEE_CRYPTO_KEY_SIZE_HW_128	0x0
#define ICE_OPTEE_CRYPTO_KEY_SIZE_HW_256	0x2
#define ICE_OPTEE_CRYPTO_USE_KEY0_HW_KEY	0x0

#define TEEC_SUCCESS	0x00000000
#define OEM_SEED_TYPE	0x1

/* Hardware Key Configuration Structure */
struct ice_hw_key_config {
	u32 index;
	u8 key_size;
	u8 algo_mode;
	u8 key_mode;
};

/* ICE Context Configuration Structure */
struct ice_context_config {
	u32 seed_type;
	u8 key_size;
	u8 algo_mode;
	u32 data_ctxt_len;
	u32 salt_ctxt_len;
};

/* Function declarations */
int qcom_ice_optee_set_key(u32 slot, const u8 *key, u32 key_len,
			   u32 cipher, u32 data_unit_size);
int qcom_ice_optee_invalidate_key(u32 slot);
int qcom_ice_optee_config_hwkey(u32 cipher, u32 key_size);
int qcom_ice_optee_set_context(u32 type, u8 key_size, u8 algo_mode,
			       u8 *data_ctxt, u32 data_ctxt_len,
			       u8 *salt_ctxt, u32 salt_ctxt_len);
int qcom_ice_optee_qce_setkey(void *conf_buf, u32 conf_size);
#endif /* __QCOM_ICE_OPTEE_H */

