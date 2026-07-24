// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/coupler.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/qcom-gpio-regulator.h>
#include <linux/string.h>

#include <soc/qcom/socinfo.h>

#define MIN_VOLT 0
#define MAX_VOLT 1

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
 * struct gpio_regulator_data - unified gpio regulator data structure
 * @regulator_name:	Regulator name which needs to be controlled
 * @params:		Pointer to regulator parameters
 */
struct gpio_regulator_data {
	const char *regulator_name;
	const struct gpio_regulator_params *params;
};

/**
 * enum type_id - Process type identifiers for voltage binning
 * @TYPE0: Lowest process corner
 * @TYPE1: Mid process corner
 * @TYPE2: Highest process corner
 *
 * Classification of a fused voltage against a rail's thresholds, used to
 * index the 2D [mode][type] voltage table. Purely internal to this driver;
 * qcom-open-loop-cpr-regulator.c never sees a type index, only the final
 * voltage returned by qcom_gpio_regulator_get_voltage().
 */
enum type_id {
	TYPE0,
	TYPE1,
	TYPE2,
	MAX_TYPES
};

static const char * const type_names[] = {"TYPE0", "TYPE1", "TYPE2"};

/**
 * struct gpio_regulator_entry - Registry entry for a GPIO-backed rail's
 *                                voltage table, keyed by owning device node
 *                                and rail name
 * @list: Linkage into gpio_regulator_registry
 * @of_node: of_node of the qcom-gpio-regulator platform device that owns
 *           this entry, matched against the gpio_np passed to
 *           qcom_gpio_regulator_get_voltage()
 * @rail_name: Rail name ("apc", ...)
 * @vconfig: Voltage table + thresholds for this rail
 */
struct gpio_regulator_entry {
	struct list_head list;
	struct device_node *of_node;
	const char *rail_name;
	const struct voltage_config *vconfig;
};

static DEFINE_MUTEX(gpio_regulator_registry_lock);
static LIST_HEAD(gpio_regulator_registry);

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
 * process_multi_threshold_table - Register the 2D voltage table for a
 *                                  GPIO-backed multi-threshold rail
 * @dev: Device pointer
 * @reg_data: Regulator configuration data
 *
 * Fuse reading, process-corner classification consumption, APM handling,
 * and regulator_dev registration for multi-threshold (GPIO-backed) rails
 * live in qcom-open-loop-cpr-regulator.c. This function only makes the
 * rail's 2D [mode][type] voltage table (plus any DT override) available
 * via qcom_gpio_regulator_get_voltage().
 *
 * Return: 0 on success, negative error code on failure
 */
static int process_multi_threshold_table(struct device *dev,
					 const struct gpio_regulator_data *reg_data)
{
	const struct voltage_config *vconfig = reg_data->params->voltage_config;
	struct voltage_config *override_vconfig;
	struct device_node *rail_node;
	struct gpio_regulator_entry *entry;

	/* Try to override from platform configuration if subnode exists */
	rail_node = of_get_child_by_name(dev->of_node, reg_data->regulator_name);
	if (rail_node) {
		override_vconfig = override_voltage_config(dev, rail_node);
		if (override_vconfig)
			vconfig = override_vconfig;

		of_node_put(rail_node);
	} else {
		dev_dbg(dev,
			"No platform override for %s, using defaults\n",
			reg_data->regulator_name);
	}

	entry = devm_kzalloc(dev, sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->of_node = dev->of_node;
	entry->rail_name = reg_data->regulator_name;
	entry->vconfig = vconfig;

	mutex_lock(&gpio_regulator_registry_lock);
	list_add_tail(&entry->list, &gpio_regulator_registry);
	mutex_unlock(&gpio_regulator_registry_lock);

	dev_info(dev, "%s: voltage table registered for GPIO-backed lookup\n",
		 reg_data->regulator_name);

	return 0;
}

/**
 * qcom_gpio_regulator_get_voltage - see qcom-gpio-regulator.h
 */
int qcom_gpio_regulator_get_voltage(struct device_node *gpio_np,
				    const char *rail_name,
				    int mode, int fused_volt,
				    bool fix_volt_max)
{
	struct gpio_regulator_entry *entry;
	const struct voltage_config *vconfig = NULL;
	bool node_found = false;
	enum type_id type;

	if (!gpio_np || !rail_name)
		return -EINVAL;

	if (mode < 0 || mode >= QCOM_GPIO_VT_MAX_MODES)
		return -EINVAL;

	mutex_lock(&gpio_regulator_registry_lock);
	list_for_each_entry(entry, &gpio_regulator_registry, list) {
		if (entry->of_node != gpio_np)
			continue;

		node_found = true;

		if (!strcmp(entry->rail_name, rail_name)) {
			vconfig = entry->vconfig;
			break;
		}
	}
	mutex_unlock(&gpio_regulator_registry_lock);

	if (!node_found)
		return -EPROBE_DEFER;

	if (!vconfig)
		return -ENOENT;

	if (fix_volt_max) {
		/*
		 * qcom,skip-voltage-scaling quirk: bypass threshold
		 * classification and use the highest process-corner column,
		 * matching how a direct-fuse (PMIC-backed) rail falls back to
		 * its ceiling voltage under the same quirk.
		 */
		type = TYPE2;
		pr_debug("qcom-gpio-regulator: %s mode=%d forced TYPE2 (skip-voltage-scaling)\n",
			 rail_name, mode);
	} else {
		type = (fused_volt <= vconfig->thresholds[MIN_VOLT]) ? TYPE0 :
		       (fused_volt <= vconfig->thresholds[MAX_VOLT]) ? TYPE1 : TYPE2;

		pr_debug("qcom-gpio-regulator: %s mode=%d fused=%duV -> %s\n",
			 rail_name, mode, fused_volt, type_names[type]);
	}

	return vconfig->voltage_table[mode * MAX_TYPES + type];
}
EXPORT_SYMBOL(qcom_gpio_regulator_get_voltage);

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
	int ret = 0;

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
			ret = process_multi_threshold_table(dev, reg_data);
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

/*
 * NSP CX has no turbo mode on GPIO-backed boards
 * Only SVS/SVS_L1/NOM are present, matching QCOM_GPIO_VT_MAX_MODES == 3.
 */
static const struct voltage_config ipq9650_nsp_voltages = {
	.voltage_table = (int[]) {
		600000, 660000, 750000,   /* SVS:    T0 T1 T2 */
		600000, 660000, 750000,   /* SVS_L1: T0 T1 T2 */
		660000, 750000, 815000,   /* NOM:    T0 T1 T2 */
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
		{7, 660000, 10000},
		{7, 753000, 10000},
		{7, 895000, 10000},
	},
	.voltage_config = &ipq9650_apc_voltages,
	.num_fuses = 3,
};

static const struct gpio_regulator_params ipq9650_nsp_params = {
	.fuse_params = (struct fuse_params[]) {
		{7, 660000, 10000},    /* SVS */
		{7, 660000, 10000},    /* SVS_L1 */
		{7, 750000, 10000},    /* NOM */
	},
	.voltage_config = &ipq9650_nsp_voltages,
	.num_fuses = 3,
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
	{ "nsp_cx",  &ipq9650_nsp_params },
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
