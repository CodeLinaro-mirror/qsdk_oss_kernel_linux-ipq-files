// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/err.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>

#include <soc/qcom/socinfo.h>

#define MIN_VOLT 0
#define MAX_VOLT 1

/**
 * enum mode_id - Operating modes for 3-gpio voltage regulators
 * @MODE_SVS: Save mode
 * @MODE_NOM: Nominal mode
 * @MODE_TUR: Turbo mode
 * @MAX_MODES: Number of operating modes
 */
enum mode_id {
	MODE_SVS,
	MODE_NOM,
	MODE_TUR,
	MAX_MODES
};

/**
 * enum type_id - Process type identifiers for voltage binning
 * @TYPE0: Type 0 (lowest voltage)
 * @TYPE1: Type 1 (nominal voltage)
 * @TYPE2: Type 2 (highest voltage)
 * @MAX_TYPES: Number of process types
 */
enum type_id {
	TYPE0,
	TYPE1,
	TYPE2,
	MAX_TYPES
};

/**
 * struct fuse_params - NVMEM fuse parameters for voltage calculation
 * @bit_len: Number of bits in the fuse value
 * @reference_volt: Reference voltage in microvolts
 * @step_volt: Voltage step size in microvolts per fuse unit
 */
struct fuse_params {
	int bit_len;
	int reference_volt;
	int step_volt;
};

/**
 * struct voltage_config - Voltage configuration parameters
 * @voltage_table: Pointer to voltage table
 *                 - For 1-threshold: points to 2-element array [min_volt, max_volt]
 *                 - For multi-threshold: points to [num_fuses][MAX_TYPES] flattened array
 * @num_thresholds: Number of threshold voltages
 *                  - For 1-threshold: 1 threshold
 *                  - For multi-threshold: 2 thresholds
 * @thresholds: Pointer to threshold voltage array
 *              - For 1-threshold: points to 1-element array [threshold_volt]
 *              - For multi-threshold: points to 2-element array [lower_limit, upper_limit]
 */
struct voltage_config {
	const int *voltage_table;
	u8 num_thresholds;
	const int *thresholds;
};

/**
 * struct gpio_regulator_params - Unified regulator parameters
 * @fuse_params: Pointer to array of fuse parameters
 *               - For single-fuse: points to 1-element array
 *               - For multi-fuse: points to N-element array
 * @voltage_config: Pointer to voltage configuration structure
 * @num_fuses: Number of fuses (determines array size)
 */
struct gpio_regulator_params {
	const struct fuse_params *fuse_params;
	const struct voltage_config *voltage_config;
	u8 num_fuses;
};

/**
 * struct reg_info - Private data for voltage regulators
 * @types: Determined process type for each mode (SVS, NOM, TUR)
 * @voltage_table: Voltage table indexed by [mode][type] in microvolts
 * @base_regulator: Pointer to the underlying GPIO regulator
 */
struct reg_info {
	enum type_id types[MAX_MODES];
	int voltage_table[MAX_MODES][MAX_TYPES];
	struct regulator *base_regulator;
};

/**
 * struct gpio_regulator_data - unified gpio regulator data structure
 * @regulator_name:	Regulator name which needs to be controlled
 * @params:		Pointer to regulator parameters
 */
struct gpio_regulator_data {
	const char *regulator_name;
	const struct gpio_regulator_params *params;
};

static const char * const type_names[] = {"TYPE0", "TYPE1", "TYPE2"};

/**
 * get_mode_name - Get mode name string from mode ID
 * @mode: Mode identifier (MODE_SVS, MODE_NOM, MODE_TUR)
 *
 * Return: Mode name string, or "unknown" for invalid mode
 */
static const char *get_mode_name(enum mode_id mode)
{
	switch (mode) {
	case MODE_SVS:
		return "svs";
	case MODE_NOM:
		return "nom";
	case MODE_TUR:
		return "tur";
	default:
		return "unknown";
	}
}

/**
 * multi_threshold_regulator_set_voltage - Set voltage for multi-threshold voltage regulator
 * @rdev: Regulator device
 * @min_uV: Requested mode (1=SVS, 2=NOM, 3=TUR)
 * @max_uV: Unused (required by regulator framework)
 * @sel: Unused selector pointer (required by regulator framework)
 *
 * This callback translates mode requests into actual voltages based on the
 * determined process type. The min_uV parameter encodes the operating mode
 * (1, 2, or 3), which is converted to an array index (0, 1, or 2) to look up
 * the appropriate voltage for the device's process type.
 *
 * Return: 0 on success, negative error code on failure
 */
static int multi_threshold_regulator_set_voltage(struct regulator_dev *rdev,
						 int min_uV, int max_uV,
						 unsigned int *sel)
{
	struct reg_info *reg_info = rdev_get_drvdata(rdev);
	enum type_id type;
	int voltage;
	int mode;

	mode = min_uV - 1;
	if (mode < 0 || mode >= MAX_MODES)
		return -EINVAL;

	/* Lookup type and voltage for the requested mode */
	type = reg_info->types[mode];
	voltage = reg_info->voltage_table[mode][type];

	dev_dbg(rdev_get_dev(rdev),
		"mode=%d type=%s -> %duV\n",
		mode,
		type_names[type],
		voltage);

	return regulator_set_voltage(reg_info->base_regulator, voltage, voltage);
}

static const struct regulator_ops multi_threshold_regulator_ops = {
	.set_voltage = multi_threshold_regulator_set_voltage,
};

/**
 * gpio_convert_open_loop_voltage_fuse - Convert fuse value to voltage
 * @ref_volt: Reference voltage in microvolts
 * @step_volt: Voltage step size in microvolts
 * @fuse: Raw fuse value read from NVMEM
 * @fuse_len: Number of bits in the fuse value
 *
 * Converts a signed fuse value to an absolute voltage. The MSB indicates sign
 * (0=positive, 1=negative), and remaining bits encode the step count.
 * Formula: voltage = ref_volt + (sign * steps * step_volt)
 *
 * Return: Calculated voltage in microvolts
 */
static int gpio_convert_open_loop_voltage_fuse(int ref_volt, int step_volt,
					       u8 fuse, int fuse_len)
{
	int steps;
	int sign;

	sign = (fuse & (1 << (fuse_len - 1))) ? -1 : 1;
	steps = fuse & ((1 << (fuse_len - 1)) - 1);

	return ref_volt + sign * steps * step_volt;
}

/**
 * process_single_threshold_regulator - Process single-threshold regulator
 * @dev: Device pointer
 * @reg_data: Regulator configuration data
 * @cpr_fuse: CPR fuse revision value
 * @fix_volt_max: Force maximum voltage flag
 *
 * Return: 0 on success, negative error code on failure
 */
static int process_single_threshold_regulator(struct device *dev,
					      const struct gpio_regulator_data *reg_data,
					      u8 cpr_fuse,
					      bool fix_volt_max)
{
	const struct gpio_regulator_params *params = reg_data->params;
	const struct voltage_config *vconfig = params->voltage_config;
	const struct fuse_params *fuse = &params->fuse_params[0];
	const int threshold_volt = vconfig->thresholds[0];
	const int *voltages = vconfig->voltage_table;
	struct regulator *gpio_regulator;
	int volt_select;
	int fused_volt;
	u16 volt_ticks;
	int ret;

	ret = nvmem_cell_read_u16(dev, reg_data->regulator_name, &volt_ticks);
	if (ret < 0)
		return dev_err_probe(dev,
				     ret,
				     "%s fuse read failed\n",
				     reg_data->regulator_name);

	fused_volt = gpio_convert_open_loop_voltage_fuse(fuse->reference_volt,
							 fuse->step_volt,
							 volt_ticks,
							 fuse->bit_len);

	gpio_regulator = devm_regulator_get(dev, reg_data->regulator_name);
	if (IS_ERR(gpio_regulator))
		return dev_err_probe(dev,
				     PTR_ERR(gpio_regulator),
				     "%s regulator get failed\n",
				     reg_data->regulator_name);

	if (!cpr_fuse || fix_volt_max)
		volt_select = voltages[MAX_VOLT];
	else
		volt_select = (fused_volt > threshold_volt) ?
			      voltages[MAX_VOLT] : voltages[MIN_VOLT];

	ret = regulator_set_voltage(gpio_regulator, volt_select, volt_select);
	if (ret < 0)
		return dev_err_probe(dev,
				     ret,
				     "%s voltage %duV set failed\n",
				     reg_data->regulator_name,
				     volt_select);

	return 0;
}

/**
 * process_multi_threshold_regulator - Process multi-threshold regulator
 * @dev: Device pointer
 * @reg_data: Regulator configuration data
 * @fix_volt_max: Force maximum voltage flag
 *
 * Return: 0 on success, negative error code on failure
 */
static int process_multi_threshold_regulator(struct device *dev,
					     const struct gpio_regulator_data *reg_data,
					     bool fix_volt_max)
{
	const struct gpio_regulator_params *params = reg_data->params;
	const struct voltage_config *vconfig = params->voltage_config;
	const struct fuse_params *fuse;
	struct regulator_config config;
	struct regulator_desc *desc;
	struct regulator_dev *rdev;
	struct reg_info *reg_info;
	char fuse_name[32];
	enum type_id type;
	int fused_volt;
	u16 volt_ticks;
	int mode;
	int ret;

	reg_info = devm_kzalloc(dev, sizeof(*reg_info), GFP_KERNEL);
	if (!reg_info)
		return -ENOMEM;

	if (params->num_fuses > MAX_MODES)
		return dev_err_probe(dev, -EINVAL, "Invalid num_fuses %d exceeds MAX_MODES %d\n",
			      params->num_fuses, MAX_MODES);

	memcpy(reg_info->voltage_table, vconfig->voltage_table,
	       sizeof(int) * params->num_fuses * MAX_TYPES);

	/* Read fuses and determine process type for each mode */
	for (mode = 0; mode < params->num_fuses; mode++) {
		fuse = &params->fuse_params[mode];

		if (fix_volt_max) {
			reg_info->types[mode] = TYPE2;
			reg_info->voltage_table[mode][TYPE0] = reg_info->voltage_table[mode][TYPE2];
			reg_info->voltage_table[mode][TYPE1] = reg_info->voltage_table[mode][TYPE2];
			dev_dbg(dev,
				"%s_%s: forced TYPE2 due to qcom,skip-voltage-scaling quirk\n",
				reg_data->regulator_name,
				get_mode_name(mode));
			continue;
		}

		snprintf(fuse_name, sizeof(fuse_name), "cpr_%s_%s",
			 reg_data->regulator_name, get_mode_name(mode));

		ret = nvmem_cell_read_u16(dev, fuse_name, &volt_ticks);
		if (ret < 0)
			return dev_err_probe(dev,
					     ret,
					     "%s fuse read failed\n",
					     fuse_name);

		fused_volt = gpio_convert_open_loop_voltage_fuse(fuse->reference_volt,
								 fuse->step_volt,
								 volt_ticks,
								 fuse->bit_len);

		type = (fused_volt <= vconfig->thresholds[MIN_VOLT]) ? TYPE0 :
		       (fused_volt <= vconfig->thresholds[MAX_VOLT]) ? TYPE1 : TYPE2;

		dev_dbg(dev,
			"%s_%s: fused=%duV, type=%s\n",
			reg_data->regulator_name,
			get_mode_name(mode),
			fused_volt,
			type_names[type]);

		reg_info->types[mode] = type;

		if (params->num_fuses == 1) {
			reg_info->types[MODE_NOM] = type;
			reg_info->types[MODE_TUR] = type;
			break;
		}
	}

	reg_info->base_regulator = devm_regulator_get(dev, reg_data->regulator_name);
	if (IS_ERR(reg_info->base_regulator))
		return dev_err_probe(dev,
				     PTR_ERR(reg_info->base_regulator),
				     "Failed to get %s regulator: %ld\n",
				     reg_data->regulator_name,
				     PTR_ERR(reg_info->base_regulator));

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->name = devm_kasprintf(dev, GFP_KERNEL, "%s_regulator",
				    reg_data->regulator_name);
	if (!desc->name)
		return -ENOMEM;

	desc->of_match = reg_data->regulator_name;
	desc->type = REGULATOR_VOLTAGE;
	desc->ops = &multi_threshold_regulator_ops;
	desc->owner = THIS_MODULE;

	config.dev = dev;
	config.driver_data = reg_info;

	rdev = devm_regulator_register(dev, desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev,
				     PTR_ERR(rdev),
				     "Failed to register %s regulator\n",
				     reg_data->regulator_name);

	dev_info(dev, "%s types: ", reg_data->regulator_name);
	for (mode = 0; mode < params->num_fuses; mode++) {
		pr_cont("%s=%s%s",
			get_mode_name(mode),
			type_names[reg_info->types[mode]],
			(mode < params->num_fuses - 1) ? ", " : "\n");
	}

	return 0;
}

/**
 * read_single_threshold_regulator_params - Read parameters for single-threshold regulators
 * @dev: Device pointer
 * @cpr_fuse: Pointer to store CPR fuse revision value
 * @fix_volt_max: Pointer to store force maximum voltage flag
 *
 * Reads the CPR (Core Power Reduction) fuse revision from NVMEM and checks
 * for platform-specific voltage scaling quirks. Used for single-threshold regulators
 * across different platforms (IPQ9574, IPQ9570, etc.).
 *
 * Return: 0 on success, negative error code on failure
 */
static int read_single_threshold_regulator_params(struct device *dev,
						  u8 *cpr_fuse,
						  bool *fix_volt_max)
{
	int ret;

	if (device_property_read_bool(dev, "skip-voltage-scaling-turboL1-sku-quirk")) {
		if (cpu_is_ipq9574() || cpu_is_ipq9570())
			*fix_volt_max = true;
	}

	ret = nvmem_cell_read_u8(dev, "cpr", cpr_fuse);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "CPR fuse revision read failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/**
 * gpio_regulator_probe - Probe function for GPIO regulator driver
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 */
static int gpio_regulator_probe(struct platform_device *pdev)
{
	const struct gpio_regulator_data *reg_data;
	struct device *dev = &pdev->dev;
	bool fix_volt_max = false;
	u8 cpr_fuse = 0;
	int ret;

	reg_data = of_device_get_match_data(dev);
	if (!reg_data)
		return -ENODEV;

	if (device_property_read_bool(dev, "qcom,skip-voltage-scaling"))
		fix_volt_max = true;

	if (reg_data->params->voltage_config->num_thresholds == 1) {
		ret = read_single_threshold_regulator_params(dev, &cpr_fuse, &fix_volt_max);
		if (ret < 0)
			return ret;
	}

	for (; reg_data->regulator_name; reg_data++) {
		if (reg_data->params->voltage_config->num_thresholds > 1)
			ret = process_multi_threshold_regulator(dev, reg_data, fix_volt_max);
		else
			ret = process_single_threshold_regulator(dev, reg_data, cpr_fuse,
								 fix_volt_max);

		if (ret < 0)
			dev_err(dev, "Failed to process %s regulator: %d\n",
				reg_data->regulator_name, ret);
	}

	return ret;
}

static const struct voltage_config ipq9574_apc_voltages = {
	.voltage_table = (int[]) {
		850000,
		925000,
	},
	.num_thresholds = 1,
	.thresholds = (int[]) {
		800000,
	},
};

static const struct voltage_config ipq9574_cx_voltages = {
	.voltage_table = (int[]) {
		800000,
		863000,
	},
	.num_thresholds = 1,
	.thresholds = (int[]) {
		800000,
	},
};

static const struct voltage_config ipq9574_mx_voltages = {
	.voltage_table = (int[]) {
		850000,
		925000,
	},
	.num_thresholds = 1,
	.thresholds = (int[]) {
		850000,
	},
};

static const struct voltage_config ipq9574_4state_apc_voltages = {
	.voltage_table = (int[]) {
		1004000,
		1068000,
	},
	.num_thresholds = 1,
	.thresholds = (int[]) {
		1002500,
	},
};

static const struct voltage_config ipq9574_4state_cx_voltages = {
	.voltage_table = (int[]) {
		850000,
		910000,
	},
	.num_thresholds = 1,
	.thresholds = (int[]) {
		850000,
	},
};

static const struct voltage_config ipq9650_apc_voltages = {
	.voltage_table = (int[]) {
		600000, 660000, 740000,
		660000, 753000, 815000,
		804000, 895000, 957000,
	},
	.num_thresholds = 2,
	.thresholds = (int[]) {
		650000,
		750000,
	},
};

static const struct voltage_config ipq9650_nsp_voltages = {
	.voltage_table = (int[]) {
		600000, 660000, 750000,
		660000, 750000, 815000,
	},
	.num_thresholds = 2,
	.thresholds = (int[]) {
		650000,
		750000,
	},
};

static const struct gpio_regulator_params ipq9574_apc_params = {
	.fuse_params = (struct fuse_params[]) {
		{6, 862500, 10000},
	},
	.voltage_config = &ipq9574_apc_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_params ipq9574_cx_params = {
	.fuse_params = (struct fuse_params[]) {
		{5, 800000, 10000},
	},
	.voltage_config = &ipq9574_cx_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_params ipq9574_mx_params = {
	.fuse_params = (struct fuse_params[]) {
		{5, 850000, 10000},
	},
	.voltage_config = &ipq9574_mx_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_params ipq9574_4state_apc_params = {
	.fuse_params = (struct fuse_params[]) {
		{6, 1062500, 10000},
	},
	.voltage_config = &ipq9574_4state_apc_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_params ipq9574_4state_cx_params = {
	.fuse_params = (struct fuse_params[]) {
		{5, 850000, 10000},
	},
	.voltage_config = &ipq9574_4state_cx_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_params ipq9650_apc_params = {
	.fuse_params = (struct fuse_params[]) {
		{6, 800000, 10000},
	},
	.voltage_config = &ipq9650_apc_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_params ipq9650_nsp_params = {
	.fuse_params = (struct fuse_params[]) {
		{6, 800000, 10000},
	},
	.voltage_config = &ipq9650_nsp_voltages,
	.num_fuses = 1,
};

static const struct gpio_regulator_data ipq9574_gpio_regulator_data[] = {
	{ "apc", &ipq9574_apc_params },
	{ "cx",  &ipq9574_cx_params },
	{ "mx",  &ipq9574_mx_params },
	{ }
};

static const struct gpio_regulator_data ipq9574_4state_regulator_data[] = {
	{ "apc", &ipq9574_4state_apc_params },
	{ "cx",  &ipq9574_4state_cx_params },
	{ }
};

static const struct gpio_regulator_data ipq9650_gpio_regulator_data[] = {
	{ "apc", &ipq9650_apc_params },
	{ "nsp", &ipq9650_nsp_params },
	{ }
};

static const struct of_device_id gpio_regulator_match_table[] = {
	{
		.compatible = "qcom,ipq9574-gpio-regulator",
		.data = &ipq9574_gpio_regulator_data
	},
	{
		.compatible = "qcom,ipq9574-4state-gpio-regulator",
		.data = &ipq9574_4state_regulator_data
	},
	{
		.compatible = "qcom,ipq9650-gpio-regulator",
		.data = &ipq9650_gpio_regulator_data
	},
	{}
};

static struct platform_driver gpio_regulator_driver = {
	.driver		= {
		.name		= "qcom,gpio-regulator",
		.of_match_table	= gpio_regulator_match_table,
	},
	.probe		= gpio_regulator_probe,
};

module_platform_driver(gpio_regulator_driver);

MODULE_DESCRIPTION("QTI GPIO regulator driver");
MODULE_LICENSE("GPL v2");
