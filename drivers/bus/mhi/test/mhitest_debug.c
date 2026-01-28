/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "commonmhitest.h"

#define QCN9224_SRAM_START			0x01300000
#define QCN9224_SRAM_SIZE			0x005A8000
#define QCN9224_SRAM_END \
		(QCN9224_SRAM_START + QCN9224_SRAM_SIZE - 1)

#define QCN9625_SRAM_START			0x02100000
#define QCN9625_SRAM_SIZE			0x00580000
#define QCN9625_SRAM_END \
				(QCN9625_SRAM_START + QCN9625_SRAM_SIZE - 1)

#define QCN9224_PCIE_BHI_ERRDBG2_REG		0x1E0E238
#define QCN9224_PCIE_BHI_ERRDBG3_REG		0x1E0E23C
#define QCN9224_PBL_LOG_SRAM_START		0x01303da0
#define QCN9224_v2_PBL_LOG_SRAM_START		0x01303e98
#define QCN9224_PBL_LOG_SRAM_MAX_SIZE		40
#define QCN9224_TCSR_PBL_LOGGING_REG		0x1B00094
#define QCN9224_PBL_WLAN_BOOT_CFG		0x1E22B34
#define QCN9224_PBL_BOOTSTRAP_STATUS		0x1A006D4
#define MAX_PBL_DATA_SNAPSHOT			2

#define QCN9625_PBL_LOG_SRAM_START		0x0210AF10
#define QCN9625_PBL_WLAN_BOOT_CONFIG		0x01F9200C

#define TCSR_DEBUG_REG0                         0x1B001C4
#define PBL_ERR_MAGIC_COOKIE                    0xAABBCCDD

/* Structure to have information about pbl_err region */
typedef struct _pbl_err_to_host_type {
	uint32_t  magic_cookie;
	uint32_t  err_region_size;
	uint32_t  err_region_base;
} pbl_err_to_host_type;


/**
 * mhitest_read_pbl_ram_info - Read PBL RAM information from TCSR_DEBUG_REG0
 * @mplat: structure of mhitest platform driver
 * @pbl_ram_start: pointer to store PBL RAM start address
 * @pbl_ram_size: pointer to store PBL RAM size
 *
 * Read PBL RAM information from TCSR_DEBUG_REG0 register. This register points
 * to a structure that contains the magic cookie, error region size, and error
 * region base address. If the magic cookie matches, use the error region base
 * and size as the PBL RAM start address and size. Otherwise, use default values.
 *
 * Return: 0 on success, negative error code on failure
 */
static int mhitest_read_pbl_ram_info(struct mhitest_platform *mplat,
				     u32 *pbl_ram_start, u32 *pbl_ram_size)
{
	u32 tcsr_debug_reg0_val = 0;
	pbl_err_to_host_type pbl_err_info = {0};
	int ret = 0;

	if (!mplat || !pbl_ram_start || !pbl_ram_size) {
		pr_err("Invalid input parameters\n");
		return -EINVAL;
	}

	/* Read the value from TCSR_DEBUG_REG0 */
	ret = mhitest_pci_reg_read(mplat, TCSR_DEBUG_REG0, &tcsr_debug_reg0_val);
	if (ret != 0) {
		pr_err("Failed to read TCSR_DEBUG_REG0\n");
		return ret;
	}

	/* Read the pbl_err_to_host_type structure using sizeof(u32) for field offsets */
	ret = mhitest_pci_reg_read(mplat,
				  tcsr_debug_reg0_val,
				  (u32 *)&pbl_err_info.magic_cookie);
	if (ret != 0) {
		pr_err("Failed to read magic_cookie\n");
		return ret;
	}

	ret = mhitest_pci_reg_read(mplat,
				  tcsr_debug_reg0_val + sizeof(u32),
				  (u32 *)&pbl_err_info.err_region_size);
	if (ret != 0) {
		pr_err("Failed to read err_region_size\n");
		return ret;
	}

	ret = mhitest_pci_reg_read(mplat,
				  tcsr_debug_reg0_val + (2 * sizeof(u32)),
				  (u32 *)&pbl_err_info.err_region_base);
	if (ret != 0) {
		pr_err("Failed to read err_region_base\n");
		return ret;
	}

	/* Verify magic cookie */
	if (pbl_err_info.magic_cookie != PBL_ERR_MAGIC_COOKIE) {
		pr_err("Invalid magic cookie: 0x%x, expected: 0x%x\n",
		       pbl_err_info.magic_cookie, PBL_ERR_MAGIC_COOKIE);
		return -EINVAL;
	}

	/* Use the values from the structure */
	*pbl_ram_start = pbl_err_info.err_region_base;
	*pbl_ram_size = pbl_err_info.err_region_size;

	pr_info("Using PBL RAM info from TCSR_DEBUG_REG0: start=0x%x, size=%u\n",
		 *pbl_ram_start, *pbl_ram_size);

	return 0;
}

#define QCN9224_SNOC_ERL_ErrVld_Low		0x1E80010
#define QCN9224_SNOC_ERL_ErrLog0_Low		0x1E80020
#define QCN9224_SNOC_ERL_ErrLog0_High		0x1E80024
#define QCN9224_SNOC_ERL_ErrLog1_Low		0x1E80028
#define QCN9224_SNOC_ERL_ErrLog1_High		0x1E8002C
#define QCN9224_SNOC_ERL_ErrLog2_Low		0x1E80030
#define QCN9224_SNOC_ERL_ErrLog2_High		0x1E80034
#define QCN9224_SNOC_ERL_ErrLog3_Low		0x1E80038
#define QCN9224_SNOC_ERL_ErrLog3_High		0x1E8003C
#define QCN9224_PCNOC_ERL_ErrVld_Low		0x1F00010
#define QCN9224_PCNOC_ERL_ErrLog0_Low		0x1F00020
#define QCN9224_PCNOC_ERL_ErrLog0_High		0x1F00024
#define QCN9224_PCNOC_ERL_ErrLog1_Low		0x1F00028
#define QCN9224_PCNOC_ERL_ErrLog1_High		0x1F0002C
#define QCN9224_PCNOC_ERL_ErrLog2_Low		0x1F00030
#define QCN9224_PCNOC_ERL_ErrLog2_High		0x1F00034
#define QCN9224_PCNOC_ERL_ErrLog3_Low		0x1F00038
#define QCN9224_PCNOC_ERL_ErrLog3_High		0x1F0003C

#define QCN9224_PCIE_PCIE_LOCAL_REG_REMAP_BAR_CTRL	0x310C
#define QCN9224_WLAON_SOC_RESET_CAUSE_SHADOW_REG	0x1F80718
#define QCN9224_PCIE_PCIE_PARF_LTSSM			0x1E081B0
#define QCN9224_GCC_RAMSS_CBCR				0x1E38200

#define QCN9625_PCIE_PCIE_LOCAL_REG_REMAP_BAR_CTRL	0x3278
#define QCN9625_GCC_RAMSS_CBCR				0x1E38218

#define PCIE_CFG_PCIE_STATUS			0x230
#define PCIE_PCIE_PARF_PM_STTS			0x1E08024
#define PCIE_PCI_MSI_CAP_ID_NEXT_CTRL_REG	0x50
#define PCIE_MSI_CAP_OFF_04H_REG		0x54
#define PCIE_MSI_CAP_OFF_08H_REG		0x58
#define PCIE_MSI_CAP_OFF_0CH_REG		0x5C
#define SBL_LOG_SIZE_MASK			0xFFFF

struct pbl_reg_addr {
	u32 pbl_log_sram_start;
	u32 pbl_log_sram_max_size;
	u32 pbl_log_sram_start_v1;
	u32 pbl_log_sram_max_size_v1;
	u32 tcsr_pbl_logging_reg;
	u32 pbl_wlan_boot_cfg;
	u32 pbl_bootstrap_status;
};

struct sbl_reg_addr {
	u32 sbl_sram_start;
	u32 sbl_sram_end;
	u32 sbl_log_start_reg;
	u32 sbl_log_size_reg;
	u32 sbl_log_size_shift;
};

struct pbl_err_data {
	u32 *pbl_vals;
	u32 *pbl_reg_tbl;
	u32 pbl_tbl_len;
};

struct dump_pbl_sbl_data {
	u32 pbl_stage;
	u32 sbl_log_start;
	u32 pbl_wlan_boot_cfg;
	u32 pbl_bootstrap_status;
	u32 remap_bar_ctrl;
	u32 soc_rc_shadow_reg;
	u32 parf_ltssm;
	u32 parf_pm_stts;
	u32 gcc_ramss_cbcr;
	u32 pcie_cfg_pcie_status;
	u32 *sbl_vals;
	u32 sbl_len;
	u32 *noc_vals;
	u16 type0_status_cmd_reg;
	u16 pci_msi_cap_id_next_ctrl_reg;
	u16 pci_msi_cap_off_04h_reg;
	u16 pci_msi_cap_off_08h_reg;
	u16 pci_msi_cap_off_0ch_reg;
	struct pbl_err_data pbl_data[MAX_PBL_DATA_SNAPSHOT];
};

struct noc_err_table {
	char *reg_name;
	unsigned long reg;
	int (*reg_handler)(struct mhitest_platform *mplat, u32 addr,
			   u32 *val);
};

static struct noc_err_table noc_err_table_list[] = {
	{"SNOC_ERL_ErrVld_Low", QCN9224_SNOC_ERL_ErrVld_Low,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog0_Low", QCN9224_SNOC_ERL_ErrLog0_Low,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog0_High", QCN9224_SNOC_ERL_ErrLog0_High,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog1_Low", QCN9224_SNOC_ERL_ErrLog1_Low,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog1_High", QCN9224_SNOC_ERL_ErrLog1_High,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog2_Low", QCN9224_SNOC_ERL_ErrLog2_Low,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog2_High", QCN9224_SNOC_ERL_ErrLog2_High,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog3_Low", QCN9224_SNOC_ERL_ErrLog3_Low,
							&mhitest_pci_reg_read},
	{"SNOC_ERL_ErrLog3_High", QCN9224_SNOC_ERL_ErrLog3_High,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrVld_Low", QCN9224_PCNOC_ERL_ErrVld_Low,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog0_Low", QCN9224_PCNOC_ERL_ErrLog0_Low,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog0_High", QCN9224_PCNOC_ERL_ErrLog0_High,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog1_Low", QCN9224_PCNOC_ERL_ErrLog1_Low,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog1_High", QCN9224_PCNOC_ERL_ErrLog1_High,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog2_Low", QCN9224_PCNOC_ERL_ErrLog2_Low,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog2_High", QCN9224_PCNOC_ERL_ErrLog2_High,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog3_Low", QCN9224_PCNOC_ERL_ErrLog3_Low,
							&mhitest_pci_reg_read},
	{"PCNOC_ERL_ErrLog3_High", QCN9224_PCNOC_ERL_ErrLog3_High,
							&mhitest_pci_reg_read},
	{ NULL },
};

static int mhitest_debug_read_pbl_data(struct mhitest_platform *mplat,
				       struct pbl_reg_addr *pbl_data,
				       struct pbl_err_data *pbl_err_data)
{
	u32 log_size_v1 = pbl_data->pbl_log_sram_max_size_v1;
	u32 log_size = pbl_data->pbl_log_sram_max_size;
	u32 total_size = log_size + log_size_v1;
	int gfp = GFP_KERNEL;
	u32 *mem_addr = NULL;
	u32 *buf = NULL;
	int i = 0;
	int j = 0;

	if (MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee)) {
		pr_err("already in Mission mode\n");
		return -EINVAL;
	}

	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	buf = kcalloc(total_size, sizeof(u32), gfp);
	if (!buf)
		return -ENOMEM;

	mem_addr = kcalloc(total_size, sizeof(u32), gfp);
	if (!mem_addr) {
		kfree(buf);
		return -ENOMEM;
	}

	for (i = 0, j = 0; i < log_size; i += sizeof(u32), j++) {
		mem_addr[j] = pbl_data->pbl_log_sram_start + i;
		if (mhitest_pci_reg_read(mplat,
					 mem_addr[j], &buf[j]))
			break;
	}

	for (i = 0; i < log_size_v1; i += sizeof(u32), j++) {
		mem_addr[j] = pbl_data->pbl_log_sram_start_v1 + i;
		if (mhitest_pci_reg_read(mplat,
					 mem_addr[j], &buf[j]))
			break;
	}

	pbl_err_data->pbl_tbl_len = j;
	pbl_err_data->pbl_vals = buf;
	pbl_err_data->pbl_reg_tbl = mem_addr;

	return 0;
}

static int mhitest_debug_read_sbl_data(struct mhitest_platform *mplat,
				       u32 log_size,
				       u32 sram_start_reg,
				       struct dump_pbl_sbl_data *pbl_sbl_err)
{
	int i = 0;
	int j = 0;
	int gfp = GFP_KERNEL;
	u32 mem_addr = 0;
	u32 *buf = NULL;

	if (MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee)) {
		pr_err("already in Mission mode\n");
		return -EINVAL;
	}

	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	buf = kcalloc(log_size, sizeof(u32), gfp);
	if (!buf)
		return -ENOMEM;

	for (i = 0, j = 0; i < log_size; i += sizeof(u32), j++) {
		mem_addr = sram_start_reg + i;
		mhitest_pci_reg_read(mplat, mem_addr, &buf[j]);
		if (buf[j] == 0)
			break;
	}
	pbl_sbl_err->sbl_vals = buf;
	pbl_sbl_err->sbl_len = i;

	return 0;
}

static int mhitest_debug_read_noc_errors(struct mhitest_platform *mplat,
				      struct dump_pbl_sbl_data *pbl_sbl_err)
{
	int i = 0;
	u32 *buf = NULL;
	int gfp = GFP_KERNEL;
	size_t len = sizeof(noc_err_table_list) /
			sizeof(noc_err_table_list[0]);

	if (MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee)) {
		pr_err("already in Mission mode\n");
		return -EINVAL;
	}
	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	buf = kcalloc(len, sizeof(u32), gfp);
	if (!buf)
		return -ENOMEM;

	switch (mplat->device_id) {
	case QCN92XX_DEVICE_ID:
	case QCN95XX_DEVICE_ID:
	case QCN96XX_DEVICE_ID:
		for (i = 0; noc_err_table_list[i].reg_name; i++)
			noc_err_table_list[i].reg_handler(mplat,
				noc_err_table_list[i].reg, &buf[i]);
		pbl_sbl_err->noc_vals = buf;
		break;
	default:
		break;
	}

	return 0;
}

static int mhitest_debug_read_misc_data(struct mhitest_platform *mplat,
					struct pbl_reg_addr *pbl_data,
					struct sbl_reg_addr *sbl_data,
					struct dump_pbl_sbl_data *pbl_sbl_err)
{

	if (MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee)) {
		pr_err("already in Mission mode\n");
		return -EINVAL;
	}
#if defined(CONFIG_PCIE_QCOM)
	pcie_parf_read(mplat->pci_dev, PCIE_CFG_PCIE_STATUS,
		       &pbl_sbl_err->pcie_cfg_pcie_status);
#endif
	mhitest_pci_reg_read(mplat,
			     PCIE_PCIE_PARF_PM_STTS,
			     &pbl_sbl_err->parf_pm_stts);
	pci_read_config_word(mplat->pci_dev, PCI_COMMAND,
			     &pbl_sbl_err->type0_status_cmd_reg);

	pci_read_config_word(mplat->pci_dev,
			     PCIE_PCI_MSI_CAP_ID_NEXT_CTRL_REG,
			     &pbl_sbl_err->pci_msi_cap_id_next_ctrl_reg);
	pci_read_config_word(mplat->pci_dev, PCIE_MSI_CAP_OFF_04H_REG,
			     &pbl_sbl_err->pci_msi_cap_off_04h_reg);
	pci_read_config_word(mplat->pci_dev, PCIE_MSI_CAP_OFF_08H_REG,
			     &pbl_sbl_err->pci_msi_cap_off_08h_reg);
	pci_read_config_word(mplat->pci_dev, PCIE_MSI_CAP_OFF_0CH_REG,
			     &pbl_sbl_err->pci_msi_cap_off_0ch_reg);

	if (mplat->device_id == QCN92XX_DEVICE_ID) {
		mhitest_pci_reg_read(mplat,
				     QCN9224_PCIE_PCIE_LOCAL_REG_REMAP_BAR_CTRL,
				     &pbl_sbl_err->remap_bar_ctrl);
		mhitest_pci_reg_read(mplat,
				     QCN9224_WLAON_SOC_RESET_CAUSE_SHADOW_REG,
				     &pbl_sbl_err->soc_rc_shadow_reg);
		mhitest_pci_reg_read(mplat,
				     QCN9224_PCIE_PCIE_PARF_LTSSM,
				     &pbl_sbl_err->parf_ltssm);
		mhitest_pci_reg_read(mplat,
				     QCN9224_GCC_RAMSS_CBCR,
				     &pbl_sbl_err->gcc_ramss_cbcr);
	}

	if (mplat->device_id == QCN95XX_DEVICE_ID ||
	    mplat->device_id == QCN96XX_DEVICE_ID) {
		mhitest_pci_reg_read(mplat,
				     QCN9625_PCIE_PCIE_LOCAL_REG_REMAP_BAR_CTRL,
				     &pbl_sbl_err->remap_bar_ctrl);
		mhitest_pci_reg_read(mplat,
				     QCN9224_WLAON_SOC_RESET_CAUSE_SHADOW_REG,
				     &pbl_sbl_err->soc_rc_shadow_reg);
		mhitest_pci_reg_read(mplat,
				     QCN9224_PCIE_PCIE_PARF_LTSSM,
				     &pbl_sbl_err->parf_ltssm);
		mhitest_pci_reg_read(mplat,
				     QCN9625_GCC_RAMSS_CBCR,
				     &pbl_sbl_err->gcc_ramss_cbcr);
	}

	if (mhitest_pci_reg_read(mplat, sbl_data->sbl_log_start_reg,
				 &pbl_sbl_err->sbl_log_start)) {
		pr_err("Invalid SBL log data\n");
		return -EINVAL;
	}

	mhitest_pci_reg_read(mplat, pbl_data->tcsr_pbl_logging_reg,
			     &pbl_sbl_err->pbl_stage);
	mhitest_pci_reg_read(mplat, pbl_data->pbl_wlan_boot_cfg,
			     &pbl_sbl_err->pbl_wlan_boot_cfg);
	mhitest_pci_reg_read(mplat, pbl_data->pbl_bootstrap_status,
			     &pbl_sbl_err->pbl_bootstrap_status);

	return 0;
}

static void mhitest_debug_collect_bl_data(struct mhitest_platform *mplat,
					  struct pbl_reg_addr *pbl_data,
					  struct sbl_reg_addr *sbl_data,
					  struct dump_pbl_sbl_data *pbl_sbl_err)
{
	u32 sbl_log_size;
	u32 sbl_log_start;

	if (MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee)) {
		pr_err("already in Mission mode\n");
		return;
	}
	/* Dump SRAM content twice before and after the register dump as
	 * Q6 team requested. Please check with Q6 team before removing one
	 * of the SRAM dump.
	 */
	if (mhitest_debug_read_pbl_data(mplat, pbl_data,
					&pbl_sbl_err->pbl_data[0])) {
		pr_err("Failed to read PBL log data\n");
		return;
	}

	if (mhitest_debug_read_misc_data(mplat, pbl_data, sbl_data,
					 pbl_sbl_err)) {
		pr_err("Failed to read Misc log data\n");
		return;
	}

	if (mhitest_debug_read_noc_errors(mplat, pbl_sbl_err)) {
		pr_err("Failed to read NOC data\n");
		return;
	}

	if (mhitest_debug_read_pbl_data(mplat, pbl_data,
					&pbl_sbl_err->pbl_data[1])) {
		pr_err("Failed to read PBL log data\n");
		return;
	}

	if (mhitest_pci_reg_read(mplat, sbl_data->sbl_log_size_reg,
				 &sbl_log_size)) {
		pr_err("Invalid SBL log data\n");
		return;
	}

	sbl_log_start = pbl_sbl_err->sbl_log_start;
	sbl_log_size = ((sbl_log_size >> sbl_data->sbl_log_size_shift) &
			SBL_LOG_SIZE_MASK);
	if (sbl_log_start < sbl_data->sbl_sram_start ||
	    sbl_log_start > sbl_data->sbl_sram_end ||
	    (sbl_log_start + sbl_log_size) > sbl_data->sbl_sram_end) {
		pr_err("Invalid SBL log data\n");
		return;
	}

	if (mhitest_debug_read_sbl_data(mplat, sbl_log_size, sbl_log_start,
					pbl_sbl_err))
		pr_err("Failed to read SBL log data\n");
}

static void mhitest_debug_print_pbl_data(struct mhitest_platform *mplat,
					 struct pbl_err_data *pbl_data)
{
	int i;

	pr_err("Dumping PBL log data\n");
	for (i = 0; i < pbl_data->pbl_tbl_len; i++)
		pr_err("SRAM[0x%x] = 0x%x\n",
			pbl_data->pbl_reg_tbl[i],
			pbl_data->pbl_vals[i]);
}

static void mhitest_debug_print_sbl_data(struct mhitest_platform *mplat,
					 struct dump_pbl_sbl_data *pbl_sbl_err)
{

	pr_err("Dumping SBL log data\n");
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_OFFSET, 32, 4,
		       pbl_sbl_err->sbl_vals, pbl_sbl_err->sbl_len, 1);
}

static void mhitest_debug_print_noc_data(struct mhitest_platform *mplat,
					 struct dump_pbl_sbl_data *pbl_sbl_err)
{
	int i;

	pr_err("Dumping NOC log data\n");
	for (i = 0; noc_err_table_list[i].reg_name; i++)
		pr_err("%s: 0x%08x\n",
			noc_err_table_list[i].reg_name,
			pbl_sbl_err->noc_vals[i]);
}

static void mhitest_debug_print_bl_data(struct mhitest_platform *mplat,
					struct dump_pbl_sbl_data *pbl_sbl_err)
{

#if defined(CONFIG_PCIE_QCOM)
	pr_err("PCIE_CFG_PCIE_STATUS: 0x%08x\n",
		pbl_sbl_err->pcie_cfg_pcie_status);
#endif
	pr_err("PARF_PM_STTS: 0x%08x, PCIE_TYPE0_STATUS_COMMAND_REG: 0x%08x\n",
		pbl_sbl_err->parf_pm_stts,
		pbl_sbl_err->type0_status_cmd_reg);

	pr_err("PCIE_PCI_MSI_CAP_ID_NEXT_CTRL_REG: 0x%08x, PCIE_MSI_CAP_OFF_04H_REG: 0x%08x\n",
		pbl_sbl_err->pci_msi_cap_id_next_ctrl_reg,
		pbl_sbl_err->pci_msi_cap_off_04h_reg);
	pr_err("PCIE_MSI_CAP_OFF_08H_REG: 0x%08x, PCIE_MSI_CAP_OFF_0CH_REG: 0x%08x\n",
		pbl_sbl_err->pci_msi_cap_off_08h_reg,
		pbl_sbl_err->pci_msi_cap_off_0ch_reg);

	mhitest_debug_print_pbl_data(mplat, &pbl_sbl_err->pbl_data[0]);

	if (mplat->device_id == QCN92XX_DEVICE_ID ||
	    mplat->device_id == QCN95XX_DEVICE_ID ||
	    mplat->device_id == QCN96XX_DEVICE_ID) {
		pr_err("LOCAL_REG_REMAP_BAR_CTRL: 0x%08x, WLAON_SOC_RESET_CAUSE_SHADOW_REG: 0x%08x, PARF_LTSSM: 0x%08x\n",
			pbl_sbl_err->remap_bar_ctrl,
			pbl_sbl_err->soc_rc_shadow_reg,
			pbl_sbl_err->parf_ltssm);
		pr_err("GCC_RAMSS_CBCR: 0x%08x\n",
			pbl_sbl_err->gcc_ramss_cbcr);

		mhitest_debug_print_noc_data(mplat, pbl_sbl_err);
	}

	pr_err("TCSR_PBL_LOGGING: 0x%08x PCIE_BHI_ERRDBG: Start: 0x%08x\n",
		pbl_sbl_err->pbl_stage, pbl_sbl_err->sbl_log_start);
	pr_err("PBL_WLAN_BOOT_CFG: 0x%08x PBL_BOOTSTRAP_STATUS: 0x%08x\n",
		pbl_sbl_err->pbl_wlan_boot_cfg,
		pbl_sbl_err->pbl_bootstrap_status);

	mhitest_debug_print_pbl_data(mplat, &pbl_sbl_err->pbl_data[1]);

	pr_err("\n");
	mhitest_debug_print_sbl_data(mplat, pbl_sbl_err);
}

void mhitest_debug_cleanup_bl_data(struct dump_pbl_sbl_data *pbl_sbl_err)
{
	int i;

	for (i = 0; i < MAX_PBL_DATA_SNAPSHOT; i++) {
		kfree(pbl_sbl_err->pbl_data[i].pbl_vals);
		kfree(pbl_sbl_err->pbl_data[i].pbl_reg_tbl);
		pbl_sbl_err->pbl_data[i].pbl_vals = NULL;
		pbl_sbl_err->pbl_data[i].pbl_reg_tbl = NULL;
	}

	kfree(pbl_sbl_err->sbl_vals);
	kfree(pbl_sbl_err->noc_vals);
	pbl_sbl_err->sbl_vals = NULL;
	pbl_sbl_err->noc_vals = NULL;

	kfree(pbl_sbl_err);
}

/**
 * mhitest_debug_dump_bl_sram_mem - Dump WLAN FW bootloader debug log
 * @mplat: structure of mhitest platform driver
 *
 * Dump Primary and secondary bootloader debug log data. For SBL check the
 * log struct address and size for validity.
 *
 * Supported on QCN9224
 *
 * Return: None
 */
void mhitest_pci_dump_bl_sram_mem(struct mhitest_platform *mplat)
{
	struct sbl_reg_addr sbl_data = {0};
	struct pbl_reg_addr pbl_data = {0};
	struct dump_pbl_sbl_data *pbl_sbl_err = NULL;
	struct mhi_controller *mhi_ctrl = mplat->mhi_ctrl;
	int gfp = GFP_KERNEL;

	if (MHITEST_IN_MISSION_MODE(mplat->mhi_ctrl->ee)) {
		pr_err("already in Mission mode\n");
		return;
	}

	switch (mplat->device_id) {
	case QCN92XX_DEVICE_ID:
		sbl_data.sbl_sram_start = QCN9224_SRAM_START;
		sbl_data.sbl_sram_end = QCN9224_SRAM_END;
		sbl_data.sbl_log_size_reg = QCN9224_PCIE_BHI_ERRDBG3_REG;
		sbl_data.sbl_log_start_reg = QCN9224_PCIE_BHI_ERRDBG2_REG;
		sbl_data.sbl_log_size_shift = 0;
		if (mhi_ctrl->major_version == 2)
			pbl_data.pbl_log_sram_start =
				QCN9224_v2_PBL_LOG_SRAM_START;
		else
			pbl_data.pbl_log_sram_start =
				QCN9224_PBL_LOG_SRAM_START;

		pbl_data.pbl_log_sram_max_size = QCN9224_PBL_LOG_SRAM_MAX_SIZE;
		pbl_data.tcsr_pbl_logging_reg = QCN9224_TCSR_PBL_LOGGING_REG;
		pbl_data.pbl_wlan_boot_cfg = QCN9224_PBL_WLAN_BOOT_CFG;
		pbl_data.pbl_bootstrap_status = QCN9224_PBL_BOOTSTRAP_STATUS;
		break;
	case QCN95XX_DEVICE_ID:
	case QCN96XX_DEVICE_ID:
		sbl_data.sbl_sram_start = QCN9625_SRAM_START;
		sbl_data.sbl_sram_end = QCN9625_SRAM_END;
		sbl_data.sbl_log_size_reg = QCN9224_PCIE_BHI_ERRDBG3_REG;
		sbl_data.sbl_log_start_reg = QCN9224_PCIE_BHI_ERRDBG2_REG;
		sbl_data.sbl_log_size_shift = 0;

		/* Try to read PBL RAM information from TCSR_DEBUG_REG0 */
		if (mhitest_read_pbl_ram_info(mplat,
					     &pbl_data.pbl_log_sram_start,
					     &pbl_data.pbl_log_sram_max_size) != 0) {
			/* Fallback to default values if reading fails */
			pbl_data.pbl_log_sram_start = QCN9625_PBL_LOG_SRAM_START;
			pbl_data.pbl_log_sram_max_size = QCN9224_PBL_LOG_SRAM_MAX_SIZE;
			pr_info("Using default PBL RAM values: start=0x%x, size=%u\n",
				pbl_data.pbl_log_sram_start, pbl_data.pbl_log_sram_max_size);
		}

		pbl_data.tcsr_pbl_logging_reg = QCN9224_TCSR_PBL_LOGGING_REG;
		pbl_data.pbl_wlan_boot_cfg = QCN9625_PBL_WLAN_BOOT_CONFIG;
		pbl_data.pbl_bootstrap_status = QCN9224_PBL_BOOTSTRAP_STATUS;
		break;
	default:
		pr_err("Unknown device type 0x%lx\n",
			mplat->device_id);
		return;
	}

	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	pbl_sbl_err = kzalloc(sizeof(*pbl_sbl_err), gfp);
	if (!pbl_sbl_err)
		return;

	mhitest_debug_collect_bl_data(mplat, &pbl_data, &sbl_data,
				      pbl_sbl_err);
	mhitest_debug_print_bl_data(mplat, pbl_sbl_err);
	mhitest_debug_cleanup_bl_data(pbl_sbl_err);
}
