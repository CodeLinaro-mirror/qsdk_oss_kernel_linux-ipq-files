/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/of.h>
#include <linux/firmware/qcom/qcom_pas.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>

#include "qcom_pas.h"

#define DRIVER_NAME "qcom-pas-tee"

/*
 * Peripheral Authentication Service (PAS) supported.
 *
 * Get PAS support status.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 */
#define PTA_QCOM_PAS_IS_SUPPORTED		1

/*
 * PAS capabilities.
 *
 * Get PAS capabilities.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 * [out] params[1].value.a:	PAS capability flags
 */
#define PTA_QCOM_PAS_CAPABILITIES		2

/*
 * PAS image initialization.
 *
 * Perform PAS image initialization.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 * [in]  params[1].memref:	Loadable firmware metadata
 */
#define PTA_QCOM_PAS_INIT_IMAGE			3

/*
 * PAS memory setup.
 *
 * Perform PAS memory setup.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 * [in]  params[0].value.b:	Relocatable firmware size
 * [in]  params[1].value.a:	32bit LSB relocatable firmware memory address
 * [in]  params[1].value.b:	32bit MSB relocatable firmware memory address
 */
#define PTA_QCOM_PAS_MEM_SETUP			4

/*
 * PAS get resource table.
 *
 * Perform PAS image initialization.
 *
 * [in]     params[0].value.a:	Unique 32bit remote processor identifier
 * [inout]  params[1].memref:	Resource table config
 */
#define PTA_QCOM_PAS_GET_RESOURCE_TABLE		5

/*
 * PAS image authentication and co-processor reset.
 *
 * Perform PAS image authentication and co-processor reset.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 * [in]  params[0].value.b:	Firmware size
 * [in]  params[1].value.a:	32bit LSB firmware memory address
 * [in]  params[1].value.b:	32bit MSB firmware memory address
 * [in]  params[2].memref:	Optional fw memory space shared/lent
 */
#define PTA_QCOM_PAS_AUTH_AND_RESET		6

/*
 * PAS co-processor set suspend/resume state.
 *
 * Perform PAS co-processor set suspend/resume state.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 * [in]  params[0].value.b:	Co-processor state identifier
 */
#define PTA_QCOM_PAS_SET_REMOTE_STATE		7

/*
 * PAS co-processor shutdown.
 *
 * Perform PAS co-processor shutdown.
 *
 * [in]  params[0].value.a:	Unique 32bit remote processor identifier
 */
#define PTA_QCOM_PAS_SHUTDOWN			8

/**
 * struct qcom_pas_tee_private - PAS service private data
 * @dev:		PAS service device.
 * @ctx:		TEE context handler.
 * @session_id:		PAS TA session identifier.
 */
struct qcom_pas_tee_private {
	struct device *dev;
	struct tee_context *ctx;
	u32 session_id;
};

static bool qcom_pas_tee_supported(struct device *dev, u32 pas_id)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret = 0;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_QCOM_PAS_IS_SUPPORTED;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = pas_id;

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_dbg(dev, "PAS not supported, pas_id: %d, err: %x\n",
			pas_id, inv_arg.ret);
		return false;
	}

	return true;
}

static int qcom_pas_tee_init_image(struct device *dev, u32 pas_id,
				   const void *metadata, size_t size,
				   struct qcom_pas_context *ctx)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_shm *mdata_shm = NULL;
	struct tee_param param[4];
	u8 *mdata_buf = NULL;
	int ret = 0, err = -ENODEV;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	mdata_shm = tee_shm_alloc_kernel_buf(data->ctx, size);
	if (IS_ERR(mdata_shm)) {
		dev_err(dev, "mdata_shm allocation failed\n");
		return PTR_ERR(mdata_shm);
	}

	mdata_buf = tee_shm_get_va(mdata_shm, 0);
	if (IS_ERR(mdata_buf)) {
		dev_err(dev, "mdata_buf get VA failed\n");
		err = PTR_ERR(mdata_buf);
		goto err_shm;
	}
	memcpy(mdata_buf, metadata, size);

	inv_arg.func = PTA_QCOM_PAS_INIT_IMAGE;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = pas_id;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[1].u.memref.shm = mdata_shm;
	param[1].u.memref.size = size;
	param[1].u.memref.shm_offs = 0;

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_err(dev, "PAS init image failed, pas_id: %d, err: %x\n",
			pas_id, inv_arg.ret);
		err = -EINVAL;
		goto err_shm;
	}
	ctx->ptr = (void *)mdata_shm;

	return 0;
err_shm:
	tee_shm_free(mdata_shm);

	return err;
}

static int qcom_pas_tee_mem_setup(struct device *dev, u32 pas_id,
				  phys_addr_t addr, phys_addr_t size)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret = 0;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_QCOM_PAS_MEM_SETUP;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = pas_id;
	param[0].u.value.b = size;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].u.value.a = lower_32_bits(addr);
	param[1].u.value.b = upper_32_bits(addr);

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_err(dev, "PAS mem setup failed, pas_id: %d, err: %x\n",
			pas_id, inv_arg.ret);
		return -EINVAL;
	}

	return 0;
}

static void *qcom_pas_tee_get_rsc_table(struct device *dev,
					struct qcom_pas_context *ctx,
					void *input_rt, size_t input_rt_size,
					size_t *output_rt_size)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_shm *rt_shm = NULL;
	struct tee_param param[4];
	void *rt_buf = NULL;
	int ret = 0, err = -ENODEV;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_QCOM_PAS_GET_RESOURCE_TABLE;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = ctx->pas_id;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	param[1].u.memref.shm = rt_shm;
	param[1].u.memref.size = input_rt_size;
	param[1].u.memref.shm_offs = 0;

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_err(dev, "PAS get RT failed, pas_id: %d, err: %x\n",
			ctx->pas_id, inv_arg.ret);
		return ERR_PTR(-EINVAL);
	}

	if (param[1].u.memref.size) {
		rt_shm = tee_shm_alloc_kernel_buf(data->ctx,
						  param[1].u.memref.size);
		if (IS_ERR(rt_shm)) {
			dev_err(dev, "rt_shm allocation failed\n");
			return rt_shm;
		}

		rt_buf = tee_shm_get_va(rt_shm, 0);
		if (IS_ERR_OR_NULL(rt_buf)) {
			dev_err(dev, "rt_buf get VA failed\n");
			err = PTR_ERR(rt_buf);
			goto err_shm;
		}
		memcpy(rt_buf, input_rt, input_rt_size);

		param[1].u.memref.shm = rt_shm;
		ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
		if (ret < 0 || inv_arg.ret != 0) {
			dev_err(dev, "PAS get RT failed, pas_id: %d, err: %x\n",
				ctx->pas_id, inv_arg.ret);
			err = -EINVAL;
			goto err_shm;
		}
		*output_rt_size = param[1].u.memref.size;
	}

	return rt_buf;
err_shm:
	tee_shm_free(rt_shm);

	return ERR_PTR(err);
}

static int __qcom_pas_tee_auth_and_reset(struct device *dev, u32 pas_id,
					 phys_addr_t mem_phys, size_t mem_size)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret = 0;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_QCOM_PAS_AUTH_AND_RESET;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = pas_id;
	param[0].u.value.b = mem_size;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].u.value.a = lower_32_bits(mem_phys);
	param[1].u.value.b = upper_32_bits(mem_phys);

	/* Reserved for fw memory space to be shared or lent */
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	param[2].u.memref.shm = NULL;
	param[2].u.memref.size = 0;
	param[2].u.memref.shm_offs = 0;

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_err(dev, "PAS auth reset failed, pas_id: %d, err: %x\n",
			pas_id, inv_arg.ret);
		return -EINVAL;
	}

	return 0;
}

static int qcom_pas_tee_auth_and_reset(struct device *dev, u32 pas_id)
{
	return __qcom_pas_tee_auth_and_reset(dev, pas_id, 0, 0);
}

static int qcom_pas_tee_prepare_and_auth_reset(struct device *dev,
					       struct qcom_pas_context *ctx)
{
	return __qcom_pas_tee_auth_and_reset(dev, ctx->pas_id, ctx->mem_phys,
					     ctx->mem_size);
}

static int qcom_pas_tee_set_remote_state(struct device *dev, u32 state,
					 u32 pas_id)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret = 0;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_QCOM_PAS_SET_REMOTE_STATE;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = pas_id;
	param[0].u.value.b = state;

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_err(dev, "PAS shutdown failed, pas_id: %d, err: %x\n",
			pas_id, inv_arg.ret);
		return -EINVAL;
	}

	return 0;
}

static int qcom_pas_tee_shutdown(struct device *dev, u32 pas_id)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret = 0;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_QCOM_PAS_SHUTDOWN;
	inv_arg.session = data->session_id;
	inv_arg.num_params = 4;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = pas_id;

	ret = tee_client_invoke_func(data->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret != 0) {
		dev_err(dev, "PAS shutdown failed, pas_id: %d, err: %x\n",
			pas_id, inv_arg.ret);
		return -EINVAL;
	}

	return 0;
}

static void qcom_pas_tee_metadata_release(struct device *dev,
					  struct qcom_pas_context *ctx)
{
	struct tee_shm *mdata_shm = ctx->ptr;

	if (!mdata_shm)
		return;

	tee_shm_free(mdata_shm);
	ctx->ptr = NULL;  /* Clear pointer to prevent double-free */
}

static struct qcom_pas_ops qcom_pas_ops_tee = {
	.drv_name		= DRIVER_NAME,
	.supported		= qcom_pas_tee_supported,
	.init_image		= qcom_pas_tee_init_image,
	.mem_setup		= qcom_pas_tee_mem_setup,
	.get_rsc_table		= qcom_pas_tee_get_rsc_table,
	.auth_and_reset		= qcom_pas_tee_auth_and_reset,
	.prepare_and_auth_reset	= qcom_pas_tee_prepare_and_auth_reset,
	.set_remote_state	= qcom_pas_tee_set_remote_state,
	.shutdown		= qcom_pas_tee_shutdown,
	.metadata_release	= qcom_pas_tee_metadata_release,
};

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	if (ver->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;
	else
		return 0;
}

static int qcom_pas_tee_probe(struct device *dev)
{
	struct qcom_pas_tee_private *data;
	struct tee_client_device *pas_device = to_tee_client_device(dev);
	struct tee_ioctl_open_session_arg sess_arg;
	int ret = 0, err = -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->ctx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(data->ctx)) {
		dev_err(dev, "Failed to open TEE context, err=%ld\n", PTR_ERR(data->ctx));
		err = -ENODEV;
		goto out_data;
	}

	memset(&sess_arg, 0, sizeof(sess_arg));
	export_uuid(sess_arg.uuid, &pas_device->id.uuid);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL;
	sess_arg.num_params = 0;

	ret = tee_client_open_session(data->ctx, &sess_arg, NULL);
	if (ret < 0 || sess_arg.ret != 0) {
		dev_err(dev, "tee_client_open_session failed, ret=%d, sess_arg.ret=%x\n",
			ret, sess_arg.ret);
		err = -EINVAL;
		goto out_ctx;
	}
	data->session_id = sess_arg.session;
	dev_info(dev, "TEE session opened successfully, session_id=%u\n", sess_arg.session);
	dev_set_drvdata(dev, data);
	qcom_pas_ops_tee.dev = dev;

	qcom_pas_ops_register(&qcom_pas_ops_tee);
	dev_info(dev, "PAS ops registered successfully\n");

	return 0;

out_ctx:
	tee_client_close_context(data->ctx);
out_data:
	kfree(data);

	return err;
}

static int qcom_pas_tee_remove(struct device *dev)
{
	struct qcom_pas_tee_private *data = dev_get_drvdata(dev);

	tee_client_close_session(data->ctx, data->session_id);
	tee_client_close_context(data->ctx);

	return 0;
}

static const struct tee_client_device_id qcom_pas_tee_id_table[] = {
	{UUID_INIT(0xcff7d191, 0x7ca0, 0x4784,
		   0xaf, 0x13, 0x48, 0x22, 0x3b, 0x9a, 0x4f, 0xbe)},
	{}
};

MODULE_DEVICE_TABLE(tee, qcom_pas_tee_id_table);

static struct tee_client_driver qcom_pas_tee_driver = {
	.id_table	= qcom_pas_tee_id_table,
	.driver		= {
		.name		= DRIVER_NAME,
		.bus		= &tee_bus_type,
		.probe		= qcom_pas_tee_probe,
		.remove		= qcom_pas_tee_remove,
	},
};

static int __init qcom_pas_tee_mod_init(void)
{
	return driver_register(&qcom_pas_tee_driver.driver);
}

static void __exit qcom_pas_tee_mod_exit(void)
{
	driver_unregister(&qcom_pas_tee_driver.driver);
}

module_init(qcom_pas_tee_mod_init);
module_exit(qcom_pas_tee_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sumit Garg <sumit.garg@oss.qualcomm.com>");
MODULE_DESCRIPTION("TEE bus based Qualcomm PAS driver");
