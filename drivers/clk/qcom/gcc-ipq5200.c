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

#include <dt-bindings/clock/qcom,ipq5200-gcc.h>
#include <dt-bindings/reset/qcom,ipq5200-gcc.h>

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

static struct clk_regmap *gcc_ipq5200_dummy_clks[] = {
	[GCC_QUPV3_AHB_MST_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_ahb_mst),
	[GCC_QUPV3_AHB_SLV_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_ahb_slv),
	[GCC_QUPV3_UART1_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_uart1),
	[GCC_SDCC1_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_sdcc1_ahb_clk),
	[GCC_SDCC1_APPS_CLK] = DEFINE_DUMMY_CLK(gcc_sdcc1_apps_clk),
	[GCC_QUPV3_SPI0_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_spi0_clk),
	[GCC_USB0_PHY_CFG_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_usb0_phy_cfg_ahb_clk),
	[GCC_USB0_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_usb0_aux_clk),
	[GCC_USB0_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_usb0_pipe_clk),
	[GCC_USB0_MASTER_CLK] = DEFINE_DUMMY_CLK(gcc_usb0_master_clk),
	[GCC_USB0_SLEEP_CLK] = DEFINE_DUMMY_CLK(gcc_usb0_sleep_clk),
	[GCC_USB0_MOCK_UTMI_CLK] = DEFINE_DUMMY_CLK(gcc_usb0_mock_utmi_clk),
	[GCC_CNOC_USB_CLK] = DEFINE_DUMMY_CLK(gcc_cnoc_usb_clk),
	[GCC_QPIC_CLK] = DEFINE_DUMMY_CLK(gcc_qpic_clk),
	[GCC_QPIC_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_qpic_ahb_clk),
	[GCC_QPIC_IO_MACRO_CLK] = DEFINE_DUMMY_CLK(gcc_qpic_io_macro_clk),
	[GCC_QUPV3_I2C0_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_i2c0_clk),
	[GCC_QUPV3_I2C1_CLK] = DEFINE_DUMMY_CLK(gcc_qupv3_i2c1_clk),
	[GCC_PCIE0_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_aux_clk),
	[GCC_PCIE0_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_ahb_clk),
	[GCC_CNOC_PCIE0_1LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_cnoc_pcie0_1lane_s_clk),
	[GCC_PCIE0_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_pipe_clk),
	[GCC_PCIE0_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_axi_m_clk),
	[GCC_PCIE0_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_axi_s_clk),
	[GCC_PCIE0_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_axi_s_bridge_clk),
	[GCC_PCIE0_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie0_rchng_clk),
	[GCC_PCIE1_AUX_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_aux_clk),
	[GCC_PCIE1_AHB_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_ahb_clk),
	[GCC_CNOC_PCIE1_2LANE_S_CLK] = DEFINE_DUMMY_CLK(gcc_cnoc_pcie1_2lane_s_clk),
	[GCC_PCIE1_PIPE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_pipe_clk),
	[GCC_PCIE1_AXI_M_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_axi_m_clk),
	[GCC_PCIE1_AXI_S_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_axi_s_clk),
	[GCC_PCIE1_AXI_S_BRIDGE_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_axi_s_bridge_clk),
	[GCC_PCIE1_RCHNG_CLK] = DEFINE_DUMMY_CLK(gcc_pcie1_rchng_clk),
};

static const struct qcom_reset_map gcc_ipq5200_resets[] = {
	[GCC_QUPV3_BCR] = { 0x01000, 0 },
	[GCC_USB_BCR] = { 0x2C000, 0 },
	[GCC_QUSB2_0_PHY_BCR] = { 0x2C068, 0 },
	[GCC_USB0_PHY_BCR] = { 0x2C06C, 0 },
	[GCC_USB3PHY_0_PHY_BCR] = { 0x2C070, 0 },
	[GCC_PCIE0_PHY_BCR] = { 0x28060, 0 },
	[GCC_PCIE0PHY_PHY_BCR] = { 0x2805C, 0 },
	[GCC_PCIE0_PIPE_ARES] = { 0x28068, 2 },
	[GCC_PCIE0_CORE_STICKY_RESET] = { 0x28058, 1 },
	[GCC_PCIE0_AXI_M_ARES] = { 0x28038, 2 },
	[GCC_PCIE0_AXI_S_ARES] = { 0x28040, 2 },
	[GCC_PCIE0_AXI_M_STICKY_RESET] = { 0x28058, 4 },
	[GCC_PCIE0_AXI_S_STICKY_RESET] = { 0x28058, 2 },
	[GCC_PCIE0_AHB_ARES] = { 0x28030, 2 },
	[GCC_PCIE0_AUX_ARES] = { 0x28070, 2 },
	[GCC_PCIE1_PHY_BCR] = { 0x29060, 0 },
	[GCC_PCIE1PHY_PHY_BCR] = { 0x2905c, 0 },
	[GCC_PCIE1_PIPE_ARES] = { 0x29068, 2 },
	[GCC_PCIE1_CORE_STICKY_RESET] = { 0x29058, 1 },
	[GCC_PCIE1_AXI_M_ARES] = { 0x29038, 2 },
	[GCC_PCIE1_AXI_S_ARES] = { 0x29040, 2 },
	[GCC_PCIE1_AXI_M_STICKY_RESET] = { 0x29058, 4 },
	[GCC_PCIE1_AXI_S_STICKY_RESET] = { 0x29058, 2 },
	[GCC_PCIE1_AHB_ARES] = { 0x29030, 2 },
	[GCC_PCIE1_AUX_ARES] = { 0x29074, 2 },
};

static const struct of_device_id gcc_ipq5200_match_table[] = {
	{ .compatible = "qcom,ipq5200-gcc" },
	{ }
};
MODULE_DEVICE_TABLE(of, gcc_ipq5200_match_table);

static const struct regmap_config gcc_ipq5200_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x3f004,
	.fast_io        = true,
};

static const struct qcom_cc_desc gcc_ipq5200_desc = {
	.config = &gcc_ipq5200_regmap_config,
	.clks = gcc_ipq5200_dummy_clks,
	.num_clks = ARRAY_SIZE(gcc_ipq5200_dummy_clks),
	.resets = gcc_ipq5200_resets,
	.num_resets = ARRAY_SIZE(gcc_ipq5200_resets),
};

static int gcc_ipq5200_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	struct qcom_cc_desc ipq5200_desc = gcc_ipq5200_desc;
	int ret;

	regmap = qcom_cc_map(pdev, &ipq5200_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = qcom_cc_really_probe(pdev, &ipq5200_desc, regmap);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register GCC clocks ret=%d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "Registered GCC clocks\n");

	return ret;
}

static struct platform_driver gcc_ipq5200_driver = {
	.probe = gcc_ipq5200_probe,
	.driver = {
		.name   = "qcom,gcc-ipq5200",
		.of_match_table = gcc_ipq5200_match_table,
	},
};

static int __init gcc_ipq5200_init(void)
{
	return platform_driver_register(&gcc_ipq5200_driver);
}
core_initcall(gcc_ipq5200_init);

static void __exit gcc_ipq5200_exit(void)
{
	platform_driver_unregister(&gcc_ipq5200_driver);
}
module_exit(gcc_ipq5200_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. GCC IPQ5200 Driver");
MODULE_LICENSE("GPLv2");
