// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>
#include <linux/slab.h>
#include <linux/seccrypt_ops.h>
#include <linux/qcom_seccrypt_tee.h>
#define DRIVER_NAME "qcom-seccrypt-tee"

/**
 * struct qcom_seccrypt_tee_private - SecCrypt TEE private data
 * @dev: Device pointer
 * @ctx: TEE context
 * @session_id: TEE session ID
 */
struct qcom_seccrypt_tee_private {
	struct device *dev;
	struct tee_context *ctx;
	u32 session_id;
};

/**
 * qcom_seccrypt_tee_available() - Check if TEE backend is available
 * @dev: Device pointer
 *
 * Returns: true (TEE availability is checked during probe)
 */
static bool qcom_seccrypt_tee_available(struct device *dev)
{
	/* TEE availability is checked during probe */
	return true;
}

/**
 * qcom_seccrypt_tee_set_key() - Set encryption key via OP-TEE
 * @dev: Device pointer
 * @conf_buf: Configuration buffer containing key parameters
 * @conf_size: Size of configuration buffer
 *
 * This function calls the OP-TEE TA to set the encryption key
 * in the secure world.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_tee_set_key(struct device *dev, void *conf_buf, u32 conf_size)
{
	struct qcom_seccrypt_tee_private *priv = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct tee_shm *config_shm;
	void *config_buf;
	int ret;
	if (!conf_buf || conf_size == 0) {
		dev_err(dev, "Invalid parameters\n");
		return -EINVAL;
	}
	/* Allocate shared memory for configuration */
	config_shm = tee_shm_alloc_kernel_buf(priv->ctx, conf_size);
	if (IS_ERR(config_shm)) {
		dev_err(dev, "Failed to allocate shared memory for key config\n");
		return PTR_ERR(config_shm);
	}
	config_buf = tee_shm_get_va(config_shm, 0);
	if (IS_ERR(config_buf)) {
		ret = PTR_ERR(config_buf);
		goto free_shm;
	}
	/* Copy configuration to shared memory */
	memcpy(config_buf, conf_buf, conf_size);
	/* Prepare invoke arguments */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));
	inv_arg.func = SECCRYPT_TA_CMD_SET_KEY;
	inv_arg.session = priv->session_id;
	inv_arg.num_params = 1;
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[0].u.memref.shm = config_shm;
	param[0].u.memref.size = conf_size;
	param[0].u.memref.shm_offs = 0;
	/* Invoke TA command: Key derivation */
	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Key derivation failed: ret=%d, ta_ret=0x%x\n",
			ret, inv_arg.ret);
		ret = -EINVAL;
	}

	dev_info(dev, "Key derivation success.\n");

free_shm:
	tee_shm_free(config_shm);

	return ret;
}

/**
 * qcom_seccrypt_tee_crypt() - Perform encryption/decryption via OP-TEE
 * @dev: Device pointer
 * @cmd_buf: Command buffer containing crypto parameters
 * @cmd_size: Size of command buffer
 *
 * This function calls the OP-TEE TA to perform encryption/decryption
 * in the secure world.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_tee_crypt(struct device *dev, void *cmd_buf, u32 cmd_size)
{
	struct qcom_seccrypt_tee_private *priv = dev_get_drvdata(dev);
	const struct secure_nand_aes_cmd_tee *cptr_tee = (const struct secure_nand_aes_cmd_tee *)cmd_buf;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct tee_shm *iv_shm = NULL, *req_shm = NULL, *rsp_shm = NULL;
	int ret;

	if (!cmd_buf || cmd_size == 0) {
		dev_err(dev, "Invalid parameters\n");
		return -EINVAL;
	}

	/* Register physical memory as shared memory for IV buffer */
	if (cptr_tee->iv_buf && cptr_tee->iv_size > 0) {
		iv_shm = tee_shm_register_kernel_buf(priv->ctx, cptr_tee->iv_buf_virt, cptr_tee->iv_size);
		if (IS_ERR(iv_shm)) {
			dev_err(dev, "Failed to register IV buffer\n");
			return PTR_ERR(iv_shm);
		}
	}

	/* Register physical memory as shared memory for request buffer */
	req_shm = tee_shm_register_kernel_buf(priv->ctx, cptr_tee->req_buf_virt, cptr_tee->reqlen);
	if (IS_ERR(req_shm)) {
		dev_err(dev, "Failed to register request buffer\n");
		ret = PTR_ERR(req_shm);
		goto free_iv_shm;
	}

	/* Register physical memory as shared memory for response buffer */
	rsp_shm = tee_shm_register_kernel_buf(priv->ctx, cptr_tee->rsp_buf_virt, cptr_tee->rsplen);
	if (IS_ERR(rsp_shm)) {
		dev_err(dev, "Failed to register response buffer\n");
		ret = PTR_ERR(rsp_shm);
		goto free_req_shm;
	}

	/* Prepare invoke arguments */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));

	inv_arg.func = SECCRYPT_TA_CMD_CRYPT;
	inv_arg.session = priv->session_id;
	inv_arg.num_params = 4;

	/* Parameter 0: Direction and Mode (as value parameters) */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = (u32)cptr_tee->direction;
	param[0].u.value.b = (u32)cptr_tee->mode;

	/* Parameter 1: IV buffer (as memref if present) */
	if (iv_shm) {
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
		param[1].u.memref.shm = iv_shm;
		param[1].u.memref.size = cptr_tee->iv_size;
		param[1].u.memref.shm_offs = 0;
	} else {
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
	}

	/* Parameter 2: Request buffer (source data as memref) */
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[2].u.memref.shm = req_shm;
	param[2].u.memref.size = cptr_tee->reqlen;
	param[2].u.memref.shm_offs = 0;

	/* Parameter 3: Response buffer (destination data as memref) */
	param[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
	param[3].u.memref.shm = rsp_shm;
	param[3].u.memref.size = cptr_tee->rsplen;
	param[3].u.memref.shm_offs = 0;

	/* Invoke TA command */
	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Crypto operation failed: ret=%d, ta_ret=0x%x\n",
			ret, inv_arg.ret);
		ret = -EINVAL;
	} else {
		dev_dbg_once(dev, "Crypto operation completed successfully via OP-TEE\n");
		ret = 0;
	}

	tee_shm_free(rsp_shm);
free_req_shm:
	tee_shm_free(req_shm);
free_iv_shm:
	if (iv_shm)
		tee_shm_free(iv_shm);

	return ret;
}

/**
 * qcom_seccrypt_tee_clear_key() - Clear encryption key via OP-TEE
 * @dev: Device pointer
 *
 * This function calls the OP-TEE TA to clear the encryption key
 * from the secure world.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_tee_clear_key(struct device *dev)
{
	struct qcom_seccrypt_tee_private *priv = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;
	/* Prepare invoke arguments */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));
	inv_arg.func = SECCRYPT_TA_CMD_CLEAR_KEY;
	inv_arg.session = priv->session_id;
	inv_arg.num_params = 0;
	/* Invoke TA command */
	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Clear key failed: ret=%d, ta_ret=0x%x\n",
			ret, inv_arg.ret);
		return -EINVAL;
	}
	dev_dbg(dev, "Key cleared successfully via OP-TEE\n");
	return 0;
}

static struct qcom_seccrypt_ops qcom_seccrypt_ops_tee = {
	.drv_name   = DRIVER_NAME,
	.available  = qcom_seccrypt_tee_available,
	.set_key    = qcom_seccrypt_tee_set_key,
	.crypt      = qcom_seccrypt_tee_crypt,
	.clear_key  = qcom_seccrypt_tee_clear_key,
};

/**
 * optee_ctx_match() - Match OP-TEE context
 * @ver: TEE version data
 * @data: Match data (unused)
 *
 * Returns: 1 if OP-TEE, 0 otherwise
 */
static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return (ver->impl_id == TEE_IMPL_ID_OPTEE) ? 1 : 0;
}

/**
 * qcom_seccrypt_tee_probe() - Probe SecCrypt TEE driver
 * @dev: Device pointer
 *
 * This function initializes the OP-TEE context and opens a session
 * with the SecCrypt TA.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_tee_probe(struct device *dev)
{
	struct qcom_seccrypt_tee_private *priv;
	struct tee_client_device *tee_device = to_tee_client_device(dev);
	struct tee_ioctl_open_session_arg sess_arg;
	int ret;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	/* Open OP-TEE context */
	priv->ctx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(priv->ctx)) {
		dev_err(dev, "Failed to open OP-TEE context\n");
		return PTR_ERR(priv->ctx);
	}
	/* Open session with SecCrypt TA */
	memset(&sess_arg, 0, sizeof(sess_arg));
	export_uuid(sess_arg.uuid, &tee_device->id.uuid);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL;
	sess_arg.num_params = 0;
	ret = tee_client_open_session(priv->ctx, &sess_arg, NULL);
	if (ret < 0 || sess_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Failed to open TEE session: ret=%d, ta_ret=0x%x\n",
			ret, sess_arg.ret);
		tee_client_close_context(priv->ctx);
		return -EINVAL;
	}
	priv->session_id = sess_arg.session;
	dev_set_drvdata(dev, priv);
	/* Register SecCrypt TEE backend */
	qcom_seccrypt_ops_tee.dev = dev;
	qcom_seccrypt_ops_register(&qcom_seccrypt_ops_tee);
	dev_info(dev, "SecCrypt TEE backend registered\n");
	return 0;
}

/**
 * qcom_seccrypt_tee_remove() - Remove SecCrypt TEE driver
 * @dev: Device pointer
 *
 * This function closes the OP-TEE session and context.
 *
 * Returns: 0 on success
 */
static int qcom_seccrypt_tee_remove(struct device *dev)
{
	struct qcom_seccrypt_tee_private *priv = dev_get_drvdata(dev);
	tee_client_close_session(priv->ctx, priv->session_id);
	tee_client_close_context(priv->ctx);
	dev_info(dev, "SecCrypt TEE backend removed\n");
	return 0;
}

static const struct tee_client_device_id qcom_seccrypt_tee_id_table[] = {
	{PTA_SEC_STORAGE_AES_UUID},
	{}
};

MODULE_DEVICE_TABLE(tee, qcom_seccrypt_tee_id_table);
static struct tee_client_driver qcom_seccrypt_tee_driver = {
	.id_table = qcom_seccrypt_tee_id_table,
	.driver = {
		.name  = DRIVER_NAME,
		.bus   = &tee_bus_type,
		.probe = qcom_seccrypt_tee_probe,
		.remove = qcom_seccrypt_tee_remove,
	},
};

static int __init qcom_seccrypt_tee_init(void)
{
	return driver_register(&qcom_seccrypt_tee_driver.driver);
}

static void __exit qcom_seccrypt_tee_exit(void)
{
	driver_unregister(&qcom_seccrypt_tee_driver.driver);
}

module_init(qcom_seccrypt_tee_init);
module_exit(qcom_seccrypt_tee_exit);

MODULE_DESCRIPTION("Qualcomm SecCrypt TEE Backend");
MODULE_LICENSE("GPL");
