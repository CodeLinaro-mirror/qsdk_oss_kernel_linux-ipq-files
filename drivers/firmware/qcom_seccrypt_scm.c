// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/seccrypt_ops.h>
#include <linux/qcom_seccrypt_tee.h>
#include <linux/of.h>

#define DRIVER_NAME "qcom-seccrypt-scm"

/**
 * qcom_seccrypt_scm_available() - Check if SCM backend is available
 * @dev: Device pointer (unused for SCM)
 *
 * Returns: true if SCM interface is available, false otherwise
 */
static bool qcom_seccrypt_scm_available(struct device *dev)
{
	return qcom_scm_is_available();
}

/**
 * qcom_seccrypt_scm_set_key() - Set encryption key via SCM
 * @dev: Device pointer (unused for SCM)
 * @conf_buf: Configuration buffer containing key parameters
 * @conf_size: Size of configuration buffer
 *
 * This function calls the SCM interface to set the encryption key
 * in the secure world (TrustZone).
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_scm_set_key(struct device *dev, void *conf_buf, u32 conf_size)
{
	int ret;

	if (!conf_buf || conf_size == 0) {
		pr_err("%s: Invalid parameters\n", DRIVER_NAME);
		return -EINVAL;
	}

	ret = qti_set_qcekey_sec(conf_buf, conf_size);
	if (ret)
		pr_err("%s: Failed to set key via SCM: %d\n", DRIVER_NAME, ret);

	return ret;
}

/**
 * qcom_seccrypt_scm_crypt() - Perform encryption/decryption via SCM
 * @dev: Device pointer (unused for SCM)
 * @cmd_buf: Command buffer containing crypto parameters
 * @cmd_size: Size of command buffer
 *
 * This function calls the SCM interface to perform encryption/decryption
 * in the secure world (TrustZone).
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_scm_crypt(struct device *dev, void *cmd_buf, u32 cmd_size)
{
	int ret;

	if (!cmd_buf || cmd_size == 0) {
		pr_err("%s: Invalid parameters\n", DRIVER_NAME);
		return -EINVAL;
	}

	ret = qti_sec_crypt(cmd_buf, cmd_size);
	if (ret)
		pr_err("%s: Failed to perform crypto operation via SCM: %d\n",
		       DRIVER_NAME, ret);

	return ret;
}

/**
 * qcom_seccrypt_scm_clear_key() - Clear encryption key via SCM
 * @dev: Device pointer (unused for SCM)
 *
 * This function calls the SCM interface to clear the encryption key
 * from the secure world (TrustZone).
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_scm_clear_key(struct device *dev)
{
	int ret;

	ret = qti_seccrypt_clearkey();
	if (ret)
		pr_err("%s: Failed to clear key via SCM: %d\n", DRIVER_NAME, ret);

	return ret;
}

static struct qcom_seccrypt_ops qcom_seccrypt_ops_scm = {
	.drv_name   = DRIVER_NAME,
	.available  = qcom_seccrypt_scm_available,
	.set_key    = qcom_seccrypt_scm_set_key,
	.crypt      = qcom_seccrypt_scm_crypt,
	.clear_key  = qcom_seccrypt_scm_clear_key,
};

/**
 * qcom_seccrypt_scm_init() - Initialize SecCrypt SCM backend
 *
 * This function registers the SCM backend with the SecCrypt core layer.
 * It uses late_initcall to ensure SCM is initialized first.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init qcom_seccrypt_scm_init(void)
{
	if (!qcom_scm_is_available()) {
		pr_info("%s: SCM interface not available\n", DRIVER_NAME);
		return -ENODEV;
	}

	qcom_seccrypt_ops_scm.dev = NULL;
	qcom_seccrypt_ops_register(&qcom_seccrypt_ops_scm);

	pr_info("%s: SecCrypt SCM backend registered\n", DRIVER_NAME);
	return 0;
}
late_initcall(qcom_seccrypt_scm_init);

MODULE_DESCRIPTION("Qualcomm SecCrypt SCM Backend");
MODULE_LICENSE("GPL");
