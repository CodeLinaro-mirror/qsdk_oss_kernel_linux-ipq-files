// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/device.h>
#include <linux/seccrypt_ops.h>

static struct qcom_seccrypt_ops *ops_ptr;

/**
 * qcom_seccrypt_ops_available() - Check if SecCrypt backend is available
 *
 * Returns: true if a backend is registered, false otherwise
 */
bool qcom_seccrypt_ops_available(void)
{
	return !!smp_load_acquire(&ops_ptr);
}
EXPORT_SYMBOL_GPL(qcom_seccrypt_ops_available);

/**
 * qcom_seccrypt_ops_register() - Register a SecCrypt backend
 * @ops: Pointer to backend operations structure
 *
 * This function registers a SecCrypt backend (SCM or OP-TEE). Only the first
 * backend to register will be used. Subsequent registrations are ignored.
 */
void qcom_seccrypt_ops_register(struct qcom_seccrypt_ops *ops)
{
	if (!ops_ptr) {
		smp_store_release(&ops_ptr, ops);
		pr_info("SecCrypt: Registered backend: %s\n", ops->drv_name);
	} else {
		pr_warn("SecCrypt: Backend already registered (%s), ignoring %s\n",
			ops_ptr->drv_name, ops->drv_name);
	}
}
EXPORT_SYMBOL_GPL(qcom_seccrypt_ops_register);

/**
 * qcom_seccrypt_set_key() - Set encryption key in secure world
 * @conf_buf: Configuration buffer containing key parameters
 * @conf_size: Size of configuration buffer
 *
 * This function forwards the key setting request to the registered backend.
 *
 * Returns: 0 on success, negative error code on failure
 */
int qcom_seccrypt_set_key(void *conf_buf, u32 conf_size)
{
	if (!ops_ptr) {
		pr_err("SecCrypt: No backend registered\n");
		return -ENODEV;
	}

	if (!ops_ptr->set_key) {
		pr_err("SecCrypt: Backend does not support set_key operation\n");
		return -EOPNOTSUPP;
	}

	return ops_ptr->set_key(ops_ptr->dev, conf_buf, conf_size);
}
EXPORT_SYMBOL_GPL(qcom_seccrypt_set_key);

/**
 * qcom_seccrypt_crypt() - Perform encryption/decryption operation
 * @cmd_buf: Command buffer containing crypto parameters
 * @cmd_size: Size of command buffer
 *
 * This function forwards the crypto operation request to the registered backend.
 *
 * Returns: 0 on success, negative error code on failure
 */
int qcom_seccrypt_crypt(void *cmd_buf, u32 cmd_size)
{
	if (!ops_ptr) {
		pr_err("SecCrypt: No backend registered\n");
		return -ENODEV;
	}

	if (!ops_ptr->crypt) {
		pr_err("SecCrypt: Backend does not support crypt operation\n");
		return -EOPNOTSUPP;
	}

	return ops_ptr->crypt(ops_ptr->dev, cmd_buf, cmd_size);
}
EXPORT_SYMBOL_GPL(qcom_seccrypt_crypt);

/**
 * qcom_seccrypt_clear_key() - Clear encryption key from secure world
 *
 * This function forwards the key clearing request to the registered backend.
 *
 * Returns: 0 on success, negative error code on failure
 */
int qcom_seccrypt_clear_key(void)
{
	if (!ops_ptr) {
		pr_err("SecCrypt: No backend registered\n");
		return -ENODEV;
	}

	if (!ops_ptr->clear_key) {
		pr_err("SecCrypt: Backend does not support clear_key operation\n");
		return -EOPNOTSUPP;
	}

	return ops_ptr->clear_key(ops_ptr->dev);
}
EXPORT_SYMBOL_GPL(qcom_seccrypt_clear_key);

MODULE_DESCRIPTION("Qualcomm SecCrypt Core Layer");
MODULE_LICENSE("GPL");
