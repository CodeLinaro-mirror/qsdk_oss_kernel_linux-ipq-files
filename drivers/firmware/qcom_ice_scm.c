// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <soc/qcom/ice.h>
#include <linux/ice_ops.h>
#include <linux/of.h>

#define DRIVER_NAME "qcom-ice-scm"

#define AES_128_CBC_KEY_SIZE			16
#define AES_256_CBC_KEY_SIZE			32
#define AES_128_XTS_KEY_SIZE			32
#define AES_256_XTS_KEY_SIZE			64

struct qcom_ice_scm_private {
	struct device *dev;
};

static int qcom_ice_get_algo_mode(u8 algorithm_id, u8 key_size,
				  enum qcom_scm_ice_cipher *cipher,
				  u32 *key_len, bool use_hwkey)
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
			*cipher = QCOM_SCM_ICE_CIPHER_AES_256_XTS;
			*key_len = AES_256_XTS_KEY_SIZE;
		} else {
			*cipher = QCOM_SCM_ICE_CIPHER_AES_128_XTS;
			*key_len = AES_128_XTS_KEY_SIZE;
		}
		break;
	case QCOM_ICE_CRYPTO_ALG_BITLOCKER_AES_CBC:
		if (key_size == QCOM_ICE_CRYPTO_KEY_SIZE_256) {
			*cipher = QCOM_SCM_ICE_CIPHER_AES_256_CBC;
			*key_len = AES_256_CBC_KEY_SIZE;
		} else {
			*cipher = QCOM_SCM_ICE_CIPHER_AES_128_CBC;
			*key_len = AES_128_CBC_KEY_SIZE;
		}
		break;
	case QCOM_ICE_CRYPTO_ALG_AES_ECB:
		/* ECB mode only supports HW key slot */
		if (!use_hwkey) {
			pr_err("Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
			       algorithm_id, key_size);
			return -EINVAL;
		}
		if (key_size == QCOM_ICE_CRYPTO_KEY_SIZE_256) {
			*cipher = QCOM_SCM_ICE_CIPHER_AES_256_ECB;
			*key_len = AES_256_CBC_KEY_SIZE;
		} else {
			*cipher = QCOM_SCM_ICE_CIPHER_AES_128_ECB;
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

int qcom_config_sec_ice(void *buf, int size)
{
	return 	__qcom_config_sec_ice(buf, size);
}
EXPORT_SYMBOL_GPL(qcom_config_sec_ice);

/**
 * qcom_scm_ice_available() - Is the ICE key programming interface available?
 *
 * Return: true iff the SCM calls wrapped by qcom_scm_ice_invalidate_key() and
 *         qcom_scm_ice_set_key() are available.
 */

bool qcom_scm_ice_available(void)
{
	return __qcom_scm_ice_available();
}
EXPORT_SYMBOL_GPL(qcom_scm_ice_available);

/**
 * qcom_scm_ice_hwkey_available() - Is the ICE HW key programming
 *                                  interface available?
 *
 * Return: true if the SCM calls wrapped by qcom_config_sec_ice() are available.
 */

bool qcom_scm_ice_hwkey_available(void)
{
	return __qcom_scm_ice_hwkey_available();
}
EXPORT_SYMBOL_GPL(qcom_scm_ice_hwkey_available);

static bool qcom_ice_scm_available(struct device *dev)
{
	return qcom_scm_is_available() &&
		qcom_scm_ice_available() &&
		qcom_scm_ice_hwkey_available();
}

static int qcom_ice_scm_config_hwkey(struct device *dev,
				     u32 cipher, u32 key_size)
{
	struct ice_config_sec *ice_settings;
	int ret;

	ice_settings = kzalloc(sizeof(*ice_settings), GFP_KERNEL);
	if (!ice_settings)
		return -ENOMEM;

	switch (cipher) {
	case QCOM_SCM_ICE_CIPHER_AES_128_XTS:
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_HW_AES_XTS;
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_HW_128;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	case QCOM_SCM_ICE_CIPHER_AES_256_XTS:
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_HW_AES_XTS;
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_HW_256;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	case QCOM_SCM_ICE_CIPHER_AES_128_ECB:
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_HW_AES_ECB;
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_HW_128;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	case QCOM_SCM_ICE_CIPHER_AES_256_ECB:
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_HW_AES_ECB;
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_HW_256;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
		break;
	default:
		dev_err(dev, "Unhandled cipher for HW Key support; cipher_id=%d\n", cipher);
		kfree(ice_settings);
		return -EINVAL;
	}

	ret = qcom_config_sec_ice(ice_settings, sizeof(*ice_settings));
	kfree(ice_settings);
	if (ret)
		dev_err(dev, "Failed to program ICE HW key: %d\n", ret);
	return ret;
}

/**
 * qcom_scm_ice_set_key() - Set an inline encryption key
 * @index: the keyslot into which to set the key
 * @key: the key to program
 * @key_size: the size of the key in bytes
 * @cipher: the encryption algorithm the key is for
 * @data_unit_size: the encryption data unit size, i.e. the size of each
 *                  individual plaintext and ciphertext.  Given in 512-byte
 *                  units, e.g. 1 = 512 bytes, 8 = 4096 bytes, etc.
 *
 * Program a key into a keyslot of Qualcomm ICE (Inline Crypto Engine), where it
 * can then be used to encrypt/decrypt UFS or eMMC I/O requests inline.
 *
 * The UFSHCI and eMMC standards define a standard way to do this, but it
 * doesn't work on these SoCs; only this SCM call does.
 *
 * It is assumed that the SoC has only one ICE instance being used, as this SCM
 * call doesn't specify which ICE instance the keyslot belongs to.
 *
 * Return: 0 on success; -errno on failure.
 */

int qcom_scm_ice_set_key(u32 index, const u8 *key, u32 key_size,
			 enum qcom_scm_ice_cipher cipher, u32 data_unit_size)
{
	return __qcom_scm_ice_set_key(index, key, key_size,
			       cipher, data_unit_size);
}
EXPORT_SYMBOL_GPL(qcom_scm_ice_set_key);

static int qcom_ice_scm_program_key(struct device *dev,
				    u8 algorithm_id, u8 key_size,
				    const u8 *crypto_key, u8 data_unit_size,
				    int slot, bool use_hwkey)
{
	enum qcom_scm_ice_cipher cipher;
	union {
		u8 bytes[AES_256_XTS_KEY_SIZE];
		u32 words[AES_256_XTS_KEY_SIZE / sizeof(u32)];
	} key;
	u32 key_len;
	int i, err;

	err = qcom_ice_get_algo_mode(algorithm_id, key_size, &cipher,
				     &key_len, use_hwkey);
	if (err) {
		dev_err(dev, "Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
			algorithm_id, key_size);
		return err;
	}

	if (use_hwkey)
		return qcom_ice_scm_config_hwkey(dev, cipher, key_size);

	memcpy(key.bytes, crypto_key, key_len);

	/* The SCM call requires that the key words are encoded in big endian */
	for (i = 0; i < ARRAY_SIZE(key.words); i++)
		__cpu_to_be32s(&key.words[i]);

	err = qcom_scm_ice_set_key(slot, key.bytes, key_len, cipher,
				   data_unit_size);

	memzero_explicit(&key, sizeof(key));

	return err;
}

int qcom_context_ice_sec(u32 type, u8 key_size,
			 u8 algo_mode, u8 *data_ctxt, u32 data_ctxt_len,
			 u8 *salt_ctxt, u32 salt_ctxt_len)
{
	return __qcom_context_ice_sec(type, key_size,
			algo_mode, data_ctxt, data_ctxt_len,
			salt_ctxt, salt_ctxt_len);
}
EXPORT_SYMBOL_GPL(qcom_context_ice_sec);

static int qcom_ice_scm_set_context(struct device *dev,
				    u32 type, u8 key_size, u8 algo_mode,
				    u8 *data_ctxt, u32 data_ctxt_len,
				    u8 *salt_ctxt, u32 salt_ctxt_len)
{
	return qcom_context_ice_sec(type, key_size, algo_mode,
				    data_ctxt, data_ctxt_len,
				    salt_ctxt, salt_ctxt_len);
}

/**
 * qcom_scm_ice_invalidate_key() - Invalidate an inline encryption key
 * @index: the keyslot to invalidate
 *
 * The UFSHCI and eMMC standards define a standard way to do this, but it
 * doesn't work on these SoCs; only this SCM call does.
 *
 * It is assumed that the SoC has only one ICE instance being used, as this SCM
 * call doesn't specify which ICE instance the keyslot belongs to.
 *
 * Return: 0 on success; -errno on failure.
 */

int qcom_scm_ice_invalidate_key(u32 index)
{
	return __qcom_scm_ice_invalidate_key(index);
}
EXPORT_SYMBOL_GPL(qcom_scm_ice_invalidate_key);

static int qcom_ice_scm_evict_key(struct device *dev, int slot)
{
	return qcom_scm_ice_invalidate_key(slot);
}

static struct qcom_ice_ops qcom_ice_ops_scm = {
	.drv_name     = DRIVER_NAME,
	.available    = qcom_ice_scm_available,
	.program_key  = qcom_ice_scm_program_key,
	.evict_key    = qcom_ice_scm_evict_key,
	.set_context  = qcom_ice_scm_set_context,
};

static int __init qcom_ice_scm_init(void)
{
	if (!qcom_scm_is_available()) {
		pr_info("qcom-ice-scm: SCM interface not available\n");
		return -ENODEV;
	}

	if (!qcom_scm_ice_available() || !qcom_scm_ice_hwkey_available()) {
		pr_info("qcom-ice-scm: SCM ICE interface not available\n");
		return -ENODEV;
	}

	qcom_ice_ops_scm.dev = NULL;
	qcom_ice_ops_register(&qcom_ice_ops_scm);

	pr_info("qcom-ice-scm: ICE SCM backend registered\n");
	return 0;
}
late_initcall(qcom_ice_scm_init);

MODULE_DESCRIPTION("Qualcomm ICE SCM Backend");
MODULE_LICENSE("GPL");

