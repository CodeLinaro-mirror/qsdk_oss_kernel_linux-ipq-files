// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/firmware/qcom/qcom_scm.h>

#define MAX_REGISTERS 32

enum sysreg_access_type {
	SYSREG_ACCESS_NORMAL,
	SYSREG_ACCESS_SYSCON,
	SYSREG_ACCESS_SECURE,
};

struct sysreg_entry {
	const char *name;
	void __iomem *base;
	struct regmap *regmap;
	phys_addr_t phys_addr;
	u32 offset;
	u32 size;
	int bit_position;
	enum sysreg_access_type access_type;
	struct class_attribute class_attr;
};

/* Global variables */
static struct class *registers_class;
static struct sysreg_entry *sysreg_entries;
static int num_sysreg_entries;

static ssize_t sysreg_show(const struct class *class,
			   const struct class_attribute *attr, char *buf)
{
	struct sysreg_entry *entry = container_of(attr, struct sysreg_entry,
			class_attr);
	u32 val;
	int ret;

	/* Read register value based on access type */
	switch (entry->access_type) {
	case SYSREG_ACCESS_SYSCON:
		ret = regmap_read(entry->regmap, entry->offset, &val);
		if (ret) {
			pr_err("Regmap read failed for %s: %d\n",
			       entry->name, ret);
			return ret;
		}
		break;

	case SYSREG_ACCESS_SECURE:
		ret = qcom_scm_io_readl(entry->phys_addr, &val);
		if (ret) {
			pr_err("Secure read failed for %s: %d\n",
			       entry->name, ret);
			return ret;
		}
		break;

	case SYSREG_ACCESS_NORMAL:
	default:
		if (!entry->base)
			return -EINVAL;
		val = readl(entry->base);
		break;
	}

	/* Extract bit if bit-level access is configured */
	if (entry->bit_position >= 0)
		val = (val >> entry->bit_position) & 0x1;

	return sysfs_emit(buf, "0x%x\n", val);
}

static ssize_t sysreg_store(const struct class *class,
			    const struct class_attribute *attr,
			    const char *buf, size_t count)
{
	struct sysreg_entry *entry = container_of(attr, struct sysreg_entry,
						  class_attr);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	/* Handle bit-level access with read-modify-write */
	if (entry->bit_position >= 0) {
		if (val > 1) {
			pr_err("Invalid value %u for bit-level access\n", val);
			return -EINVAL;
		}
		u32 mask = BIT(entry->bit_position);
		u32 new_val = val ? mask : 0;

		switch (entry->access_type) {
		case SYSREG_ACCESS_SYSCON:
			ret = regmap_update_bits(entry->regmap, entry->offset,
						 mask, new_val);
			if (ret) {
				pr_err("Regmap RMW failed for %s: %d\n",
				       entry->name, ret);
				return ret;
			}
			break;

		case SYSREG_ACCESS_SECURE:
			ret = qcom_scm_io_rmw(entry->phys_addr, mask, new_val);
			if (ret) {
				pr_err("Secure RMW failed for %s: %d\n",
				       entry->name, ret);
				return ret;
			}
			break;

		case SYSREG_ACCESS_NORMAL:
		default:
			if (!entry->base)
				return -EINVAL;
			val = readl(entry->base);
			val = (val & ~mask) | new_val;
			writel(val, entry->base);
			break;
		}
	} else {
		/* Full register write */
		switch (entry->access_type) {
		case SYSREG_ACCESS_SYSCON:
			ret = regmap_write(entry->regmap, entry->offset, val);
			if (ret) {
				pr_err("Regmap write failed for %s: %d\n",
				       entry->name, ret);
				return ret;
			}
			break;

		case SYSREG_ACCESS_SECURE:
			ret = qcom_scm_io_writel(entry->phys_addr, val);
			if (ret) {
				pr_err("Secure write failed for %s: %d\n",
				       entry->name, ret);
				return ret;
			}
			break;

		case SYSREG_ACCESS_NORMAL:
		default:
			if (!entry->base)
				return -EINVAL;
			writel(val, entry->base);
			break;
		}
	}

	return count;
}

static int qcom_sysreg_parse_config(struct device *dev, int index,
				    struct sysreg_entry *entry)
{
	struct device_node *np = dev->of_node;
	u32 bit_val;
	const char *access_type_str;

	/* Read access type for this register (default: normal) */
	if (of_property_read_string_index(np, "reg-access-type",
					  index, &access_type_str) == 0) {
		if (strcmp(access_type_str, "syscon") == 0)
			entry->access_type = SYSREG_ACCESS_SYSCON;
		else if (strcmp(access_type_str, "secure") == 0)
			entry->access_type = SYSREG_ACCESS_SECURE;
		else
			entry->access_type = SYSREG_ACCESS_NORMAL;
	} else {
		entry->access_type = SYSREG_ACCESS_NORMAL;
	}

	/* Read bit position for this register (default: -1 for full register) */
	if (of_property_read_u32_index(np, "reg-bit-position", index, &bit_val) == 0)
		entry->bit_position = (int)bit_val;
	else
		entry->bit_position = -1;

	/* Validate bit position: must be -1 (full register) or 0-31 */
	if (entry->bit_position != -1 &&
	    (entry->bit_position < 0 || entry->bit_position > 31)) {
		dev_err(dev, "Invalid bit position %d for %s (must be -1 or 0-31)\n",
			entry->bit_position, entry->name);
		return -EINVAL;
	}

	return 0;
}

static int qcom_sysreg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	int i, ret, num_regs;
	int syscon_index = 0, secure_index = 0, normal_index = 0;

	/* Count number of registers */
	num_regs = of_property_count_strings(np, "reg-names");
	if (num_regs <= 0) {
		dev_err(dev, "No reg-names property found\n");
		return -EINVAL;
	}

	if (num_regs > MAX_REGISTERS) {
		dev_err(dev, "Too many registers (%d > %d)\n", num_regs, MAX_REGISTERS);
		return -EINVAL;
	}

	sysreg_entries = devm_kcalloc(dev, num_regs, sizeof(*sysreg_entries),
				      GFP_KERNEL);
	if (!sysreg_entries)
		return -ENOMEM;

	num_sysreg_entries = num_regs;

	/* Create /sys/class/registers/ */
	registers_class = class_create("registers");
	if (IS_ERR(registers_class)) {
		dev_err(dev, "Failed to create registers class\n");
		return PTR_ERR(registers_class);
	}

	/* Parse and map each register */
	for (i = 0; i < num_regs; i++) {
		const char *name;

		ret = of_property_read_string_index(np, "reg-names", i, &name);
		if (ret) {
			dev_err(dev, "Failed to read reg-name %d\n", i);
			goto err_cleanup;
		}

		sysreg_entries[i].name = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!sysreg_entries[i].name) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		/* Parse configuration (access type, bit position) */
		ret = qcom_sysreg_parse_config(dev, i, &sysreg_entries[i]);
		if (ret)
			goto err_cleanup;

		/* Setup access based on type */
		if (sysreg_entries[i].access_type == SYSREG_ACCESS_SYSCON) {
			/* IMEM/TCSR access via syscon/regmap */
			struct device_node *phandle_np;
			u32 offset;

			phandle_np = of_parse_phandle(np, "reg-phandle", syscon_index);
			if (!phandle_np) {
				dev_err(dev, "No reg-phandle for syscon register %s\n", name);
				ret = -EINVAL;
				goto err_cleanup;
			}

			sysreg_entries[i].regmap = syscon_node_to_regmap(phandle_np);
			of_node_put(phandle_np);

			if (IS_ERR(sysreg_entries[i].regmap)) {
				dev_err(dev, "Failed to get regmap for %s\n", name);
				ret = PTR_ERR(sysreg_entries[i].regmap);
				goto err_cleanup;
			}

			/* Get offset within syscon region */
			if (of_property_read_u32_index(np, "reg-offset", syscon_index, &offset)) {
				dev_err(dev, "No reg-offset for syscon register %s\n", name);
				ret = -EINVAL;
				goto err_cleanup;
			}
			sysreg_entries[i].offset = offset;
			sysreg_entries[i].size = 4;

			dev_dbg(dev, "Registered %s at offset 0x%x\n",
				name, offset);
			syscon_index++;

		} else if (sysreg_entries[i].access_type == SYSREG_ACCESS_SECURE) {
			/* Secure TCSR access via SCM */
			struct device_node *phandle_np;
			struct resource phandle_res;
			u32 offset;
			int phandle_index = syscon_index + secure_index;

			phandle_np = of_parse_phandle(np, "reg-phandle", phandle_index);
			if (!phandle_np) {
				dev_err(dev, "No reg-phandle for secure register %s\n", name);
				ret = -EINVAL;
				goto err_cleanup;
			}

			ret = of_address_to_resource(phandle_np, 0, &phandle_res);
			of_node_put(phandle_np);
			if (ret) {
				dev_err(dev, "Failed to get addr for reg %s\n",
					name);
				goto err_cleanup;
			}

			/* Get offset */
			if (of_property_read_u32_index(np, "reg-offset", phandle_index, &offset)) {
				dev_err(dev, "No reg-offset for secure register %s\n",
					name);
				ret = -EINVAL;
				goto err_cleanup;
			}

			/* Calculate physical address */
			sysreg_entries[i].phys_addr = phandle_res.start + offset;
			sysreg_entries[i].offset = offset;
			sysreg_entries[i].size = 4;

			dev_dbg(dev, "Registered %s at 0x%llx (secure)\n",
				name, (u64)sysreg_entries[i].phys_addr);
			secure_index++;

		} else {
			/* Normal access - get resource */
			res = platform_get_resource(pdev, IORESOURCE_MEM, normal_index);
			if (!res) {
				dev_err(dev, "Failed to get resource for %s\n", name);
				ret = -EINVAL;
				goto err_cleanup;
			}

			sysreg_entries[i].size = resource_size(res);
			sysreg_entries[i].phys_addr = res->start;
			sysreg_entries[i].base = devm_ioremap_resource(dev, res);
			if (IS_ERR(sysreg_entries[i].base)) {
				dev_err(dev, "Failed to map register %s\n", name);
				ret = PTR_ERR(sysreg_entries[i].base);
				goto err_cleanup;
			}

			dev_dbg(dev, "Registered %s at 0x%llx (normal)\n",
				name, (u64)res->start);
			normal_index++;
		}

		/* Create class attribute - this creates a file
		 * directly under /sys/class/registers/
		 */
		sysfs_attr_init(&sysreg_entries[i].class_attr.attr);
		sysreg_entries[i].class_attr.attr.name = sysreg_entries[i].name;
		sysreg_entries[i].class_attr.attr.mode = 0644;
		sysreg_entries[i].class_attr.show = sysreg_show;
		sysreg_entries[i].class_attr.store = sysreg_store;

		ret = class_create_file(registers_class, &sysreg_entries[i].class_attr);
		if (ret) {
			dev_err(dev, "Failed to create class file for %s\n", name);
			goto err_cleanup;
		}
	}

	return 0;

err_cleanup:
	/* Remove any created class files */
	while (i > 0) {
		i--;
		class_remove_file(registers_class, &sysreg_entries[i].class_attr);
	}
	class_destroy(registers_class);
	return ret;
}

static int qcom_sysreg_remove(struct platform_device *pdev)
{
	int i;

	/* Remove class files */
	for (i = 0; i < num_sysreg_entries; i++)
		class_remove_file(registers_class, &sysreg_entries[i].class_attr);

	class_destroy(registers_class);

	return 0;
}

static const struct of_device_id qcom_sysreg_of_match[] = {
	{ .compatible = "qcom,sysregister" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_sysreg_of_match);

static struct platform_driver qcom_sysreg_driver = {
	.probe = qcom_sysreg_probe,
	.remove = qcom_sysreg_remove,
	.driver = {
		.name = "qcom-sysregister",
		.of_match_table = qcom_sysreg_of_match,
	},
};

module_platform_driver(qcom_sysreg_driver);

MODULE_DESCRIPTION("Qualcomm System Register Driver");
MODULE_LICENSE("GPLv2");

