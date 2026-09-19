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
 * @ctx1_buf: Must be NULL (no SCM equivalent)
 * @ctx1_size: Must be 0
 * @ctx2_buf: Must be NULL (no SCM equivalent)
 * @ctx2_size: Must be 0
 *
 * This function calls the SCM interface to set the encryption key
 * in the secure world (TrustZone). Requests needing SW context or a
 * non-CRBK seed are rejected rather than silently dropped.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_scm_set_key(struct device *dev, void *conf_buf, u32 conf_size,
				     void *ctx1_buf, u32 ctx1_size,
				     void *ctx2_buf, u32 ctx2_size)
{
	const struct aes_set_key_req *req = conf_buf;
	u32 *keylen;
	int ret;

	if (!conf_buf || conf_size != sizeof(struct aes_set_key_req)) {
		pr_err("%s: Invalid parameters\n", DRIVER_NAME);
		return -EINVAL;
	}

	/* TZ only supports the fixed CRBK key, without SW context or XTS */
	if (req->seed_type != AES_CRBK || ctx1_buf || ctx1_size ||
	    ctx2_buf || ctx2_size || req->algo_mode == AES_MODE_XTS) {
		pr_err("%s: seed_type %u / algo_mode %u / SW context not supported\n",
		       DRIVER_NAME, req->seed_type, req->algo_mode);
		return -EOPNOTSUPP;
	}

	if (req->key_size != 16 && req->key_size != 32) {
		pr_err("%s: Invalid key_size %u\n", DRIVER_NAME, req->key_size);
		return -EINVAL;
	}

	/* Legacy TZ wire format is a single u32 keylen; must be DMA-able */
	keylen = kmalloc(sizeof(*keylen), GFP_KERNEL);
	if (!keylen)
		return -ENOMEM;
	*keylen = req->key_size;

	ret = qti_set_qcekey_sec(keylen, sizeof(*keylen));
	if (ret)
		pr_err("%s: Failed to set key via SCM: %d\n", DRIVER_NAME, ret);

	kfree(keylen);
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
