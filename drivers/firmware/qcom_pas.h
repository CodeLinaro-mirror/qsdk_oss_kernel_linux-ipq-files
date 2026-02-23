/**
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QCOM_PAS_INT_H
#define __QCOM_PAS_INT_H

struct device;

/**
 * struct qcom_pas_ops - Qcom Peripheral Authentication Service (PAS) ops
 * @drv_name:		PAS driver name.
 * @dev:		PAS device pointer.
 * @supported:		Peripheral supported callback.
 * @init_image:		Peripheral image initialization callback.
 * @mem_setup:		Peripheral memory setup callback.
 * @auth_and_reset:	Peripheral firmware authentication and reset callback.
 * @shutdown:		Peripheral shutdown callback.
 * @metadata_release:	Image metadata release callback.
 */
struct qcom_pas_ops {
	const char *drv_name;
	struct device *dev;
	bool (*supported)(struct device *dev, u32 peripheral);
	int (*init_image)(struct device *dev, u32 peripheral,
			  const void *metadata, size_t size,
			  struct qcom_pas_metadata *ctx);
	int (*mem_setup)(struct device *dev, u32 peripheral,
			 phys_addr_t addr, phys_addr_t size);
	int (*auth_and_reset)(struct device *dev, u32 peripheral);
	int (*shutdown)(struct device *dev, u32 peripheral);
	void (*metadata_release)(struct device *dev,
				 struct qcom_pas_metadata *ctx);
};

void qcom_pas_ops_register(struct qcom_pas_ops *ops);

#endif /* __QCOM_PAS_INT_H */
