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

/*
 * PTA SET_KEY contract: key_size is per key (16/32); CRBK takes no SW
 * context; OEM_PRODSEED needs ctx1, plus ctx2 only (and always) for XTS.
 */
static int qcom_seccrypt_tee_check_key_req(struct device *dev,
					   const struct aes_set_key_req *cfg,
					   u32 ctx1_size, u32 ctx2_size)
{
	if (cfg->key_size != 16 && cfg->key_size != 32) {
		dev_err(dev, "Invalid key_size %u (expected 16 or 32)\n",
			cfg->key_size);
		return -EINVAL;
	}

	if (cfg->algo_mode >= AES_MODE_MAX) {
		dev_err(dev, "Invalid algo_mode %u\n", cfg->algo_mode);
		return -EINVAL;
	}

	switch (cfg->seed_type) {
	case AES_CRBK:
		if (ctx1_size || ctx2_size) {
			dev_err(dev, "CRBK seed does not accept SW context\n");
			return -EINVAL;
		}
		break;
	case AES_OEM_PRODSEED:
		if (!ctx1_size) {
			dev_err(dev, "OEM_PRODSEED requires SW context 1\n");
			return -EINVAL;
		}
		if (cfg->algo_mode == AES_MODE_XTS && !ctx2_size) {
			dev_err(dev, "OEM_PRODSEED XTS requires SW context 2\n");
			return -EINVAL;
		}
		if (cfg->algo_mode != AES_MODE_XTS && ctx2_size) {
			dev_err(dev, "SW context 2 is only valid for XTS\n");
			return -EINVAL;
		}
		break;
	default:
		dev_err(dev, "Invalid seed_type %u\n", cfg->seed_type);
		return -EINVAL;
	}

	return 0;
}

/**
 * qcom_seccrypt_tee_set_key() - Set encryption key via OP-TEE
 * @dev: Device pointer
 * @conf_buf: Configuration buffer containing key parameters
 * @conf_size: Size of configuration buffer
 * @ctx1_buf: Optional SW context 1 buffer (data key context)
 * @ctx1_size: Size of ctx1_buf, or 0 if unused
 * @ctx2_buf: Optional SW context 2 buffer (salt/tweak key context, AES-XTS only)
 * @ctx2_size: Size of ctx2_buf, or 0 if unused
 *
 * This function calls the OP-TEE TA to set the encryption key
 * in the secure world.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int qcom_seccrypt_tee_set_key(struct device *dev, void *conf_buf, u32 conf_size,
				     void *ctx1_buf, u32 ctx1_size,
				     void *ctx2_buf, u32 ctx2_size)
{
	struct qcom_seccrypt_tee_private *priv = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct tee_shm *config_shm, *ctx1_shm = NULL, *ctx2_shm = NULL;
	void *config_buf, *ctx_buf;
	int ret;
	if (!conf_buf || conf_size != sizeof(struct aes_set_key_req)) {
		dev_err(dev, "Invalid key configuration size: %u (expected %zu)\n",
			conf_size, sizeof(struct aes_set_key_req));
		return -EINVAL;
	}
	if ((ctx1_buf && !ctx1_size) || (!ctx1_buf && ctx1_size) ||
	    (ctx2_buf && !ctx2_size) || (!ctx2_buf && ctx2_size)) {
		dev_err(dev, "Inconsistent SW context pointer/length pair\n");
		return -EINVAL;
	}
	if (ctx1_size > AES_SW_CONTEXT_MAX_LEN || ctx2_size > AES_SW_CONTEXT_MAX_LEN) {
		dev_err(dev, "SW context size exceeds maximum (%u)\n",
			AES_SW_CONTEXT_MAX_LEN);
		return -EINVAL;
	}
	ret = qcom_seccrypt_tee_check_key_req(dev, conf_buf, ctx1_size, ctx2_size);
	if (ret)
		return ret;
	/* Allocate shared memory for configuration */
	config_shm = tee_shm_alloc_kernel_buf(priv->ctx, conf_size);
	if (IS_ERR(config_shm)) {
		dev_err(dev, "Failed to allocate shared memory for key config\n");
		return PTR_ERR(config_shm);
	}
	config_buf = tee_shm_get_va(config_shm, 0);
	if (IS_ERR(config_buf)) {
		ret = PTR_ERR(config_buf);
		goto free_config_shm;
	}
	/* Copy configuration to shared memory */
	memcpy(config_buf, conf_buf, conf_size);

	/* Allocate shared memory for SW context 1, if provided */
	if (ctx1_buf && ctx1_size > 0) {
		ctx1_shm = tee_shm_alloc_kernel_buf(priv->ctx, ctx1_size);
		if (IS_ERR(ctx1_shm)) {
			dev_err(dev, "Failed to allocate shared memory for SW context 1\n");
			ret = PTR_ERR(ctx1_shm);
			goto free_config_shm;
		}
		ctx_buf = tee_shm_get_va(ctx1_shm, 0);
		if (IS_ERR(ctx_buf)) {
			ret = PTR_ERR(ctx_buf);
			goto free_ctx1_shm;
		}
		memcpy(ctx_buf, ctx1_buf, ctx1_size);
	}

	/* Allocate shared memory for SW context 2, if provided */
	if (ctx2_buf && ctx2_size > 0) {
		ctx2_shm = tee_shm_alloc_kernel_buf(priv->ctx, ctx2_size);
		if (IS_ERR(ctx2_shm)) {
			dev_err(dev, "Failed to allocate shared memory for SW context 2\n");
			ret = PTR_ERR(ctx2_shm);
			goto free_ctx1_shm;
		}
		ctx_buf = tee_shm_get_va(ctx2_shm, 0);
		if (IS_ERR(ctx_buf)) {
			ret = PTR_ERR(ctx_buf);
			goto free_ctx2_shm;
		}
		memcpy(ctx_buf, ctx2_buf, ctx2_size);
	}

	/* Prepare invoke arguments */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));
	inv_arg.func = SECCRYPT_TA_CMD_SET_KEY;
	inv_arg.session = priv->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[0].u.memref.shm = config_shm;
	param[0].u.memref.size = conf_size;
	param[0].u.memref.shm_offs = 0;

	if (ctx1_shm) {
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
		param[1].u.memref.shm = ctx1_shm;
		param[1].u.memref.size = ctx1_size;
		param[1].u.memref.shm_offs = 0;
	} else {
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
	}

	if (ctx2_shm) {
		param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
		param[2].u.memref.shm = ctx2_shm;
		param[2].u.memref.size = ctx2_size;
		param[2].u.memref.shm_offs = 0;
	} else {
		param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
	}

	param[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;

	/* Invoke TA command: Key derivation */
	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Key derivation failed: ret=%d, ta_ret=0x%x\n",
			ret, inv_arg.ret);
		ret = -EINVAL;
	} else {
		dev_dbg(dev, "Key derivation success.\n");
	}

free_ctx2_shm:
	if (ctx2_shm)
		tee_shm_free(ctx2_shm);
free_ctx1_shm:
	if (ctx1_shm)
		tee_shm_free(ctx1_shm);
free_config_shm:
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
	bool is_ecb, has_iv;
	int ret;

	if (!cmd_buf || cmd_size != sizeof(struct secure_nand_aes_cmd_tee)) {
		dev_err(dev, "Invalid command size: %u (expected %zu)\n",
			cmd_size, sizeof(struct secure_nand_aes_cmd_tee));
		return -EINVAL;
	}

	if (!cptr_tee->req_buf_virt || !cptr_tee->rsp_buf_virt) {
		dev_err(dev, "Invalid request/response buffer\n");
		return -EINVAL;
	}

	if (!cptr_tee->reqlen || cptr_tee->reqlen != cptr_tee->rsplen) {
		dev_err(dev, "Invalid request/response length: reqlen=%llu rsplen=%llu\n",
			cptr_tee->reqlen, cptr_tee->rsplen);
		return -EINVAL;
	}

	if (cptr_tee->reqlen < AES_HUK_DATA_MIN_LEN ||
	    cptr_tee->reqlen > AES_HUK_DATA_MAX_LEN ||
	    cptr_tee->reqlen % AES_HUK_IV_SIZE) {
		dev_err(dev, "Invalid data length %llu (16..4096, multiple of 16)\n",
			cptr_tee->reqlen);
		return -EINVAL;
	}

	if (cptr_tee->direction > 1 || cptr_tee->mode >= AES_MODE_MAX) {
		dev_err(dev, "Invalid direction %llu / mode %llu\n",
			cptr_tee->direction, cptr_tee->mode);
		return -EINVAL;
	}

	if ((cptr_tee->iv_buf_virt && cptr_tee->iv_size != AES_HUK_IV_SIZE) ||
	    (!cptr_tee->iv_buf_virt && cptr_tee->iv_size)) {
		dev_err(dev, "Invalid IV buffer/length pair\n");
		return -EINVAL;
	}

	/* ECB takes no IV; CBC/CTR/XTS need an explicit one (no zero-IV fallback) */
	is_ecb = cptr_tee->mode == AES_MODE_ECB;
	has_iv = cptr_tee->iv_buf_virt;
	if (is_ecb == has_iv) {
		dev_err(dev, "IV %s for mode %llu\n",
			cptr_tee->iv_buf_virt ? "not allowed" : "required",
			cptr_tee->mode);
		return -EINVAL;
	}

	/* Register physical memory as shared memory for IV buffer */
	if (cptr_tee->iv_buf_virt && cptr_tee->iv_size > 0) {
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
	/* PTA expects 4 params, all NONE (memset above = TYPE_NONE) */
	inv_arg.num_params = 4;
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

/*
 * Built-in: register via device_initcall_sync, after OP-TEE core's own
 * device_initcall has enumerated the SecCrypt PTA on the TEE bus (so
 * probe() fires deterministically), but before qcom_seccrypt_scm's
 * late_initcall registration — the core layer only keeps the first
 * backend to register, so TEE must win that race over SCM whenever
 * OP-TEE is actually available. When built as a module this maps to
 * module_init().
 */
device_initcall_sync(qcom_seccrypt_tee_init);
module_exit(qcom_seccrypt_tee_exit);

MODULE_DESCRIPTION("Qualcomm SecCrypt TEE Backend");
MODULE_LICENSE("GPL");
