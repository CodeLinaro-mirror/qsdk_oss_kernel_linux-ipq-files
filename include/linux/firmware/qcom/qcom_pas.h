/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QCOM_PAS_H
#define __QCOM_PAS_H

#include <linux/err.h>
#include <linux/types.h>
#include <linux/cpumask.h>

struct qcom_pas_metadata {
	void *ptr;
	dma_addr_t phys;
	ssize_t size;
};

bool qcom_pas_is_available(void);
int qcom_pas_init_image(u32 peripheral, const void *metadata, size_t size,
			struct qcom_pas_metadata *ctx);
void qcom_pas_metadata_release(struct qcom_pas_metadata *ctx);
int qcom_pas_mem_setup(u32 peripheral, phys_addr_t addr, phys_addr_t size);
int qcom_pas_auth_and_reset(u32 peripheral);
int qcom_pas_shutdown(u32 peripheral);
bool qcom_pas_supported(u32 peripheral);

#endif /* __QCOM_PAS_H */
