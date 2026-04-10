// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,ipq9650-nsscc.h>
#include <dt-bindings/reset/qcom,ipq9650-nsscc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-pll.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "clk-regmap-divider.h"
#include "clk-regmap-mux.h"
#include "common.h"
#include "reset.h"

/* Need to match the order of clocks in DT binding */
enum {
	DT_XO,
	DT_CMN_PLL_NSS_CLK_750M,
	DT_CMN_PLL_NSS_CLK_462M,
	DT_GCC_GPLL0_OUT_AUX,
	DT_UNIPHY0_NSS_RX_CLK,
	DT_UNIPHY0_NSS_TX_CLK,
	DT_UNIPHY1_NSS_RX_CLK,
	DT_UNIPHY1_NSS_TX_CLK,
	DT_UNIPHY2_NSS_RX_CLK,
	DT_UNIPHY2_NSS_TX_CLK,
};

enum {
	P_XO,
	P_CMN_PLL_NSS_CLK_750M,
	P_CMN_PLL_NSS_CLK_462M,
	P_GCC_GPLL0_OUT_AUX,
	P_UNIPHY0_NSS_RX_CLK,
	P_UNIPHY0_NSS_TX_CLK,
	P_UNIPHY1_NSS_RX_CLK,
	P_UNIPHY1_NSS_TX_CLK,
	P_UNIPHY2_NSS_RX_CLK,
	P_UNIPHY2_NSS_TX_CLK,
};

static const struct parent_map nss_cc_parent_map_ppe[] = {
	{ P_XO, 0 },
	{ P_GCC_GPLL0_OUT_AUX, 2 },
	{ P_CMN_PLL_NSS_CLK_462M, 6 },
};

static const struct clk_parent_data nss_cc_ppe_parent_data_ppe[] = {
	{ .index = DT_XO },
	{ .index = DT_GCC_GPLL0_OUT_AUX },
	{ .index = DT_CMN_PLL_NSS_CLK_462M },
};

static const struct clk_parent_data nss_cc_ppe_parent_data_eip[] = {
	{ .index = DT_XO },
	{ .index = DT_GCC_GPLL0_OUT_AUX },
	{ .index = DT_CMN_PLL_NSS_CLK_462M },
	{ .index = DT_CMN_PLL_NSS_CLK_750M },
};

static const struct parent_map nss_cc_parent_map_port1_4_rx[] = {
	{ P_XO, 0 },
	{ P_UNIPHY0_NSS_RX_CLK, 3 },
};

static const struct parent_map nss_cc_parent_map_port1_4_tx[] = {
	{ P_XO, 0 },
	{ P_UNIPHY0_NSS_TX_CLK, 4 },
};

static const struct clk_parent_data gcc_xo_uniphy0_rx[] = {
	{ .index = DT_XO },
	{ .index = DT_UNIPHY0_NSS_RX_CLK },
};

static const struct clk_parent_data gcc_xo_uniphy0_tx[] = {
	{ .index = DT_XO },
	{ .index = DT_UNIPHY0_NSS_TX_CLK },
};

static const struct parent_map nss_cc_parent_map_port_5_rx[] = {
	{ P_XO, 0 },
	{ P_UNIPHY0_NSS_RX_CLK, 3 },
	{ P_UNIPHY1_NSS_RX_CLK, 5 },
};

static const struct parent_map nss_cc_parent_map_port_5_tx[] = {
	{ P_XO, 0 },
	{ P_UNIPHY0_NSS_TX_CLK, 4 },
	{ P_UNIPHY1_NSS_TX_CLK, 6 },
};

static const struct clk_parent_data gcc_xo_uniphy0_uniphy1_rx[] = {
	{ .index = DT_XO },
	{ .index = DT_UNIPHY0_NSS_RX_CLK },
	{ .index = DT_UNIPHY1_NSS_RX_CLK },
};

static const struct clk_parent_data gcc_xo_uniphy0_uniphy1_tx[] = {
	{ .index = DT_XO },
	{ .index = DT_UNIPHY0_NSS_TX_CLK },
	{ .index = DT_UNIPHY1_NSS_TX_CLK },
};

static const struct parent_map nss_cc_parent_map_port_6_rx[] = {
	{ P_XO, 0 },
	{ P_UNIPHY2_NSS_RX_CLK, 3 },
};

static const struct parent_map nss_cc_parent_map_port_6_tx[] = {
	{ P_XO, 0 },
	{ P_UNIPHY2_NSS_TX_CLK, 4 },
};

static const struct clk_parent_data gcc_xo_uniphy2_rx[] = {
	{ .index = DT_XO },
	{ .index = DT_UNIPHY2_NSS_RX_CLK },
};

static const struct clk_parent_data gcc_xo_uniphy2_tx[] = {
	{ .index = DT_XO },
	{ .index = DT_UNIPHY2_NSS_TX_CLK },
};


static const struct freq_tbl ftbl_nss_cc_ppe_clk_src[] = {
	F(24000000, P_XO, 1, 0, 0),
	F(462000000, P_CMN_PLL_NSS_CLK_462M, 1, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_ppe_clk_src = {
	.cmd_rcgr = 0x3ec,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_ppe,
	.freq_tbl = ftbl_nss_cc_ppe_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_ppe_clk_src",
		.parent_data = nss_cc_ppe_parent_data_ppe,
		.num_parents = ARRAY_SIZE(nss_cc_ppe_parent_data_ppe),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nss_cc_xgmac0_ptp_ref_div_clk_src = {
	.reg = 0x3f4,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_xgmac0_ptp_ref_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ppe_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_regmap_div nss_cc_xgmac1_ptp_ref_div_clk_src = {
	.reg = 0x3f8,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_xgmac1_ptp_ref_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ppe_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_regmap_div nss_cc_xgmac2_ptp_ref_div_clk_src = {
	.reg = 0x3fc,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_xgmac2_ptp_ref_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ppe_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_regmap_div nss_cc_xgmac3_ptp_ref_div_clk_src = {
	.reg = 0x400,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_xgmac3_ptp_ref_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ppe_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_regmap_div nss_cc_xgmac4_ptp_ref_div_clk_src = {
	.reg = 0x404,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_xgmac4_ptp_ref_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ppe_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_regmap_div nss_cc_xgmac5_ptp_ref_div_clk_src = {
	.reg = 0x408,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_xgmac5_ptp_ref_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ppe_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_branch nss_cc_ppe_switch_ipe_clk = {
	.halt_reg = 0x424,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x424,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ppe_switch_ipe_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_ppe_switch_btq_clk = {
	.halt_reg = 0x42c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x42c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ppe_switch_btq_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_ppe_switch_clk = {
	.halt_reg = 0x434,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x434,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ppe_switch_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_ppe_switch_cfg_clk = {
	.halt_reg = 0x43c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x43c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ppe_switch_cfg_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_ppe_edma_clk = {
	.halt_reg = 0x440,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x440,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ppe_edma_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_ppe_edma_cfg_clk = {
	.halt_reg = 0x448,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x448,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ppe_edma_cfg_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port1_mac_clk = {
	.halt_reg = 0x44c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x44c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port1_mac_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port2_mac_clk = {
	.halt_reg = 0x454,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x454,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port2_mac_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port3_mac_clk = {
	.halt_reg = 0x45c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x45c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port3_mac_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port4_mac_clk = {
	.halt_reg = 0x464,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x464,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port4_mac_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port5_mac_clk = {
	.halt_reg = 0x46c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x46c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port5_mac_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port6_mac_clk = {
	.halt_reg = 0x474,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x474,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port6_mac_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_eip_ppe_clk = {
	.halt_reg = 0x47c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x47c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_eip_ppe_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_xgmac0_ptp_ref_clk = {
	.halt_reg = 0x488,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x488,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_xgmac0_ptp_ref_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_xgmac0_ptp_ref_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_xgmac1_ptp_ref_clk = {
	.halt_reg = 0x48c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x48c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_xgmac1_ptp_ref_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_xgmac1_ptp_ref_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_xgmac2_ptp_ref_clk = {
	.halt_reg = 0x490,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x490,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_xgmac2_ptp_ref_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_xgmac2_ptp_ref_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_xgmac3_ptp_ref_clk = {
	.halt_reg = 0x494,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x494,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_xgmac3_ptp_ref_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_xgmac3_ptp_ref_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_xgmac4_ptp_ref_clk = {
	.halt_reg = 0x498,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x498,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_xgmac4_ptp_ref_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_xgmac4_ptp_ref_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_xgmac5_ptp_ref_clk = {
	.halt_reg = 0x49c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x49c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_xgmac5_ptp_ref_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_xgmac5_ptp_ref_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_nssnoc_ppe_clk = {
	.halt_reg = 0x4a4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x4a4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nssnoc_ppe_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_nssnoc_ppe_cfg_clk = {
	.halt_reg = 0x4a8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x4a8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nssnoc_ppe_cfg_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ppe_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static const struct freq_conf ftbl_nss_cc_port1_4_rx_clk_src_25[] = {
	C(P_UNIPHY0_NSS_RX_CLK, 12.5, 0, 0),
	C(P_UNIPHY0_NSS_RX_CLK, 5, 0, 0),
};

static const struct freq_conf ftbl_nss_cc_port1_4_rx_clk_src_125[] = {
	C(P_UNIPHY0_NSS_RX_CLK, 2.5, 0, 0),
	C(P_UNIPHY0_NSS_RX_CLK, 1, 0, 0),
};

static const struct freq_multi_tbl ftbl_nss_cc_port1_4_rx_clk_src[] = {
	FMS(24000000, P_XO, 1, 0, 0),
	FM(25000000, ftbl_nss_cc_port1_4_rx_clk_src_25),
	FMS(78125000, P_UNIPHY0_NSS_RX_CLK, 4, 0, 0),
	FM(125000000, ftbl_nss_cc_port1_4_rx_clk_src_125),
	FMS(156250000, P_UNIPHY0_NSS_RX_CLK, 2, 0, 0),
	FMS(312500000, P_UNIPHY0_NSS_RX_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_port1_rx_clk_src = {
	.cmd_rcgr = 0x4b4,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_rx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_rx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port1_rx_clk_src",
		.parent_data = gcc_xo_uniphy0_rx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_rx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port1_rx_div_clk_src = {
	.reg = 0x4bc,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port1_rx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port1_rx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static const struct freq_conf ftbl_nss_cc_port1_4_tx_clk_src_25[] = {
	C(P_UNIPHY0_NSS_TX_CLK, 12.5, 0, 0),
	C(P_UNIPHY0_NSS_TX_CLK, 5, 0, 0),
};

static const struct freq_conf ftbl_nss_cc_port1_4_tx_clk_src_125[] = {
	C(P_UNIPHY0_NSS_TX_CLK, 2.5, 0, 0),
	C(P_UNIPHY0_NSS_TX_CLK, 1, 0, 0),
};

static const struct freq_multi_tbl ftbl_nss_cc_port1_4_tx_clk_src[] = {
	FMS(24000000, P_XO, 1, 0, 0),
	FM(25000000, ftbl_nss_cc_port1_4_tx_clk_src_25),
	FMS(78125000, P_UNIPHY0_NSS_TX_CLK, 4, 0, 0),
	FM(125000000, ftbl_nss_cc_port1_4_tx_clk_src_125),
	FMS(156250000, P_UNIPHY0_NSS_TX_CLK, 2, 0, 0),
	FMS(312500000, P_UNIPHY0_NSS_TX_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_port1_tx_clk_src = {
	.cmd_rcgr = 0x4c0,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_tx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_tx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port1_tx_clk_src",
		.parent_data = gcc_xo_uniphy0_tx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_tx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port1_tx_div_clk_src = {
	.reg = 0x4c8,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port1_tx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port1_tx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port2_rx_clk_src = {
	.cmd_rcgr = 0x4cc,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_rx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_rx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port2_rx_clk_src",
		.parent_data = gcc_xo_uniphy0_rx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_rx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port2_rx_div_clk_src = {
	.reg = 0x4d4,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port2_rx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port2_rx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port2_tx_clk_src = {
	.cmd_rcgr = 0x4d8,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_tx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_tx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port2_tx_clk_src",
		.parent_data = gcc_xo_uniphy0_tx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_tx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port2_tx_div_clk_src = {
	.reg = 0x4e0,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port2_tx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port2_tx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port3_rx_clk_src = {
	.cmd_rcgr = 0x4e4,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_rx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_rx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port3_rx_clk_src",
		.parent_data = gcc_xo_uniphy0_rx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_rx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port3_rx_div_clk_src = {
	.reg = 0x4ec,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port3_rx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port3_rx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port3_tx_clk_src = {
	.cmd_rcgr = 0x4f0,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_tx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_tx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port3_tx_clk_src",
		.parent_data = gcc_xo_uniphy0_tx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_tx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port3_tx_div_clk_src = {
	.reg = 0x4f8,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port3_tx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port3_tx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port4_rx_clk_src = {
	.cmd_rcgr = 0x4fc,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_rx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_rx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port4_rx_clk_src",
		.parent_data = gcc_xo_uniphy0_rx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_rx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port4_rx_div_clk_src = {
	.reg = 0x504,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port4_rx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port4_rx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port4_tx_clk_src = {
	.cmd_rcgr = 0x508,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port1_4_tx,
	.freq_multi_tbl = ftbl_nss_cc_port1_4_tx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port4_tx_clk_src",
		.parent_data = gcc_xo_uniphy0_tx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_tx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port4_tx_div_clk_src = {
	.reg = 0x510,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port4_tx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port4_tx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static const struct freq_conf ftbl_nss_cc_port5_rx_clk_src_25[] = {
	C(P_UNIPHY1_NSS_RX_CLK, 12.5, 0, 0),
	C(P_UNIPHY1_NSS_RX_CLK, 5, 0, 0),
	C(P_UNIPHY0_NSS_RX_CLK, 5, 0, 0),
};

static const struct freq_conf ftbl_nss_cc_port5_rx_clk_src_125[] = {
	C(P_UNIPHY1_NSS_RX_CLK, 2.5, 0, 0),
	C(P_UNIPHY1_NSS_RX_CLK, 1, 0, 0),
	C(P_UNIPHY0_NSS_RX_CLK, 1, 0, 0),
};

static const struct freq_multi_tbl ftbl_nss_cc_port5_rx_clk_src[] = {
	FMS(24000000, P_XO, 1, 0, 0),
	FM(25000000, ftbl_nss_cc_port5_rx_clk_src_25),
	FMS(78125000, P_UNIPHY1_NSS_RX_CLK, 4, 0, 0),
	FM(125000000, ftbl_nss_cc_port5_rx_clk_src_125),
	FMS(156250000, P_UNIPHY1_NSS_RX_CLK, 2, 0, 0),
	FMS(312500000, P_UNIPHY1_NSS_RX_CLK, 1, 0, 0),
	FMS(378788000, P_UNIPHY1_NSS_RX_CLK, 1, 0, 0),
	FMS(781250000, P_UNIPHY1_NSS_RX_CLK, 1, 0, 0),
	{ }
};

static const struct freq_conf ftbl_nss_cc_port5_tx_clk_src_25[] = {
	C(P_UNIPHY1_NSS_TX_CLK, 12.5, 0, 0),
	C(P_UNIPHY1_NSS_TX_CLK, 5, 0, 0),
	C(P_UNIPHY0_NSS_TX_CLK, 5, 0, 0),
};

static const struct freq_conf ftbl_nss_cc_port5_tx_clk_src_125[] = {
	C(P_UNIPHY1_NSS_TX_CLK, 2.5, 0, 0),
	C(P_UNIPHY1_NSS_TX_CLK, 1, 0, 0),
	C(P_UNIPHY0_NSS_TX_CLK, 1, 0, 0),
};

static const struct freq_multi_tbl ftbl_nss_cc_port5_tx_clk_src[] = {
	FMS(24000000, P_XO, 1, 0, 0),
	FM(25000000, ftbl_nss_cc_port5_tx_clk_src_25),
	FMS(78125000, P_UNIPHY1_NSS_TX_CLK, 4, 0, 0),
	FM(125000000, ftbl_nss_cc_port5_tx_clk_src_125),
	FMS(156250000, P_UNIPHY1_NSS_TX_CLK, 2, 0, 0),
	FMS(312500000, P_UNIPHY1_NSS_TX_CLK, 1, 0, 0),
	FMS(378788000, P_UNIPHY1_NSS_TX_CLK, 1, 0, 0),
	FMS(781250000, P_UNIPHY1_NSS_TX_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_port5_rx_clk_src = {
	.cmd_rcgr = 0x514,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port_5_rx,
	.freq_multi_tbl = ftbl_nss_cc_port5_rx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port5_rx_clk_src",
		.parent_data = gcc_xo_uniphy0_uniphy1_rx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_uniphy1_rx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port5_rx_div_clk_src = {
	.reg = 0x51c,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port5_rx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port5_rx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_rcg2 nss_cc_port5_tx_clk_src = {
	.cmd_rcgr = 0x520,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port_5_tx,
	.freq_multi_tbl = ftbl_nss_cc_port5_tx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port5_tx_clk_src",
		.parent_data = gcc_xo_uniphy0_uniphy1_tx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy0_uniphy1_tx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port5_tx_div_clk_src = {
	.reg = 0x528,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port5_tx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port5_tx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static const struct freq_conf ftbl_nss_cc_port6_rx_clk_src_25[] = {
	C(P_UNIPHY2_NSS_RX_CLK, 12.5, 0, 0),
	C(P_UNIPHY2_NSS_RX_CLK, 5, 0, 0),
};

static const struct freq_conf ftbl_nss_cc_port6_rx_clk_src_125[] = {
	C(P_UNIPHY2_NSS_RX_CLK, 2.5, 0, 0),
	C(P_UNIPHY2_NSS_RX_CLK, 1, 0, 0),
};

static const struct freq_multi_tbl ftbl_nss_cc_port6_rx_clk_src[] = {
	FMS(24000000, P_XO, 1, 0, 0),
	FM(25000000, ftbl_nss_cc_port6_rx_clk_src_25),
	FMS(78125000, P_UNIPHY2_NSS_RX_CLK, 4, 0, 0),
	FM(125000000, ftbl_nss_cc_port6_rx_clk_src_125),
	FMS(156250000, P_UNIPHY2_NSS_RX_CLK, 2, 0, 0),
	FMS(312500000, P_UNIPHY2_NSS_RX_CLK, 1, 0, 0),
	FMS(378788000, P_UNIPHY2_NSS_RX_CLK, 1, 0, 0),
	FMS(781250000, P_UNIPHY2_NSS_RX_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_port6_rx_clk_src = {
	.cmd_rcgr = 0x52c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port_6_rx,
	.freq_multi_tbl = ftbl_nss_cc_port6_rx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port6_rx_clk_src",
		.parent_data = gcc_xo_uniphy2_rx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy2_rx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port6_rx_div_clk_src = {
	.reg = 0x534,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port6_rx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port6_rx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static const struct freq_conf ftbl_nss_cc_port6_tx_clk_src_25[] = {
	C(P_UNIPHY2_NSS_TX_CLK, 12.5, 0, 0),
	C(P_UNIPHY2_NSS_TX_CLK, 5, 0, 0),
};

static const struct freq_conf ftbl_nss_cc_port6_tx_clk_src_125[] = {
	C(P_UNIPHY2_NSS_TX_CLK, 2.5, 0, 0),
	C(P_UNIPHY2_NSS_TX_CLK, 1, 0, 0),
};

static const struct freq_multi_tbl ftbl_nss_cc_port6_tx_clk_src[] = {
	FMS(24000000, P_XO, 1, 0, 0),
	FM(25000000, ftbl_nss_cc_port6_tx_clk_src_25),
	FMS(78125000, P_UNIPHY2_NSS_TX_CLK, 4, 0, 0),
	FM(125000000, ftbl_nss_cc_port6_tx_clk_src_125),
	FMS(156250000, P_UNIPHY2_NSS_TX_CLK, 2, 0, 0),
	FMS(312500000, P_UNIPHY2_NSS_TX_CLK, 1, 0, 0),
	FMS(378788000, P_UNIPHY2_NSS_TX_CLK, 1, 0, 0),
	FMS(781250000, P_UNIPHY2_NSS_TX_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_port6_tx_clk_src = {
	.cmd_rcgr = 0x538,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_port_6_tx,
	.freq_multi_tbl = ftbl_nss_cc_port6_tx_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_port6_tx_clk_src",
		.parent_data = gcc_xo_uniphy2_tx,
		.num_parents = ARRAY_SIZE(gcc_xo_uniphy2_tx),
		.ops = &clk_rcg2_fm_ops,
	},
};

static struct clk_regmap_div nss_cc_port6_tx_div_clk_src = {
	.reg = 0x540,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_port6_tx_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_port6_tx_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_branch nss_cc_port1_rx_clk = {
	.halt_reg = 0x548,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x548,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port1_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port1_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port1_tx_clk = {
	.halt_reg = 0x550,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x550,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port1_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port1_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port2_rx_clk = {
	.halt_reg = 0x558,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x558,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port2_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port2_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port2_tx_clk = {
	.halt_reg = 0x560,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x560,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port2_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port2_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port3_rx_clk = {
	.halt_reg = 0x568,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x568,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port3_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port3_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port3_tx_clk = {
	.halt_reg = 0x570,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x570,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port3_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port3_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port4_rx_clk = {
	.halt_reg = 0x578,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x578,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port4_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port4_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port4_tx_clk = {
	.halt_reg = 0x580,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x580,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port4_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port4_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port5_rx_clk = {
	.halt_reg = 0x588,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x588,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port5_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port5_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port5_tx_clk = {
	.halt_reg = 0x590,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x590,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port5_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port5_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port6_rx_clk = {
	.halt_reg = 0x598,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x598,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port6_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port6_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_port6_tx_clk = {
	.halt_reg = 0x5a0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5a0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_port6_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port6_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port1_rx_clk = {
	.halt_reg = 0x5e0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5e0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port1_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port1_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port1_tx_clk = {
	.halt_reg = 0x5e4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5e4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port1_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port1_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port2_rx_clk = {
	.halt_reg = 0x5e8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5e8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port2_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port2_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port2_tx_clk = {
	.halt_reg = 0x5ec,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5ec,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port2_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port2_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port3_rx_clk = {
	.halt_reg = 0x5f0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5f0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port3_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port3_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port3_tx_clk = {
	.halt_reg = 0x5f4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5f4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port3_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port3_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port4_rx_clk = {
	.halt_reg = 0x5f8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5f8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port4_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port4_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port4_tx_clk = {
	.halt_reg = 0x5fc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x5fc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port4_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port4_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port5_rx_clk = {
	.halt_reg = 0x600,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x600,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port5_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port5_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port5_tx_clk = {
	.halt_reg = 0x604,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x604,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port5_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port5_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port6_rx_clk = {
	.halt_reg = 0x608,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x608,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port6_rx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port6_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_uniphy_port6_tx_clk = {
	.halt_reg = 0x60c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x60c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_uniphy_port6_tx_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_port6_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_rcg2 nss_cc_ce_clk_src = {
	.cmd_rcgr = 0x644,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_ppe,
	.freq_tbl = ftbl_nss_cc_ppe_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_ce_clk_src",
		.parent_data = nss_cc_ppe_parent_data_ppe,
		.num_parents = ARRAY_SIZE(nss_cc_ppe_parent_data_ppe),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nss_cc_ce_div_clk_src = {
	.reg = 0x64c,
	.shift = 0,
	.width = 9,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nss_cc_ce_div_clk_src",
		.parent_hws = (const struct clk_hw*[]){
			&nss_cc_ce_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_branch nss_cc_ce_apb_clk = {
	.halt_reg = 0x650,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x650,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ce_apb_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ce_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_nssnoc_ce_apb_clk = {
	.halt_reg = 0x65c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x65c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nssnoc_ce_apb_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ce_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_ce_axi_clk = {
	.halt_reg = 0x654,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x654,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_ce_axi_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ce_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_nssnoc_ce_axi_clk = {
	.halt_reg = 0x660,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x660,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nssnoc_ce_axi_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_ce_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static const struct freq_tbl ftbl_nss_cc_eip_bfdcd_clk_src[] = {
	F(462000000, P_CMN_PLL_NSS_CLK_462M, 1, 0, 0),
	F(750000000, P_CMN_PLL_NSS_CLK_750M, 1, 0, 0),
	F(800000000, P_GCC_GPLL0_OUT_AUX, 1, 0, 0),
	{ }
};

static const struct parent_map nss_cc_parent_map_eip[] = {
	{ P_XO, 0 },
	{ P_GCC_GPLL0_OUT_AUX, 2 },
	{ P_CMN_PLL_NSS_CLK_750M, 5 },
	{ P_CMN_PLL_NSS_CLK_462M, 6 },
};

static struct clk_rcg2 nss_cc_eip_bfdcd_clk_src = {
	.cmd_rcgr = 0x6a8,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_eip,
	.freq_tbl = ftbl_nss_cc_eip_bfdcd_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_eip_bfdcd_clk_src",
		.parent_data = nss_cc_ppe_parent_data_eip,
		.num_parents = ARRAY_SIZE(nss_cc_ppe_parent_data_eip),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch nss_cc_eip_clk = {
	.halt_reg = 0x6bc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x6bc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_eip_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_eip_bfdcd_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_nssnoc_eip_clk = {
	.halt_reg = 0x6c4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x6c4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nssnoc_eip_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_eip_bfdcd_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static const struct freq_tbl ftbl_nss_cc_cfg_clk_src[] = {
	F(100000000, P_GCC_GPLL0_OUT_AUX, 8, 0, 0),
	{ }
};

static struct clk_rcg2 nss_cc_cfg_clk_src = {
	.cmd_rcgr = 0x70c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = nss_cc_parent_map_ppe,
	.freq_tbl = ftbl_nss_cc_cfg_clk_src,
	.clkr.hw.init = &(const struct clk_init_data){
		.name = "nss_cc_cfg_clk_src",
		.parent_data = nss_cc_ppe_parent_data_ppe,
		.num_parents = ARRAY_SIZE(nss_cc_ppe_parent_data_ppe),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch nss_cc_nss_csr_clk = {
	.halt_reg = 0x714,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x714,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nss_csr_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_cfg_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch nss_cc_nssnoc_nss_csr_clk = {
	.halt_reg = 0x718,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x718,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data){
			.name = "nss_cc_nssnoc_nss_csr_clk",
			.parent_hws = (const struct clk_hw*[]){
				&nss_cc_cfg_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_regmap *nss_cc_ipq9650_clocks[] = {
	[NSS_CC_PPE_CLK_SRC] = &nss_cc_ppe_clk_src.clkr,
	[NSS_CC_PPE_SWITCH_IPE_CLK] = &nss_cc_ppe_switch_ipe_clk.clkr,
	[NSS_CC_PPE_SWITCH_BTQ_CLK] = &nss_cc_ppe_switch_btq_clk.clkr,
	[NSS_CC_PPE_SWITCH_CLK] = &nss_cc_ppe_switch_clk.clkr,
	[NSS_CC_PPE_SWITCH_CFG_CLK] = &nss_cc_ppe_switch_cfg_clk.clkr,
	[NSS_CC_PPE_EDMA_CLK] = &nss_cc_ppe_edma_clk.clkr,
	[NSS_CC_PPE_EDMA_CFG_CLK] = &nss_cc_ppe_edma_cfg_clk.clkr,
	[NSS_CC_EIP_PPE_CLK] = &nss_cc_eip_ppe_clk.clkr,
	[NSS_CC_PORT1_MAC_CLK] = &nss_cc_port1_mac_clk.clkr,
	[NSS_CC_PORT1_RX_CLK] = &nss_cc_port1_rx_clk.clkr,
	[NSS_CC_PORT1_RX_CLK_SRC] = &nss_cc_port1_rx_clk_src.clkr,
	[NSS_CC_PORT1_RX_DIV_CLK_SRC] = &nss_cc_port1_rx_div_clk_src.clkr,
	[NSS_CC_PORT1_TX_CLK] = &nss_cc_port1_tx_clk.clkr,
	[NSS_CC_PORT1_TX_CLK_SRC] = &nss_cc_port1_tx_clk_src.clkr,
	[NSS_CC_PORT1_TX_DIV_CLK_SRC] = &nss_cc_port1_tx_div_clk_src.clkr,
	[NSS_CC_PORT2_MAC_CLK] = &nss_cc_port2_mac_clk.clkr,
	[NSS_CC_PORT2_RX_CLK] = &nss_cc_port2_rx_clk.clkr,
	[NSS_CC_PORT2_RX_CLK_SRC] = &nss_cc_port2_rx_clk_src.clkr,
	[NSS_CC_PORT2_RX_DIV_CLK_SRC] = &nss_cc_port2_rx_div_clk_src.clkr,
	[NSS_CC_PORT2_TX_CLK] = &nss_cc_port2_tx_clk.clkr,
	[NSS_CC_PORT2_TX_CLK_SRC] = &nss_cc_port2_tx_clk_src.clkr,
	[NSS_CC_PORT2_TX_DIV_CLK_SRC] = &nss_cc_port2_tx_div_clk_src.clkr,
	[NSS_CC_PORT3_MAC_CLK] = &nss_cc_port3_mac_clk.clkr,
	[NSS_CC_PORT3_RX_CLK] = &nss_cc_port3_rx_clk.clkr,
	[NSS_CC_PORT3_RX_CLK_SRC] = &nss_cc_port3_rx_clk_src.clkr,
	[NSS_CC_PORT3_RX_DIV_CLK_SRC] = &nss_cc_port3_rx_div_clk_src.clkr,
	[NSS_CC_PORT3_TX_CLK] = &nss_cc_port3_tx_clk.clkr,
	[NSS_CC_PORT3_TX_CLK_SRC] = &nss_cc_port3_tx_clk_src.clkr,
	[NSS_CC_PORT3_TX_DIV_CLK_SRC] = &nss_cc_port3_tx_div_clk_src.clkr,
	[NSS_CC_PORT4_MAC_CLK] = &nss_cc_port4_mac_clk.clkr,
	[NSS_CC_PORT4_RX_CLK] = &nss_cc_port4_rx_clk.clkr,
	[NSS_CC_PORT4_RX_CLK_SRC] = &nss_cc_port4_rx_clk_src.clkr,
	[NSS_CC_PORT4_RX_DIV_CLK_SRC] = &nss_cc_port4_rx_div_clk_src.clkr,
	[NSS_CC_PORT4_TX_CLK] = &nss_cc_port4_tx_clk.clkr,
	[NSS_CC_PORT4_TX_CLK_SRC] = &nss_cc_port4_tx_clk_src.clkr,
	[NSS_CC_PORT4_TX_DIV_CLK_SRC] = &nss_cc_port4_tx_div_clk_src.clkr,
	[NSS_CC_PORT5_MAC_CLK] = &nss_cc_port5_mac_clk.clkr,
	[NSS_CC_PORT5_RX_CLK] = &nss_cc_port5_rx_clk.clkr,
	[NSS_CC_PORT5_RX_CLK_SRC] = &nss_cc_port5_rx_clk_src.clkr,
	[NSS_CC_PORT5_RX_DIV_CLK_SRC] = &nss_cc_port5_rx_div_clk_src.clkr,
	[NSS_CC_PORT5_TX_CLK] = &nss_cc_port5_tx_clk.clkr,
	[NSS_CC_PORT5_TX_CLK_SRC] = &nss_cc_port5_tx_clk_src.clkr,
	[NSS_CC_PORT5_TX_DIV_CLK_SRC] = &nss_cc_port5_tx_div_clk_src.clkr,
	[NSS_CC_PORT6_MAC_CLK] = &nss_cc_port6_mac_clk.clkr,
	[NSS_CC_PORT6_RX_CLK] = &nss_cc_port6_rx_clk.clkr,
	[NSS_CC_PORT6_RX_CLK_SRC] = &nss_cc_port6_rx_clk_src.clkr,
	[NSS_CC_PORT6_RX_DIV_CLK_SRC] = &nss_cc_port6_rx_div_clk_src.clkr,
	[NSS_CC_PORT6_TX_CLK] = &nss_cc_port6_tx_clk.clkr,
	[NSS_CC_PORT6_TX_CLK_SRC] = &nss_cc_port6_tx_clk_src.clkr,
	[NSS_CC_PORT6_TX_DIV_CLK_SRC] = &nss_cc_port6_tx_div_clk_src.clkr,
	[NSS_CC_NSSNOC_PPE_CLK] = &nss_cc_nssnoc_ppe_clk.clkr,
	[NSS_CC_NSSNOC_PPE_CFG_CLK] = &nss_cc_nssnoc_ppe_cfg_clk.clkr,
	[NSS_CC_UNIPHY_PORT1_RX_CLK] = &nss_cc_uniphy_port1_rx_clk.clkr,
	[NSS_CC_UNIPHY_PORT1_TX_CLK] = &nss_cc_uniphy_port1_tx_clk.clkr,
	[NSS_CC_UNIPHY_PORT2_RX_CLK] = &nss_cc_uniphy_port2_rx_clk.clkr,
	[NSS_CC_UNIPHY_PORT2_TX_CLK] = &nss_cc_uniphy_port2_tx_clk.clkr,
	[NSS_CC_UNIPHY_PORT3_RX_CLK] = &nss_cc_uniphy_port3_rx_clk.clkr,
	[NSS_CC_UNIPHY_PORT3_TX_CLK] = &nss_cc_uniphy_port3_tx_clk.clkr,
	[NSS_CC_UNIPHY_PORT4_RX_CLK] = &nss_cc_uniphy_port4_rx_clk.clkr,
	[NSS_CC_UNIPHY_PORT4_TX_CLK] = &nss_cc_uniphy_port4_tx_clk.clkr,
	[NSS_CC_UNIPHY_PORT5_RX_CLK] = &nss_cc_uniphy_port5_rx_clk.clkr,
	[NSS_CC_UNIPHY_PORT5_TX_CLK] = &nss_cc_uniphy_port5_tx_clk.clkr,
	[NSS_CC_UNIPHY_PORT6_RX_CLK] = &nss_cc_uniphy_port6_rx_clk.clkr,
	[NSS_CC_UNIPHY_PORT6_TX_CLK] = &nss_cc_uniphy_port6_tx_clk.clkr,
	[NSS_CC_XGMAC0_PTP_REF_CLK] = &nss_cc_xgmac0_ptp_ref_clk.clkr,
	[NSS_CC_XGMAC0_PTP_REF_DIV_CLK_SRC] = &nss_cc_xgmac0_ptp_ref_div_clk_src.clkr,
	[NSS_CC_XGMAC1_PTP_REF_CLK] = &nss_cc_xgmac1_ptp_ref_clk.clkr,
	[NSS_CC_XGMAC1_PTP_REF_DIV_CLK_SRC] = &nss_cc_xgmac1_ptp_ref_div_clk_src.clkr,
	[NSS_CC_XGMAC2_PTP_REF_CLK] = &nss_cc_xgmac2_ptp_ref_clk.clkr,
	[NSS_CC_XGMAC2_PTP_REF_DIV_CLK_SRC] = &nss_cc_xgmac2_ptp_ref_div_clk_src.clkr,
	[NSS_CC_XGMAC3_PTP_REF_CLK] = &nss_cc_xgmac3_ptp_ref_clk.clkr,
	[NSS_CC_XGMAC3_PTP_REF_DIV_CLK_SRC] = &nss_cc_xgmac3_ptp_ref_div_clk_src.clkr,
	[NSS_CC_XGMAC4_PTP_REF_CLK] = &nss_cc_xgmac4_ptp_ref_clk.clkr,
	[NSS_CC_XGMAC4_PTP_REF_DIV_CLK_SRC] = &nss_cc_xgmac4_ptp_ref_div_clk_src.clkr,
	[NSS_CC_XGMAC5_PTP_REF_CLK] = &nss_cc_xgmac5_ptp_ref_clk.clkr,
	[NSS_CC_XGMAC5_PTP_REF_DIV_CLK_SRC] = &nss_cc_xgmac5_ptp_ref_div_clk_src.clkr,
	[NSS_CC_CE_CLK_SRC] = &nss_cc_ce_clk_src.clkr,
	[NSS_CC_CE_APB_CLK] = &nss_cc_ce_apb_clk.clkr,
	[NSS_CC_NSSNOC_CE_APB_CLK] = &nss_cc_nssnoc_ce_apb_clk.clkr,
	[NSS_CC_CE_DIV_CLK_SRC] = &nss_cc_ce_div_clk_src.clkr,
	[NSS_CC_CE_AXI_CLK] = &nss_cc_ce_axi_clk.clkr,
	[NSS_CC_NSSNOC_CE_AXI_CLK] = &nss_cc_nssnoc_ce_axi_clk.clkr,
	[NSS_CC_EIP_BFDCD_CLK_SRC] = &nss_cc_eip_bfdcd_clk_src.clkr,
	[NSS_CC_EIP_CLK] = &nss_cc_eip_clk.clkr,
	[NSS_CC_NSSNOC_EIP_CLK] = &nss_cc_nssnoc_eip_clk.clkr,
	[NSS_CC_CFG_CLK_SRC] = &nss_cc_cfg_clk_src.clkr,
	[NSS_CC_NSS_CSR_CLK] = &nss_cc_nss_csr_clk.clkr,
	[NSS_CC_NSSNOC_NSS_CSR_CLK] = &nss_cc_nssnoc_nss_csr_clk.clkr,
};

static const struct qcom_reset_map nss_cc_ipq9650_resets[] = {
	[NSS_CC_PPE_BCR] = { 0x3e8 },
	[NSS_CC_PPE_SWITCH_IPE_CLK_ARES] = { 0x424, 2 },
	[NSS_CC_PPE_SWITCH_BTQ_CLK_ARES] = { 0x42c, 2 },
	[NSS_CC_PPE_SWITCH_CLK_ARES] = { 0x434, 2 },
	[NSS_CC_PPE_SWITCH_CFG_CLK_ARES] = { 0x43c, 2 },
	[NSS_CC_PPE_EDMA_CLK_ARES] = { 0x440, 2 },
	[NSS_CC_PPE_EDMA_CFG_CLK_ARES] = { 0x448, 2 },
	[NSS_CC_PORT1_MAC_CLK_ARES] = { 0x44c, 2 },
	[NSS_CC_PORT2_MAC_CLK_ARES] = { 0x454, 2 },
	[NSS_CC_PORT3_MAC_CLK_ARES] = { 0x45c, 2 },
	[NSS_CC_PORT4_MAC_CLK_ARES] = { 0x464, 2 },
	[NSS_CC_PORT5_MAC_CLK_ARES] = { 0x46c, 2 },
	[NSS_CC_PORT6_MAC_CLK_ARES] = { 0x474, 2 },
	[NSS_CC_EIP_PPE_CLK_ARES] = { 0x47c, 2 },
	[NSS_CC_XGMAC0_PTP_REF_CLK_ARES] = { 0x488, 2 },
	[NSS_CC_XGMAC1_PTP_REF_CLK_ARES] = { 0x48c, 2 },
	[NSS_CC_XGMAC2_PTP_REF_CLK_ARES] = { 0x490, 2 },
	[NSS_CC_XGMAC3_PTP_REF_CLK_ARES] = { 0x494, 2 },
	[NSS_CC_XGMAC4_PTP_REF_CLK_ARES] = { 0x498, 2 },
	[NSS_CC_XGMAC5_PTP_REF_CLK_ARES] = { 0x49c, 2 },
	[NSS_CC_NSSNOC_PPE_CLK_ARES] = { 0x4a4, 2 },
	[NSS_CC_NSSNOC_PPE_CFG_CLK_ARES] = { 0x4a8, 2 },
	[NSS_CC_PORT1_RX_CLK_ARES] = { 0x548, 2 },
	[NSS_CC_PORT1_TX_CLK_ARES] = { 0x550, 2 },
	[NSS_CC_PORT2_RX_CLK_ARES] = { 0x558, 2 },
	[NSS_CC_PORT2_TX_CLK_ARES] = { 0x560, 2 },
	[NSS_CC_PORT3_RX_CLK_ARES] = { 0x568, 2 },
	[NSS_CC_PORT3_TX_CLK_ARES] = { 0x570, 2 },
	[NSS_CC_PORT4_RX_CLK_ARES] = { 0x578, 2 },
	[NSS_CC_PORT4_TX_CLK_ARES] = { 0x580, 2 },
	[NSS_CC_PORT5_RX_CLK_ARES] = { 0x588, 2 },
	[NSS_CC_PORT5_TX_CLK_ARES] = { 0x590, 2 },
	[NSS_CC_PORT6_RX_CLK_ARES] = { 0x598, 2 },
	[NSS_CC_PORT6_TX_CLK_ARES] = { 0x5a0, 2 },
	[NSS_CC_UNIPHY_PORT1_RX_CLK_ARES] = { 0x5e0, 2 },
	[NSS_CC_UNIPHY_PORT1_TX_CLK_ARES] = { 0x5e4, 2 },
	[NSS_CC_UNIPHY_PORT2_RX_CLK_ARES] = { 0x5e8, 2 },
	[NSS_CC_UNIPHY_PORT2_TX_CLK_ARES] = { 0x5ec, 2 },
	[NSS_CC_UNIPHY_PORT3_RX_CLK_ARES] = { 0x5f0, 2 },
	[NSS_CC_UNIPHY_PORT3_TX_CLK_ARES] = { 0x5f4, 2 },
	[NSS_CC_UNIPHY_PORT4_RX_CLK_ARES] = { 0x5f8, 2 },
	[NSS_CC_UNIPHY_PORT4_TX_CLK_ARES] = { 0x5fc, 2 },
	[NSS_CC_UNIPHY_PORT5_RX_CLK_ARES] = { 0x600, 2 },
	[NSS_CC_UNIPHY_PORT5_TX_CLK_ARES] = { 0x604, 2 },
	[NSS_CC_UNIPHY_PORT6_RX_CLK_ARES] = { 0x608, 2 },
	[NSS_CC_UNIPHY_PORT6_TX_CLK_ARES] = { 0x60c, 2 },
	[NSS_CC_CE_APB_CLK_ARES] = { 0x650, 2 },
	[NSS_CC_CE_AXI_CLK_ARES] = { 0x654, 2 },
	[NSS_CC_NSSNOC_CE_APB_CLK_ARES] = { 0x65c, 2 },
	[NSS_CC_NSSNOC_CE_AXI_CLK_ARES] = { 0x660, 2 },
	[NSS_CC_EIP_CLK_ARES] = { 0x6bc, 2 },
	[NSS_CC_NSSNOC_EIP_CLK_ARES] = { 0x6c4, 2 },
	[NSS_CC_NSS_CSR_CLK_ARES] = { 0x714, 2 },
	[NSS_CC_NSSNOC_NSS_CSR_CLK_ARES] = { 0x718, 2 },
	[NSS_CC_DEBUG_CLK_ARES] = { 0x770, 2 },
};

static const struct regmap_config nss_cc_ipq9650_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x800,
	.fast_io = true,
};

static const struct qcom_cc_desc nss_cc_ipq9650_desc = {
	.config = &nss_cc_ipq9650_regmap_config,
	.clks = nss_cc_ipq9650_clocks,
	.num_clks = ARRAY_SIZE(nss_cc_ipq9650_clocks),
	.resets = nss_cc_ipq9650_resets,
	.num_resets = ARRAY_SIZE(nss_cc_ipq9650_resets),
};

static const struct of_device_id nss_cc_ipq9650_match_table[] = {
	{ .compatible = "qcom,ipq9650-nsscc" },
	{ }
};
MODULE_DEVICE_TABLE(of, nss_cc_ipq9650_match_table);

static int nss_cc_ipq9650_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	int ret;

	regmap = qcom_cc_map(pdev, &nss_cc_ipq9650_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = qcom_cc_really_probe(&pdev->dev, &nss_cc_ipq9650_desc, regmap);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to register NSS CC clocks with error: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "Registered NSS CC clocks\n");

	return ret;
}

static struct platform_driver nss_cc_ipq9650_driver = {
	.probe = nss_cc_ipq9650_probe,
	.driver = {
		.name = "qcom,ipq9650-nsscc",
		.of_match_table = nss_cc_ipq9650_match_table,
	},
};

static int __init nss_cc_ipq9650_init(void)
{
	return platform_driver_register(&nss_cc_ipq9650_driver);
}
core_initcall(nss_cc_ipq9650_init);

static void __exit nss_cc_ipq9650_exit(void)
{
	platform_driver_unregister(&nss_cc_ipq9650_driver);
}
module_exit(nss_cc_ipq9650_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. NSSCC IPQ9650 Driver");
MODULE_LICENSE("GPL v2");
