// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/hex.h>
#include <linux/key.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/seccrypt_ops.h>
#include <linux/qcom_seccrypt_tee.h>

#define CONTEXT_SIZE_CMDLINE	257
#define CONTEXT_SIZE		128
#define MAX_KEY_SIZE		32

#define ENC_BLOB_SIZE_CMDLINE	513
#define ENC_BLOB_SIZE		256

#define IV_SIZE_CMDLINE		(2 * AES_HUK_IV_SIZE + 1)

#define KEYRING_TYPE		"user"
#define KEYRING_DESC_HW_KEY_O	"hw_key_o"
#define KEYRING_DESC_HW_KEY_CR	"hw_key_cr"

static char ctx_s[CONTEXT_SIZE_CMDLINE];
static char ctx_d[CONTEXT_SIZE_CMDLINE];

static char enc_blob[ENC_BLOB_SIZE_CMDLINE];
static char iv_o[IV_SIZE_CMDLINE];
static int algo_mode = -1;

static int hw_key_cr_len, hw_key_o_len;

extern int look_up_user_keyrings(struct key **, struct key **);

static __init int hw_key_store_in_keyring(const char *type, const char *description,
					  const u8 *key, size_t key_len)
{
	key_ref_t key_ref, keyring_ref;
	struct key *user_key_ref;
	int ret;

	ret = look_up_user_keyrings(&user_key_ref, NULL);
	if (ret < 0)
		return ret;

	keyring_ref = make_key_ref(user_key_ref, 1);
	if (IS_ERR(keyring_ref))
		return PTR_ERR(keyring_ref);

	key_ref = key_create_or_update(keyring_ref, type, description,
				       key, key_len, KEY_PERM_UNDEF,
				       KEY_ALLOC_IN_QUOTA);
	if (!IS_ERR(key_ref)) {
		key_ref_put(key_ref);
		ret = 0;
	} else {
		ret = PTR_ERR(key_ref);
	}

	key_ref_put(keyring_ref);
	return ret;
}

static __init int str_to_hex(const char *in, size_t in_len, u8 *out)
{
	if (in_len % 2 != 0)
		return -EINVAL;

	/* hex2bin() rejects any non-hex character, including whitespace */
	if (hex2bin(out, in, in_len / 2))
		return -EINVAL;

	return in_len / 2;
}

/**
 * struct hw_key_optee_req - OP-TEE HUK derive + blob decrypt request
 * @seed_type: AES_CRBK or AES_OEM_PRODSEED
 * @key_len: AES key size (16 or 32)
 * @mode: AES mode (AES_MODE_ECB/CBC/CTR/XTS)
 * @ctx_data: SW context 1 (data key context), or NULL for CRBK
 * @ctx_data_len: Length of ctx_data, or 0
 * @ctx_salt: SW context 2 (salt/tweak key context, AES-XTS only), or NULL
 * @ctx_salt_len: Length of ctx_salt, or 0
 * @iv: AES_HUK_IV_SIZE-byte IV/tweak for CBC/CTR/XTS, or NULL to use an
 *      all-zero IV/tweak (always NULL for ECB)
 * @iv_len: Length of iv (0 or AES_HUK_IV_SIZE)
 * @enc_blob: Offline-encrypted ciphertext
 * @enc_blob_len: Length of enc_blob
 */
struct hw_key_optee_req {
	u32 seed_type;
	u32 key_len;
	int mode;
	const u8 *ctx_data;
	size_t ctx_data_len;
	const u8 *ctx_salt;
	size_t ctx_salt_len;
	const u8 *iv;
	size_t iv_len;
	const u8 *enc_blob;
	size_t enc_blob_len;
};

/**
 * hw_key_optee_derive_decrypt() - Re-derive an offline-generated HUK key via
 * OP-TEE and decrypt an offline-encrypted blob with it.
 * @req: Derive/decrypt request parameters
 * @out: Output buffer for the decrypted value, req->enc_blob_len bytes
 *
 * Returns: 0 on success, negative error code on failure
 */
static __init int hw_key_optee_derive_decrypt(const struct hw_key_optee_req *req,
					      u8 *out)
{
	struct aes_set_key_req cfg;
	struct secure_nand_aes_cmd_tee cmd;
	u8 *iv_buf = NULL, *req_buf = NULL, *rsp_buf = NULL;
	size_t iv_len = req->iv_len;
	int ret, clear_ret;

	switch (req->mode) {
	case AES_MODE_ECB:
		if (iv_len) {
			pr_err("hw_key: IV/tweak not supported for ECB mode\n");
			return -EINVAL;
		}
		break;
	case AES_MODE_CBC:
	case AES_MODE_CTR:
	case AES_MODE_XTS:
		if (iv_len && (!req->iv || iv_len != AES_HUK_IV_SIZE)) {
			pr_err("hw_key: algo_mode %d requires a %u-byte IV/tweak\n",
			       req->mode, AES_HUK_IV_SIZE);
			return -EINVAL;
		}
		break;
	default:
		pr_err("hw_key: algo_mode %d unsupported, set hw_key.algo_mode (0=ECB 1=CBC 2=CTR 3=XTS)\n",
		       req->mode);
		return -EINVAL;
	}

	if (req->seed_type == AES_OEM_PRODSEED && req->mode == AES_MODE_XTS &&
	    (!req->ctx_salt || !req->ctx_salt_len)) {
		pr_err("hw_key: XTS requires salt context (ctx_s)\n");
		return -EINVAL;
	}

	if (req->enc_blob_len < AES_HUK_DATA_MIN_LEN ||
	    req->enc_blob_len > AES_HUK_DATA_MAX_LEN ||
	    req->enc_blob_len % AES_HUK_IV_SIZE) {
		pr_err("hw_key: enc_blob length %zu must be 16..4096 and a multiple of 16\n",
		       req->enc_blob_len);
		return -EINVAL;
	}

	cfg.seed_type = req->seed_type;
	cfg.key_size = req->key_len;
	cfg.algo_mode = req->mode;

	pr_debug("hw_key: derive: seed_type=%u key_len=%u algo_mode=%d ctx_data_len=%zu ctx_salt_len=%zu iv_len=%zu enc_blob_len=%zu\n",
		 req->seed_type, req->key_len, req->mode, req->ctx_data_len,
		 req->ctx_salt_len, iv_len, req->enc_blob_len);

	ret = qcom_seccrypt_set_key(&cfg, sizeof(cfg),
				    (void *)req->ctx_data, (u32)req->ctx_data_len,
				    (void *)req->ctx_salt, (u32)req->ctx_salt_len);
	if (ret < 0) {
		pr_err("hw_key: set_key failed, ret=%d\n", ret);
		return ret;
	}

	/*
	 * CRYPT registers these buffers with tee_shm_register_kernel_buf(),
	 * which takes/drops page refs via virt_to_page(). They must not be
	 * on the stack: the second page of an order-1 kernel stack has a
	 * zero refcount, so the final put_page() frees the live stack page.
	 */
	req_buf = kmemdup(req->enc_blob, req->enc_blob_len, GFP_KERNEL);
	rsp_buf = kzalloc(req->enc_blob_len, GFP_KERNEL);
	/* Non-ECB modes default to an all-zero IV/tweak when none is given */
	if (req->mode != AES_MODE_ECB) {
		iv_buf = iv_len ? kmemdup(req->iv, iv_len, GFP_KERNEL) :
				  kzalloc(AES_HUK_IV_SIZE, GFP_KERNEL);
		iv_len = AES_HUK_IV_SIZE;
	}
	if (!req_buf || !rsp_buf || (iv_len && !iv_buf)) {
		ret = -ENOMEM;
		goto out_clear;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.direction = 0; /* decrypt */
	cmd.mode = req->mode;
	cmd.iv_buf_virt = iv_buf;
	cmd.iv_size = iv_len;
	cmd.req_buf_virt = req_buf;
	cmd.reqlen = req->enc_blob_len;
	cmd.rsp_buf_virt = rsp_buf;
	cmd.rsplen = req->enc_blob_len;

	ret = qcom_seccrypt_crypt(&cmd, sizeof(cmd));
	pr_debug("hw_key: crypt: direction=%llu mode=%llu iv_size=%llu reqlen=%llu ret=%d\n",
		 cmd.direction, cmd.mode, cmd.iv_size, cmd.reqlen, ret);
	if (ret >= 0)
		memcpy(out, rsp_buf, req->enc_blob_len);
	else
		pr_err("hw_key: blob decryption failed, ret=%d\n", ret);

out_clear:
	kfree(iv_buf);
	kfree(req_buf);
	kfree_sensitive(rsp_buf);

	/* A derived key left loaded in the PTA is a failure in its own right */
	clear_ret = qcom_seccrypt_clear_key();
	if (clear_ret < 0) {
		pr_err("hw_key: failed to clear key after derive: %d\n", clear_ret);
		if (ret >= 0)
			ret = clear_ret;
	}

	return ret;
}

/**
 * struct hw_key_gen_req - Key generation and keyring store request
 * @type: Keyring key type
 * @description: Keyring key description
 * @hw_key_len: Key length (16 or 32)
 * @ctx_data: SW context for the data key, or NULL
 * @ctx_data_len: Length of ctx_data, or 0
 * @ctx_salt: SW context for the salt key, or NULL
 * @ctx_salt_len: Length of ctx_salt, or 0
 * @seed_type: AES_CRBK or AES_OEM_PRODSEED (OP-TEE only)
 * @iv: IV/tweak for CBC/CTR/XTS, or NULL for all-zero (OP-TEE only)
 * @iv_len: Length of iv, or 0 (OP-TEE only)
 * @blob: Hex string of the offline-encrypted blob (OP-TEE only)
 */
struct hw_key_gen_req {
	const char *type;
	const char *description;
	u32 hw_key_len;
	u8 *ctx_data;
	size_t ctx_data_len;
	u8 *ctx_salt;
	size_t ctx_salt_len;
	u32 seed_type;
	const u8 *iv;
	size_t iv_len;
	const char *blob;
};

static __init int hw_key_gen_store_optee(const struct hw_key_gen_req *gen)
{
	struct hw_key_optee_req req;
	u8 enc_data[ENC_BLOB_SIZE] = {0};
	u8 out[ENC_BLOB_SIZE] = {0};
	size_t enc_len = strlen(gen->blob);
	int enc_data_len;
	int ret;

	if (gen->hw_key_len != 16 && gen->hw_key_len != 32)
		return -EINVAL;

	if (enc_len == 0) {
		pr_err("hw_key: encrypted blob (hw_key.enc_blob) not provided\n");
		return -EINVAL;
	}

	enc_data_len = str_to_hex(gen->blob, enc_len, enc_data);
	if (enc_data_len < 0) {
		pr_err("hw_key: invalid hex encrypted blob, error = %d\n",
		       enc_data_len);
		return enc_data_len;
	}

	/*
	 * Salt (SW context 2) is only meaningful for XTS mode;
	 * ECB/CBC/CTR use data context (Param1) alone.
	 */
	req = (struct hw_key_optee_req) {
		.seed_type	= gen->seed_type,
		.key_len	= gen->hw_key_len,
		.mode		= algo_mode,
		.ctx_data	= gen->ctx_data,
		.ctx_data_len	= gen->ctx_data_len,
		.ctx_salt	= algo_mode == AES_MODE_XTS ? gen->ctx_salt : NULL,
		.ctx_salt_len	= algo_mode == AES_MODE_XTS ?
					gen->ctx_salt_len : 0,
		.iv		= gen->iv,
		.iv_len		= gen->iv_len,
		.enc_blob	= enc_data,
		.enc_blob_len	= enc_data_len,
	};

	ret = hw_key_optee_derive_decrypt(&req, out);
	if (ret < 0)
		goto out_optee;

	ret = hw_key_store_in_keyring(gen->type, gen->description, out,
				      enc_data_len);

out_optee:
	memzero_explicit(out, sizeof(out));
	return ret;
}

static __init int hw_key_gen_store_scm(const struct hw_key_gen_req *gen)
{
	u8 hw_key[MAX_KEY_SIZE * 2] = {0};
	int key_len = 0;
	int ret;

	if (gen->hw_key_len != 16 && gen->hw_key_len != 32)
		return -EINVAL;

	ret = qcom_scm_derive_and_share_key(gen->hw_key_len, gen->ctx_data,
					    (u32)gen->ctx_data_len, hw_key,
					    gen->hw_key_len);
	if (ret < 0)
		goto out_scm;

	key_len = gen->hw_key_len;

	/* If ctx_data_len = 0, derive single key */
	if (gen->ctx_data_len > 0 && gen->ctx_salt_len > 0) {
		ret = qcom_scm_derive_and_share_key(gen->hw_key_len, gen->ctx_salt,
						    (u32)gen->ctx_salt_len,
						    &hw_key[key_len],
						    gen->hw_key_len);
		if (ret < 0)
			goto out_scm;

		key_len += gen->hw_key_len;
	}

	ret = hw_key_store_in_keyring(gen->type, gen->description, hw_key,
				      key_len);

out_scm:
	memzero_explicit(hw_key, sizeof(hw_key));
	return ret;
}

static __init int hw_key_gen_store(const struct hw_key_gen_req *gen)
{
	if (IS_ENABLED(CONFIG_QTI_HW_KEY_OPTEE))
		return hw_key_gen_store_optee(gen);

	return hw_key_gen_store_scm(gen);
}

static __init int tmel_hw_key_init(void)
{
	struct hw_key_gen_req gen;
	uint8_t context_salt[CONTEXT_SIZE] = {0};
	uint8_t context_data[CONTEXT_SIZE] = {0};
	size_t ctx_d_len = strlen(ctx_d);
	size_t ctx_s_len = strlen(ctx_s);
	int context_data_len = 0;
	int context_salt_len = 0;
	int ret = 0;
	u8 iv_o_bin[AES_HUK_IV_SIZE] = {0};
	size_t iv_o_str_len = strlen(iv_o);
	int iv_o_len = 0;

	/*
	 * Context to generate salt key is required only for XTS mode but the
	 * context to generate key for data is mandatory.
	 */
	if (ctx_d_len == 0) {
		pr_err("Context is not provided, skipping key init");
		return ret;
	}

	/*
	 * Convert the hex string to hex-value
	 */
	context_data_len = str_to_hex(ctx_d, ctx_d_len, context_data);
	if (context_data_len < 0) {
		pr_err("Invalid hex context data, error = %d", context_data_len);
		return context_data_len;
	}

	if (ctx_s_len) {
		context_salt_len = str_to_hex(ctx_s, ctx_s_len, context_salt);
		if (context_salt_len < 0) {
			pr_err("Invalid hex context salt, error = %d", context_salt_len);
			return context_salt_len;
		}
	}

	if (IS_ENABLED(CONFIG_QTI_HW_KEY_OPTEE) && iv_o_str_len != 0) {
		iv_o_len = str_to_hex(iv_o, iv_o_str_len, iv_o_bin);
		if (iv_o_len < 0) {
			pr_err("Invalid hex IV for hw_key_o, error = %d\n", iv_o_len);
			return iv_o_len;
		}
	}

	gen = (struct hw_key_gen_req) {
		.type		= KEYRING_TYPE,
		.description	= KEYRING_DESC_HW_KEY_O,
		.hw_key_len	= (u32)hw_key_o_len,
		.ctx_data	= context_data,
		.ctx_data_len	= context_data_len,
		.ctx_salt	= context_salt,
		.ctx_salt_len	= context_salt_len,
		.seed_type	= AES_OEM_PRODSEED,
		.iv		= iv_o_len > 0 ? iv_o_bin : NULL,
		.iv_len		= iv_o_len,
		.blob		= enc_blob,
	};

	ret = hw_key_gen_store(&gen);
	if (ret) {
		pr_err("Failed to generate key for given context, error = 0x%x",
		       ret);
		return ret;
	}

	/* OP-TEE path configures a single entry (hw_key_o) only */
	if (!IS_ENABLED(CONFIG_QTI_HW_KEY_OPTEE)) {
		gen = (struct hw_key_gen_req) {
			.type		= KEYRING_TYPE,
			.description	= KEYRING_DESC_HW_KEY_CR,
			.hw_key_len	= (u32)hw_key_cr_len,
		};

		ret = hw_key_gen_store(&gen);
		if (ret) {
			pr_err("Failed to generate key for NULL context, error = 0x%x",
			       ret);
			return ret;
		}
	}

	return ret;
}

module_param_string(ctx_s, ctx_s, sizeof(ctx_s), 0);
module_param_string(ctx_d, ctx_d, sizeof(ctx_d), 0);

module_param(hw_key_o_len, int, 0);
module_param(hw_key_cr_len, int, 0);

#ifdef CONFIG_QTI_HW_KEY_OPTEE
module_param_string(enc_blob, enc_blob, sizeof(enc_blob), 0);
module_param_string(iv_o, iv_o, sizeof(iv_o), 0);
module_param(algo_mode, int, 0);
#endif

/*
 * late_initcall so the key is in the keyring before dm-init (also
 * late_initcall, drivers/md links after drivers/soc) sets up dm-mod.create
 * targets. The OP-TEE SecCrypt backend registers earlier, at
 * device_initcall_sync. The SCM SecCrypt backend registers after this, but
 * the OP-TEE path cannot use it and the non-OP-TEE path calls qcom_scm
 * directly.
 */
late_initcall(tmel_hw_key_init);
