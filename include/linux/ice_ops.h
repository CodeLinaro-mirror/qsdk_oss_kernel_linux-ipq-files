// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#ifndef __QCOM_ICE_OPS_H
#define __QCOM_ICE_OPS_H

#include <linux/types.h>

struct qcom_ice;

/**
 * struct qcom_ice_ops - ICE backend operations
 * @drv_name:        Backend driver name
 * @dev:             Backend device pointer
 * @available:       Check if backend is available
 * @program_key:     Program encryption key to a slot
 * @evict_key:       Evict/invalidate key from a slot
 * @config_hwkey:    Configure hardware key
 * @set_context:     Set encryption context
 */
struct qcom_ice_ops {
	const char *drv_name;
	struct device *dev;

	bool (*available)(struct device *dev);
	int (*program_key)(struct device *dev, u8 algorithm_id, u8 key_size,
			   const u8 *crypto_key, u8 data_unit_size,
			   int slot, bool use_hwkey);
	int (*evict_key)(struct device *dev, int slot);
	int (*config_hwkey)(struct device *dev, u32 cipher, u32 key_size);
	int (*set_context)(struct device *dev, u32 type, u8 key_size,
			   u8 algo_mode, u8 *data_ctxt, u32 data_ctxt_len,
			   u8 *salt_ctxt, u32 salt_ctxt_len);
};

void qcom_ice_ops_register(struct qcom_ice_ops *ops);
bool qcom_ice_ops_available(void);
int qcom_ice_config_hwkey(struct qcom_ice *ice, u32 cipher, u32 key_size);
int qcom_ice_set_context(u32 type, u8 key_size,
			 u8 algo_mode, u8 *data_ctxt, u32 data_ctxt_len,
			 u8 *salt_ctxt, u32 salt_ctxt_len);
int qcom_ice_qce_setkey(struct qcom_ice *ice, void *conf_buf, u32 conf_size);
#endif /* __QCOM_ICE_OPS_H */

