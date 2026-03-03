/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/firmware/qcom/qcom_pas.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "qcom_pas.h"
#include "qcom_scm.h"

static struct qcom_pas_ops *ops_ptr;

bool qcom_pas_is_available(void)
{
	/* The barrier is needed to synchronize with client drivers. */
	bool available = !!smp_load_acquire(&ops_ptr);

	return available;
}
EXPORT_SYMBOL_GPL(qcom_pas_is_available);

int qcom_pas_init_image(u32 peripheral, const void *metadata, size_t size,
			struct qcom_pas_context *ctx)
{
	if (ops_ptr)
		return ops_ptr->init_image(ops_ptr->dev, peripheral,
					   metadata, size, ctx);

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(qcom_pas_init_image);

void qcom_pas_metadata_release(struct qcom_pas_context *ctx)
{
	if (ops_ptr && ctx && ctx->ptr)
		ops_ptr->metadata_release(ops_ptr->dev, ctx);
}
EXPORT_SYMBOL_GPL(qcom_pas_metadata_release);

int qcom_pas_mem_setup(u32 peripheral, phys_addr_t addr, phys_addr_t size)
{
	if (ops_ptr)
		return ops_ptr->mem_setup(ops_ptr->dev, peripheral, addr, size);

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(qcom_pas_mem_setup);

int qcom_pas_auth_and_reset(u32 peripheral)
{
	if (ops_ptr)
		return ops_ptr->auth_and_reset(ops_ptr->dev, peripheral);

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(qcom_pas_auth_and_reset);

int qcom_pas_shutdown(u32 peripheral)
{
	if (ops_ptr)
		return ops_ptr->shutdown(ops_ptr->dev, peripheral);

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(qcom_pas_shutdown);

bool qcom_pas_supported(u32 peripheral)
{
	if (ops_ptr)
		return ops_ptr->supported(ops_ptr->dev, peripheral);

	return false;
}
EXPORT_SYMBOL_GPL(qcom_pas_supported);

void qcom_pas_ops_register(struct qcom_pas_ops *ops)
{
	/* The barrier is needed to synchronize with client drivers. */
	smp_store_release(&ops_ptr, ops);
}
EXPORT_SYMBOL_GPL(qcom_pas_ops_register);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sumit Garg <sumit.garg@oss.qualcomm.com>");
MODULE_DESCRIPTION("Qualcomm common TZ PAS driver");
