// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/device.h>
#include <soc/qcom/ice.h>
#include <linux/ice_ops.h>

static struct qcom_ice_ops *ops_ptr;

bool qcom_ice_ops_available(void)
{
	return !!smp_load_acquire(&ops_ptr);
}
EXPORT_SYMBOL_GPL(qcom_ice_ops_available);

int qcom_ice_available(void)
{
	if (!ops_ptr) {
		pr_err("No ICE backend registered\n");
		return -ENODEV;
	}

	return ops_ptr->available(ops_ptr->dev);
}
EXPORT_SYMBOL_GPL(qcom_ice_available);

void qcom_ice_ops_register(struct qcom_ice_ops *ops)
{
	if (!ops_ptr) {
		smp_store_release(&ops_ptr, ops);
		pr_info("ICE: Registered backend: %s\n", ops->drv_name);
	} else {
		pr_warn("ICE: Backend already registered (%s), ignoring %s\n",
			ops_ptr->drv_name, ops->drv_name);
	}
}
EXPORT_SYMBOL_GPL(qcom_ice_ops_register);

int qcom_ice_program_key(struct qcom_ice *ice, u8 algorithm_id, u8 key_size,
			 const u8 crypto_key[], u8 data_unit_size, int slot,
			 bool use_hwkey)
{
	if (!ops_ptr) {
		dev_err(ice->dev, "No ICE backend registered\n");
		return -ENODEV;
	}

	return ops_ptr->program_key(ops_ptr->dev, algorithm_id, key_size,
				    crypto_key, data_unit_size, slot, use_hwkey);
}
EXPORT_SYMBOL_GPL(qcom_ice_program_key);

int qcom_ice_evict_key(struct qcom_ice *ice, int slot)
{
	if (!ops_ptr) {
		dev_err(ice->dev, "No ICE backend registered\n");
		return -ENODEV;
	}
	return ops_ptr->evict_key(ops_ptr->dev, slot);
}
EXPORT_SYMBOL_GPL(qcom_ice_evict_key);

int qcom_ice_config_hwkey(struct qcom_ice *ice, u32 cipher, u32 key_size)
{
	if (!ops_ptr || !ops_ptr->config_hwkey)
		return -EOPNOTSUPP;

	return ops_ptr->config_hwkey(ops_ptr->dev, cipher, key_size);
}
EXPORT_SYMBOL_GPL(qcom_ice_config_hwkey);

int qcom_ice_set_context(u32 type, u8 key_size, u8 algo_mode,
				u8 *data_ctxt, u32 data_ctxt_len,
				u8 *salt_ctxt, u32 salt_ctxt_len)
{
	if (!ops_ptr || !ops_ptr->set_context)
		return -EOPNOTSUPP;

	return ops_ptr->set_context(ops_ptr->dev, type, key_size, algo_mode,
				    data_ctxt, data_ctxt_len,
				    salt_ctxt, salt_ctxt_len);
}
EXPORT_SYMBOL_GPL(qcom_ice_set_context);

MODULE_DESCRIPTION("Qualcomm ICE Core Layer");
MODULE_LICENSE("GPL");

