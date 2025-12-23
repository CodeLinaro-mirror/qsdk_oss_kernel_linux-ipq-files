// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019 Monolithic Power Systems, Inc
 * Author: Saravanan Sekar <sravanhome@gmail.com>
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/time.h>
#include "mp8899.h"

/**
 * enum mp8899_regulators - MP8899 buck converter identifiers
 * @MP8899_BUCK1: Buck converter 1
 * @MP8899_BUCK2: Buck converter 2
 * @MP8899_BUCK3: Buck converter 3
 * @MP8899_BUCK4: Buck converter 4
 * @MP8899_MAX_REGULATORS: Total number of regulators (used for array sizing)
 *
 * Enumeration of all buck converters supported by the MP8899 PMIC.
 */
enum mp8899_regulators {
	MP8899_BUCK1,
	MP8899_BUCK2,
	MP8899_BUCK3,
	MP8899_BUCK4,
	MP8899_MAX_REGULATORS,
};

/**
 * struct mp8899_regulator_info - MP8899 driver private data
 * @regmap: Regmap handle for I2C register access
 * @dev: Pointer to device structure
 * @rdesc: Pointer to array of regulator descriptors (dynamically allocated)
 *
 * This structure holds all driver-specific data for an MP8899 PMIC instance.
 */
struct mp8899_regulator_info {
	struct regmap *regmap;
	struct device *dev;
	struct regulator_desc *rdesc;
};

/**
 * mp8899_regmap_config - Regmap configuration for MP8899 I2C interface
 *
 * Configures the regmap subsystem for 8-bit register addresses and 8-bit
 * register values. Maximum register address is 0x2D. Cache is disabled
 * (REGCACHE_NONE) to ensure all register accesses go directly to hardware.
 */
static const struct regmap_config mp8899_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x2D,
	.cache_type = REGCACHE_NONE,
};

/* Linear voltage ranges for MP8899 buck converters */

/* VOUT_SELECT = 0: 0.5mV step mode
 * Hardware selector range: 800 to 4095
 * Voltage range: 400mV (0.4V) to 2047.5mV (2.0475V)
 * Step size: 0.5mV (500µV)
 */
static const struct linear_range mp8899_buck_ranges_500uv[] = {
	REGULATOR_LINEAR_RANGE(400000, 800, 4095, 500),
};

/* VOUT_SELECT = 1: 1.0mV step mode
 * Hardware selector range: 400 to 3600
 * Voltage range: 400mV (0.4V) to 3600mV (3.6V)
 * Step size: 1.0mV (1000µV)
 */
static const struct linear_range mp8899_buck_ranges_1000uv[] = {
	REGULATOR_LINEAR_RANGE(400000, 400, 3600, 1000),
};

/* Current limits array (in uA) */
static const unsigned int mp8899_current_limits[] = {
	2000000,	/* 2A valley current for 1A output */
	3000000,	/* 3A valley current for 2A output */
	4200000,	/* 4.2A valley current for 3A output */
	5000000,	/* 5A valley current for 4A output */
};

/**
 * mp8899_wait_for_go_bit_clear() - Wait for GO_BIT to clear after voltage change
 * @info: Pointer to mp8899_regulator_info structure
 * @buck_id: Buck converter ID (0-3 for BUCK1-BUCK4)
 * @timeout_ms: Timeout in milliseconds
 *
 * The GO_BIT mechanism is used to apply voltage changes. After writing new
 * voltage selector values to CTL4/CTL5 registers, the GO_BIT must be set in
 * CTL6 register. The hardware then transitions to the new voltage and clears
 * the GO_BIT when the transition is complete.
 *
 * Return: 0 on success, negative error code on timeout or I2C failure
 */
static int mp8899_wait_for_go_bit_clear(struct mp8899_regulator_info *info,
					int buck_id,
					unsigned int timeout_ms)
{
	unsigned int timeout_us = timeout_ms * USEC_PER_MSEC;
	unsigned int go_bit_reg;
	int ret;

	dev_dbg(info->dev, "Buck%d: Waiting for GO_BIT to clear\n", buck_id + 1);

	ret = regmap_read_poll_timeout(info->regmap,
				       MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL6),
				       go_bit_reg,
				       !(go_bit_reg & MP8899_GO_BIT_MASK),
				       MP8899_GO_BIT_POLL_INTERVAL_US,
				       timeout_us);

	if (ret) {
		dev_err(info->dev,
			"Buck%d: GO_BIT timeout after %dms\n",
			buck_id + 1,
			timeout_ms);

		return ret;
	}

	dev_dbg(info->dev, "Buck%d: GO_BIT cleared - voltage transition complete\n", buck_id + 1);

	return 0;
}

/**
 * mp8899_get_voltage_sel() - Read current voltage selector from hardware
 * @rdev: Regulator device
 *
 * Reads the 12-bit hardware voltage selector value from CTL4 (high 4 bits)
 * and CTL5 (low 8 bits) registers using bulk read.
 *
 * Hardware formula: Voltage (mV) = Selector × 0.5 (VOUT = VREF / 2)
 * For Mode 0 (0.5mV step): Valid selectors 800-4095 = 400mV-2047.5mV
 *
 * Return: Hardware selector value on success, negative error code on failure
 */
static int mp8899_get_voltage_sel(struct regulator_dev *rdev)
{
	struct mp8899_regulator_info *info = rdev_get_drvdata(rdev);
	unsigned int hw_sel;
	int buck_id = rdev_get_id(rdev);
	u8 regs[2];
	int ret;

	/* Bulk read CTL4 and CTL5 in single I2C transaction */
	ret = regmap_bulk_read(info->regmap, MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL4), regs, 2);
	if (ret) {
		dev_err(info->dev, "Buck%d: CTL4/CTL5 bulk read failed: %d\n", buck_id + 1, ret);
		return ret;
	}

	/* Combine to get 12-bit hardware selector value */
	hw_sel = ((regs[0] & MP8899_VREF_HIGH_MASK) << 8) | regs[1];

	dev_dbg(info->dev, "Buck%d: Read hw_sel=%u (CTL4=0x%02x, CTL5=0x%02x)\n",
		buck_id + 1, hw_sel, regs[0], regs[1]);

	/* Return hardware selector directly - linear_ranges will handle voltage calculation */
	return hw_sel;
}

/**
 * mp8899_set_voltage_sel() - Set voltage using hardware selector value
 * @rdev: Regulator device
 * @sel: Hardware selector value from linear range framework
 *
 * Sets the output voltage by writing the 12-bit selector value to CTL4/CTL5
 * registers and triggering a voltage transition using the GO_BIT mechanism.
 * Uses bulk write for improved I2C efficiency.
 *
 * Sequence:
 * 1. Prepare CTL4 (voltage high), CTL5 (voltage low), CTL6 (GO_BIT)
 * 2. Bulk write all three registers
 * 3. Wait for GO_BIT to clear (voltage transition complete)
 *
 * Return: 0 on success, negative error code on failure
 */
static int mp8899_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	struct mp8899_regulator_info *info = rdev_get_drvdata(rdev);
	int buck_id = rdev_get_id(rdev);
	u8 regs[3];
	int ret;

	/* sel is the hardware selector value from linear_ranges */
	dev_dbg(info->dev, "Buck%d: Setting hw_sel=%u\n", buck_id + 1, sel);

	/* Prepare all three registers for bulk write */
	regs[0] = (sel >> 8) & MP8899_VREF_HIGH_MASK;
	regs[1] = sel & 0xFF;
	regs[2] = MP8899_GO_BIT_MASK;

	dev_dbg(info->dev,
		"Buck%d: Bulk writing CTL4=0x%02x, CTL5=0x%02x, CTL6=0x%02x\n",
		buck_id + 1, regs[0], regs[1], regs[2]);

	/* Bulk write: CTL4, CTL5, CTL6 in single I2C transaction */
	ret = regmap_bulk_write(info->regmap, MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL4), regs, 3);
	if (ret) {
		dev_err(info->dev, "Buck%d: Bulk write failed: %d\n", buck_id + 1, ret);
		return ret;
	}

	/* Wait for GO_BIT to clear (indicating voltage transition complete) */
	ret = mp8899_wait_for_go_bit_clear(info, buck_id, MP8899_GO_BIT_TIMEOUT_MS);
	if (ret) {
		dev_err(info->dev,
			"Buck%d: Voltage transition failed - GO_BIT timeout\n",
			buck_id + 1);
		return ret;
	}

	dev_dbg(info->dev, "Buck%d: Voltage setting complete: hw_sel=%u [OK]\n", buck_id + 1, sel);

	return 0;
}

/**
 * mp8899_get_status() - Get regulator status including power good indication
 * @rdev: Regulator device
 *
 * Returns the current status of the regulator. First checks if the regulator
 * is enabled via SYSTEM1 register. If enabled, checks the power good status
 * from the STATUS register.
 *
 * Return: REGULATOR_STATUS_OFF, REGULATOR_STATUS_ON, or REGULATOR_STATUS_ERROR
 */
static int mp8899_get_status(struct regulator_dev *rdev)
{
	struct mp8899_regulator_info *info = rdev_get_drvdata(rdev);
	unsigned int system1, status, pg_mask;
	int buck_id = rdev_get_id(rdev);
	int ret;

	/* Check if regulator is enabled */
	ret = regmap_read(info->regmap, MP8899_SYSTEM1, &system1);
	if (ret)
		return ret;

	if (!(system1 & BIT(MP8899_EN_BASE - buck_id)))
		return REGULATOR_STATUS_OFF;

	/* Read STATUS register */
	ret = regmap_read(info->regmap, MP8899_STATUS, &status);
	if (ret)
		return ret;

	/* Check power good status for this buck (STATUS bits 7:4) */
	pg_mask = BIT(MP8899_PG_BIT_BASE - buck_id);

	return (status & pg_mask) ? REGULATOR_STATUS_ON : REGULATOR_STATUS_ERROR;
}

/**
 * mp8899_get_error_flags() - Read thermal error flags from STATUS register
 * @rdev: Regulator device
 * @flags: Pointer to store error flags
 *
 * Monitors thermal conditions by reading the STATUS register:
 * - Bit 3: Over-temperature warning (>120°C)
 * - Bit 2: Over-temperature protection (>160°C, device shutdown)
 *
 * Return: 0 on success, negative error code on I2C failure
 */
static int mp8899_get_error_flags(struct regulator_dev *rdev, unsigned int *flags)
{
	struct mp8899_regulator_info *info = rdev_get_drvdata(rdev);
	unsigned int status;
	int ret;

	*flags = 0;

	ret = regmap_read(info->regmap, MP8899_STATUS, &status);
	if (ret)
		return ret;

	dev_info(info->dev, "STATUS register: 0x%02x\n", status);

	/* Check thermal warning (STATUS bit 3) */
	if (status & MP8899_OT_WARNING_MASK) {
		*flags |= REGULATOR_ERROR_OVER_TEMP_WARN;
		dev_warn(info->dev, "Over-temperature warning detected (>120°C)\n");
	}

	/* Check thermal protection (STATUS bit 2) */
	if (status & MP8899_OT_PROTECTION_MASK) {
		*flags |= REGULATOR_ERROR_OVER_TEMP;
		dev_err(info->dev, "Over-temperature protection triggered (>160°C)\n");
	}

	return 0;
}

/* Forward declaration */
static int mp8899_parse_cb(struct device_node *np,
			   const struct regulator_desc *desc,
			   struct regulator_config *config);

/**
 * mp8899_buck_ops - Regulator operations for MP8899 buck converters
 *
 * Defines the set of operations supported by MP8899 buck converters:
 * - Enable/disable control via SYSTEM1 register
 * - Voltage control using linear ranges with GO_BIT mechanism
 * - Current limit configuration (2A/3A/4.2A/5A valley current)
 * - Active discharge control
 * - Soft start timing configuration
 * - Power good status monitoring
 * - Thermal error flag reporting
 */
static const struct regulator_ops mp8899_buck_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.get_voltage_sel = mp8899_get_voltage_sel,
	.set_voltage_sel = mp8899_set_voltage_sel,
	.set_active_discharge = regulator_set_active_discharge_regmap,
	.set_soft_start = regulator_set_soft_start_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
	.get_status = mp8899_get_status,
	.get_error_flags = mp8899_get_error_flags,
};

#define MP8899_BUCK_DESC(_name, _id) \
	[MP8899_BUCK##_id] = { \
		.id = MP8899_BUCK##_id, \
		.name = _name, \
		.of_match = _name, \
		.regulators_node = "regulators", \
		.of_parse_cb = mp8899_parse_cb, \
		.ops = &mp8899_buck_ops, \
		.type = REGULATOR_VOLTAGE, \
		.min_uV = MP8899_VREF_MIN_UV, \
		.n_voltages = MP8899_N_VOLTAGES, \
		.vsel_reg = MP8899_BUCK##_id##_CTL5, \
		.vsel_mask = 0xFF, \
		.apply_reg = MP8899_BUCK##_id##_CTL6, \
		.apply_bit = 7, \
		.enable_reg = MP8899_SYSTEM1, \
		.enable_mask = MP8899_EN##_id##_MASK, \
		.curr_table = mp8899_current_limits, \
		.n_current_limits = ARRAY_SIZE(mp8899_current_limits), \
		.csel_reg = MP8899_BUCK##_id##_CTL3, \
		.csel_mask = MP8899_CURRENT_LIMIT_MASK, \
		.active_discharge_reg = MP8899_BUCK##_id##_CTL1, \
		.active_discharge_mask = MP8899_VOUT_DIS_EN_MASK, \
		.soft_start_reg = MP8899_BUCK##_id##_CTL2, \
		.soft_start_mask = MP8899_SOFT_START_TIME_MASK, \
		.owner = THIS_MODULE, \
	}

static struct regulator_desc mp8899_regulators_desc[MP8899_MAX_REGULATORS] = {
	MP8899_BUCK_DESC("buck1", 1),
	MP8899_BUCK_DESC("buck2", 2),
	MP8899_BUCK_DESC("buck3", 3),
	MP8899_BUCK_DESC("buck4", 4),
};

/**
 * mp8899_parse_cb() - Parse device tree properties for each buck
 * @np: Device node pointer
 * @desc: Regulator descriptor
 * @config: Regulator configuration
 *
 * Callback function invoked during regulator registration to parse
 * buck-specific device tree properties including:
 * - Phase delay configuration
 * - Soft start timing
 * - Soft stop enable and timing
 * - OVP (Over-Voltage Protection) disable
 *
 * Return: 0 on success, negative error code on failure
 */
static int mp8899_parse_cb(struct device_node *np,
			   const struct regulator_desc *desc,
			   struct regulator_config *config)
{
	struct mp8899_regulator_info *info = config->driver_data;
	struct regulator_desc *rdesc;
	int buck_id = desc->id;
	int ret;
	u8 val;

	/* Read buck phase delay from DTS */
	ret = of_property_read_u8(np, "mps,buck-phase-delay", &val);
	if (!ret) {
		ret = regmap_update_bits(config->regmap,
					 MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL1),
					 MP8899_PHASE_DELAY_MASK,
					 (val & 3) << MP8899_PHASE_DELAY_SHIFT);
		if (ret) {
			dev_err(config->dev, "Failed to set phase delay for buck%d: %d\n",
				buck_id + 1, ret);
			return ret;
		}
	}

	/* Read buck soft start from DTS */
	ret = of_property_read_u8(np, "mps,buck-softstart", &val);
	if (!ret) {
		rdesc = &info->rdesc[buck_id];
		rdesc->soft_start_val_on = (val & 3) << MP8899_SOFT_START_TIME_SHIFT;
	}

	/* Read buck soft stop enable and configuration from DTS */
	if (of_property_read_bool(np, "mps,buck-softstop-enable")) {
		/* Enable soft stop */
		ret = regmap_update_bits(config->regmap,
					 MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL2),
					 MP8899_SOFT_STOP_EN_MASK,
					 MP8899_SOFT_STOP_EN_MASK);
		if (ret) {
			dev_err(config->dev, "Failed to enable soft stop for buck%d: %d\n",
				buck_id + 1, ret);
			return ret;
		}

		/* Read soft stop timing configuration */
		ret = of_property_read_u8(np, "mps,buck-softstop", &val);
		if (!ret) {
			ret = regmap_update_bits(config->regmap,
						 MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL2),
						 MP8899_SOFT_STOP_TIME_MASK,
						 val & 3);
			if (ret) {
				dev_err(config->dev, "Failed to set soft stop timing for buck%d: %d\n",
					buck_id + 1, ret);
				return ret;
			}
		}
	}

	/* OVP disable configuration */
	if (of_property_read_bool(np, "mps,buck-ovp-disable")) {
		ret = regmap_update_bits(config->regmap,
					 MP8899_BUCK_REG(buck_id, MP8899_BUCK1_CTL1),
					 MP8899_VOUT_OVP_EN_MASK, 0);
		if (ret) {
			dev_err(config->dev, "Failed to disable OVP for buck%d: %d\n",
				buck_id + 1, ret);
			return ret;
		}
		dev_info(config->dev, "OVP disabled for buck%d\n", buck_id + 1);
	}

	return 0;
}

/**
 * mp8899_parse_dt() - Parse global device tree properties
 * @dev: Device pointer
 * @info: Pointer to mp8899_regulator_info structure
 *
 * Parses global device tree properties that apply to all buck converters,
 * such as switching frequency configuration.
 */
static void mp8899_parse_dt(struct device *dev,
			    struct mp8899_regulator_info *info)
{
	struct device_node *np = dev->of_node;
	int ret;
	u8 freq;

	np = of_get_child_by_name(np, "regulators");
	if (!np) {
		dev_err(dev, "missing 'regulators' subnode in DT\n");
		return;
	}

	/* Read switching frequency from DTS */
	ret = of_property_read_u8(np, "mps,switch-freq", &freq);
	if (!ret) {
		ret = regmap_update_bits(info->regmap, MP8899_SYSTEM2,
					 MP8899_FREQ_MASK,
					 (freq & 7) << 5);
		if (ret)
			dev_err(dev, "Failed to set switching frequency: %d\n", ret);
	}

	of_node_put(np);
}

/**
 * mp8899_identify_device() - Verify MP8899 device presence
 * @info: Pointer to mp8899_regulator_info structure
 *
 * Reads and validates the vendor ID from SYSTEM4 register to confirm
 * the device is a genuine MP8899 PMIC.
 *
 * Return: 0 on success, -ENODEV if vendor ID doesn't match
 */
static int mp8899_identify_device(struct mp8899_regulator_info *info)
{
	unsigned int vendor_id;
	int ret;

	ret = regmap_read(info->regmap, MP8899_SYSTEM4, &vendor_id);
	if (ret) {
		dev_err(info->dev, "Failed to read vendor ID: %d\n", ret);
		return ret;
	}

	vendor_id = (vendor_id & MP8899_VENDOR_ID_MASK) >> 4;
	if (vendor_id != MP8899_VENDOR_ID_VALUE) {
		dev_err(info->dev, "Invalid vendor ID: 0x%x\n", vendor_id);
		return -ENODEV;
	}

	dev_dbg(info->dev, "PMIC MP8899 device detected\n");
	return 0;
}

/**
 * mp8899_i2c_probe() - I2C driver probe function
 * @client: I2C client device
 *
 * Initializes the MP8899 PMIC driver:
 * 1. Allocates driver data structures
 * 2. Initializes I2C regmap interface
 * 3. Verifies device identity
 * 4. Parses device tree configuration
 * 5. Read the BUCK1_CTL3 register of each buck and configure the linear ranges accordingly
 * 6. Registers regulator devices
 *
 * Return: 0 on success, negative error code on failure
 */
static int mp8899_i2c_probe(struct i2c_client *client)
{
	struct mp8899_regulator_info *info;
	struct regulator_config config = {};
	struct device *dev = &client->dev;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int vout_select;
	int i, ret;

	info = devm_kzalloc(dev, sizeof(struct mp8899_regulator_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	/* Allocate separate regulator_desc array for dynamic configuration */
	info->rdesc = devm_kmemdup(dev, mp8899_regulators_desc,
				   sizeof(mp8899_regulators_desc), GFP_KERNEL);
	if (!info->rdesc)
		return -ENOMEM;

	info->dev = dev;

	regmap = devm_regmap_init_i2c(client, &mp8899_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to allocate regmap\n");

	info->regmap = regmap;
	i2c_set_clientdata(client, info);

	/* Identify the device */
	ret = mp8899_identify_device(info);
	if (ret)
		return ret;

	/* Parse device tree properties */
	if (client->dev.of_node)
		mp8899_parse_dt(&client->dev, info);

	/* Configure linear ranges for each buck based on VOUT_SELECT */
	for (i = 0; i < MP8899_MAX_REGULATORS; i++) {
		ret = regmap_read(info->regmap,
				  MP8899_BUCK_REG(i, MP8899_BUCK1_CTL3),
				  &vout_select);
		if (ret) {
			dev_err(dev, "Failed to read VOUT_SELECT for buck%d: %d\n", i + 1, ret);
			return ret;
		}

		if (vout_select & MP8899_VOUT_SELECT_MASK) {
			/* 1.0mV step mode */
			info->rdesc[i].linear_ranges = mp8899_buck_ranges_1000uv;
			info->rdesc[i].n_linear_ranges = ARRAY_SIZE(mp8899_buck_ranges_1000uv);
			info->rdesc[i].n_voltages = MP8899_N_VOLTAGES_1MV;  /* 3201 voltages */
			dev_dbg(dev, "Buck%d: 1.0mV step mode\n", i + 1);
		} else {
			/* 0.5mV step mode */
			info->rdesc[i].linear_ranges = mp8899_buck_ranges_500uv;
			info->rdesc[i].n_linear_ranges = ARRAY_SIZE(mp8899_buck_ranges_500uv);
			info->rdesc[i].n_voltages = MP8899_N_VOLTAGES;  /* 3296 voltages */
			dev_dbg(dev, "Buck%d: 0.5mV step mode\n", i + 1);
		}
	}

	config.dev = dev;
	config.regmap = regmap;
	config.driver_data = info;

	/* Register regulators */
	for (i = 0; i < MP8899_MAX_REGULATORS; i++) {
		rdev = devm_regulator_register(dev, &info->rdesc[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev,
					     PTR_ERR(rdev),
					     "Failed to register regulator %d\n",
					     i);
	}

	dev_info(dev, "MP8899 PMIC regulator driver registered successfully\n");

	return 0;
}

static const struct of_device_id mp8899_of_match[] = {
	{ .compatible = "mps,mp8899" },
	{},
};
MODULE_DEVICE_TABLE(of, mp8899_of_match);

static const struct i2c_device_id mp8899_id[] = {
	{ "mp8899", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, mp8899_id);

static struct i2c_driver mp8899_regulator_driver = {
	.driver = {
		.name = "mp8899",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = mp8899_of_match,
	},
	.probe = mp8899_i2c_probe,
	.id_table = mp8899_id,
};

module_i2c_driver(mp8899_regulator_driver);

MODULE_DESCRIPTION("MP8899 PMIC regulator driver");
MODULE_LICENSE("GPL");
