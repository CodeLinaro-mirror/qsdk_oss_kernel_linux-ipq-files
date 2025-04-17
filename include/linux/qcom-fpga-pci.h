/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#ifndef _QCOM_FPGA_PCI_H
#define _QCOM_FPGA_PCI_H

#include <linux/regmap.h>

#define MODULENAME			"qcom-fpga-pci"
#define PCIE_MEM_ACCESS_BASE_ADDR_REG	0x4

#define QTI_FPGA_PON_VENDOR_ID		0x10EE
#define QTI_FPGA_PON_DEVICE_ID		0x9022

#define QCOM_GET_NEW_ADDR(reg)		(((reg) & 0xfffff) | 0x100000)
#define QCOM_GET_BASE_ADDR(reg)		(((reg) & 0xfff00000) | 1)

void qcom_fpga_mem_write(u32 reg, u32 val);
u32 qcom_fpga_mem_read(u32 reg);
int qcom_fpga_bulk_reg_write(u32 reg, const u32 *val,
			     size_t val_count);
int qcom_fpga_bulk_reg_read(u32 reg, u32 *val, size_t val_count);

int qcom_fpga_multi_reg_write(const struct reg_sequence *regs,
			      int num_regs);
int qcom_fpga_multi_reg_read(struct reg_sequence *regs, int num_regs);
#endif
