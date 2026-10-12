// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/of_platform.h>
#include "qmip.h"
#include <linux/sysfs.h>

/* Danger register offsets (per-client base) */
#define DANGER_CTRL_OFFSET		0x00
#define DANGER_OT_LEVEL1_OFFSET		0x08
#define DANGER_OT_LEVEL2_OFFSET		0x0c
#define DANGER_OT_LEVEL3_OFFSET		0x10
#define DANGER_OT_LEVEL4_OFFSET		0x14
#define DANGER_OT_LEVEL5_OFFSET		0x18
#define DANGER_DUR_L1_R_OFFSET		0x20
#define DANGER_DUR_L1_W_OFFSET		0x24
#define DANGER_DUR_L1_TOTAL_OFFSET	0x28
#define DANGER_DUR_L2_R_OFFSET		0x2c
#define DANGER_DUR_L2_W_OFFSET		0x30
#define DANGER_DUR_L2_TOTAL_OFFSET	0x34
#define DANGER_LEVEL_THRESHOLD_OFFSET	0x50	/* hardcoded 0x7 */

/* Danger ctrl hardcoded values */
#define DANGER_LEVEL_THRESHOLD_VAL	0x7
#define DANGER_CTRL_EN			BIT(2)

/* Throttle register offsets (per-client base) */
#define THROTTLE_CNTRL_OFFSET		0x00
#define THROTTLE_CGC_CNTRL_OFFSET	0x04
#define THROTTLE_RESET_CNTRL_OFFSET	0x08
#define THROTTLE_GRANT_PERIOD_OFFSET	0x10
#define THROTTLE_GRANT_COUNT_OFFSET	0x14
#define THROTTLE_THRESHOLD_00_OFFSET	0x18
#define THROTTLE_THRESHOLD_01_OFFSET	0x1c
#define THROTTLE_THRESHOLD_02_OFFSET	0x20
#define THROTTLE_THRESHOLD_03_OFFSET	0x24
#define THROTTLE_PEAK_CREDIT_OFFSET	0x28

/* Throttle ctrl hardcoded values */
#define THROTTLE_CTRL_ENABLE		0xff01
#define THROTTLE_PEAK_CREDIT_VAL	0x1000

/* QM register offsets (from qm_base = 0x69000) */
#define QM_CORE_CLK_CGC_CNTRL_OFFSET		0x004
#define QM_DANGER_SAFE_CNTRL_OFFSET		0x014
#define QM_CLNT_DANGER_AGGR_BASE_OFFSET		0x020	/* stride 0x4 per client */
#define QM_CLNT_THROTTLE_OUTPUT_BASE_OFFSET	0x150	/* stride 0x4 per client */
#define QM_CLNT_THROTTLE_OUTPUT_EN_BASE_OFFSET	0x1e0	/* stride 0x4 per client */
#define QM_THROTTLE_LEVEL_LEGACY_SELECT_OFFSET	0x740

/* QM hardcoded values */
#define QM_AGGR_CNTRL_VAL		0x1fff
#define QM_DSP_AGGR_CNTRL_VAL		0xffff
#define QM_THROTTLE_OUTPUT_VAL		0xfac688
#define QM_THROTTLE_OUTPUT_EN_VAL	0x1
#define QM_DANGER_SAFE_CNTRL_VAL	0x1
#define QM_LEGACY_SELECT_VAL		0x1
#define QM_CGC_CNTRL_VAL		0x1



static const char * const danger_names[NUM_DANGER_CLIENTS] = {
	"danger-apss",
	"danger-nss",
	"danger-primess",
	"danger-nsp0",
	"danger-nsp1",
	"danger-pcie0",
	"danger-pcie4",
	"danger-pcie1",
	"danger-pcie2",
	"danger-pcie3",
};

static const char * const throttle_names[NUM_THROTTLE_CLIENTS] = {
	"throttle-primess",	/* 0 */
	"throttle-nss",		/* 1 */
	"throttle-pcie0",	/* 2 */
	"throttle-pcie4",	/* 3 */
	"throttle-pcie1",	/* 4 */
	"throttle-pcie2",	/* 5 */
	"throttle-pcie3",	/* 6 */
	NULL,			/* 7 — unused */
	NULL,			/* 8 — unused */
	NULL,			/* 9 — unused */
	"throttle-nsp0",	/* 10 */
	"throttle-nsp1",	/* 11 */
};

/* Forward declarations */
static void qmip_configure_throttle(struct qmip_drvdata *drvdata);
static void qmip_configure_danger(struct qmip_drvdata *drvdata);
static void qmip_configure_qm(struct qmip_drvdata *drvdata);

static ssize_t danger_ot_level1_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[0] = val;
	return count;
}

static ssize_t danger_ot_level1_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[0]);
	return len;
}

static DEVICE_ATTR_RW(danger_ot_level1);

static ssize_t danger_ot_level2_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[1] = val;
	return count;
}

static ssize_t danger_ot_level2_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[1]);
	return len;
}

static DEVICE_ATTR_RW(danger_ot_level2);

static ssize_t danger_ot_level3_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[2] = val;
	return count;
}

static ssize_t danger_ot_level3_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[2]);
	return len;
}

static DEVICE_ATTR_RW(danger_ot_level3);

static ssize_t danger_ot_level4_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[3] = val;
	return count;
}

static ssize_t danger_ot_level4_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[3]);
	return len;
}

static DEVICE_ATTR_RW(danger_ot_level4);

static ssize_t danger_ot_level5_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[4] = val;
	return count;
}

static ssize_t danger_ot_level5_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[4]);
	return len;
}

static DEVICE_ATTR_RW(danger_ot_level5);

static ssize_t danger_dur_l1_r_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[5] = val;
	return count;
}

static ssize_t danger_dur_l1_r_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[5]);
	return len;
}

static DEVICE_ATTR_RW(danger_dur_l1_r);

static ssize_t danger_dur_l1_w_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[6] = val;
	return count;
}

static ssize_t danger_dur_l1_w_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[6]);
	return len;
}

static DEVICE_ATTR_RW(danger_dur_l1_w);

static ssize_t danger_dur_l1_total_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[7] = val;
	return count;
}

static ssize_t danger_dur_l1_total_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[7]);
	return len;
}

static DEVICE_ATTR_RW(danger_dur_l1_total);

static ssize_t danger_dur_l2_r_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[8] = val;
	return count;
}

static ssize_t danger_dur_l2_r_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[8]);
	return len;
}

static DEVICE_ATTR_RW(danger_dur_l2_r);

static ssize_t danger_dur_l2_w_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[9] = val;
	return count;
}

static ssize_t danger_dur_l2_w_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[9]);
	return len;
}

static DEVICE_ATTR_RW(danger_dur_l2_w);

static ssize_t danger_dur_l2_total_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_DANGER_CLIENTS)
		return -EINVAL;

	drvdata->danger_cfg[client].danger_var[10] = val;
	return count;
}

static ssize_t danger_dur_l2_total_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->danger_cfg[i].danger_var[10]);
	return len;
}

static DEVICE_ATTR_RW(danger_dur_l2_total);

/* Throttle attributes */
static ssize_t throttle_grant_period_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->throttle_cfg[client].grant_period = val;
	return count;
}

static ssize_t throttle_grant_period_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->throttle_cfg[i].grant_period);
	return len;
}

static DEVICE_ATTR_RW(throttle_grant_period);

static ssize_t throttle_grant_count_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->throttle_cfg[client].grant_count = val;
	return count;
}

static ssize_t throttle_grant_count_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->throttle_cfg[i].grant_count);
	return len;
}

static DEVICE_ATTR_RW(throttle_grant_count);

static ssize_t throttle_bw_threshold0_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->throttle_cfg[client].threshold_level[0] = val;
	return count;
}

static ssize_t throttle_bw_threshold0_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->throttle_cfg[i].threshold_level[0]);
	return len;
}

static DEVICE_ATTR_RW(throttle_bw_threshold0);

static ssize_t throttle_bw_threshold1_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->throttle_cfg[client].threshold_level[1] = val;
	return count;
}

static ssize_t throttle_bw_threshold1_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->throttle_cfg[i].threshold_level[1]);
	return len;
}

static DEVICE_ATTR_RW(throttle_bw_threshold1);

static ssize_t throttle_bw_threshold2_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->throttle_cfg[client].threshold_level[2] = val;
	return count;
}

static ssize_t throttle_bw_threshold2_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->throttle_cfg[i].threshold_level[2]);
	return len;
}

static DEVICE_ATTR_RW(throttle_bw_threshold2);

static ssize_t throttle_bw_threshold3_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->throttle_cfg[client].threshold_level[3] = val;
	return count;
}

static ssize_t throttle_bw_threshold3_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->throttle_cfg[i].threshold_level[3]);
	return len;
}

static DEVICE_ATTR_RW(throttle_bw_threshold3);

/* QM aggr_mask sysfs */
static ssize_t qm_aggr_mask_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->qm_cfg.aggr_mask[client] = val;
	return count;
}

static ssize_t qm_aggr_mask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->qm_cfg.aggr_mask[i]);
	return len;
}

static DEVICE_ATTR_RW(qm_aggr_mask);

/* QM throttle_lut sysfs */
static ssize_t qm_throttle_lut_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;

	drvdata->qm_cfg.throttle_lut[client] = val;
	return count;
}

static ssize_t qm_throttle_lut_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				drvdata->qm_cfg.throttle_lut[i]);
	return len;
}

static DEVICE_ATTR_RW(qm_throttle_lut);

/* qmip_enable trigger */
static ssize_t qmip_enable_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int val;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;
	if (val != 1)
		return -EINVAL;

	qmip_configure_throttle(drvdata);
	qmip_configure_danger(drvdata);
	qmip_configure_qm(drvdata);
	drvdata->enabled = true;
	return count;
}

static ssize_t qmip_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", drvdata->enabled);
}

static DEVICE_ATTR_RW(qmip_enable);

static ssize_t throttle_enable_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned int client, val;
	void __iomem *base;

	if (sscanf(buf, "%u %x", &client, &val) != 2)
		return -EINVAL;
	if (client >= NUM_THROTTLE_CLIENTS)
		return -EINVAL;
	if (!drvdata->throttle_base[client])
		return -EINVAL;

	base = drvdata->throttle_base[client];
	drvdata->throttle_cfg[client].enable_val = val;
	writel(val, base + THROTTLE_CNTRL_OFFSET);

	dev_info(dev, "throttle client %d %s\n", client, val ? "enabled" : "disabled");
	return count;
}

static ssize_t throttle_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qmip_drvdata *drvdata = dev_get_drvdata(dev);
	int i, len = 0;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++) {
		if (!drvdata->throttle_base[i])
			continue;
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"client%d: 0x%x\n", i,
				readl(drvdata->throttle_base[i] + THROTTLE_CNTRL_OFFSET));
	}
	return len;
}

static DEVICE_ATTR_RW(throttle_enable);

/* Attribute group */
static struct attribute *qmip_attrs[] = {
	&dev_attr_danger_ot_level1.attr,
	&dev_attr_danger_ot_level2.attr,
	&dev_attr_danger_ot_level3.attr,
	&dev_attr_danger_ot_level4.attr,
	&dev_attr_danger_ot_level5.attr,
	&dev_attr_danger_dur_l1_r.attr,
	&dev_attr_danger_dur_l1_w.attr,
	&dev_attr_danger_dur_l1_total.attr,
	&dev_attr_danger_dur_l2_r.attr,
	&dev_attr_danger_dur_l2_w.attr,
	&dev_attr_danger_dur_l2_total.attr,
	&dev_attr_throttle_grant_period.attr,
	&dev_attr_throttle_grant_count.attr,
	&dev_attr_throttle_bw_threshold0.attr,
	&dev_attr_throttle_bw_threshold1.attr,
	&dev_attr_throttle_bw_threshold2.attr,
	&dev_attr_throttle_bw_threshold3.attr,
	&dev_attr_qm_aggr_mask.attr,
	&dev_attr_qm_throttle_lut.attr,
	&dev_attr_qmip_enable.attr,
	&dev_attr_throttle_enable.attr,
	NULL,
};

static const struct attribute_group qmip_attr_group = {
	.attrs = qmip_attrs,
};

static void qmip_configure_throttle(struct qmip_drvdata *drvdata)
{
	int i;
	void __iomem *base;
	struct qmip_throttle_cfg *cfg;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++) {
		/* Skip PrimeSS (0) and unused indices 7, 8, 9 */
		if (i == THROTTLE_CLIENT_PRIMESS ||
		    (i >= 7 && i <= 9)) {
			dev_info(drvdata->dev, "skipping throttle client %d\n", i);
			continue;
		}

		base = drvdata->throttle_base[i];
		cfg  = &drvdata->throttle_cfg[i];

		dev_info(drvdata->dev, "configuring throttle client %d\n", i);
		writel(0x0, base + THROTTLE_CNTRL_OFFSET);
		writel(0x0, base + THROTTLE_CGC_CNTRL_OFFSET);
		writel(0x1, base + THROTTLE_RESET_CNTRL_OFFSET);
		writel(0x0, base + THROTTLE_RESET_CNTRL_OFFSET);
		writel(0x1, base + THROTTLE_CGC_CNTRL_OFFSET);

		writel(cfg->grant_period,       base + THROTTLE_GRANT_PERIOD_OFFSET);
		writel(cfg->grant_count,        base + THROTTLE_GRANT_COUNT_OFFSET);
		writel(cfg->threshold_level[0], base + THROTTLE_THRESHOLD_00_OFFSET);
		writel(cfg->threshold_level[1], base + THROTTLE_THRESHOLD_01_OFFSET);
		writel(cfg->threshold_level[2], base + THROTTLE_THRESHOLD_02_OFFSET);
		writel(cfg->threshold_level[3], base + THROTTLE_THRESHOLD_03_OFFSET);
		writel(THROTTLE_PEAK_CREDIT_VAL, base + THROTTLE_PEAK_CREDIT_OFFSET);

		/* second write — apply enable value set via sysfs */
		writel(cfg->enable_val, base + THROTTLE_CNTRL_OFFSET);
		dev_info(drvdata->dev, "throttle client %d configured\n", i);
	}
}

static void qmip_configure_danger(struct qmip_drvdata *drvdata)
{
	int i;
	void __iomem *base;
	struct qmip_danger_cfg *cfg;

	for (i = 0; i < NUM_DANGER_CLIENTS; i++) {
		/* Skip PrimeSS (2) only */
		if (i == DANGER_CLIENT_PRIMESS) {
			dev_info(drvdata->dev, "skipping danger client %d\n", i);
			continue;
		}

		base = drvdata->danger_base[i];
		cfg  = &drvdata->danger_cfg[i];

		dev_info(drvdata->dev, "configuring danger client %d\n", i);
		writel(DANGER_LEVEL_THRESHOLD_VAL, base + DANGER_LEVEL_THRESHOLD_OFFSET);

		writel(cfg->danger_var[0],  base + DANGER_OT_LEVEL1_OFFSET);
		writel(cfg->danger_var[1],  base + DANGER_OT_LEVEL2_OFFSET);
		writel(cfg->danger_var[2],  base + DANGER_OT_LEVEL3_OFFSET);
		writel(cfg->danger_var[3],  base + DANGER_OT_LEVEL4_OFFSET);
		writel(cfg->danger_var[4],  base + DANGER_OT_LEVEL5_OFFSET);

		writel(cfg->danger_var[5],  base + DANGER_DUR_L1_R_OFFSET);
		writel(cfg->danger_var[6],  base + DANGER_DUR_L1_W_OFFSET);
		writel(cfg->danger_var[7],  base + DANGER_DUR_L1_TOTAL_OFFSET);
		writel(cfg->danger_var[8],  base + DANGER_DUR_L2_R_OFFSET);
		writel(cfg->danger_var[9],  base + DANGER_DUR_L2_W_OFFSET);
		writel(cfg->danger_var[10], base + DANGER_DUR_L2_TOTAL_OFFSET);

		writel(DANGER_CTRL_EN, base + DANGER_CTRL_OFFSET);
		dev_info(drvdata->dev, "danger client %d done\n", i);
	}
}

static void qmip_configure_qm(struct qmip_drvdata *drvdata)
{
	int i;

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		writel(drvdata->qm_cfg.aggr_mask[i],
			drvdata->qm_base + QM_CLNT_DANGER_AGGR_BASE_OFFSET + (i * 4));

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		writel(drvdata->qm_cfg.throttle_lut[i],
			drvdata->qm_base + QM_CLNT_THROTTLE_OUTPUT_BASE_OFFSET + (i * 4));

	for (i = 0; i < NUM_THROTTLE_CLIENTS; i++)
		writel(QM_THROTTLE_OUTPUT_EN_VAL,
			drvdata->qm_base + QM_CLNT_THROTTLE_OUTPUT_EN_BASE_OFFSET + (i * 4));

	writel(QM_DANGER_SAFE_CNTRL_VAL,
		drvdata->qm_base + QM_DANGER_SAFE_CNTRL_OFFSET);

	writel(QM_LEGACY_SELECT_VAL,
		drvdata->qm_base + QM_THROTTLE_LEVEL_LEGACY_SELECT_OFFSET);

	writel(QM_CGC_CNTRL_VAL,
		drvdata->qm_base + QM_CORE_CLK_CGC_CNTRL_OFFSET);
}

static int qmip_probe(struct platform_device *pdev) {

	struct device *dev = &pdev->dev;
	struct qmip_drvdata *drvdata;
	struct resource *res;
	int ret = 0;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);

	if(!drvdata) {
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, drvdata);
	drvdata->dev = &pdev->dev;

	for (int i = 0; i < NUM_DANGER_CLIENTS; i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, danger_names[i]);
		if (!res) {
			dev_err(dev, "failed to get resource %s\n", danger_names[i]);
			return -EINVAL;
		}
		drvdata->danger_base[i] = devm_ioremap(dev, res->start, resource_size(res));
		if (!drvdata->danger_base[i])
			return -ENOMEM;
	}

	for (int i = 0; i < NUM_THROTTLE_CLIENTS; i++) {
		if (!throttle_names[i]) {
			drvdata->throttle_base[i] = NULL;
			continue;
		}
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, throttle_names[i]);
		if (!res) {
			dev_err(dev, "failed to get resource %s\n", throttle_names[i]);
			return -EINVAL;
		}
		drvdata->throttle_base[i] = devm_ioremap(dev, res->start, resource_size(res));
		if (!drvdata->throttle_base[i])
			return -ENOMEM;
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qm-base");
	if (!res) {
		dev_err(dev, "failed to get resource qm \n");
		return -EINVAL;
	}
	drvdata->qm_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!drvdata->qm_base)
		return -ENOMEM;

	/* Initialize default QM values — can be overridden via sysfs */
	for (int i = 0; i < NUM_THROTTLE_CLIENTS; i++) {
		drvdata->qm_cfg.aggr_mask[i]    = QM_AGGR_CNTRL_VAL;
		drvdata->qm_cfg.throttle_lut[i] = QM_THROTTLE_OUTPUT_VAL;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &qmip_attr_group);
	if (ret) {
		dev_err(dev, "failed to create sysfs group\n");
		return ret;
	}

	dev_info(dev, "QMIP driver probed successfully\n");
	return 0;
}

static const struct of_device_id qcom_qmip_of_match[] = {
	{.compatible = "qcom,qmip"},
	{},
};

MODULE_DEVICE_TABLE(of, qcom_qmip_of_match);

static int qmip_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &qmip_attr_group);
	return 0;
}

static struct platform_driver qmip_driver = {
	.probe =  qmip_probe,
	.remove = qmip_remove,
	.driver = {
		.name = "qmip",
		.of_match_table = qcom_qmip_of_match,
	},
};

module_platform_driver(qmip_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QMIP Driver for IPQ96XX" );
