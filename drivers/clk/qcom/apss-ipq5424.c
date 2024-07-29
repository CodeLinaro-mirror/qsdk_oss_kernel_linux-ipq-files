// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,apss-ipq.h>
#include <dt-bindings/arm/qcom,ids.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "common.h"

enum {
	P_XO,
	P_GPLL0,
	P_APSS_PLL_EARLY,
};

/*
 * IPQ5424 Huayra PLL offsets are different from the one mentioned in the
 * clk-alpha-pll.c, hence define the IPQ5424 offsets here
 */
static const u8 ipq5424_pll_offsets[][PLL_OFF_MAX_REGS] = {
	[CLK_ALPHA_PLL_TYPE_HUAYRA] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_CONFIG_CTL] = 0x10,
		[PLL_OFF_CONFIG_CTL_U] = 0x14,
		[PLL_OFF_CONFIG_CTL_U1] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_TEST_CTL_U] = 0x20,
		[PLL_OFF_TEST_CTL_U1] = 0x24,
		[PLL_OFF_STATUS] = 0x38,
	},
};

static struct clk_alpha_pll ipq5424_apss_pll = {
	.offset = 0x0,
	.regs = ipq5424_pll_offsets[CLK_ALPHA_PLL_TYPE_HUAYRA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = 0x0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "apss_pll_early",
			.parent_data = &(const struct clk_parent_data) {
				.fw_name = "xo",
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_huayra_ops,
		},
	},
};

static const struct clk_parent_data parents_apcs_alias0_clk_src[] = {
	{ .fw_name = "xo" },
	{ .fw_name = "gpll0" },
	{ .fw_name = "pll" },
};

static const struct parent_map parents_apcs_alias0_clk_src_map[] = {
	{ P_XO, 0 },
	{ P_GPLL0, 4 },
	{ P_APSS_PLL_EARLY, 5 },
};

static struct clk_rcg2 apcs_alias0_clk_src = {
	.cmd_rcgr = 0x0080,
	.hid_width = 5,
	.parent_map = parents_apcs_alias0_clk_src_map,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "apcs_alias0_clk_src",
		.parent_data = parents_apcs_alias0_clk_src,
		.num_parents = ARRAY_SIZE(parents_apcs_alias0_clk_src),
		.ops = &clk_rcg2_mux_closest_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_branch apcs_alias0_core_clk = {
	.halt_reg = 0x008c,
	.clkr = {
		.enable_reg = 0x008c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "apcs_alias0_core_clk",
			.parent_hws = (const struct clk_hw *[]){
				&apcs_alias0_clk_src.clkr.hw },
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static const struct regmap_config apss_ipq5424_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x10000,
	.fast_io        = true,
};

static struct clk_regmap *apss_ipq5424_clks[] = {
	[P_APSS_PLL_EARLY] = &ipq5424_apss_pll.clkr,
	[APCS_ALIAS0_CLK_SRC] = &apcs_alias0_clk_src.clkr,
	[APCS_ALIAS0_CORE_CLK] = &apcs_alias0_core_clk.clkr,
};

static const struct qcom_cc_desc apss_ipq5424_desc = {
	.config = &apss_ipq5424_regmap_config,
	.clks = apss_ipq5424_clks,
	.num_clks = ARRAY_SIZE(apss_ipq5424_clks),
};

static const struct alpha_pll_config ipq5424_pll_config = {
	.l = 0x3b,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.test_ctl_val = 0x0,
	.test_ctl_hi_val = 0x0,
	.test_ctl_hi1_val = 0x0,
	.user_ctl_val = 0x1,
	.early_output_mask = BIT(3),
	.aux2_output_mask = BIT(2),
	.aux_output_mask = BIT(1),
	.main_output_mask = BIT(0),
};

static int cpu_clk_notifier_fn(struct notifier_block *nb, unsigned long action,
				void *data)
{
	struct clk_hw *hw;
	u8 index;
	int err;

	if (action == PRE_RATE_CHANGE)
		index = P_GPLL0;
	else if (action == POST_RATE_CHANGE || action == ABORT_RATE_CHANGE)
		index = P_APSS_PLL_EARLY;
	else
		return NOTIFY_OK;

	hw = &apcs_alias0_clk_src.clkr.hw;
	err = clk_rcg2_mux_closest_ops.set_parent(hw, index);

	return notifier_from_errno(err);
}

static int apss_ipq5424_probe(struct platform_device *pdev)
{
	struct clk_hw *hw = &apcs_alias0_clk_src.clkr.hw;
	struct notifier_block *cpu_clk_notifier;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	void __iomem *base;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &apss_ipq5424_regmap_config);
	if (!regmap)
		return PTR_ERR(regmap);

	clk_alpha_pll_configure(&ipq5424_apss_pll, regmap, &ipq5424_pll_config);

	ret = qcom_cc_really_probe(pdev, &apss_ipq5424_desc, regmap);
	if (ret)
		return ret;

	cpu_clk_notifier = devm_kzalloc(&pdev->dev,
					sizeof(*cpu_clk_notifier),
					GFP_KERNEL);
	if (!cpu_clk_notifier)
		return -ENOMEM;

	cpu_clk_notifier->notifier_call = cpu_clk_notifier_fn;

	ret = devm_clk_notifier_register(&pdev->dev, hw->clk, cpu_clk_notifier);
	if (ret)
		return ret;

	return 0;
}

static struct platform_driver apss_ipq5424_driver = {
	.probe = apss_ipq5424_probe,
	.driver = {
		.name   = "qcom,apss-ipq5424-clk",
	},
};

module_platform_driver(apss_ipq5424_driver);

MODULE_DESCRIPTION("QCOM APSS IPQ5424 CLK Driver");
MODULE_LICENSE("GPL v2");
