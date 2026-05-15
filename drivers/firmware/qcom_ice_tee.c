// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>
#include <linux/slab.h>
#include <soc/qcom/ice.h>
#include <linux/ice_ops.h>
#include <linux/qcom_ice_tee.h>

#define DRIVER_NAME "qcom-ice-tee"

#define AES_128_CBC_KEY_SIZE			16
#define AES_256_CBC_KEY_SIZE			32
#define AES_128_XTS_KEY_SIZE			32
#define AES_256_XTS_KEY_SIZE			64

struct qcom_ice_tee_private {
	struct device *dev;
	struct tee_context *ctx;
	u32 session_id;
};

static int qcom_ice_get_algo_mode_tee(u8 algorithm_id, u8 key_size,
				      u32 *cipher, u32 *key_len, bool use_hwkey)
{
	switch (key_size) {
	case QCOM_ICE_CRYPTO_KEY_SIZE_128:
	case QCOM_ICE_CRYPTO_KEY_SIZE_256:
		break;
	default:
		pr_err("Unhandled crypto key size %d\n", key_size);
		return -EINVAL;
	}

	switch (algorithm_id) {
	case QCOM_ICE_CRYPTO_ALG_AES_XTS:
		if (key_size == QCOM_ICE_CRYPTO_KEY_SIZE_256) {
			*cipher = QCOM_OPTEE_ICE_CIPHER_AES_256_XTS;
			*key_len = AES_256_XTS_KEY_SIZE;
		} else {
			*cipher = QCOM_OPTEE_ICE_CIPHER_AES_128_XTS;
			*key_len = AES_128_XTS_KEY_SIZE;
		}
		break;
	case QCOM_ICE_CRYPTO_ALG_BITLOCKER_AES_CBC:
		if (key_size == QCOM_ICE_CRYPTO_KEY_SIZE_256) {
			*cipher = QCOM_OPTEE_ICE_CIPHER_AES_256_CBC;
			*key_len = AES_256_CBC_KEY_SIZE;
		} else {
			*cipher = QCOM_OPTEE_ICE_CIPHER_AES_128_CBC;
			*key_len = AES_128_CBC_KEY_SIZE;
		}
		break;
	case QCOM_ICE_CRYPTO_ALG_AES_ECB:
		if (!use_hwkey) {
			pr_err("Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
			       algorithm_id, key_size);
			return -EINVAL;
		}
		if (key_size == QCOM_ICE_CRYPTO_KEY_SIZE_256) {
			*cipher = QCOM_OPTEE_ICE_CIPHER_AES_256_ECB;
			*key_len = AES_256_CBC_KEY_SIZE;
		} else {
			*cipher = QCOM_OPTEE_ICE_CIPHER_AES_128_ECB;
			*key_len = AES_128_CBC_KEY_SIZE;
		}
		break;
	default:
		pr_err("Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
		       algorithm_id, key_size);
		return -EINVAL;
	}

	return 0;
}

static bool qcom_ice_tee_available(struct device *dev)
{
	/* TEE availability is checked during probe */
	return true;
}

static int qcom_ice_tee_config_hwkey(struct device *dev,
				     u32 cipher, u32 key_size)
{
	struct qcom_ice_tee_private *priv = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct tee_shm *hwkey_shm;
	struct ice_hw_key_config *hwkey_config;
	int ret;

	pr_info("####func:%s and line no:%d\n", __func__, __LINE__);

	hwkey_shm = tee_shm_alloc_kernel_buf(priv->ctx, sizeof(*hwkey_config));
	if (IS_ERR(hwkey_shm)) {
		dev_err(dev, "Failed to allocate shared memory for HW key config\n");
		return PTR_ERR(hwkey_shm);
	}

	hwkey_config = tee_shm_get_va(hwkey_shm, 0);
	if (IS_ERR(hwkey_config)) {
		ret = PTR_ERR(hwkey_config);
		goto free_shm;
	}

	memset(hwkey_config, 0, sizeof(*hwkey_config));

	switch (cipher) {
	case QCOM_OPTEE_ICE_CIPHER_AES_128_XTS:
		hwkey_config->algo_mode = ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_XTS;
		hwkey_config->key_size = ICE_OPTEE_CRYPTO_KEY_SIZE_HW_128;
		hwkey_config->key_mode = ICE_OPTEE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	case QCOM_OPTEE_ICE_CIPHER_AES_256_XTS:
		hwkey_config->algo_mode = ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_XTS;
		hwkey_config->key_size = ICE_OPTEE_CRYPTO_KEY_SIZE_HW_256;
		hwkey_config->key_mode = ICE_OPTEE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	case QCOM_OPTEE_ICE_CIPHER_AES_128_ECB:
		hwkey_config->algo_mode = ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_ECB;
		hwkey_config->key_size = ICE_OPTEE_CRYPTO_KEY_SIZE_HW_128;
		hwkey_config->key_mode = ICE_OPTEE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	case QCOM_OPTEE_ICE_CIPHER_AES_256_ECB:
		hwkey_config->algo_mode = ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_ECB;
		hwkey_config->key_size = ICE_OPTEE_CRYPTO_KEY_SIZE_HW_256;
		hwkey_config->key_mode = ICE_OPTEE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	default:
		dev_err(dev, "Unhandled cipher for HW Key support; cipher_id=%d\n", cipher);
		ret = -EINVAL;
		goto free_shm;
	}

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));

	inv_arg.func = ICE_TA_CMD_CONFIG_HW_KEY;
	inv_arg.session = priv->session_id;
	inv_arg.num_params = 1;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[0].u.memref.shm = hwkey_shm;
	param[0].u.memref.size = sizeof(*hwkey_config);
	param[0].u.memref.shm_offs = 0;

	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Config HW key failed, ret=0x%x\n", inv_arg.ret);
		ret = -EINVAL;
	} else {
		dev_info(dev, "HW key configured successfully via OP-TEE\n");
		ret = 0;
	}

free_shm:
	tee_shm_free(hwkey_shm);
	return ret;
}

static int qcom_ice_tee_program_key(struct device *dev, u8 algorithm_id,
				    u8 key_size, const u8 *crypto_key,
				    u8 data_unit_size, int slot, bool use_hwkey)
{
	struct qcom_ice_tee_private *priv = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct tee_shm *key_shm = NULL;
	u32 cipher;
	u32 key_len;
	void *key_buf;
	int ret;

	ret = qcom_ice_get_algo_mode_tee(algorithm_id, key_size, &cipher,
					 &key_len, use_hwkey);
	if (ret) {
		dev_err(dev, "Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
			algorithm_id, key_size);
		return ret;
	}

	if (use_hwkey)
		return qcom_ice_tee_config_hwkey(dev, cipher, key_size);

	key_shm = tee_shm_alloc_kernel_buf(priv->ctx, key_len);
	if (IS_ERR(key_shm)) {
		dev_err(dev, "Failed to allocate shared memory for key\n");
		return PTR_ERR(key_shm);
	}

	key_buf = tee_shm_get_va(key_shm, 0);
	if (IS_ERR(key_buf)) {
		ret = PTR_ERR(key_buf);
		goto free_shm;
	}

	memcpy(key_buf, crypto_key, key_len);
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));

	inv_arg.func = ICE_TA_CMD_SET_KEY;
	inv_arg.session = priv->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = slot;

	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[1].u.memref.shm = key_shm;
	param[1].u.memref.size = key_len;
	param[1].u.memref.shm_offs = 0;

	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[2].u.value.a = cipher;

	param[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[3].u.value.a = data_unit_size;

	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Set key failed: ret=%d, ta_ret=0x%x\n",
			ret, inv_arg.ret);
		ret = -EINVAL;
	} else {
		dev_dbg(dev, "Key set successfully for slot %u via OP-TEE\n", slot);
		ret = 0;
	}

free_shm:
	tee_shm_free(key_shm);
	return ret;
}

/* Invalidate Key not required in hwkey flow as ICE programming fully overwrites
 * keyslots, so explicit eviction is not required.
 */

static int qcom_ice_tee_evict_key(struct device *dev, int slot)
{
	return 0;
}

static int qcom_ice_tee_set_context(struct device *dev, u32 type, u8 key_size,
				    u8 algo_mode, u8 *data_ctxt, u32 data_ctxt_len,
				    u8 *salt_ctxt, u32 salt_ctxt_len)
{
	struct qcom_ice_tee_private *priv = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct tee_shm *config_shm, *data_shm = NULL, *salt_shm = NULL;
	struct ice_context_config *config;
	void *data_buf, *salt_buf;
	int ret;

	pr_info("fun:%s with line no:%d in file:%s\n", __func__, __LINE__, __FILE__);
	config_shm = tee_shm_alloc_kernel_buf(priv->ctx, sizeof(*config));
	if (IS_ERR(config_shm)) {
		dev_err(dev, "Failed to allocate shared memory for context config\n");
		return PTR_ERR(config_shm);
	}

	config = tee_shm_get_va(config_shm, 0);
	if (IS_ERR(config)) {
		ret = PTR_ERR(config);
		goto free_config_shm;
	}

	memset(config, 0, sizeof(*config));
	config->seed_type = type;
	config->key_size = key_size;
	config->algo_mode = algo_mode;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(param, 0, sizeof(param));

	inv_arg.func = ICE_TA_CMD_SET_CONTEXT;
	inv_arg.session = priv->session_id;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[0].u.memref.shm = config_shm;
	param[0].u.memref.size = sizeof(*config);
	param[0].u.memref.shm_offs = 0;

	if (type == OEM_SEED_TYPE && data_ctxt && data_ctxt_len > 0) {
		data_shm = tee_shm_alloc_kernel_buf(priv->ctx, data_ctxt_len);
		if (IS_ERR(data_shm)) {
			dev_err(dev, "Failed to allocate shared memory for data context\n");
			ret = PTR_ERR(data_shm);
			goto free_config_shm;
		}

		data_buf = tee_shm_get_va(data_shm, 0);
		if (IS_ERR(data_buf)) {
			ret = PTR_ERR(data_buf);
			goto free_data_shm;
		}

		memcpy(data_buf, data_ctxt, data_ctxt_len);

		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
		param[1].u.memref.shm = data_shm;
		param[1].u.memref.size = data_ctxt_len;
		param[1].u.memref.shm_offs = 0;

		if (algo_mode == ICE_OPTEE_CRYPTO_ALGO_MODE_HW_AES_XTS &&
		    salt_ctxt && salt_ctxt_len > 0) {
			salt_shm = tee_shm_alloc_kernel_buf(priv->ctx, salt_ctxt_len);
			if (IS_ERR(salt_shm)) {
				dev_err(dev, "Failed to allocate shared memory for salt context\n");
				ret = PTR_ERR(salt_shm);
				goto free_data_shm;
			}

			salt_buf = tee_shm_get_va(salt_shm, 0);
			if (IS_ERR(salt_buf)) {
				ret = PTR_ERR(salt_buf);
				goto free_salt_shm;
			}

			memcpy(salt_buf, salt_ctxt, salt_ctxt_len);
			param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
			param[2].u.memref.shm = salt_shm;
			param[2].u.memref.size = salt_ctxt_len;
			param[2].u.memref.shm_offs = 0;
			inv_arg.num_params = 3;
		} else {
			inv_arg.num_params = 2;
		}

	} else {
		inv_arg.num_params = 1;
	}

	ret = tee_client_invoke_func(priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Set context failed, ret=0x%x\n", inv_arg.ret);
		ret = -EINVAL;
	} else {
		dev_info(dev, "Context set successfully via OP-TEE\n");
		ret = 0;
	}

free_salt_shm:
	if (salt_shm)
		tee_shm_free(salt_shm);
free_data_shm:
	if (data_shm)
		tee_shm_free(data_shm);
free_config_shm:
	tee_shm_free(config_shm);
	return ret;
}

static struct qcom_ice_ops qcom_ice_ops_tee = {
	.drv_name     = DRIVER_NAME,
	.available    = qcom_ice_tee_available,
	.program_key  = qcom_ice_tee_program_key,
	.evict_key    = qcom_ice_tee_evict_key,
	.set_context  = qcom_ice_tee_set_context,
};

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return (ver->impl_id == TEE_IMPL_ID_OPTEE) ? 1 : 0;
}

static int qcom_ice_tee_probe(struct device *dev)
{
	struct qcom_ice_tee_private *priv;
	struct tee_client_device *tee_device = to_tee_client_device(dev);
	struct tee_ioctl_open_session_arg sess_arg;
	int ret;

	pr_info("fun:%s with line no:%d in file:%s\n", __func__, __LINE__, __FILE__);
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	priv->ctx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(priv->ctx)) {
		dev_err(dev, "Failed to open OP-TEE context\n");
		return PTR_ERR(priv->ctx);
	}

	memset(&sess_arg, 0, sizeof(sess_arg));
	export_uuid(sess_arg.uuid, &tee_device->id.uuid);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL;
	sess_arg.num_params = 0;

	pr_info("fun:%s with line no:%d in file:%s\n", __func__, __LINE__, __FILE__);
	ret = tee_client_open_session(priv->ctx, &sess_arg, NULL);
	if (ret < 0 || sess_arg.ret != TEEC_SUCCESS) {
		dev_err(dev, "Failed to open TEE session: ret=%d, ta_ret=0x%x\n",
			ret, sess_arg.ret);
		tee_client_close_context(priv->ctx);
		return -EINVAL;
	}

	priv->session_id = sess_arg.session;
	dev_set_drvdata(dev, priv);

	qcom_ice_ops_tee.dev = dev;
	qcom_ice_ops_register(&qcom_ice_ops_tee);

	dev_info(dev, "ICE TEE backend registered\n");
	return 0;
}

static int qcom_ice_tee_remove(struct device *dev)
{
	struct qcom_ice_tee_private *priv = dev_get_drvdata(dev);

	tee_client_close_session(priv->ctx, priv->session_id);
	tee_client_close_context(priv->ctx);

	return 0;
}

static const struct tee_client_device_id qcom_ice_tee_id_table[] = {
	{ICE_TA_UUID},
	{}
};
MODULE_DEVICE_TABLE(tee, qcom_ice_tee_id_table);

static struct tee_client_driver qcom_ice_tee_driver = {
	.id_table = qcom_ice_tee_id_table,
	.driver = {
		.name  = DRIVER_NAME,
		.bus   = &tee_bus_type,
		.probe = qcom_ice_tee_probe,
		.remove = qcom_ice_tee_remove,
	},
};

static int __init qcom_ice_tee_init(void)
{
	return driver_register(&qcom_ice_tee_driver.driver);
}

static void __exit qcom_ice_tee_exit(void)
{
	driver_unregister(&qcom_ice_tee_driver.driver);
}

module_init(qcom_ice_tee_init);
module_exit(qcom_ice_tee_exit);

MODULE_DESCRIPTION("Qualcomm ICE TEE Backend");
MODULE_LICENSE("GPL");


