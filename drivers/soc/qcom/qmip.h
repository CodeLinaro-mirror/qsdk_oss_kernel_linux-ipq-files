/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __QMIP_H
#define __QMIP_H

#include <linux/types.h>
#include <linux/device.h>

#define NUM_DANGER_CLIENTS		10
#define NUM_THROTTLE_CLIENTS		8

/* Danger client indices */
#define DANGER_CLIENT_APSS		0
#define DANGER_CLIENT_NSS		1
#define DANGER_CLIENT_PRIMESS		2
#define DANGER_CLIENT_NSP0		3
#define DANGER_CLIENT_NSP1		4
#define DANGER_CLIENT_PCIE0		5
#define DANGER_CLIENT_PCIE4		6
#define DANGER_CLIENT_PCIE1		7
#define DANGER_CLIENT_PCIE2		8
#define DANGER_CLIENT_PCIE3		9

/* Throttle client indices */
#define THROTTLE_CLIENT_PRIMESS		0
#define THROTTLE_CLIENT_NSS		1
#define THROTTLE_CLIENT_PCIE0		2
#define THROTTLE_CLIENT_PCIE4		3
#define THROTTLE_CLIENT_PCIE1		4
#define THROTTLE_CLIENT_PCIE2		5
#define THROTTLE_CLIENT_PCIE3		6
#define THROTTLE_CLIENT_NSP		7


struct qmip_danger_cfg {
	u32 danger_var[11];
};

struct qmip_throttle_cfg{
	u32 grant_period;
	u32 grant_count;
	u32 threshold_level[4];
};

struct qmip_qm_cfg {
	u32 aggr_mask[NUM_THROTTLE_CLIENTS];
	u32 throttle_lut[NUM_THROTTLE_CLIENTS];
};

struct	qmip_drvdata {
	void __iomem *danger_base[NUM_DANGER_CLIENTS];
	struct qmip_danger_cfg danger_cfg[NUM_DANGER_CLIENTS];
	void __iomem *throttle_base[NUM_THROTTLE_CLIENTS];
	struct qmip_throttle_cfg throttle_cfg[NUM_THROTTLE_CLIENTS];
	void __iomem *qm_base;
	struct qmip_qm_cfg qm_cfg;
	struct device *dev;
	bool enabled;
};





#endif /* __QMIP_H */
