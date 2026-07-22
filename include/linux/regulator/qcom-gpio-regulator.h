/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QCOM_GPIO_REGULATOR_H__
#define __QCOM_GPIO_REGULATOR_H__

#include <linux/err.h>

#define QCOM_GPIO_VT_MAX_MODES  3

struct device_node;

/**
 * qcom_gpio_regulator_get_voltage - Look up the GPIO-backed voltage for a
 *                                    fused voltage on a given rail/mode
 * @gpio_np: Device node of the qcom-gpio-regulator platform device that owns
 *           the rail's 2D voltage table (the node pointed to by a
 *           "qcom,gpio-regulator" phandle)
 * @rail_name: Rail name ("apc", "nsp-cx", ...), matches the gpio-regulator
 *             match-data entry and DT subnode name
 * @mode: Operating mode index (0=SVS, 1=NOM, 2=TUR)
 * @fused_volt: Fuse-corrected voltage in microvolts for this mode, used to
 *              classify the process-corner type (TYPE0/1/2) before the 2D
 *              table lookup, unless @fix_volt_max is set
 * @fix_volt_max: Skip threshold classification and use the highest
 *                process-corner column (TYPE2) directly. Set when the
 *                caller's qcom,skip-voltage-scaling quirk is active,
 *                mirroring how a fuse-corrected voltage is bypassed in
 *                favor of the ceiling voltage for direct-fuse (PMIC-backed)
 *                rails.
 *
 * qcom-gpio-regulator.c owns the 2D [mode][type] voltage table and the
 * threshold classification for GPIO-backed rails; this call performs both
 * steps and returns the final actuation voltage. The caller (the open loop
 * CPR regulator driver) has already derived @fused_volt from its own fuse
 * read and only needs the resulting µV value.
 *
 * Return: Voltage in microvolts on success, -EPROBE_DEFER if the target
 *         gpio-regulator device hasn't probed yet, -ENOENT if @rail_name is
 *         not found, or another negative error code on failure
 */
#ifdef CONFIG_REGULATOR_QTI_GPIO
int qcom_gpio_regulator_get_voltage(struct device_node *gpio_np,
				    const char *rail_name,
				    int mode, int fused_volt, bool fix_volt_max);
#else
static inline int qcom_gpio_regulator_get_voltage(struct device_node *gpio_np,
						  const char *rail_name,
						  int mode, int fused_volt,
						  bool fix_volt_max)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __QCOM_GPIO_REGULATOR_H__ */

