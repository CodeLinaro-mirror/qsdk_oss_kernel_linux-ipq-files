// SPDX-License-Identifier: ISC
/*
 * qcom-open-loop-cpr-regulator.c - Qualcomm Open Loop CPR Regulator Driver
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * This driver implements open loop Core Power Reduction (CPR) voltage
 * regulation for the Qualcomm SoC's.
 * It reads fuse correction values from eFuse registers via NVMEM and
 * calculates the final operating voltages for a rail's operating modes
 * (SVS, NOM, TUR for APC; SVS, SVS_L1, NOM, TUR for NSP CX/MX).
 * The calculated voltages are supplied through the regulator set_voltage()
 * callback when invoked by the CPUFREQ framework during dynamic voltage
 * and frequency scaling (DVFS) operations.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/coupler.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/qcom-gpio-regulator.h>

#include <linux/power/qcom/apm.h>

#include "internal.h"

/*
 * MAX_MODES - Largest number of operating modes any rail uses.
 *
 * Rails do not share a single mode order: APC uses 3 modes (SVS, NOM, TUR)
 * while NSP CX/MX use 4 modes (SVS, SVS_L1, NOM, TUR). There is no global
 * semantic index map - each rail's mode names and count come from its own
 * open_loop_cpr_regulator_params (mode_names[], num_fuses). MAX_MODES is
 * only used to size the fixed-length arrays below and as an upper bound
 * on any rail's num_fuses.
 */
#define MAX_MODES 4

/**
 * struct fuse_params - NVMEM fuse parameters for voltage calculation
 * @bit_len: Number of bits in the fuse value
 * @reference_volt: Reference (ceiling) voltage in microvolts
 * @step_volt: Voltage step size in microvolts per fuse unit
 * @floor_volt: Minimum allowed voltage in microvolts for this mode
 * @ceiling_volt: Maximum allowed voltage in microvolts for this mode
 */
struct fuse_params {
	int bit_len;
	int reference_volt;
	int step_volt;
	int floor_volt;
	int ceiling_volt;
};

/**
 * struct voltage_config - Voltage configuration parameters
 * @voltage_table: Pointer to array of ceiling voltages per operating mode.
 *                 Array length is determined at runtime by the num_fuses
 *                 device tree property, with one entry per operating mode.
 * @num_thresholds: Number of voltage thresholds.
 * @thresholds: Array of voltage thresholds in microvolts
 */
struct voltage_config {
	const int *voltage_table;
	u8 num_thresholds;
	const int *thresholds;
};

/**
 * struct open_loop_cpr_regulator_params - Open loop CPR regulator parameters
 * @fuse_params: Pointer to array of fuse parameters, one entry per mode
 * @voltage_config: Pointer to voltage configuration structure
 * @mode_names: Array of mode name strings, one entry per mode, in the same
 *              order as fuse_params/voltage_table (e.g. APC: {"svs", "nom",
 *              "tur"}; NSP CX/MX: {"svs", "svs_l1", "nom", "tur"})
 * @num_fuses: Number of fuses (equals number of operating modes)
 * @part_type_supported: Whether part type differentiation is supported
 * @mx_rail_available: property to state the availability of mx rail
 * @apm_supported: Whether this rail drives APM switching.
 */
struct open_loop_cpr_regulator_params {
	const struct fuse_params *fuse_params;
	const struct voltage_config *voltage_config;
	const char * const *mode_names;
	u8 num_fuses;
	bool part_type_supported;
	bool mx_rail_available;
	bool apm_supported;
};

/**
 * struct reg_info - Private data for the open loop CPR voltage regulator
 * @voltage_table: Final fuse-corrected voltage per operating mode in
 *                 microvolts, indexed by mode (0..num_modes-1)
 * @floor_table: Minimum allowed voltage per operating mode in microvolts,
 *               indexed by mode (0..num_modes-1)
 * @ceiling_table: Maximum allowed voltage per operating mode in microvolts,
 *                 indexed by mode (0..num_modes-1)
 * @mode_names: Rail-specific mode name strings, same array/order as
 *              open_loop_cpr_regulator_params.mode_names
 * @num_modes: Number of operating modes this rail actually uses (equals
 *             num_fuses at process time), used as the bound for mode
 *             indices instead of the global MAX_MODES
 * @base_rdev: Pointer to the regulator_dev of the supply regulator, used
 *             to call regulator_set_voltage_rdev() from within ops callbacks
 *             to avoid recursive locking deadlocks
 * @current_voltage: Current mode index, -1 if not set
 * @apm: Handle to the APM controller device
 * @apm_threshold_volt: Voltage threshold in microvolts for APM supply switching
 * @apm_high_supply: APM supply to use when voltage >= apm_threshold_volt
 * @apm_low_supply: APM supply to use when voltage < apm_threshold_volt
 * @apm_park_switch: True for park-then-switch APM handling (PMIC-backed
 *                    rails), false for order-change handling with no park
 *                    step (GPIO-backed rails)
 * @last_volt: Last voltage set in microvolts, used for APM threshold crossing
 */
struct reg_info {
	int voltage_table[MAX_MODES];
	int floor_table[MAX_MODES];
	int ceiling_table[MAX_MODES];
	const char * const *mode_names;
	u8 num_modes;
	struct regulator_dev *base_rdev;
	int current_voltage;
	struct msm_apm_ctrl_dev *apm;
	int apm_threshold_volt;
	enum msm_apm_supply apm_high_supply;
	enum msm_apm_supply apm_low_supply;
	bool apm_park_switch;
	int last_volt;
};

/**
 * struct open_loop_cpr_regulator_data - Open loop CPR regulator data structure
 * @regulator_name: Regulator name which needs to be controlled
 * @params: Pointer to regulator parameters
 */
struct open_loop_cpr_regulator_data {
	const char *regulator_name;
	const struct open_loop_cpr_regulator_params *params;
};

/**
 * get_mode_name - Get mode name string from a rail's mode name array
 * @mode_names: Rail-specific mode name array (from
 *              open_loop_cpr_regulator_params.mode_names or
 *              reg_info.mode_names)
 * @num_modes: Number of modes in @mode_names
 * @mode: Mode index (0..num_modes-1)
 *
 * A single global mode_id enum cannot name modes correctly across rails:
 * APC uses {svs, nom, tur} while NSP CX/MX use {svs, svs_l1, nom, tur} -
 * index 1 means "nom" for APC but "svs_l1" for NSP CX/MX. Mode names are
 * therefore looked up per-rail rather than through a fixed enum.
 *
 * Return: Mode name string, or "unknown" for an out-of-range mode
 */
static const char *get_mode_name(const char * const *mode_names,
				 u8 num_modes, int mode)
{
	if (mode >= 0 && mode < num_modes)
		return mode_names[mode];
	return "unknown";
}

/**
 * switch_apm_park - Switch APM supply with safe voltage parking
 * @rdev: Regulator device
 * @new_volt: Target voltage in microvolts
 *
 * PMIC-backed rails: when the voltage transition crosses the APM threshold,
 * this function:
 * 1. Parks the voltage at the APM threshold (safe intermediate voltage)
 * 2. Switches the APM supply rails
 * The caller is then responsible for setting the final target voltage.
 *
 * Return: 0 on success, negative error code on failure
 */
static int switch_apm_park(struct regulator_dev *rdev, int new_volt)
{
	struct reg_info *reg_info = rdev_get_drvdata(rdev);
	int apm_volt = reg_info->apm_threshold_volt;
	struct device *dev = rdev_get_dev(rdev);
	int last_volt = reg_info->last_volt;
	bool apm_crossing;
	int rc;

	/* Skip if APM is not configured */
	if (!reg_info->apm || apm_volt <= 0)
		return 0;

	/* Detect if voltage transition crosses APM threshold */
	apm_crossing = (last_volt < apm_volt && new_volt >= apm_volt) ||
		       (last_volt >= apm_volt && new_volt < apm_volt);

	if (!apm_crossing)
		return 0;

	/* Step 1: Park at APM threshold voltage as a safe intermediate point */
	rc = regulator_set_voltage_rdev(reg_info->base_rdev,
					apm_volt, apm_volt,
					PM_SUSPEND_ON);
	if (rc) {
		dev_err(dev, "Failed to park at APM threshold %duV, rc=%d\n",
			apm_volt, rc);
		return rc;
	}

	dev_dbg(dev, "Parked at APM threshold %duV\n", apm_volt);

	/* Step 2: Switch APM supply based on target voltage direction */
	rc = msm_apm_set_supply(reg_info->apm,
				new_volt >= apm_volt ?
				reg_info->apm_high_supply :
				reg_info->apm_low_supply);
	if (rc) {
		dev_err(dev, "APM switch failed, rc=%d\n", rc);
		return rc;
	}

	dev_dbg(dev, "APM switched to %s supply: last=%duV, threshold=%duV, new=%duV\n",
		new_volt >= apm_volt ? "APCC" : "MX",
		last_volt, apm_volt, new_volt);

	return 0;
}

/**
 * switch_apm_order - Switch APM supply without a park step
 * @rdev: Regulator device
 * @new_volt: Target voltage in microvolts
 *
 * GPIO-backed rails: no intermediate parking voltage is set here. The
 * caller is responsible for ordering this call relative to the actual
 * voltage-set operation depending on scaling direction (voltage set first
 * on scale up, switched first on scale down/same).
 *
 * Return: 0 on success, negative error code on failure
 */
static int switch_apm_order(struct regulator_dev *rdev, int new_volt)
{
	struct reg_info *reg_info = rdev_get_drvdata(rdev);
	int apm_volt = reg_info->apm_threshold_volt;
	struct device *dev = rdev_get_dev(rdev);
	int last_volt = reg_info->last_volt;
	bool apm_crossing;
	int rc;

	/* Skip if APM is not configured */
	if (!reg_info->apm || apm_volt <= 0)
		return 0;

	/* Detect if voltage transition crosses APM threshold */
	apm_crossing = (last_volt < apm_volt && apm_volt <= new_volt) ||
		       (last_volt >= apm_volt && apm_volt > new_volt);

	if (!apm_crossing)
		return 0;

	rc = msm_apm_set_supply(reg_info->apm,
				new_volt >= apm_volt ?
				reg_info->apm_high_supply :
				reg_info->apm_low_supply);
	if (rc) {
		dev_err(dev, "APM switch failed, rc=%d\n", rc);
		return rc;
	}

	dev_dbg(dev, "APM switched to %s supply: last=%duV, threshold=%duV, new=%duV\n",
		new_volt >= apm_volt ? "APCC" : "MX",
		last_volt, apm_volt, new_volt);

	return 0;
}

/**
 * open_loop_cpr_regulator_set_voltage - Set voltage for open loop CPR regulator
 * @rdev: Regulator device
 * @min_uV: Requested mode, 1-based (1=first mode, 2=second mode, ...)
 * @max_uV: Unused (required by regulator framework)
 * @sel: Unused selector pointer (required by regulator framework)
 *
 * This callback translates mode requests into actual voltages based on the
 * pre-calculated open loop CPR voltage table. The min_uV parameter encodes
 * the 1-based operating mode, converted to a 0-based array index to look up
 * the fuse-corrected voltage for that mode. The valid mode range is rail
 * dependent (reg_info->num_modes: 3 for APC, 4 for NSP CX/MX).
 *
 * PMIC-backed rails (@apm_park_switch true) use switch_apm_park(): the
 * voltage is parked at the APM threshold and the APM rails switched before
 * the final target voltage is applied.
 *
 * GPIO-backed rails (@apm_park_switch false) use switch_apm_order(): no
 * park step, and the ordering relative to the voltage-set operation depends
 * on scaling direction - scale up sets the target voltage first then
 * switches APM, scale down/same switches APM first then sets voltage. This
 * matches the ordering logic previously implemented in
 * qcom-gpio-regulator.c's multi-threshold regulator ops.
 *
 * Return: 0 on success, negative error code on failure
 */
static int open_loop_cpr_regulator_set_voltage(struct regulator_dev *rdev,
					       int min_uV, int max_uV,
					       unsigned int *sel)
{
	struct reg_info *reg_info = rdev_get_drvdata(rdev);
	struct device *dev = rdev_get_dev(rdev);
	int voltage;
	int mode;
	int rc;

	mode = min_uV - 1;
	if (mode < 0 || mode >= reg_info->num_modes)
		return -EINVAL;

	/* Look up the pre-calculated fuse-corrected voltage for this mode */
	voltage = reg_info->voltage_table[mode];

	if (voltage > reg_info->ceiling_table[mode]) {
		dev_warn(dev, "%s: mode=%s requested %duV exceeds ceiling %duV, clamping\n",
			 rdev_get_name(rdev),
			 get_mode_name(reg_info->mode_names, reg_info->num_modes, mode),
			 voltage, reg_info->ceiling_table[mode]);
		voltage = reg_info->ceiling_table[mode];
	} else if (voltage < reg_info->floor_table[mode]) {
		dev_warn(dev, "%s: mode=%s requested %duV below floor %duV, clamping\n",
			 rdev_get_name(rdev),
			 get_mode_name(reg_info->mode_names, reg_info->num_modes, mode),
			 voltage, reg_info->floor_table[mode]);
		voltage = reg_info->floor_table[mode];
	}

	dev_dbg(dev, "mode=%s(%d) -> %duV\n",
		get_mode_name(reg_info->mode_names, reg_info->num_modes, mode),
		mode, voltage);

	if (!reg_info->last_volt) {
		rc = regulator_get_voltage_rdev(reg_info->base_rdev);
		if (rc < 0) {
			dev_err(dev, "Failed to get voltage, rc=%d\n", rc);
			return rc;
		}
		reg_info->last_volt = rc;
	}

	if (reg_info->apm_park_switch) {
		/*
		 * Park-then-switch: park at threshold and switch APM rails
		 * first, then set the final voltage - no reordering needed
		 * since parking already handles the crossing safely.
		 */
		rc = switch_apm_park(rdev, voltage);
		if (rc) {
			dev_err(dev, "APM switching failed for mode=%s, rc=%d\n",
				get_mode_name(reg_info->mode_names,
					      reg_info->num_modes, mode), rc);
			return rc;
		}

		rc = regulator_set_voltage_rdev(reg_info->base_rdev,
						voltage, voltage,
						PM_SUSPEND_ON);
		if (rc) {
			dev_err(dev, "Failed to set voltage to %duV, rc=%d\n",
				voltage, rc);
			return rc;
		}
	} else {
		/* Scale up: set target voltage first, then switch APM */
		if (voltage > reg_info->last_volt) {
			rc = regulator_set_voltage_rdev(reg_info->base_rdev,
							voltage, voltage,
							PM_SUSPEND_ON);
			if (rc) {
				dev_err(dev, "Failed to set voltage to %duV, rc=%d\n",
					voltage, rc);
				return rc;
			}
		}

		rc = switch_apm_order(rdev, voltage);
		if (rc) {
			dev_err(dev, "APM switching failed for mode=%s, rc=%d\n",
				get_mode_name(reg_info->mode_names,
					      reg_info->num_modes, mode), rc);
			return rc;
		}

		/* Scale down/same: switch APM first, then set target voltage */
		if (voltage <= reg_info->last_volt) {
			rc = regulator_set_voltage_rdev(reg_info->base_rdev,
							voltage, voltage,
							PM_SUSPEND_ON);
			if (rc) {
				dev_err(dev, "Failed to set voltage to %duV, rc=%d\n",
					voltage, rc);
				return rc;
			}
		}
	}

	reg_info->current_voltage = mode;
	reg_info->last_volt = voltage;

	dev_dbg(dev, "mode=%s(%d) -> %duV config done\n",
		get_mode_name(reg_info->mode_names, reg_info->num_modes, mode),
		mode, reg_info->last_volt);

	return 0;
}

/**
 * open_loop_cpr_regulator_get_voltage - Get current voltage as mode index
 * @rdev: Regulator device
 *
 * Returns the current 1-based operating mode using the tracked
 * current_voltage field. Returns 1 (the rail's first mode, SVS) if not yet
 * set. This avoids hardware access and potential deadlocks.
 *
 * Return: 1-based mode number
 */
static int open_loop_cpr_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct reg_info *reg_info = rdev_get_drvdata(rdev);

	if (reg_info->current_voltage == -1)
		return 1;

	return reg_info->current_voltage + 1;
}

/**
 * open_loop_cpr_regulator_list_voltage - List supported voltages (mode indices)
 * @rdev: Regulator device
 * @selector: Voltage selector, 0-based
 *
 * Maps a 0-based selector to the 1-based mode index used by the OPP
 * framework. The valid selector range is rail dependent
 * (reg_info->num_modes: 3 for APC, 4 for NSP CX/MX).
 *
 * Return: 1-based mode index, or -EINVAL for an out-of-range selector
 */
static int open_loop_cpr_regulator_list_voltage(struct regulator_dev *rdev,
						unsigned int selector)
{
	struct reg_info *reg_info = rdev_get_drvdata(rdev);

	if (selector >= reg_info->num_modes)
		return -EINVAL;

	return selector + 1;
}

static const struct regulator_ops open_loop_cpr_regulator_ops = {
	.list_voltage = open_loop_cpr_regulator_list_voltage,
	.set_voltage  = open_loop_cpr_regulator_set_voltage,
	.get_voltage  = open_loop_cpr_regulator_get_voltage,
};

/**
 * convert_open_loop_voltage_fuse - Convert fuse value to voltage
 * @ref_volt: Reference voltage in microvolts
 * @step_volt: Voltage step size in microvolts
 * @fuse: Raw fuse value read from NVMEM
 * @fuse_len: Number of bits in the fuse value
 *
 * Converts a signed fuse value to an absolute voltage. The MSB indicates
 * sign (0=positive, 1=negative), and the remaining bits encode the step
 * count. Formula: voltage = ref_volt + (sign * steps * step_volt)
 *
 * Return: Calculated voltage in microvolts
 */
static int convert_open_loop_voltage_fuse(int ref_volt, int step_volt,
					  u8 fuse, int fuse_len)
{
	int steps;
	int sign;

	sign = (fuse & (1 << (fuse_len - 1))) ? -1 : 1;
	steps = fuse & ((1 << (fuse_len - 1)) - 1);

	return ref_volt + sign * steps * step_volt;
}

/**
 * parse_array_property - Parse integer array from device tree property
 * @dev: Device pointer
 * @node: Device tree node
 * @prop_name: Property name to parse
 * @len: Length of the property in bytes
 *
 * Allocates memory and reads an integer array property from device tree.
 * Logs each parsed value at debug level.
 *
 * Return: Pointer to allocated array, or ERR_PTR on error
 */
static int *parse_array_property(struct device *dev,
				 struct device_node *node,
				 const char *prop_name,
				 int len)
{
	int count = len / sizeof(u32);
	int *array;
	int ret, i;

	array = devm_kzalloc(dev, len, GFP_KERNEL);
	if (!array)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32_array(node, prop_name, (u32 *)array, count);
	if (ret) {
		dev_err(dev, "Failed to read %s: %d\n", prop_name, ret);
		return ERR_PTR(ret);
	}

	for (i = 0; i < count; i++)
		dev_dbg(dev, "  %s[%d] = %d\n", prop_name, i, array[i]);

	return array;
}

/**
 * override_voltage_config - Parse voltage configuration from platform subnode
 * @dev: Device pointer
 * @node: Platform subnode
 *
 * Parses qcom,voltage-table, qcom,thresholds, and qcom,num-thresholds from
 * platform configuration. Allocates and returns a new voltage_config if
 * property's are found.
 *
 * Return: Pointer to allocated voltage_config, or NULL if not found
 */
static struct voltage_config *override_voltage_config(struct device *dev,
						      struct device_node *node)
{
	struct voltage_config *vconfig;
	int vtable_len = 0, thresh_len = 0;
	int *vtable;
	int *thresh;
	u32 val;

	/* Check if all voltage config properties exist, capturing lengths */
	if (!of_find_property(node, "qcom,voltage-table", &vtable_len) ||
	    !of_find_property(node, "qcom,thresholds", &thresh_len) ||
	    of_property_read_u32(node, "qcom,num-thresholds", &val) != 0) {
		dev_info(dev, "No voltage config properties in platform configuration\n");
		return NULL;
	}

	/* Sanitize lengths */
	if (vtable_len <= 0 || (vtable_len % sizeof(u32)) != 0 ||
	    thresh_len <= 0 || (thresh_len % sizeof(u32)) != 0) {
		dev_err(dev, "Invalid property lengths: vtable=%d, thresh=%d\n",
			vtable_len, thresh_len);
		return ERR_PTR(-EINVAL);
	}

	vconfig = devm_kzalloc(dev, sizeof(*vconfig), GFP_KERNEL);
	if (!vconfig)
		return ERR_PTR(-ENOMEM);

	/* Parse voltage_table */
	vtable = parse_array_property(dev, node, "qcom,voltage-table", vtable_len);
	if (IS_ERR(vtable))
		return ERR_PTR(PTR_ERR(vtable));
	vconfig->voltage_table = vtable;

	/* Parse thresholds */
	thresh = parse_array_property(dev, node, "qcom,thresholds", thresh_len);
	if (IS_ERR(thresh))
		return ERR_PTR(PTR_ERR(thresh));
	vconfig->thresholds = thresh;

	/* Parse num_thresholds */
	vconfig->num_thresholds = (u8)val;
	dev_dbg(dev, "Parsed num_thresholds = %u\n", vconfig->num_thresholds);

	return vconfig;
}

/**
 * override_fuse_params - Parse fuse parameters from platform subnode
 * @dev: Device pointer
 * @node: Platform subnode
 * @num_fuses_out: Output parameter for number of fuses
 *
 * Parses qcom,fuse-params (5-tuples: bit_len, reference_volt, step_volt,
 * floor_volt, ceiling_volt) and qcom,num-fuses from platform configuration.
 * Allocates and returns a new fuse_params array if the properties are found.
 *
 * Return: Pointer to allocated fuse_params array, NULL if not found,
 *         or ERR_PTR on error
 */
static struct fuse_params *override_fuse_params(struct device *dev,
						struct device_node *node,
						u8 *num_fuses_out)
{
	struct fuse_params *fparams;
	int *tmp_buf;
	int len, num_fuses, i;
	u32 val;

	/* Check if fuse params properties exist in platform configuration */
	if (!of_find_property(node, "qcom,fuse-params", &len) ||
	    of_property_read_u32(node, "qcom,num-fuses", &val) != 0) {
		dev_info(dev,
			 "No fuse params properties in platform configuration\n");
		return NULL;
	}

	/* Sanitize length - must be positive, aligned, and in 5-tuples */
	if (len <= 0 || (len % sizeof(u32)) != 0 ||
	    (len % (5 * sizeof(u32))) != 0) {
		dev_err(dev,
			"Invalid fuse-params length: %d (must be multiple of %zu)\n",
			len, 5 * sizeof(u32));
		return ERR_PTR(-EINVAL);
	}

	num_fuses = len / (5 * sizeof(u32));

	tmp_buf = parse_array_property(dev, node, "qcom,fuse-params", len);
	if (IS_ERR(tmp_buf))
		return ERR_CAST(tmp_buf);

	fparams = devm_kzalloc(dev, sizeof(*fparams) * num_fuses, GFP_KERNEL);
	if (!fparams) {
		devm_kfree(dev, tmp_buf);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < num_fuses; i++) {
		fparams[i].bit_len        = tmp_buf[i * 5];
		fparams[i].reference_volt = tmp_buf[i * 5 + 1];
		fparams[i].step_volt      = tmp_buf[i * 5 + 2];
		fparams[i].floor_volt     = tmp_buf[i * 5 + 3];
		fparams[i].ceiling_volt   = tmp_buf[i * 5 + 4];
	}
	devm_kfree(dev, tmp_buf);

	*num_fuses_out = (u8)val;

	return fparams;
}

/**
 * read_cpr_fusing_revision - Read CPR fusing revision from split registers
 * @dev: Device pointer
 * @cpr_rev: Output parameter for CPR revision (0-7)
 *
 * For IPQ9650, the 3-bit CPR fusing revision is split across two registers:
 * - BIT0: Register 0xA0414, bit 23 (nvmem cell "cpr_fusing_rev_bit0")
 * - BIT2:1: Register 0xA0418, bits 1:0 (nvmem cell "cpr_fusing_rev_bit2_1")
 *
 * Return: 0 on success, negative error code on failure
 */
static int read_cpr_fusing_revision(struct device *dev, u8 *cpr_rev)
{
	u8 bit0, bit2_1;
	int rc;

	rc = nvmem_cell_read_u8(dev, "cpr_fusing_rev_bit0", &bit0);
	if (rc)
		return dev_err_probe(dev, rc,
				     "Failed to read cpr_fusing_rev_bit0\n");
	bit0 &= 0x1;

	rc = nvmem_cell_read_u8(dev, "cpr_fusing_rev_bit2_1", &bit2_1);
	if (rc)
		return dev_err_probe(dev, rc,
				     "Failed to read cpr_fusing_rev_bit2_1\n");
	bit2_1 &= 0x3;

	/* Combine: [BIT2 BIT1 BIT0] */
	*cpr_rev = (bit2_1 << 1) | bit0;

	dev_dbg(dev, "CPR fusing revision: %d (bit0=%d, bit2_1=%d)\n",
		*cpr_rev, bit0, bit2_1);

	return 0;
}

/**
 * determine_part_type - Determine part type based on fused voltage
 * @dev: Device pointer
 * @params: Regulator parameters containing default voltage thresholds
 * @vconfig: Voltage configuration, may contain overridden thresholds from DTS
 * @highest_fuse_volt: Highest fuse corner voltage in microvolts
 * @part_type: Output parameter for part type index
 *
 */
static void determine_part_type(struct device *dev,
				const struct open_loop_cpr_regulator_params *params,
				const struct voltage_config *vconfig,
				int highest_fuse_volt,
				u8 *part_type)
{
	int i;

	if (!params->part_type_supported || !vconfig->thresholds) {
		*part_type = 0;
		return;
	}

	for (i = 0; i < vconfig->num_thresholds; i++) {
		if (highest_fuse_volt < vconfig->thresholds[i]) {
			*part_type = i;
			dev_dbg(dev,
				"Part type %d: voltage %duV < threshold[%d] %duV\n",
				*part_type, highest_fuse_volt, i,
				vconfig->thresholds[i]);
			return;
		}
	}

	*part_type = vconfig->num_thresholds;
	dev_dbg(dev, "Part type %d: voltage %duV >= all thresholds\n",
		*part_type, highest_fuse_volt);
}

/**
 * apply_open_loop_voltage_adjustment - Apply CPR revision-based voltage adjustments
 * @dev: Device pointer
 * @node: Device tree node containing the adjustment property
 * @cpr_rev: CPR fusing revision (0-7)
 * @part_type: part type index
 * @part_type_supported: Whether part type differentiation is supported
 * @fuse_volt: Array of fused voltages to adjust (modified in place)
 * @num_fuses: Number of fuse corners
 *
 * Reads qcom,cpr-open-loop-voltage-fuse-adjustment[-{part_type}] from the
 * device tree. The property is a matrix of 8 rows (CPR revisions 0-7) by
 * num_fuses columns (one per fuse corner). Selects the row for cpr_rev and
 * adds the adjustment values to fuse_volt.
 *
 * Return: 0 on success, negative error code on failure
 */
static int apply_open_loop_voltage_adjustment(struct device *dev,
					      struct device_node *node,
					      u8 cpr_rev,
					      u8 part_type,
					      bool part_type_supported,
					      int *fuse_volt,
					      int num_fuses)
{
	int prev_volt, offset, len, rc, i;
	bool has_adjustments;
	char prop_name[80];
	int *adjustments;

	if (part_type_supported)
		snprintf(prop_name, sizeof(prop_name),
			 "qcom,cpr-open-loop-voltage-fuse-adjustment-%d",
			 part_type);
	else
		snprintf(prop_name, sizeof(prop_name),
			 "qcom,cpr-open-loop-voltage-fuse-adjustment");

	if (!of_find_property(node, prop_name, &len))
		return 0;

	if (len != num_fuses * 8 * sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "Invalid %s length: %d, expected %zu\n",
				     prop_name, len,
				     (size_t)(num_fuses * 8 * sizeof(u32)));

	adjustments = kcalloc(num_fuses, sizeof(*adjustments), GFP_KERNEL);
	if (!adjustments)
		return -ENOMEM;

	offset = cpr_rev * num_fuses;
	for (i = 0; i < num_fuses; i++) {
		rc = of_property_read_u32_index(node, prop_name,
						offset + i,
						(u32 *)&adjustments[i]);
		if (rc) {
			kfree(adjustments);
			return dev_err_probe(dev, rc,
					     "Failed to read %s[%d]\n",
					     prop_name, offset + i);
		}
	}

	/* Skip applying if all adjustments for this CPR revision are zero */
	has_adjustments = false;
	for (i = 0; i < num_fuses; i++) {
		if (adjustments[i]) {
			has_adjustments = true;
			break;
		}
	}

	if (!has_adjustments)
		goto free_adjustment;

	for (i = 0; i < num_fuses; i++) {
		if (adjustments[i]) {
			prev_volt = fuse_volt[i];
			fuse_volt[i] += adjustments[i];
			dev_info(dev,
				 "Adjusted mode %d: %duV -> %duV (+%duV) [CPR rev %d, part type %d]\n",
				 i, prev_volt, fuse_volt[i], adjustments[i],
				 cpr_rev, part_type);
		}
	}

free_adjustment:
	kfree(adjustments);
	return 0;
}

/**
 * process_open_loop_cpr_regulator - Process open loop CPR regulator
 * @dev: Device pointer
 * @reg_data: Regulator configuration data
 * @fix_volt_max: Force maximum (ceiling) voltage flag
 *
 * Reads fuse parameters from device tree, accesses eFuse hardware registers
 * via NVMEM, reads reference voltages, applies fuse corrections, and
 * populates the voltage table with the final operating voltage for each of
 * the rail's modes. The fuse value is interpreted as a signed offset from the
 * reference voltage and directly applied without process corner classification.
 * Registers the regulator with the Linux regulator framework.
 *
 * Return: 0 on success, negative error code on failure
 */
static int process_open_loop_cpr_regulator(struct device *dev,
					   const struct open_loop_cpr_regulator_data *reg_data,
					   bool fix_volt_max)
{
	const struct open_loop_cpr_regulator_params *default_params = reg_data->params;
	const struct voltage_config *vconfig = default_params->voltage_config;
	const struct fuse_params *fparams = default_params->fuse_params;
	u8 num_fuses = default_params->num_fuses;
	int fused_volt_table[MAX_MODES];
	struct voltage_config *override_vconfig;
	struct fuse_params *override_fparams;
	struct device_node *gpio_np = NULL;
	const struct fuse_params *fuse;
	struct regulator_config config = {};
	struct device_node *rail_node;
	struct device_node *adj_node;
	struct regulator_desc *desc;
	struct regulator_dev *rdev;
	struct reg_info *reg_info;
	struct nvmem_cell *cell;
	u8 override_num_fuses;
	u8 cpr_rev, part_type;
	char fuse_name[32];
	int fused_volt;
	u16 volt_ticks;
	size_t len;
	void *buf;
	int mode;
	int rc;

	if (default_params->mx_rail_available) {
		char supply_prop[32];

		snprintf(supply_prop, sizeof(supply_prop), "%s-supply",
			 reg_data->regulator_name);
		if (!of_find_property(dev->of_node, supply_prop, NULL)) {
			dev_info(dev, "%s rail absent on this board, skipping\n",
				 reg_data->regulator_name);
			return 0;
		}
	}

	/* Try to override from platform configuration if subnode exists */
	rail_node = of_get_child_by_name(dev->of_node, reg_data->regulator_name);
	if (rail_node) {
		override_vconfig = override_voltage_config(dev, rail_node);
		if (override_vconfig)
			vconfig = override_vconfig;

		override_fparams = override_fuse_params(dev, rail_node, &override_num_fuses);
		if (override_fparams) {
			fparams   = override_fparams;
			num_fuses = override_num_fuses;
		}

		/*
		 * Presence of qcom,gpio-regulator selects the type-binned
		 * lookup algorithm for this rail (GPIO-backed): the fused
		 * voltage is handed to qcom-gpio-regulator.c, which owns the
		 * 2D [mode][type] table and returns the final actuation
		 * voltage. Absence keeps today's direct-fuse behavior
		 * (PMIC-backed rails).
		 */
		gpio_np = of_parse_phandle(rail_node, "qcom,gpio-regulator", 0);

		of_node_put(rail_node);
	} else {
		dev_info(dev,
			 "No platform override for %s, using defaults\n",
			 reg_data->regulator_name);
	}

	reg_info = devm_kzalloc(dev, sizeof(*reg_info), GFP_KERNEL);
	if (!reg_info) {
		of_node_put(gpio_np);
		return -ENOMEM;
	}

	reg_info->current_voltage = -1;

	if (num_fuses > MAX_MODES) {
		of_node_put(gpio_np);
		return dev_err_probe(dev, -EINVAL,
				     "Invalid num_fuses %u exceeds MAX_MODES %d\n",
				     num_fuses, MAX_MODES);
	}

	reg_info->mode_names = default_params->mode_names;
	reg_info->num_modes = num_fuses;

	/* Initialize fused-voltage table with ceiling voltages as safe defaults */
	for (mode = 0; mode < num_fuses; mode++)
		fused_volt_table[mode] = vconfig->voltage_table[mode];

	/* Read fuses and calculate operating voltage for each mode */
	for (mode = 0; mode < num_fuses; mode++) {
		fuse = &fparams[mode];

		if (fix_volt_max) {
			/*
			 * Use ceiling voltage directly when voltage scaling is
			 * disabled via qcom,skip-voltage-scaling quirk.
			 */
			dev_dbg(dev,
				"%s_%s: forced ceiling voltage %duV\n",
				reg_data->regulator_name,
				get_mode_name(reg_info->mode_names, num_fuses, mode),
				fused_volt_table[mode]);
			continue;
		}

		snprintf(fuse_name, sizeof(fuse_name), "cpr_%s_%s",
			 reg_data->regulator_name,
			 get_mode_name(reg_info->mode_names, num_fuses, mode));

		cell = nvmem_cell_get(dev, fuse_name);
		if (IS_ERR(cell)) {
			of_node_put(gpio_np);
			return dev_err_probe(dev,
					     PTR_ERR(cell),
					     "%s cell get failed\n",
					     fuse_name);
		}

		buf = nvmem_cell_read(cell, &len);
		nvmem_cell_put(cell);
		dev_dbg(dev, "fuse_name(%s) len:%zu\n", fuse_name, len);

		if (IS_ERR(buf)) {
			of_node_put(gpio_np);
			return dev_err_probe(dev,
					     PTR_ERR(buf),
					     "%s fuse read failed\n",
					     fuse_name);
		}

		memcpy(&volt_ticks, buf, min(len, sizeof(volt_ticks)));
		kfree(buf);

		/*
		 * Apply fuse correction to the reference voltage to obtain
		 * the final operating voltage for this mode. The fuse value
		 * is interpreted as a signed offset (MSB = sign bit, remaining
		 * bits = step count) applied directly to the reference voltage.
		 * No process corner classification is performed here - that
		 * only happens below, and only for GPIO-backed rails.
		 */
		fused_volt = convert_open_loop_voltage_fuse(fuse->reference_volt,
							    fuse->step_volt,
							    volt_ticks,
							    fuse->bit_len);

		dev_dbg(dev, "%s_%s: ref=%duV, fused=%duV\n",
			reg_data->regulator_name,
			get_mode_name(reg_info->mode_names, num_fuses, mode),
			fuse->reference_volt, fused_volt);

		/* Store the fuse-corrected voltage directly for this mode */
		fused_volt_table[mode] = fused_volt;
	}

	/*
	 * CPR revision-based voltage adjustment matrices are specific to the
	 * direct-fuse (PMIC-backed) algorithm; the GPIO-backed type-binned
	 * lookup never used them, so skip this block for GPIO-backed rails.
	 */
	if (!fix_volt_max && !gpio_np) {
		rc = read_cpr_fusing_revision(dev, &cpr_rev);
		if (rc)
			return dev_err_probe(dev, rc,
					     "Failed to read CPR fusing revision\n");

		determine_part_type(dev, default_params, vconfig,
				    fused_volt_table[num_fuses - 1],
				    &part_type);

		adj_node = of_get_child_by_name(dev->of_node,
						reg_data->regulator_name);
		if (adj_node) {
			rc = apply_open_loop_voltage_adjustment(dev, adj_node,
								cpr_rev,
								part_type,
								default_params->part_type_supported,
								fused_volt_table,
								num_fuses);
			of_node_put(adj_node);
			if (rc)
				return dev_err_probe(dev, rc,
						     "Failed to apply voltage adjustment\n");
		}
	}

	/*
	 * Resolve the final actuation voltage per mode. GPIO-backed rails
	 * hand the fused voltage to qcom-gpio-regulator.c, which classifies
	 * it against the rail's thresholds and looks up the final voltage in
	 * its 2D [mode][type] table (or forces the TYPE2 column when
	 * fix_volt_max is set, same intent as the ceiling fallback used below
	 * for PMIC-backed rails). PMIC-backed rails use the fused voltage
	 * directly, as before.
	 *
	 * Regardless of path, also copy the per-mode floor/ceiling bounds
	 * from fparams onto reg_info so they are available at set_voltage()
	 * time to check the value actually handed to the regulator framework.
	 */
	for (mode = 0; mode < num_fuses; mode++) {
		if (gpio_np) {
			rc = qcom_gpio_regulator_get_voltage(gpio_np,
							     reg_data->regulator_name,
							     mode,
							     fused_volt_table[mode],
							     fix_volt_max);
			if (rc < 0) {
				of_node_put(gpio_np);
				return dev_err_probe(dev, rc,
						     "%s_%s: gpio voltage lookup failed\n",
						     reg_data->regulator_name,
						     get_mode_name(reg_info->mode_names,
								   num_fuses, mode));
			}
			reg_info->voltage_table[mode] = rc;
		} else {
			reg_info->voltage_table[mode] = fused_volt_table[mode];
		}

		reg_info->floor_table[mode]   = fparams[mode].floor_volt;
		reg_info->ceiling_table[mode] = fparams[mode].ceiling_volt;
	}

	/*
	 * GPIO-backed rails (qcom,gpio-regulator present) use order-change APM
	 * switching with no park step; PMIC-backed rails keep park-then-switch.
	 * This mirrors the same condition used for the voltage table dispatch
	 * above, so no separate DT property is needed to select the strategy.
	 */
	reg_info->apm_park_switch = !gpio_np;

	of_node_put(gpio_np);

	/*
	 * APM tracks the APSS/APC cluster's power source, not any other
	 * rail - skip it entirely for rails that don't drive APM.
	 */
	if (!default_params->apm_supported) {
		reg_info->apm = NULL;
	} else {
		reg_info->apm = msm_apm_ctrl_dev_get(dev);
		if (IS_ERR(reg_info->apm)) {
			if (PTR_ERR(reg_info->apm) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			reg_info->apm = NULL;
		} else {
			of_property_read_u32(dev->of_node, "qcom,apm-threshold-voltage",
					     &reg_info->apm_threshold_volt);
			reg_info->apm_high_supply = MSM_APM_SUPPLY_APCC;
			reg_info->apm_low_supply  = MSM_APM_SUPPLY_MX;

			dev_dbg(dev, "%s: APM configured, threshold=%duV, %s\n",
				reg_data->regulator_name, reg_info->apm_threshold_volt,
				reg_info->apm_park_switch ? "park-and-switch" : "order-switch");
		}
	}

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->name = devm_kasprintf(dev, GFP_KERNEL, "%s_regulator",
				    reg_data->regulator_name);
	if (!desc->name)
		return -ENOMEM;

	desc->of_match = reg_data->regulator_name;
	desc->type = REGULATOR_VOLTAGE;
	desc->ops = &open_loop_cpr_regulator_ops;
	desc->owner = THIS_MODULE;
	desc->min_uV = 1;
	desc->n_voltages = num_fuses;
	desc->supply_name = reg_data->regulator_name;
	config.dev = dev;
	config.driver_data = reg_info;
	rdev = devm_regulator_register(dev, desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev,
				     PTR_ERR(rdev),
				     "Failed to register %s regulator\n",
				     reg_data->regulator_name);

	reg_info->base_rdev = rdev->supply->rdev;

	for (mode = 0; mode < num_fuses; mode++)
		dev_info(dev, "%s voltage[%s] = %duV\n",
			 reg_data->regulator_name,
			get_mode_name(reg_info->mode_names, num_fuses, mode),
			reg_info->voltage_table[mode]);

	return 0;
}

/**
 * open_loop_cpr_regulator_probe - Probe function for open loop CPR regulator
 * @pdev: Platform device pointer
 *
 * Retrieves platform-specific regulator data from the device tree match
 * table, checks for voltage scaling quirks, and invokes
 * process_open_loop_cpr_regulator() for each regulator entry to perform
 * fuse reading, voltage calculation, and regulator registration.
 *
 * Return: 0 on success, negative error code on failure
 */
static int open_loop_cpr_regulator_probe(struct platform_device *pdev)
{
	const struct open_loop_cpr_regulator_data *reg_data;
	struct device *dev = &pdev->dev;
	bool fix_volt_max = false;
	int ret = 0;

	reg_data = of_device_get_match_data(dev);
	if (!reg_data)
		return -ENODEV;

	if (device_property_read_bool(dev, "qcom,skip-voltage-scaling"))
		fix_volt_max = true;

	for (; reg_data->regulator_name; reg_data++) {
		ret = process_open_loop_cpr_regulator(dev, reg_data,
						      fix_volt_max);
		if (ret < 0)
			return dev_err_probe(dev,
					     ret,
					     "Failed to process %s regulator: %d\n",
					     reg_data->regulator_name, ret);
	}

	return ret;
}

static const int ipq9650_voltage_thresholds[] = {
	650000,
	750000,
};

/*
 * IPQ9650 APC ceiling voltages per operating mode.
 * These serve as reference voltages for fuse correction and as safe
 * defaults when voltage scaling is disabled. Array length matches
 * num_fuses (one entry per mode: SVS, NOM, TUR).
 */
static const int ipq9650_apc_voltage_table[] = {
	735000,		/* SVS ceiling voltage in microvolts */
	815000,		/* NOM ceiling voltage in microvolts */
	960000,		/* TUR ceiling voltage in microvolts */
};

static const struct voltage_config ipq9650_apc_voltages = {
	.voltage_table  = ipq9650_apc_voltage_table,
	.num_thresholds = 2,
	.thresholds     = ipq9650_voltage_thresholds,
};

/*
 * IPQ9650 NSP CX/MX ceiling voltages per operating mode (SVS, SVS_L1, NOM,
 * TUR). Array length matches num_fuses = 4.
 */
static const int ipq9650_nsp_cx_voltage_table[] = {
	735000,		/* SVS ceiling voltage in microvolts */
	790000,		/* SVS_L1 ceiling voltage in microvolts */
	815000,		/* NOM ceiling voltage in microvolts */
	960000,		/* TUR ceiling voltage in microvolts */
};

static const int ipq9650_nsp_mx_voltage_table[] = {
	750000,		/* SVS ceiling voltage in microvolts */
	790000,		/* SVS_L1 ceiling voltage in microvolts */
	815000,		/* NOM ceiling voltage in microvolts */
	815000,		/* TUR ceiling voltage in microvolts */
};

static const struct voltage_config ipq9650_nsp_cx_voltages = {
	.voltage_table  = ipq9650_nsp_cx_voltage_table,
	.num_thresholds = 2,
	.thresholds     = ipq9650_voltage_thresholds,
};

static const struct voltage_config ipq9650_nsp_mx_voltages = {
	.voltage_table  = ipq9650_nsp_mx_voltage_table,
	.num_thresholds = 2,
	.thresholds     = ipq9650_voltage_thresholds,
};

/*
 * IPQ9650 APC fuse parameters per operating mode.
 * Each entry specifies: fuse bit length, reference voltage (uV),
 * voltage step size, floor voltage and ceiling voltage for the
 * signed fuse correction.
 */
static const struct fuse_params ipq9650_apc_fuse_params[] = {
	{ 7, 630000, 5000, 550000, 735000 },	/* SVS */
	{ 7, 750000, 5000, 650000, 815000 },	/* NOM */
	{ 7, 890000, 5000, 800000, 960000 },	/* TUR */
};

/*
 * IPQ9650 NSP CX/MX fuse parameters per operating mode (SVS, SVS_L1, NOM,
 * TUR).
 */
static const struct fuse_params ipq9650_nsp_cx_fuse_params[] = {
	{ 7, 630000, 5000, 550000, 735000 },	/* SVS */
	{ 7, 685000, 5000, 600000, 790000 },	/* SVS_L1 */
	{ 7, 750000, 5000, 650000, 815000 },	/* NOM */
	{ 7, 890000, 5000, 750000, 960000 },	/* TUR */
};

static const struct fuse_params ipq9650_nsp_mx_fuse_params[] = {
	{ 7, 750000, 5000, 750000, 750000 },	/* SVS */
	{ 7, 750000, 5000, 750000, 790000 },	/* SVS_L1 */
	{ 7, 750000, 5000, 750000, 815000 },	/* NOM */
	{ 7, 750000, 5000, 750000, 815000 },	/* TUR */
};

/*
 * Mode name arrays, one entry per fuse/mode position. APC has no SVS_L1
 * mode while NSP CX/MX does.
 */
static const char * const ipq9650_apc_mode_names[] = {
	"svs", "nom", "tur",
};

static const char * const ipq9650_nsp_mode_names[] = {
	"svs", "svs_l1", "nom", "tur",
};

static const struct open_loop_cpr_regulator_params ipq9650_apc_params = {
	.fuse_params         = ipq9650_apc_fuse_params,
	.voltage_config      = &ipq9650_apc_voltages,
	.mode_names          = ipq9650_apc_mode_names,
	.num_fuses           = 3,
	.part_type_supported = true,
	.apm_supported       = true,
};

static const struct open_loop_cpr_regulator_params ipq9650_nsp_cx_params = {
	.fuse_params         = ipq9650_nsp_cx_fuse_params,
	.voltage_config      = &ipq9650_nsp_cx_voltages,
	.mode_names          = ipq9650_nsp_mode_names,
	.num_fuses           = 4,
	.part_type_supported = true,
};

static const struct open_loop_cpr_regulator_params ipq9650_nsp_mx_params = {
	.fuse_params         = ipq9650_nsp_mx_fuse_params,
	.voltage_config      = &ipq9650_nsp_mx_voltages,
	.mode_names          = ipq9650_nsp_mode_names,
	.num_fuses           = 4,
	.part_type_supported = true,
	.mx_rail_available   = true,
};

static const struct open_loop_cpr_regulator_data
		ipq9650_open_loop_cpr_regulator_data[] = {
	{ "apc", &ipq9650_apc_params },
	{ "nsp_cx", &ipq9650_nsp_cx_params },
	{ "nsp_mx", &ipq9650_nsp_mx_params },
	{ }
};

static const struct of_device_id open_loop_cpr_regulator_match_table[] = {
	{
		.compatible = "qcom,ipq9650-open-loop-cpr-regulator",
		.data       = &ipq9650_open_loop_cpr_regulator_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, open_loop_cpr_regulator_match_table);

static struct platform_driver open_loop_cpr_regulator_driver = {
	.driver = {
		.name           = "qcom,open-loop-cpr-regulator",
		.of_match_table = open_loop_cpr_regulator_match_table,
	},
	.probe = open_loop_cpr_regulator_probe,
};

module_platform_driver(open_loop_cpr_regulator_driver);

MODULE_DESCRIPTION("Qualcomm Open Loop CPR regulator driver");
MODULE_LICENSE("Dual BSD/GPL");
