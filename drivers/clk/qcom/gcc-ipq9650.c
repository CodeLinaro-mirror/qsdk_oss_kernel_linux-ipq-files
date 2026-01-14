// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018,2020 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,ipq9650-gcc.h>
#include <dt-bindings/reset/qcom,ipq9650-gcc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "clk-regmap-divider.h"
#include "clk-regmap-mux.h"
#include "clk-regmap-phy-mux.h"
#include "common.h"
#include "reset.h"

static int clk_dummy_is_enabled(struct clk_hw *hw)
{
	return 1;
};

static int clk_dummy_enable(struct clk_hw *hw)
{
	return 0;
};

static void clk_dummy_disable(struct clk_hw *hw)
{
	return;
};

static u8 clk_dummy_get_parent(struct clk_hw *hw)
{
	return 0;
};

static int clk_dummy_set_parent(struct clk_hw *hw, u8 index)
{
	return 0;
};

static int clk_dummy_set_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	return 0;
};

static int clk_dummy_determine_rate(struct clk_hw *hw,
		struct clk_rate_request *req)
{
	return 0;
};

static unsigned long clk_dummy_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	return parent_rate;
};

static const struct clk_ops clk_dummy_ops = {
	.is_enabled = clk_dummy_is_enabled,
	.enable = clk_dummy_enable,
	.disable = clk_dummy_disable,
	.get_parent = clk_dummy_get_parent,
	.set_parent = clk_dummy_set_parent,
	.set_rate = clk_dummy_set_rate,
	.recalc_rate = clk_dummy_recalc_rate,
	.determine_rate = clk_dummy_determine_rate,
};

#define DEFINE_DUMMY_CLK(clk_name)                       \
	(&(struct clk_regmap) {                          \
	 .hw.init = &(struct clk_init_data){             \
	 .name = #clk_name,                              \
	 .parent_names = (const char *[]){ "xo"},        \
	 .num_parents = 1,                               \
	 .ops = &clk_dummy_ops,                          \
	 },                                              \
	 })

static struct clk_regmap *gcc_ipq9650_dummy_clks[] = {
	[GCC_QUPV3_AHB_MST_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_ahb_mst),
	[GCC_QUPV3_AHB_SLV_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_ahb_slv),
	[GCC_QUPV3_UART0_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_uart0),
	[GCC_QUPV3_UART1_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_uart1),
	[GCC_SDCC1_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_sdcc1_ahb_clk),
	[GCC_SDCC1_APPS_CLK] = DEFINE_DUMMY_CLK(gcc_sdcc1_apps_clk),
	[GCC_CMN_12GPLL_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_cmn_12gpll_ahb),
	[GCC_CMN_12GPLL_SYS_CLK] = DEFINE_DUMMY_CLK(gcc_cmn_12gpll_sys),
	[GCC_UNIPHY0_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_uniphy0_ahb),
	[GCC_UNIPHY1_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_uniphy1_ahb),
	[GCC_UNIPHY2_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_uniphy2_ahb),
	[GCC_UNIPHY0_SYS_CLK] = DEFINE_DUMMY_CLK(gcc_uniphy0_sys),
	[GCC_UNIPHY1_SYS_CLK] = DEFINE_DUMMY_CLK(gcc_uniphy1_sys),
	[GCC_UNIPHY2_SYS_CLK] = DEFINE_DUMMY_CLK(gcc_uniphy2_sys),
	[GCC_NSSNOC_NSSCC_CLK] = DEFINE_DUMMY_CLK(gcc_nssnoc_nsscc),
	[GCC_NSSCC_CLK] = DEFINE_DUMMY_CLK(gcc_nsscc),
	[GCC_NSSNOC_SNOC_CLK] = DEFINE_DUMMY_CLK(gcc_nssnoc_snoc),
	[GCC_NSSNOC_SNOC_1_CLK] = DEFINE_DUMMY_CLK(gcc_nssnoc_snoc_1),
	[GCC_PCIE0_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_aux_clk),
	[GCC_PCIE0_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_ahb_clk),
	[GCC_ANOC_PCIE0_1LANE_M_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie0_1lane_m_clk),
	[GCC_ANOC_PCIE0_1LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie0_1lane_s_clk),
	[GCC_PCIE0_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_axi_m_clk),
	[GCC_PCIE0_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_axi_s_clk),
	[GCC_PCIE0_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_axi_s_bridge_clk),
	[GCC_PCIE0_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_rchng_clk),
	[GCC_PCIE0_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_pipe_clk),
	[GCC_PCIE1_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_aux_clk),
	[GCC_PCIE1_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_ahb_clk),
	[GCC_ANOC_PCIE1_2LANE_M_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie1_2lane_m_clk),
	[GCC_ANOC_PCIE1_2LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie1_2lane_s_clk),
	[GCC_PCIE1_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_axi_m_clk),
	[GCC_PCIE1_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_axi_s_clk),
	[GCC_PCIE1_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_axi_s_bridge_clk),
	[GCC_PCIE1_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_rchng_clk),
	[GCC_PCIE1_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_pipe_clk),
	[GCC_PCIE2_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_aux_clk),
	[GCC_PCIE2_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_ahb_clk),
	[GCC_ANOC_PCIE2_2LANE_M_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie2_2lane_m_clk),
	[GCC_ANOC_PCIE2_2LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie2_2lane_s_clk),
	[GCC_PCIE2_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_axi_m_clk),
	[GCC_PCIE2_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_axi_s_clk),
	[GCC_PCIE2_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_axi_s_bridge_clk),
	[GCC_PCIE2_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_rchng_clk),
	[GCC_PCIE2_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie2_pipe_clk),
	[GCC_PCIE3_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_aux_clk),
	[GCC_PCIE3_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_ahb_clk),
	[GCC_ANOC_PCIE3_2LANE_M_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie3_2lane_m_clk),
	[GCC_ANOC_PCIE3_2LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie3_2lane_s_clk),
	[GCC_PCIE3_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_axi_m_clk),
	[GCC_PCIE3_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_axi_s_clk),
	[GCC_PCIE3_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_axi_s_bridge_clk),
	[GCC_PCIE3_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_rchng_clk),
	[GCC_PCIE3_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie3_pipe_clk),
	[GCC_PCIE4_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_aux_clk),
	[GCC_PCIE4_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_ahb_clk),
	[GCC_ANOC_PCIE4_1LANE_M_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie4_1lane_m_clk),
	[GCC_ANOC_PCIE4_1LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_anoc_pcie4_1lane_s_clk),
	[GCC_PCIE4_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_axi_m_clk),
	[GCC_PCIE4_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_axi_s_clk),
	[GCC_PCIE4_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_axi_s_bridge_clk),
	[GCC_PCIE4_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_rchng_clk),
	[GCC_PCIE4_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie4_pipe_clk),
	[GCC_QDSS_DAP_CLK] = DEFINE_DUMMY_CLK(gcc_qdss_dap),
	[GCC_QDSS_AT_CLK] = DEFINE_DUMMY_CLK(gcc_qdss_at),
	[GCC_QUPV3_SPI0_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_spi0),
	[GCC_QPIC_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_qpic_ahb),
	[GCC_QPIC_CLK] = DEFINE_DUMMY_CLK(gcc_qpic),
	[GCC_QPIC_IO_MACRO_CLK] = DEFINE_DUMMY_CLK(gcc_qpic_io_macro),
	[GCC_QUPV3_I2C0_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_i2c0),
	[GCC_QUPV3_I2C1_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_i2c1),
};

static const struct qcom_reset_map gcc_ipq9650_resets[] = {
	[GCC_QUPV3_BCR] = { 0x01000, 0 },
	[GCC_UNIPHY0_BCR] = { 0x17044, 0 },
	[GCC_UNIPHY1_BCR] = { 0x17054, 0 },
	[GCC_UNIPHY2_BCR] = { 0x17064, 0 },
	[GCC_UNIPHY0_SYS_ARES] = { 0x17048, 2 },
	[GCC_UNIPHY0_AHB_ARES] = { 0x1704c, 2 },
	[GCC_UNIPHY1_SYS_ARES] = { 0x17058, 2 },
	[GCC_UNIPHY1_AHB_ARES] = { 0x1705c, 2 },
	[GCC_UNIPHY2_SYS_ARES] = { 0x17068, 2 },
	[GCC_UNIPHY2_AHB_ARES] = { 0x1706c, 2 },
	[GCC_UNIPHY0_XPCS_ARES] = { 0x17050, 2 },
	[GCC_UNIPHY1_XPCS_ARES] = { 0x17060, 2 },
	[GCC_UNIPHY2_XPCS_ARES] = { 0x17070, 2 },
	[GCC_PCIE0_PHY_BCR] = { 0x28060, 0 },
	[GCC_PCIE0PHY_PHY_BCR] = { 0x2805c, 0 },
	[GCC_PCIE0_PIPE_ARES] = { 0x28068, 2 },
	[GCC_PCIE0_CORE_STICKY_RESET] = { 0x28058, 1 },
	[GCC_PCIE0_AXI_S_STICKY_RESET] = { 0x28058, 2 },
	[GCC_PCIE0_AXI_M_STICKY_RESET] = { 0x28058, 4 },
	[GCC_PCIE0_AXI_M_ARES] = { 0x28038, 2 },
	[GCC_PCIE0_AXI_S_ARES] = { 0x28040, 2 },
	[GCC_PCIE0_AHB_ARES] = { 0x28030, 2 },
	[GCC_PCIE0_AUX_ARES] = { 0x28070, 2 },
	[GCC_PCIE1_PHY_BCR] = { 0x29060, 0 },
	[GCC_PCIE1PHY_PHY_BCR] = { 0x2905c, 0 },
	[GCC_PCIE1_PIPE_ARES] = { 0x29068, 2 },
	[GCC_PCIE1_CORE_STICKY_RESET] = { 0x29058, 1 },
	[GCC_PCIE1_AXI_S_STICKY_RESET] = { 0x29058, 2 },
	[GCC_PCIE1_AXI_M_STICKY_RESET] = { 0x29058, 4 },
	[GCC_PCIE1_AXI_M_ARES] = { 0x29038, 2 },
	[GCC_PCIE1_AXI_S_ARES] = { 0x29040, 2 },
	[GCC_PCIE1_AHB_ARES] = { 0x29030, 2 },
	[GCC_PCIE1_AUX_ARES] = { 0x29074, 2 },
	[GCC_PCIE2_PHY_BCR] = { 0x2a060, 0 },
	[GCC_PCIE2PHY_PHY_BCR] = { 0x2a05c, 0 },
	[GCC_PCIE2_PIPE_ARES] = { 0x2a068, 2 },
	[GCC_PCIE2_CORE_STICKY_RESET] = { 0x2a058, 1 },
	[GCC_PCIE2_AXI_S_STICKY_RESET] = { 0x2a058, 2 },
	[GCC_PCIE2_AXI_M_STICKY_RESET] = { 0x2a058, 4 },
	[GCC_PCIE2_AXI_M_ARES] = { 0x2a038, 2 },
	[GCC_PCIE2_AXI_S_ARES] = { 0x2a040, 2 },
	[GCC_PCIE2_AHB_ARES] = { 0x2a030, 2 },
	[GCC_PCIE2_AUX_ARES] = { 0x2a078, 2 },
	[GCC_PCIE3_PHY_BCR] = { 0x2b060, 0 },
	[GCC_PCIE3PHY_PHY_BCR] = { 0x2b05c, 0 },
	[GCC_PCIE3_PIPE_ARES] = { 0x2b068, 2 },
	[GCC_PCIE3_CORE_STICKY_RESET] = { 0x2b058, 1 },
	[GCC_PCIE3_AXI_S_STICKY_RESET] = { 0x2b058, 2 },
	[GCC_PCIE3_AXI_M_STICKY_RESET] = { 0x2b058, 4 },
	[GCC_PCIE3_AXI_M_ARES] = { 0x2b038, 2 },
	[GCC_PCIE3_AXI_S_ARES] = { 0x2b040, 2 },
	[GCC_PCIE3_AHB_ARES] = { 0x2b030, 2 },
	[GCC_PCIE3_AUX_ARES] = { 0x2b07c, 2 },
	[GCC_PCIE4_PHY_BCR] = { 0x2504c, 0 },
	[GCC_PCIE4PHY_PHY_BCR] = { 0x25048, 0 },
	[GCC_PCIE4_PIPE_ARES] = { 0x2503C, 2 },
	[GCC_PCIE4_CORE_STICKY_RESET] = { 0x25054, 1 },
	[GCC_PCIE4_AXI_S_STICKY_RESET] = { 0x25054, 2 },
	[GCC_PCIE4_AXI_M_STICKY_RESET] = { 0x25054, 4 },
	[GCC_PCIE4_AXI_M_ARES] = { 0x25028, 2 },
	[GCC_PCIE4_AXI_S_ARES] = { 0x25030, 2 },
	[GCC_PCIE4_AHB_ARES] = { 0x2501c, 2 },
	[GCC_PCIE4_AUX_ARES] = { 0x25020, 2 },
};

static const struct of_device_id gcc_ipq9650_match_table[] = {
	{ .compatible = "qcom,ipq9650-gcc" },
	{ }
};
MODULE_DEVICE_TABLE(of, gcc_ipq9650_match_table);

static const struct regmap_config gcc_ipq9650_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x3f024,
	.fast_io        = true,
};

static const struct qcom_cc_desc gcc_ipq9650_dummy_desc = {
	.config = &gcc_ipq9650_regmap_config,
	.clks = gcc_ipq9650_dummy_clks,
	.num_clks = ARRAY_SIZE(gcc_ipq9650_dummy_clks),
	.resets = gcc_ipq9650_resets,
	.num_resets = ARRAY_SIZE(gcc_ipq9650_resets),
};

static int gcc_ipq9650_probe(struct platform_device *pdev)
{
	return qcom_cc_probe(pdev, &gcc_ipq9650_dummy_desc);
}

static struct platform_driver gcc_ipq9650_driver = {
	.probe = gcc_ipq9650_probe,
	.driver = {
		.name   = "qcom,gcc-ipq9650",
		.of_match_table = gcc_ipq9650_match_table,
	},
};

static int __init gcc_ipq9650_init(void)
{
	return platform_driver_register(&gcc_ipq9650_driver);
}
core_initcall(gcc_ipq9650_init);

static void __exit gcc_ipq9650_exit(void)
{
	platform_driver_unregister(&gcc_ipq9650_driver);
}
module_exit(gcc_ipq9650_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. GCC IPQ9650 Driver");
MODULE_LICENSE("GPLv2");
