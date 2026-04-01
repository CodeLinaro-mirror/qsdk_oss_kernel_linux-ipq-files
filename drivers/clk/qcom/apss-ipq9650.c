// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/interconnect-provider.h>

#include <dt-bindings/clock/qcom,apss-ipq.h>
#include <dt-bindings/arm/qcom,ids.h>
#include <dt-bindings/interconnect/qcom,ipq9650.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-regmap.h"
#include "common.h"

enum {
	P_XO,
	P_GPLL0,
	P_SILVER_PLL,
	P_GOLD_PLL,
	P_L3_PLL,
};

/* PLL Base Offsets */
#define SILVER_PLL_BASE			0x0
#define GOLD_PLL_BASE			0x10000
#define L3_PLL_BASE			0x20000

/* GFMUX Register Offsets */
#define SILVER_GFMUX_REG		(SILVER_PLL_BASE + 0x84)
#define GOLD_GFMUX_REG			(GOLD_PLL_BASE + 0x84)
#define L3_GFMUX_REG			(L3_PLL_BASE + 0x84)
#define GFMUX_SRC_SEL_MASK		0xF

/* CBCR Register Offsets */
#define SILVER_CBCR			(SILVER_PLL_BASE + 0x8c)
#define GOLD_CBCR			(GOLD_PLL_BASE + 0x8c)
#define L3_CBCR				(L3_PLL_BASE + 0x8c)

/* Gold PLL Post-Divider */
#define GOLD_PLL_POST_DIV_SHIFT		8
#define GOLD_PLL_POST_DIV_WIDTH		2

/* Clock Rates */
#define GPLL0_RATE			800000000UL

#define SILVER_PLL_VCO_MIN		816000000UL
#define SILVER_PLL_VCO_MAX		1968000000UL
#define GOLD_PLL_VCO_MIN		696000000UL
#define GOLD_PLL_VCO_MAX		1056000000UL
#define L3_PLL_VCO_MIN			696000000UL
#define L3_PLL_VCO_MAX			1512000000UL

#define PLL_EARLY_INDEX			2

static struct pll_vco silver_pll_vco[] = {
	{ SILVER_PLL_VCO_MIN, SILVER_PLL_VCO_MAX, 0 },
};

static struct pll_vco gold_pll_vco[] = {
	{ GOLD_PLL_VCO_MIN, GOLD_PLL_VCO_MAX, 0 },
};

static struct pll_vco l3_pll_vco[] = {
	{ L3_PLL_VCO_MIN, L3_PLL_VCO_MAX, 0 },
};

static const struct alpha_pll_config silver_apss_pll_config = {
	.l = 0x23,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.user_ctl_val = 0x01000009,
	.test_ctl_val = 0x0,
	.test_ctl_hi_val = 0x0,
	.test_ctl_hi1_val = 0x0,
};

static const struct alpha_pll_config gold_apss_pll_config = {
	.l = 0x26,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.user_ctl_val = 0x01000009,
	.test_ctl_val = 0x0,
	.test_ctl_hi_val = 0x0,
	.test_ctl_hi1_val = 0x0,
};

static const struct alpha_pll_config l3_pll_config = {
	.l = 0x1e,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.user_ctl_val = 0x01000009,
	.test_ctl_val = 0x0,
	.test_ctl_hi_val = 0x0,
	.test_ctl_hi1_val = 0x0,
};

static struct clk_alpha_pll silver_apss_pll = {
	.offset = SILVER_PLL_BASE,
	.vco_table = silver_pll_vco,
	.num_vco = ARRAY_SIZE(silver_pll_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = SILVER_PLL_BASE,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "silver_apss_pll",
			.parent_data = &(const struct clk_parent_data){
				.fw_name = "xo",
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL,
			.ops = &clk_alpha_pll_zonda_ops,
		},
	},
};

static struct clk_alpha_pll gold_apss_pll = {
	.offset = GOLD_PLL_BASE,
	.vco_table = gold_pll_vco,
	.num_vco = ARRAY_SIZE(gold_pll_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = GOLD_PLL_BASE,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gold_apss_pll",
			.parent_data = &(const struct clk_parent_data){
				.fw_name = "xo",
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL,
			.ops = &clk_alpha_pll_zonda_ops,
		},
	},
};

static const struct clk_div_table gold_apss_pll_post_div_table[] = {
	{ 0x0, 1 },
	{ 0x1, 2 },
	{ }
};

static struct clk_alpha_pll_postdiv gold_apss_pll_postdiv = {
	.offset = GOLD_PLL_BASE,
	.post_div_shift = GOLD_PLL_POST_DIV_SHIFT,
	.post_div_table = gold_apss_pll_post_div_table,
	.num_post_div = ARRAY_SIZE(gold_apss_pll_post_div_table),
	.width = GOLD_PLL_POST_DIV_WIDTH,
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gold_apss_pll_postdiv",
		.parent_hws = (const struct clk_hw*[]){
			&gold_apss_pll.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_alpha_pll_postdiv_zonda_ops,
	},
};

static struct clk_alpha_pll l3_pll = {
	.offset = L3_PLL_BASE,
	.vco_table = l3_pll_vco,
	.num_vco = ARRAY_SIZE(l3_pll_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = L3_PLL_BASE,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "l3_pll",
			.parent_data = &(const struct clk_parent_data){
				.fw_name = "xo",
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL,
			.ops = &clk_alpha_pll_zonda_ops,
		},
	},
};

/**
 * struct clk_gfm - Glitch-Free Mux clock
 * @mux_reg:  register offset for mux source selection
 * @mux_mask: bitmask for source selection field
 * @hw:       handle between common and hardware-specific interfaces
 * @base:     MMIO base address of the APSS clock region
 *
 * Represents a glitch-free multiplexer used to switch the CPU cluster
 * clock source between XO, GPLL0, and the dedicated cluster PLL without
 * introducing glitches on the output clock.
 */
struct clk_gfm {
	u32 mux_reg;
	u32 mux_mask;
	struct clk_hw hw;
	void __iomem *base;
};

#define to_clk_gfm(_hw) container_of(_hw, struct clk_gfm, hw)

/**
 * clk_gfm_get_parent() - Get current parent of a GFMUX clock
 * @hw: clock hardware handle
 *
 * Reads the mux register and returns the index of the currently
 * selected parent clock.
 *
 * Return: index of the current parent clock
 */
static u8 clk_gfm_get_parent(struct clk_hw *hw)
{
	struct clk_gfm *gfm = to_clk_gfm(hw);
	u32 val;

	val = readl(gfm->base + gfm->mux_reg);
	return val & gfm->mux_mask;
}

/**
 * clk_gfm_set_parent() - Set parent of a GFMUX clock
 * @hw:    clock hardware handle
 * @index: index of the parent clock to select
 *
 * Writes the mux register to switch the GFMUX to the specified parent.
 *
 * Return: 0 on success
 */
static int clk_gfm_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_gfm *gfm = to_clk_gfm(hw);
	u32 val;

	val = readl(gfm->base + gfm->mux_reg);
	val &= ~gfm->mux_mask;
	val |= (index & gfm->mux_mask);
	writel(val, gfm->base + gfm->mux_reg);

	return 0;
}

static const struct clk_ops clk_gfm_ops = {
	.get_parent = clk_gfm_get_parent,
	.set_parent = clk_gfm_set_parent,
	.determine_rate = __clk_mux_determine_rate_closest,
};

static struct clk_gfm silver_apss_pll_gfmux = {
	.mux_reg = SILVER_GFMUX_REG,
	.mux_mask = GFMUX_SRC_SEL_MASK,
	.hw.init = &(struct clk_init_data){
		.name = "silver_apss_pll_gfmux",
		.parent_data = (const struct clk_parent_data[]){
			{ .fw_name = "xo" },
			{ .fw_name = "gpll0" },
			{ .hw = &silver_apss_pll.clkr.hw },
		},
		.num_parents = 3,
		.ops = &clk_gfm_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_gfm gold_apss_pll_gfmux = {
	.mux_reg = GOLD_GFMUX_REG,
	.mux_mask = GFMUX_SRC_SEL_MASK,
	.hw.init = &(struct clk_init_data){
		.name = "gold_apss_pll_gfmux",
		.parent_data = (const struct clk_parent_data[]){
			{ .fw_name = "xo" },
			{ .fw_name = "gpll0" },
			{ .hw = &gold_apss_pll_postdiv.clkr.hw },
		},
		.num_parents = 3,
		.ops = &clk_gfm_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_gfm l3_pll_gfmux = {
	.mux_reg = L3_GFMUX_REG,
	.mux_mask = GFMUX_SRC_SEL_MASK,
	.hw.init = &(struct clk_init_data){
		.name = "l3_pll_gfmux",
		.parent_data = (const struct clk_parent_data[]){
			{ .fw_name = "xo" },
			{ .fw_name = "gpll0" },
			{ .hw = &l3_pll.clkr.hw },
		},
		.num_parents = 3,
		.ops = &clk_gfm_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_branch apss_silver_core_clk = {
	.halt_reg = SILVER_CBCR,
	.clkr = {
		.enable_reg = SILVER_CBCR,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "apss_silver_clk",
			.parent_hws = (const struct clk_hw*[]){
				&silver_apss_pll_gfmux.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch apss_gold_core_clk = {
	.halt_reg = GOLD_CBCR,
	.clkr = {
		.enable_reg = GOLD_CBCR,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "apss_gold_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gold_apss_pll_gfmux.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch l3_core_clk = {
	.halt_reg = L3_CBCR,
	.clkr = {
		.enable_reg = L3_CBCR,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "l3_core_clk",
			.parent_hws = (const struct clk_hw*[]){
				&l3_pll_gfmux.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_hw *apss_hws[] = {
	&silver_apss_pll_gfmux.hw,
	&gold_apss_pll_gfmux.hw,
	&l3_pll_gfmux.hw,
};

static struct clk_regmap *apss_clks[] = {
	[APSS_PLL_EARLY] = &silver_apss_pll.clkr,
	[APSS_GOLD_PLL] = &gold_apss_pll.clkr,
	[APSS_GOLD_PLL_POSTDIV] = &gold_apss_pll_postdiv.clkr,
	[L3_PLL] = &l3_pll.clkr,
	[APSS_SILVER_CORE_CLK] = &apss_silver_core_clk.clkr,
	[APSS_GOLD_CORE_CLK] = &apss_gold_core_clk.clkr,
	[L3_CORE_CLK] = &l3_core_clk.clkr,
};

static const struct regmap_config apss_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x30000,
	.fast_io = true,
};

#define IPQ_APPS_PLL_ID			(9650 * 3)	/* some unique value */

static struct qcom_icc_hws_data icc_cpu_l3[] = {
	{ MASTER_CPU_SILVER, SLAVE_L3, L3_CORE_CLK },
	{ MASTER_CPU_GOLD, SLAVE_L3, L3_CORE_CLK },
};

static const struct qcom_cc_desc apss_desc = {
	.config = &apss_regmap_config,
	.clks = apss_clks,
	.num_clks = ARRAY_SIZE(apss_clks),
	.icc_hws = icc_cpu_l3,
	.num_icc_hws = ARRAY_SIZE(icc_cpu_l3),
	.icc_first_node_id = IPQ_APPS_PLL_ID,
};

/**
 * apss_ipq9650_probe() - Probe the IPQ9650 APSS clock controller
 * @pdev: platform device
 *
 * Maps the APSS register region, configures the Silver, Gold, and L3
 * Zonda PLLs with their initial SVS settings, registers the GFMUX
 * clocks, and registers all regmap-based clocks via qcom_cc_really_probe().
 *
 * Return: 0 on success, negative error code on failure
 */
static int apss_ipq9650_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	struct clk_gfm *gfm;
	void __iomem *base;
	int ret, i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(&pdev->dev, base,
				       &apss_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Configure PLLs with initial settings */
	clk_zonda_pll_configure(&silver_apss_pll, regmap, &silver_apss_pll_config);
	clk_zonda_pll_configure(&gold_apss_pll, regmap, &gold_apss_pll_config);
	clk_zonda_pll_configure(&l3_pll, regmap, &l3_pll_config);

	/* Register GFMUX clocks */
	for (i = 0; i < ARRAY_SIZE(apss_hws); i++) {
		gfm = to_clk_gfm(apss_hws[i]);
		gfm->base = base;

		ret = devm_clk_hw_register(&pdev->dev, apss_hws[i]);
		if (ret)
			return ret;
	}

	/* Register all other clocks */
	ret = qcom_cc_really_probe(&pdev->dev, &apss_desc, regmap);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register APSS clocks\n");
		return ret;
	}

	dev_dbg(&pdev->dev, "Registered IPQ9650 APSS clock provider\n");

	return 0;
}

static const struct of_device_id apss_ipq9650_match_table[] = {
	{ .compatible = "qcom,apss-ipq9650-clk" },
	{ }
};
MODULE_DEVICE_TABLE(of, apss_ipq9650_match_table);

static struct platform_driver apss_ipq9650_driver = {
	.probe = apss_ipq9650_probe,
	.driver = {
		.name = "qcom,ipq9650-apss",
		.of_match_table = apss_ipq9650_match_table,
		.sync_state = icc_sync_state,
	},
};

module_platform_driver(apss_ipq9650_driver);

MODULE_DESCRIPTION("Qualcomm IPQ9650 APSS Clock Driver");
MODULE_LICENSE("GPL");
