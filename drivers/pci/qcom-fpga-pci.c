// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * FPGA PCIe driver to access the FPGA via PCI
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/proc_fs.h>

#define MODULENAME			"qcom-fpga-pci"
#define PCIE_MEM_ACCESS_BASE_ADDR_REG	0x4

#define QTI_FPGA_PON_VENDOR_ID		0x10EE
#define QTI_FPGA_PON_DEVICE_ID		0x9021

struct qcom_fpga_pci_priv {
	void __iomem *mmio_addr_base;
	struct pci_dev *pci_dev;
	u32 last_prog_reg;
};

static struct qcom_fpga_pci_priv *qcom_fpga_pci;
static DEFINE_SPINLOCK(pci_lock);

void qcom_program_window(u32 reg)
{
	int retry = 10;

	if (qcom_fpga_pci->last_prog_reg == reg)
		return;

	writel(reg, qcom_fpga_pci->mmio_addr_base +
	       PCIE_MEM_ACCESS_BASE_ADDR_REG);

	while (retry--) {
		mdelay(10);
		if (readl(qcom_fpga_pci->mmio_addr_base +
			  PCIE_MEM_ACCESS_BASE_ADDR_REG) == reg)
			break;
	}

	qcom_fpga_pci->last_prog_reg = reg;
}

u32 qcom_fpga_mem_read(u32 reg)
{
	u32 base_addr_val, new_addr, val;
	unsigned long flags;

	spin_lock_irqsave(&pci_lock, flags);

	new_addr = (reg & 0xfffff) | 0x100000;
	base_addr_val = (reg & 0xfff00000) | 1;

	qcom_program_window(base_addr_val);

	val = readl(qcom_fpga_pci->mmio_addr_base + new_addr);

	spin_unlock_irqrestore(&pci_lock, flags);

	return val;
}
EXPORT_SYMBOL(qcom_fpga_mem_read);

void qcom_fpga_mem_write(u32 reg, u32 val)
{
	u32 base_addr_val, new_addr;
	unsigned long flags;

	spin_lock_irqsave(&pci_lock, flags);

	new_addr = (reg & 0xfffff) | 0x100000;
	base_addr_val = (reg & 0xfff00000) | 1;

	qcom_program_window(base_addr_val);

	writel(val, qcom_fpga_pci->mmio_addr_base + new_addr);

	spin_unlock_irqrestore(&pci_lock, flags);
}
EXPORT_SYMBOL(qcom_fpga_mem_write);

static ssize_t fpga_reg_write_store(struct device *device,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	u32 addr, val;

	if (sscanf(buf, "%X %X", &addr, &val) != 2)
		return -EINVAL;

	qcom_fpga_mem_write(addr, val);

	return count;
}

static ssize_t fpga_reg_read_store(struct device *device,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	u32 addr, val;

	if (kstrtouint(buf, 16, &addr))
		return -EINVAL;

	val = qcom_fpga_mem_read(addr);

	pr_info("\n0x%X: 0x%X\n", addr, val);
	return count;
}

static DEVICE_ATTR(fpga_reg_write, 0644, NULL, fpga_reg_write_store);
static DEVICE_ATTR(fpga_reg_read, 0644, NULL, fpga_reg_read_store);

static struct attribute *qcom_fpga_reg_wr_attrs[] = {
	&dev_attr_fpga_reg_write.attr,
	&dev_attr_fpga_reg_read.attr,
	NULL,
};

ATTRIBUTE_GROUPS(qcom_fpga_reg_wr);

static int fpga_pci_init(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int  region, ret = 0;
	void __iomem * const *iomap_table;

	qcom_fpga_pci = kmalloc(sizeof(*qcom_fpga_pci), GFP_KERNEL);
	if (!qcom_fpga_pci)
		return -ENOMEM;

	memset(qcom_fpga_pci, 0, sizeof(struct qcom_fpga_pci_priv));

	qcom_fpga_pci->pci_dev = pdev;

	/* Enable device (incl. PCI PM wakeup and hotplug setup) */
	ret = pcim_enable_device(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to Enable PCI device Error:%d\n",
			ret);
		return ret;
	}

	ret = pcim_set_mwi(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Mem-Wr-Inval unavailable Error:%d\n",
			ret);
		return ret;
	}

	/* Use the first MMIO region */
	region = ffs(pci_select_bars(pdev, IORESOURCE_MEM)) - 1;
	if (region < 0) {
		dev_err(&pdev->dev, "No MMIO resource found\n");
		return -ENODEV;
	}

	/* Check for weird/broken PCI region reporting */
	if (pci_resource_len(pdev, region) < 256) {
		dev_err(&pdev->dev, "Invalid PCI region size(s), aborting\n");
		return -ENODEV;
	}

	ret = pcim_iomap_regions(pdev, BIT(region), MODULENAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "cannot remap MMIO, aborting\n");
		return ret;
	}

	iomap_table = pcim_iomap_table(pdev);
	if (!iomap_table) {
		dev_err(&pdev->dev, "pci iomap allocation table failed\n");
		return -ENOMEM;
	}

	qcom_fpga_pci->mmio_addr_base = iomap_table[region];
	if (!qcom_fpga_pci->mmio_addr_base) {
		dev_err(&pdev->dev, "unable to map PCI I/O memory regions\n");
		return -ENOMEM;
	}

	return 0;
}

static void fpga_pci_remove(struct pci_dev *pdev)
{
	kfree(qcom_fpga_pci);
}

static const struct pci_device_id fpga_pci_tbl[] = {
	{QTI_FPGA_PON_VENDOR_ID, QTI_FPGA_PON_DEVICE_ID, PCI_ANY_ID,
		PCI_ANY_ID},
	{}
};

static struct pci_driver fpga_pci_driver = {
	.name		= MODULENAME,
	.id_table	= fpga_pci_tbl,
	.probe		= fpga_pci_init,
	.remove		= fpga_pci_remove,
	.dev_groups	= qcom_fpga_reg_wr_groups,
};

MODULE_DEVICE_TABLE(pci, fpga_pci_tbl);
module_pci_driver(fpga_pci_driver);
MODULE_LICENSE("GPL");
