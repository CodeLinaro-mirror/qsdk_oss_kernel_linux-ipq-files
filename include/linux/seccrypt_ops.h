// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#ifndef __SECCRYPT_OPS_H
#define __SECCRYPT_OPS_H

#include <linux/types.h>

/**
 * struct qcom_seccrypt_ops - SecCrypt backend operations
 * @drv_name:           Backend driver name (e.g., "qcom-seccrypt-scm", "qcom-seccrypt-tee")
 * @dev:                Backend device pointer
 * @available:          Check if backend is available
 * @set_key:            Set encryption key in secure world
 * @crypt:              Perform encryption/decryption operation
 * @clear_key:          Clear encryption key from secure world
 *
 * This structure defines the operations that SecCrypt backends (SCM or OP-TEE)
 * must implement to provide secure crypto functionality.
 */
struct qcom_seccrypt_ops {
	const char *drv_name;
	struct device *dev;

	bool (*available)(struct device *dev);
	int (*set_key)(struct device *dev, void *conf_buf, u32 conf_size);
	int (*crypt)(struct device *dev, void *cmd_buf, u32 cmd_size);
	int (*clear_key)(struct device *dev);
};

/* Core registration functions */
void qcom_seccrypt_ops_register(struct qcom_seccrypt_ops *ops);
bool qcom_seccrypt_ops_available(void);

/* Wrapper functions for SecCrypt operations */
int qcom_seccrypt_set_key(void *conf_buf, u32 conf_size);
int qcom_seccrypt_crypt(void *cmd_buf, u32 cmd_size);
int qcom_seccrypt_clear_key(void);
const char *qcom_seccrypt_get_backend_name(void);

#endif /* __SECCRYPT_OPS_H */
