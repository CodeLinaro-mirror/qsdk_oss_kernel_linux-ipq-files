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
#include <linux/of.h>
#include <linux/platform_device.h>

#include <linux/qcom-fpga-pci.h>

struct qcom_soc_reg_cfg {
	u64 start;
	u64 end;
	u64 offset;
};

struct qcom_fpga_pci_priv {
	void __iomem *mmio_addr_base;
	struct pci_dev *pci_dev;
	u64 last_prog_reg;
	struct qcom_soc_reg_cfg *soc_cfg;
	size_t soc_cfg_cnt;
	bool use_soc_cfg;
};

static struct qcom_fpga_pci_priv *qcom_fpga_pci;
static DEFINE_SPINLOCK(pci_lock);
static DEFINE_MUTEX(init_lock);

void qcom_program_window(u64 reg)
{
	int retry = 100000;

	if (!qcom_fpga_pci) {
		pr_warn_once("%s: PCI device not initialized\n", __func__);
		return;
	}

	if (qcom_fpga_pci->last_prog_reg == reg)
		return;

	writel(reg, qcom_fpga_pci->mmio_addr_base +
	       PCIE_MEM_ACCESS_BASE_ADDR_REG);

	while (retry--) {
		udelay(1);
		if (readl(qcom_fpga_pci->mmio_addr_base +
			  PCIE_MEM_ACCESS_BASE_ADDR_REG) == reg)
			break;
	}

	qcom_fpga_pci->last_prog_reg = reg;
}

static bool qcom_soc_cfg_translate(u64 reg, u64 *new_addr)
{
	size_t i;
	u64 start, end, offset;

	if (!qcom_fpga_pci) {
		pr_warn_once("%s: PCI device not initialized\n", __func__);
		return false;
	}

	for (i = 0; i < qcom_fpga_pci->soc_cfg_cnt; i++) {
		start = qcom_fpga_pci->soc_cfg[i].start;
		end = qcom_fpga_pci->soc_cfg[i].end;
		offset = qcom_fpga_pci->soc_cfg[i].offset;

		if (reg >= start && reg <= end) {
			u64 reg_offset = reg - start;
			/* Check for overflow before addition */
			if (offset > (U64_MAX - reg_offset))
				return false;
			*new_addr = offset + reg_offset;
			return true;
		}
	}
	return false;
}

static void qcom_parse_soc_reg_config(struct device *dev)
{
	const __be32 *prop;
	int len, i, cnt;

	if (!qcom_fpga_pci) {
		pr_warn_once("%s: PCI device not initialized\n", __func__);
		return;
	}

	qcom_fpga_pci->use_soc_cfg = false;

	prop = of_get_property(dev->of_node, "qcom,soc-reg-config", &len);
	if (!prop)
		return;

	len /= sizeof(__be32);

	if (len % 3) {
		dev_err(dev, "Invalid qcom,soc-reg-config length\n");
		return;
	}

	cnt = len / 3;

	if (!qcom_fpga_pci->soc_cfg) {
		qcom_fpga_pci->soc_cfg = devm_kmalloc_array(dev, cnt,
							    sizeof(*qcom_fpga_pci->soc_cfg),
							    GFP_KERNEL);
		if (!qcom_fpga_pci->soc_cfg) {
			dev_err(dev, "qcom_fpga_pci->soc_cfg memory allocation failed\n");
			return;
		}
	}

	qcom_fpga_pci->soc_cfg_cnt = cnt;

	for (i = 0; i < cnt; i++) {
		u64 start  = be32_to_cpup(&prop[i * 3]);
		u64 size   = be32_to_cpup(&prop[i * 3 + 1]);
		u64 offset = be32_to_cpup(&prop[i * 3 + 2]);

		/* Check for overflow in end address calculation */
		if (size == 0 || size > (U64_MAX - start)) {
			dev_err(dev, "Invalid qcom,soc-reg-config: address range overflow\n");
			devm_kfree(dev, qcom_fpga_pci->soc_cfg);
			qcom_fpga_pci->soc_cfg = NULL;
			qcom_fpga_pci->soc_cfg_cnt = 0;
			qcom_fpga_pci->use_soc_cfg = false;
			return;
		}

		qcom_fpga_pci->soc_cfg[i].start  = start;
		qcom_fpga_pci->soc_cfg[i].end    = start + size;
		qcom_fpga_pci->soc_cfg[i].offset = offset;
	}

	qcom_fpga_pci->use_soc_cfg = true;
}

/**
 * qcom_fpga_mem_read() - Read the value from the FPGA register through PCI
 *
 * @reg : FPGA register value
 *
 * The read value will be returned on success, a negative errno will be
 * returned in error cases.
 */
u64 qcom_fpga_mem_read(u64 reg)
{
	u64 base_addr, new_addr, val;
	unsigned long flags;

	if (!qcom_fpga_pci) {
		pr_warn_once("qcom_fpga_mem_read: PCI device not initialized\n");
		return ~0ULL;
	}

	spin_lock_irqsave(&pci_lock, flags);

	if (qcom_fpga_pci->use_soc_cfg) {
		if (!qcom_soc_cfg_translate(reg, &new_addr)) {
			spin_unlock_irqrestore(&pci_lock, flags);
			pr_warn_once("Register value is not within qcom,soc-reg-config range\n");
			return ~0ULL;
		}
	} else {
		new_addr = QCOM_GET_NEW_ADDR(reg);
		base_addr = QCOM_GET_BASE_ADDR(reg);
		qcom_program_window(base_addr);
	}

	val = readl(qcom_fpga_pci->mmio_addr_base + new_addr);

	spin_unlock_irqrestore(&pci_lock, flags);

	return val;
}
EXPORT_SYMBOL(qcom_fpga_mem_read);

/**
 * qcom_fpga_mem_write() - Write the value to the FPGA register through PCI
 *
 * @reg : FPGA register value
 * @val : Value to be written in FPGA register
 *
 */
void qcom_fpga_mem_write(u64 reg, u64 val)
{
	u64 base_addr, new_addr;
	unsigned long flags;

	if (!qcom_fpga_pci) {
		pr_warn_once("qcom_fpga_mem_write: PCI device not initialized\n");
		return;
	}

	spin_lock_irqsave(&pci_lock, flags);

	if (qcom_fpga_pci->use_soc_cfg) {
		if (!qcom_soc_cfg_translate(reg, &new_addr)) {
			spin_unlock_irqrestore(&pci_lock, flags);
			pr_warn_once("Register value is not within qcom,soc-reg-config range\n");
			return;
		}
	} else {
		new_addr = QCOM_GET_NEW_ADDR(reg);
		base_addr = QCOM_GET_BASE_ADDR(reg);
		qcom_program_window(base_addr);
	}

	writel(val, qcom_fpga_pci->mmio_addr_base + new_addr);

	spin_unlock_irqrestore(&pci_lock, flags);
}
EXPORT_SYMBOL(qcom_fpga_mem_write);

/**
 * qcom_fpga_bulk_reg_write() - Sequentially write the bulk values to the
 *                              FPGA registers through PCI
 *
 * @reg : Start of the FPGA register value for the sequential write
 * @val : Array of values to be written in FPGA register
 * @val_count : Number of values to be written
 *
 */
u64 qcom_fpga_bulk_reg_write(u64 reg, const u64 *val,
			     size_t val_count)
{
	u64 i;

	for (i = 0; i < val_count; i++) {
		qcom_fpga_mem_write(reg, val[i]);
		reg += 4;
	}

	return val_count;
}
EXPORT_SYMBOL(qcom_fpga_bulk_reg_write);

/**
 * qcom_fpga_bulk_reg_read() - Sequentially read the bulk values from the
 *                             FPGA registers through PCI
 *
 * @reg : Start of the FPGA register value for the sequential read
 * @val : Array of values to be read from the FPGA register
 * @val_count : Number of values to be read
 *
 */
u64 qcom_fpga_bulk_reg_read(u64 reg, u64 *val, size_t val_count)
{
	u64 i;

	for (i = 0; i < val_count; i++) {
		val[i] = qcom_fpga_mem_read(reg);
		reg += 4;
	}

	return val_count;
}
EXPORT_SYMBOL(qcom_fpga_bulk_reg_read);

/**
 * qcom_fpga_multi_reg_write() - Write the multiple FPGA registers through PCI
 *
 * @regs : Array of structures containing register,value to be written
 * @num_regs : Number of registers to write
 *
 */
u64 qcom_fpga_multi_reg_write(const struct reg_sequence *regs,
			      u64 num_regs)
{
	u64 i;

	for (i = 0; i < num_regs; i++) {
		qcom_fpga_mem_write(regs[i].reg, regs[i].def);
		if (regs[i].delay_us)
			udelay(regs[i].delay_us);
	}

	return num_regs;
}
EXPORT_SYMBOL(qcom_fpga_multi_reg_write);

/**
 * qcom_fpga_multi_reg_read() - Read the multiple FPGA registers through PCI
 *
 * @regs : Array of structures containing register,value to be read
 * @num_regs : Number of registers to read
 *
 */
u64 qcom_fpga_multi_reg_read(struct reg_sequence *regs, u64 num_regs)
{
	u64 i;

	for (i = 0; i < num_regs; i++) {
		regs[i].def = qcom_fpga_mem_read(regs[i].reg);
		if (regs[i].delay_us)
			udelay(regs[i].delay_us);
	}

	return num_regs;
}
EXPORT_SYMBOL(qcom_fpga_multi_reg_read);

static ssize_t fpga_reg_write_store(struct device *device,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	u64 addr, val;

	if (sscanf(buf, "%llX %llX", &addr, &val) != 2)
		return -EINVAL;

	qcom_fpga_mem_write(addr, val);

	return count;
}

static ssize_t fpga_reg_read_store(struct device *device,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	u64 addr, val;

	if (kstrtoull(buf, 16, &addr))
		return -EINVAL;

	val = qcom_fpga_mem_read(addr);

	pr_info("\n0x%llX: 0x%llX\n", addr, val);
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

static void qcom_memnoc_configuration(struct device *dev)
{
	int len, i;
	const __be32 *prop;
	u32 addr, val;

	prop = of_get_property(dev->of_node, "qcom,memnoc-config", &len);
	if (!prop)
		return;

	len /= sizeof(__be32);

	if (len % 2) {
		dev_err(dev, "Invalid qcom,memnoc-config length\n");
		return;
	}

	for (i = 0; i < len; i += 2) {
		addr = be32_to_cpup(&prop[i]);
		val = be32_to_cpup(&prop[i + 1]);
		qcom_fpga_mem_write(addr, val);
	}

	dev_info(dev, "MEMNOC configuration done");
}

struct device *get_pcie_controller_dev(struct pci_dev *pdev)
{
	struct pci_host_bridge *bridge;

	if (!pdev || !pdev->bus)
		return NULL;

	bridge = pci_find_host_bridge(pdev->bus);
	if (!bridge)
		return NULL;

	/*
	 * Example sysfs path:
	 * /sys/devices/platform/soc@0/28000000.pci/pci0000:00/0000:00:00.0/
	 * 0000:01:00.0/fpga_reg_read
	 *
	 * In this hierarchy:
	 *   - bridge->dev corresponds to "pci0000:00" (the PCI host bridge device)
	 *   - Its parent is the platform device representing the PCIe controller ("28000000.pci")
	 */

	return bridge->dev.parent;
}

static int fpga_pci_init(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int  region, ret = 0;
	void __iomem * const *iomap_table;
	resource_size_t bar_phys;
	resource_size_t bar_size;
	struct device *dev;
	struct qcom_fpga_pci_priv *priv;

	mutex_lock(&init_lock);
	if (qcom_fpga_pci) {
		dev_err(&pdev->dev, "Driver already initialized\n");
		mutex_unlock(&init_lock);
		return -EBUSY;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		mutex_unlock(&init_lock);
		goto err;
	}
	qcom_fpga_pci = priv;
	mutex_unlock(&init_lock);

	qcom_fpga_pci->pci_dev = pdev;

	/* Enable device (incl. PCI PM wakeup and hotplug setup) */
	ret = pcim_enable_device(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to Enable PCI device Error:%d\n",
			ret);
		goto err;
	}

	ret = pcim_set_mwi(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Mem-Wr-Inval unavailable Error:%d\n",
			ret);
		goto err;
	}

	/* Use the first MMIO region */
	region = ffs(pci_select_bars(pdev, IORESOURCE_MEM)) - 1;
	if (region < 0) {
		dev_err(&pdev->dev, "No MMIO resource found\n");
		ret = -ENODEV;
		goto err;
	}

	/* Check for weird/broken PCI region reporting */
	if (pci_resource_len(pdev, region) < 256) {
		dev_err(&pdev->dev, "Invalid PCI region size(s), aborting\n");
		ret = -ENODEV;
		goto err;
	}

	ret = pcim_iomap_regions(pdev, BIT(region), MODULENAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "cannot remap MMIO, aborting\n");
		goto err;
	}

	iomap_table = pcim_iomap_table(pdev);
	if (!iomap_table) {
		dev_err(&pdev->dev, "pci iomap allocation table failed\n");
		ret = -ENOMEM;
		goto err;
	}

	qcom_fpga_pci->mmio_addr_base = iomap_table[region];
	if (!qcom_fpga_pci->mmio_addr_base) {
		dev_err(&pdev->dev, "unable to map PCI I/O memory regions\n");
		ret = -ENOMEM;
		goto err;
	}

	pci_set_master(pdev);

	/* Get physical BAR base and size */
	bar_phys = pci_resource_start(pdev, region);
	bar_size = pci_resource_len(pdev, region);
	dev_info(&pdev->dev, "BAR%u phys=%pa size=%pa vaddr=%p\n", region,
		 &bar_phys, &bar_size, qcom_fpga_pci->mmio_addr_base);

	dev = get_pcie_controller_dev(pdev);
	if (!dev) {
		pr_warn_once("%s: pcie controller dev is NULL\n", __func__);
		ret = -ENXIO;
		goto err;
	}

	if (of_property_read_bool(dev->of_node, "enable-soc-reg-config")) {
		dev_info(&pdev->dev, "enable-soc-reg-config found");
		qcom_parse_soc_reg_config(dev);
	} else {
		qcom_memnoc_configuration(dev);
	}

	return ret;
err:
	qcom_fpga_pci = NULL;
	return ret;
}

static void fpga_pci_remove(struct pci_dev *pdev)
{
	mutex_lock(&init_lock);
	qcom_fpga_pci = NULL;
	mutex_unlock(&init_lock);
	/* Memory allocated with devm_kmalloc is automatically freed */
}

static const struct pci_device_id fpga_pci_tbl[] = {
	{ PCI_DEVICE(QTI_FPGA_PON_VENDOR_ID_1, QTI_FPGA_PON_DEVICE_ID_1) },
	{ PCI_DEVICE(QTI_FPGA_PON_VENDOR_ID_1, QTI_FPGA_PON_DEVICE_ID_2) },
	{ PCI_DEVICE(QTI_FPGA_PON_VENDOR_ID_1, QTI_FPGA_PON_DEVICE_ID_3) },
	{ PCI_DEVICE(QTI_FPGA_PON_VENDOR_ID_1, QTI_FPGA_PON_DEVICE_ID_4) },
	{ PCI_DEVICE(QTI_FPGA_PON_VENDOR_ID_2, QTI_FPGA_PON_DEVICE_ID_5) },
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
