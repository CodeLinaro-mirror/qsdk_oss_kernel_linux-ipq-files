// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2018,2020 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "pinctrl-msm.h"

#define REG_SIZE 0x1000
#define PINGROUP(id, f1, f2, f3, f4, f5, f6, f7, f8, f9)	\
	{					        \
		.grp = PINCTRL_PINGROUP("gpio" #id,     \
			gpio##id##_pins,                \
			ARRAY_SIZE(gpio##id##_pins)),   \
		.funcs = (int[]){			\
			msm_mux_gpio, /* gpio mode */	\
			msm_mux_##f1,			\
			msm_mux_##f2,			\
			msm_mux_##f3,			\
			msm_mux_##f4,			\
			msm_mux_##f5,			\
			msm_mux_##f6,			\
			msm_mux_##f7,			\
			msm_mux_##f8,			\
			msm_mux_##f9			\
		},				        \
		.nfuncs = 10,				\
		.ctl_reg = REG_SIZE * id,	        \
		.io_reg = 0x4 + REG_SIZE * id,		\
		.intr_cfg_reg = 0x8 + REG_SIZE * id,	\
		.intr_status_reg = 0xc + REG_SIZE * id,	\
		.intr_target_reg = 0x8 + REG_SIZE * id,	\
		.mux_bit = 2,			\
		.pull_bit = 0,			\
		.drv_bit = 6,			\
		.oe_bit = 9,			\
		.in_bit = 0,			\
		.out_bit = 1,			\
		.intr_enable_bit = 0,		\
		.intr_status_bit = 0,		\
		.intr_target_bit = 5,		\
		.intr_target_kpss_val = 3,      \
		.intr_raw_status_bit = 4,	\
		.intr_polarity_bit = 1,		\
		.intr_detection_bit = 2,	\
		.intr_detection_width = 2,	\
	}

static const struct pinctrl_pin_desc ipq5200_pins[] = {
	PINCTRL_PIN(0, "GPIO_0"),
	PINCTRL_PIN(1, "GPIO_1"),
	PINCTRL_PIN(2, "GPIO_2"),
	PINCTRL_PIN(3, "GPIO_3"),
	PINCTRL_PIN(4, "GPIO_4"),
	PINCTRL_PIN(5, "GPIO_5"),
	PINCTRL_PIN(6, "GPIO_6"),
	PINCTRL_PIN(7, "GPIO_7"),
	PINCTRL_PIN(8, "GPIO_8"),
	PINCTRL_PIN(9, "GPIO_9"),
	PINCTRL_PIN(10, "GPIO_10"),
	PINCTRL_PIN(11, "GPIO_11"),
	PINCTRL_PIN(12, "GPIO_12"),
	PINCTRL_PIN(13, "GPIO_13"),
	PINCTRL_PIN(14, "GPIO_14"),
	PINCTRL_PIN(15, "GPIO_15"),
	PINCTRL_PIN(16, "GPIO_16"),
	PINCTRL_PIN(17, "GPIO_17"),
	PINCTRL_PIN(18, "GPIO_18"),
	PINCTRL_PIN(19, "GPIO_19"),
	PINCTRL_PIN(20, "GPIO_20"),
	PINCTRL_PIN(21, "GPIO_21"),
	PINCTRL_PIN(22, "GPIO_22"),
	PINCTRL_PIN(23, "GPIO_23"),
	PINCTRL_PIN(24, "GPIO_24"),
	PINCTRL_PIN(25, "GPIO_25"),
	PINCTRL_PIN(26, "GPIO_26"),
	PINCTRL_PIN(27, "GPIO_27"),
	PINCTRL_PIN(28, "GPIO_28"),
	PINCTRL_PIN(29, "GPIO_29"),
	PINCTRL_PIN(30, "GPIO_30"),
	PINCTRL_PIN(31, "GPIO_31"),
	PINCTRL_PIN(32, "GPIO_32"),
	PINCTRL_PIN(33, "GPIO_33"),
	PINCTRL_PIN(34, "GPIO_34"),
	PINCTRL_PIN(35, "GPIO_35"),
	PINCTRL_PIN(36, "GPIO_36"),
	PINCTRL_PIN(37, "GPIO_37"),
	PINCTRL_PIN(38, "GPIO_38"),
	PINCTRL_PIN(39, "GPIO_39"),
	PINCTRL_PIN(40, "GPIO_40"),
	PINCTRL_PIN(41, "GPIO_41"),
	PINCTRL_PIN(42, "GPIO_42"),
	PINCTRL_PIN(43, "GPIO_43"),
	PINCTRL_PIN(44, "GPIO_44"),
	PINCTRL_PIN(45, "GPIO_45"),
	PINCTRL_PIN(46, "GPIO_46"),
	PINCTRL_PIN(47, "GPIO_47"),
	PINCTRL_PIN(48, "GPIO_48"),
	PINCTRL_PIN(49, "GPIO_49"),
	PINCTRL_PIN(50, "GPIO_50"),
	PINCTRL_PIN(51, "GPIO_51"),
	PINCTRL_PIN(52, "GPIO_52"),
	PINCTRL_PIN(53, "GPIO_53"),
};

#define DECLARE_MSM_GPIO_PINS(pin) \
	static const unsigned int gpio##pin##_pins[] = { pin }
DECLARE_MSM_GPIO_PINS(0);
DECLARE_MSM_GPIO_PINS(1);
DECLARE_MSM_GPIO_PINS(2);
DECLARE_MSM_GPIO_PINS(3);
DECLARE_MSM_GPIO_PINS(4);
DECLARE_MSM_GPIO_PINS(5);
DECLARE_MSM_GPIO_PINS(6);
DECLARE_MSM_GPIO_PINS(7);
DECLARE_MSM_GPIO_PINS(8);
DECLARE_MSM_GPIO_PINS(9);
DECLARE_MSM_GPIO_PINS(10);
DECLARE_MSM_GPIO_PINS(11);
DECLARE_MSM_GPIO_PINS(12);
DECLARE_MSM_GPIO_PINS(13);
DECLARE_MSM_GPIO_PINS(14);
DECLARE_MSM_GPIO_PINS(15);
DECLARE_MSM_GPIO_PINS(16);
DECLARE_MSM_GPIO_PINS(17);
DECLARE_MSM_GPIO_PINS(18);
DECLARE_MSM_GPIO_PINS(19);
DECLARE_MSM_GPIO_PINS(20);
DECLARE_MSM_GPIO_PINS(21);
DECLARE_MSM_GPIO_PINS(22);
DECLARE_MSM_GPIO_PINS(23);
DECLARE_MSM_GPIO_PINS(24);
DECLARE_MSM_GPIO_PINS(25);
DECLARE_MSM_GPIO_PINS(26);
DECLARE_MSM_GPIO_PINS(27);
DECLARE_MSM_GPIO_PINS(28);
DECLARE_MSM_GPIO_PINS(29);
DECLARE_MSM_GPIO_PINS(30);
DECLARE_MSM_GPIO_PINS(31);
DECLARE_MSM_GPIO_PINS(32);
DECLARE_MSM_GPIO_PINS(33);
DECLARE_MSM_GPIO_PINS(34);
DECLARE_MSM_GPIO_PINS(35);
DECLARE_MSM_GPIO_PINS(36);
DECLARE_MSM_GPIO_PINS(37);
DECLARE_MSM_GPIO_PINS(38);
DECLARE_MSM_GPIO_PINS(39);
DECLARE_MSM_GPIO_PINS(40);
DECLARE_MSM_GPIO_PINS(41);
DECLARE_MSM_GPIO_PINS(42);
DECLARE_MSM_GPIO_PINS(43);
DECLARE_MSM_GPIO_PINS(44);
DECLARE_MSM_GPIO_PINS(45);
DECLARE_MSM_GPIO_PINS(46);
DECLARE_MSM_GPIO_PINS(47);
DECLARE_MSM_GPIO_PINS(48);
DECLARE_MSM_GPIO_PINS(49);
DECLARE_MSM_GPIO_PINS(50);
DECLARE_MSM_GPIO_PINS(51);
DECLARE_MSM_GPIO_PINS(52);
DECLARE_MSM_GPIO_PINS(53);

enum ipq5200_functions {
	msm_mux_gpio,
	msm_mux__,
};

static const char * const gpio_groups[] = {
	"gpio0", "gpio1", "gpio2", "gpio3", "gpio4", "gpio5", "gpio6", "gpio7",
	"gpio8", "gpio9", "gpio10", "gpio11", "gpio12", "gpio13", "gpio14",
	"gpio15", "gpio16", "gpio17", "gpio18", "gpio19", "gpio20", "gpio21",
	"gpio22", "gpio23", "gpio24", "gpio25", "gpio26", "gpio27", "gpio28",
	"gpio29", "gpio30", "gpio31", "gpio32", "gpio33", "gpio34", "gpio35",
	"gpio36", "gpio37", "gpio38", "gpio39", "gpio40", "gpio41", "gpio42",
	"gpio43", "gpio44", "gpio45", "gpio46", "gpio47", "gpio48", "gpio49",
	"gpio50", "gpio51", "gpio52", "gpio53",
};

static const struct pinfunction ipq5200_functions[] = {
	MSM_PIN_FUNCTION(gpio),
};

static const struct msm_pingroup ipq5200_groups[] = {
	PINGROUP(0, _, _, _, _, _, _, _, _, _),
	PINGROUP(1, _, _, _, _, _, _, _, _, _),
	PINGROUP(2, _, _, _, _, _, _, _, _, _),
	PINGROUP(3, _, _, _, _, _, _, _, _, _),
	PINGROUP(4, _, _, _, _, _, _, _, _, _),
	PINGROUP(5, _, _, _, _, _, _, _, _, _),
	PINGROUP(6, _, _, _, _, _, _, _, _, _),
	PINGROUP(7, _, _, _, _, _, _, _, _, _),
	PINGROUP(8, _, _, _, _, _, _, _, _, _),
	PINGROUP(9, _, _, _, _, _, _, _, _, _),
	PINGROUP(10, _, _, _, _, _, _, _, _, _),
	PINGROUP(11, _, _, _, _, _, _, _, _, _),
	PINGROUP(12, _, _, _, _, _, _, _, _, _),
	PINGROUP(13, _, _, _, _, _, _, _, _, _),
	PINGROUP(14, _, _, _, _, _, _, _, _, _),
	PINGROUP(15, _, _, _, _, _, _, _, _, _),
	PINGROUP(16, _, _, _, _, _, _, _, _, _),
	PINGROUP(17, _, _, _, _, _, _, _, _, _),
	PINGROUP(18, _, _, _, _, _, _, _, _, _),
	PINGROUP(19, _, _, _, _, _, _, _, _, _),
	PINGROUP(20, _, _, _, _, _, _, _, _, _),
	PINGROUP(21, _, _, _, _, _, _, _, _, _),
	PINGROUP(22, _, _, _, _, _, _, _, _, _),
	PINGROUP(23, _, _, _, _, _, _, _, _, _),
	PINGROUP(24, _, _, _, _, _, _, _, _, _),
	PINGROUP(25, _, _, _, _, _, _, _, _, _),
	PINGROUP(26, _, _, _, _, _, _, _, _, _),
	PINGROUP(27, _, _, _, _, _, _, _, _, _),
	PINGROUP(28, _, _, _, _, _, _, _, _, _),
	PINGROUP(29, _, _, _, _, _, _, _, _, _),
	PINGROUP(30, _, _, _, _, _, _, _, _, _),
	PINGROUP(31, _, _, _, _, _, _, _, _, _),
	PINGROUP(32, _, _, _, _, _, _, _, _, _),
	PINGROUP(33, _, _, _, _, _, _, _, _, _),
	PINGROUP(34, _, _, _, _, _, _, _, _, _),
	PINGROUP(35, _, _, _, _, _, _, _, _, _),
	PINGROUP(36, _, _, _, _, _, _, _, _, _),
	PINGROUP(37, _, _, _, _, _, _, _, _, _),
	PINGROUP(38, _, _, _, _, _, _, _, _, _),
	PINGROUP(39, _, _, _, _, _, _, _, _, _),
	PINGROUP(40, _, _, _, _, _, _, _, _, _),
	PINGROUP(41, _, _, _, _, _, _, _, _, _),
	PINGROUP(42, _, _, _, _, _, _, _, _, _),
	PINGROUP(43, _, _, _, _, _, _, _, _, _),
	PINGROUP(44, _, _, _, _, _, _, _, _, _),
	PINGROUP(45, _, _, _, _, _, _, _, _, _),
	PINGROUP(46, _, _, _, _, _, _, _, _, _),
	PINGROUP(47, _, _, _, _, _, _, _, _, _),
	PINGROUP(48, _, _, _, _, _, _, _, _, _),
	PINGROUP(49, _, _, _, _, _, _, _, _, _),
	PINGROUP(50, _, _, _, _, _, _, _, _, _),
	PINGROUP(51, _, _, _, _, _, _, _, _, _),
	PINGROUP(52, _, _, _, _, _, _, _, _, _),
	PINGROUP(53, _, _, _, _, _, _, _, _, _),
};

static const struct msm_pinctrl_soc_data ipq5200_pinctrl = {
	.pins = ipq5200_pins,
	.npins = ARRAY_SIZE(ipq5200_pins),
	.functions = ipq5200_functions,
	.nfunctions = ARRAY_SIZE(ipq5200_functions),
	.groups = ipq5200_groups,
	.ngroups = ARRAY_SIZE(ipq5200_groups),
	.ngpios = 54,
};

static int ipq5200_pinctrl_probe(struct platform_device *pdev)
{
	return msm_pinctrl_probe(pdev, &ipq5200_pinctrl);
}

static const struct of_device_id ipq5200_pinctrl_of_match[] = {
	{ .compatible = "qcom,ipq5200-tlmm", },
	{ },
};
MODULE_DEVICE_TABLE(of, ipq5200_pinctrl_of_match);

static struct platform_driver ipq5200_pinctrl_driver = {
	.driver = {
		.name = "ipq5200-tlmm",
		.of_match_table = ipq5200_pinctrl_of_match,
	},
	.probe = ipq5200_pinctrl_probe,
	.remove = msm_pinctrl_remove,
};

static int __init ipq5200_pinctrl_init(void)
{
	return platform_driver_register(&ipq5200_pinctrl_driver);
}
arch_initcall(ipq5200_pinctrl_init);

static void __exit ipq5200_pinctrl_exit(void)
{
	platform_driver_unregister(&ipq5200_pinctrl_driver);
}
module_exit(ipq5200_pinctrl_exit);

MODULE_DESCRIPTION("QTI IPQ5200 TLMM driver");
MODULE_LICENSE("GPL");
