/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/phy.h>
#include <linux/mdio.h>

#include <dt-bindings/clock/qcom,qce2204-nsscc.h>
#include <dt-bindings/reset/qcom,qce2204-nsscc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "clk-regmap-divider.h"
#include "clk-regmap-mux.h"
#include "common.h"
#include "reset.h"

#define QCE2204_CLK_REG_BASE		0x6800000

enum {
	DT_XO,
	DT_UNIPHY1_RX,
	DT_UNIPHY1_TX,
	DT_UNIPHY1_PPE_250M,
	DT_UNIPHY0_RX_312P5M,
	DT_UNIPHY0_TX_312P5M,
	DT_UNIPHY1_RX_312P5M,
	DT_UNIPHY1_TX_312P5M,
};

enum {
	P_XO,
	P_UNIPHY1_RX,
	P_UNIPHY1_TX,
	P_UNIPHY1_PPE_250M,
	P_UNIPHY0_RX_312P5M,
	P_UNIPHY0_TX_312P5M,
	P_UNIPHY1_RX_312P5M,
	P_UNIPHY1_TX_312P5M,
};

/* QCE2204 MDIO access functions - reuse from DSA driver pattern */
static inline void qce2204_split_addr(u32 regaddr, u16 *reg_low, u16 *reg_mid,
				       u16 *reg_high)
{
	/* bit2 is 1 for writing/reading high byte data[31, 16],
	 * bit2 is 0 for writing/reading low byte data[15, 0].
	 */
	*reg_low = FIELD_GET(GENMASK(3, 0), regaddr);
	*reg_low &= 0xc;
	*reg_low <<= 1;

	*reg_mid = FIELD_GET(GENMASK(19, 4), regaddr);

	*reg_high = FIELD_GET(GENMASK(23, 20), regaddr);
	*reg_high <<= 1;
	*reg_high |= BIT(0);
}

static int qce2204_ahb_read(struct mii_bus *bus, int addr, u32 reg, u32 *val)
{
	u16 reg_low, reg_mid, reg_high;
	int ret, data;

	qce2204_split_addr(reg, &reg_low, &reg_mid, &reg_high);

	mutex_lock(&bus->mdio_lock);
	/* write ahb address bit4~bit23 */
	__mdiobus_write(bus, addr, reg_high & 0x1f, reg_mid);
	usleep_range(100, 200);

	/* write ahb address bit0~bit3 and read low 16bit data */
	ret = __mdiobus_read(bus, addr, reg_low);
	if (ret >= 0) {
		data = ret;
		/* write ahb address bit0~bit3 and read high 16 bit data */
		ret = __mdiobus_read(bus, addr, (reg_low | BIT(2)));
		if (ret >= 0)
			*val = data | ret << 16;
	}
	mutex_unlock(&bus->mdio_lock);

	return ret < 0 ? ret : 0;
}

static int qce2204_ahb_write(struct mii_bus *bus, int addr, u32 reg, u32 val)
{
	u16 reg_low, reg_mid, reg_high;
	int ret;

	qce2204_split_addr(reg, &reg_low, &reg_mid, &reg_high);

	mutex_lock(&bus->mdio_lock);
	/* write ahb address bit4~bit23 */
	__mdiobus_write(bus, addr, reg_high & 0x1f, reg_mid);
	usleep_range(100, 200);

	/* write ahb address bit0~bit3 and write low 16 bit data */
	ret = __mdiobus_write(bus, addr, reg_low, lower_16_bits(val));
	/* write ahb address bit0~bit3 and write high 16 bit data */
	if (!ret)
		ret = __mdiobus_write(bus, addr, (reg_low | BIT(2)), upper_16_bits(val));

	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int qce2204_ahb_update(struct mii_bus *bus, int addr, u32 reg,
			      u32 mask, u32 value)
{
	u16 reg_low, reg_mid, reg_high;
	u32 data;
	int ret;

	qce2204_split_addr(reg, &reg_low, &reg_mid, &reg_high);

	mutex_lock(&bus->mdio_lock);
	/* write ahb address bit4~bit23 */
	__mdiobus_write(bus, addr, reg_high & 0x1f, reg_mid);
	usleep_range(100, 200);

	/* write ahb address bit0~bit3 and read low 16bit data */
	ret = __mdiobus_read(bus, addr, reg_low);
	if (ret >= 0) {
		data = ret;
		/* write ahb address bit0~bit3 and read high 16 bit data */
		ret = __mdiobus_read(bus, addr, (reg_low | BIT(2)));
		if (ret >= 0)
			data |= ret << 16;
	}

	if (ret < 0)
		goto ahb_update_exit;

	data &= ~mask;
	data |= value;

	/* write ahb address bit0~bit3 and write low 16 bit data */
	ret = __mdiobus_write(bus, addr, reg_low, lower_16_bits(data));
	/* write ahb address bit0~bit3 and write high 16 bit data */
	if (!ret)
		ret = __mdiobus_write(bus, addr, (reg_low | BIT(2)), upper_16_bits(data));

ahb_update_exit:
	mutex_unlock(&bus->mdio_lock);

	return ret < 0 ? ret : 0;
}

static int qce2204_regmap_read(void *context, unsigned int regaddr, unsigned int *val)
{
	struct mdio_device *mdiodev = context;
	u32 addr = regaddr + QCE2204_CLK_REG_BASE;

	return qce2204_ahb_read(mdiodev->bus, mdiodev->addr, addr, val);
}

static int qce2204_regmap_write(void *context, unsigned int regaddr, unsigned int val)
{
	struct mdio_device *mdiodev = context;
	u32 addr = regaddr + QCE2204_CLK_REG_BASE;

	return qce2204_ahb_write(mdiodev->bus, mdiodev->addr, addr, val);
}

static int qce2204_regmap_update_bits(void *context, unsigned int regaddr,
				      unsigned int mask, unsigned int value)
{
	struct mdio_device *mdiodev = context;
	u32 addr = regaddr + QCE2204_CLK_REG_BASE;

	return qce2204_ahb_update(mdiodev->bus, mdiodev->addr, addr, mask, value);
}

static const struct regmap_config nsscc_qce2204_regmap_config = {
	.reg_bits = 12,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x428,
	.reg_read = qce2204_regmap_read,
	.reg_write = qce2204_regmap_write,
	.reg_update_bits = qce2204_regmap_update_bits,
	.disable_locking = true,
};

static const struct clk_parent_data nsscc_xo_data[] = {
	{ .index = DT_XO },
};

static const struct parent_map nsscc_xo_map[] = {
	{ P_XO, 0 },
};

static const struct clk_parent_data nsscc_switch_core_data[] = {
	{ .index = DT_UNIPHY1_PPE_250M },
};

static const struct parent_map nsscc_switch_core_map[] = {
	{ P_UNIPHY1_PPE_250M, 2 },
};

static const struct clk_parent_data nsscc_ahb_sys_data[] = {
	{ .index = DT_UNIPHY1_TX_312P5M },
};

static const struct parent_map nsscc_ahb_sys_map[] = {
	{ P_UNIPHY1_TX_312P5M, 2 },
};

/* Port0 clocks - UNIPHY1 RX/TX */
static const struct clk_parent_data nsscc_port0_rx_data[] = {
	{ .index = DT_UNIPHY1_RX },
};

static const struct parent_map nsscc_port0_rx_map[] = {
	{ P_UNIPHY1_RX, 1 },
};

static const struct clk_parent_data nsscc_port0_tx_data[] = {
	{ .index = DT_UNIPHY1_TX },
};

static const struct parent_map nsscc_port0_tx_map[] = {
	{ P_UNIPHY1_TX, 2 },
};

/* Port1-3 clocks - UNIPHY1 312.5MHz with mux options */
static const struct clk_parent_data nsscc_port1_3_rx_data[] = {
	{ .index = DT_UNIPHY1_TX_312P5M },
};

static const struct parent_map nsscc_port1_3_rx_map[] = {
	{ P_UNIPHY1_TX_312P5M, 6 },
};

static const struct clk_parent_data nsscc_port1_3_tx_data[] = {
	{ .index = DT_UNIPHY1_TX_312P5M },
	{ .index = DT_UNIPHY1_RX_312P5M },
};

static const struct parent_map nsscc_port1_3_tx_map[] = {
	{ P_UNIPHY1_TX_312P5M, 6 },
	{ P_UNIPHY1_RX_312P5M, 7 },
};

/* Port4 clocks - Multiple UNIPHY options */
static const struct clk_parent_data nsscc_port4_rx_data[] = {
	{ .index = DT_UNIPHY0_TX_312P5M },
	{ .index = DT_UNIPHY1_TX_312P5M },
};

static const struct parent_map nsscc_port4_rx_map[] = {
	{ P_UNIPHY0_TX_312P5M, 2 },
	{ P_UNIPHY1_TX_312P5M, 3 },
};

static const struct clk_parent_data nsscc_port4_tx_data[] = {
	{ .index = DT_UNIPHY0_RX_312P5M },
	{ .index = DT_UNIPHY1_TX_312P5M },
	{ .index = DT_UNIPHY1_RX_312P5M },
};

static const struct parent_map nsscc_port4_tx_map[] = {
	{ P_UNIPHY0_RX_312P5M, 1 },
	{ P_UNIPHY1_TX_312P5M, 3 },
	{ P_UNIPHY1_RX_312P5M, 7 },
};

/* Port5 clocks - UNIPHY0 312.5MHz */
static const struct clk_parent_data nsscc_port5_rx_data[] = {
	{ .index = DT_UNIPHY0_RX_312P5M },
};

static const struct parent_map nsscc_port5_rx_map[] = {
	{ P_UNIPHY0_RX_312P5M, 1 },
};

static const struct clk_parent_data nsscc_port5_tx_data[] = {
	{ .index = DT_UNIPHY0_TX_312P5M },
};

static const struct parent_map nsscc_port5_tx_map[] = {
	{ P_UNIPHY0_TX_312P5M, 2 },
};

/* Switch Core Clock */
static struct clk_rcg2 nsscc_switch_core_clk_src = {
	.cmd_rcgr = 0x0,
	.hid_width = 5,
	.parent_map = nsscc_switch_core_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_switch_core_clk_src",
		.parent_data = nsscc_switch_core_data,
		.num_parents = ARRAY_SIZE(nsscc_switch_core_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch nsscc_switch_core_clk = {
	.halt_reg = 0x10,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x10,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_core_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Switch IPE Clock */
static struct clk_branch nsscc_switch_ipe_clk = {
	.halt_reg = 0x18,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x18,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_ipe_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Switch BTQ Clock */
static struct clk_branch nsscc_switch_btq_clk = {
	.halt_reg = 0x20,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x20,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_btq_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Switch CFG Clock */
static struct clk_branch nsscc_switch_cfg_clk = {
	.halt_reg = 0x28,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x28,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_cfg_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC0-5 Clocks */
static struct clk_branch nsscc_switch_mac0_clk = {
	.halt_reg = 0x30,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x30,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_mac0_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_switch_mac1_clk = {
	.halt_reg = 0x38,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x38,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_mac1_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_switch_mac2_clk = {
	.halt_reg = 0x40,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x40,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_mac2_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_switch_mac3_clk = {
	.halt_reg = 0x48,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x48,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_mac3_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_switch_mac4_clk = {
	.halt_reg = 0x50,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x50,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_mac4_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_switch_mac5_clk = {
	.halt_reg = 0x58,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x58,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_switch_mac5_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* XGMAC PTP Reference Clocks */
static struct clk_branch nsscc_xgmac0_ptp_ref_clk = {
	.halt_reg = 0x60,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x60,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_xgmac0_ptp_ref_clk",
			.parent_data = nsscc_xo_data,
			.num_parents = ARRAY_SIZE(nsscc_xo_data),
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_xgmac1_ptp_ref_clk = {
	.halt_reg = 0x68,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x68,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_xgmac1_ptp_ref_clk",
			.parent_data = nsscc_xo_data,
			.num_parents = ARRAY_SIZE(nsscc_xo_data),
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* APB Bridge Clock */
static struct clk_branch nsscc_apb_bridge_clk = {
	.halt_reg = 0x70,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x70,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_apb_bridge_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_switch_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC0 TX Clock - UNIPHY1 TX */
static struct clk_rcg2 nsscc_mac0_tx_clk_src = {
	.cmd_rcgr = 0x78,
	.hid_width = 5,
	.parent_map = nsscc_port0_tx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac0_tx_clk_src",
		.parent_data = nsscc_port0_tx_data,
		.num_parents = ARRAY_SIZE(nsscc_port0_tx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac0_tx_div_clk_src = {
	.reg = 0x80,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac0_tx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac0_tx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac0_tx_clk = {
	.halt_reg = 0x88,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x88,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac0_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac0_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac0_tx_srds1_clk = {
	.halt_reg = 0x8c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac0_tx_srds1_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac0_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC0 RX Clock - UNIPHY1 RX */
static struct clk_rcg2 nsscc_mac0_rx_clk_src = {
	.cmd_rcgr = 0x94,
	.hid_width = 5,
	.parent_map = nsscc_port0_rx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac0_rx_clk_src",
		.parent_data = nsscc_port0_rx_data,
		.num_parents = ARRAY_SIZE(nsscc_port0_rx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac0_rx_div_clk_src = {
	.reg = 0x9c,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac0_rx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac0_rx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac0_rx_clk = {
	.halt_reg = 0xa4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac0_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac0_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac0_rx_srds1_clk = {
	.halt_reg = 0xac,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xac,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac0_rx_srds1_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac0_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC1 TX Clock - UNIPHY1 312.5MHz */
static struct clk_rcg2 nsscc_mac1_tx_clk_src = {
	.cmd_rcgr = 0xb4,
	.hid_width = 5,
	.parent_map = nsscc_port1_3_tx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac1_tx_clk_src",
		.parent_data = nsscc_port1_3_tx_data,
		.num_parents = ARRAY_SIZE(nsscc_port1_3_tx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac1_tx_div_clk_src = {
	.reg = 0xbc,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_tx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_tx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac1_srds1_ch0_xgmii_rx_div_clk_src = {
	.reg = 0xc4,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_srds1_ch0_xgmii_rx_div_clk_src",
			.parent_data = nsscc_port1_3_rx_data,
			.num_parents = ARRAY_SIZE(nsscc_port1_3_rx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_srds1_ch0_rx_clk = {
	.halt_reg = 0xcc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xcc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_srds1_ch0_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_srds1_ch0_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_tx_clk = {
	.halt_reg = 0xd0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xd0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_gephy0_tx_clk = {
	.halt_reg = 0xd4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xd4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_gephy0_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_srds1_ch0_xgmii_rx_clk = {
	.halt_reg = 0xd8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xd8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_srds1_ch0_xgmii_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_srds1_ch0_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC1 RX Clock */
static struct clk_rcg2 nsscc_mac1_rx_clk_src = {
	.cmd_rcgr = 0xe0,
	.hid_width = 5,
	.parent_map = nsscc_port1_3_rx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac1_rx_clk_src",
		.parent_data = nsscc_port1_3_rx_data,
		.num_parents = ARRAY_SIZE(nsscc_port1_3_rx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac1_rx_div_clk_src = {
	.reg = 0xe8,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_rx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_rx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac1_srds1_ch0_xgmii_tx_div_clk_src = {
	.reg = 0xf0,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_srds1_ch0_xgmii_tx_div_clk_src",
			.parent_data = nsscc_port1_3_tx_data,
			.num_parents = ARRAY_SIZE(nsscc_port1_3_tx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_srds1_ch0_tx_clk = {
	.halt_reg = 0xf8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xf8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_srds1_ch0_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_srds1_ch0_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_rx_clk = {
	.halt_reg = 0xfc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xfc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_gephy0_rx_clk = {
	.halt_reg = 0x104,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x104,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_gephy0_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac1_srds1_ch0_xgmii_tx_clk = {
	.halt_reg = 0x108,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x108,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac1_srds1_ch0_xgmii_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac1_srds1_ch0_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC2 TX Clock - UNIPHY1 312.5MHz */
static struct clk_rcg2 nsscc_mac2_tx_clk_src = {
	.cmd_rcgr = 0x110,
	.hid_width = 5,
	.parent_map = nsscc_port1_3_tx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac2_tx_clk_src",
		.parent_data = nsscc_port1_3_tx_data,
		.num_parents = ARRAY_SIZE(nsscc_port1_3_tx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac2_tx_div_clk_src = {
	.reg = 0x118,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_tx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_tx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac2_srds1_ch1_xgmii_rx_div_clk_src = {
	.reg = 0x120,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_srds1_ch1_xgmii_rx_div_clk_src",
			.parent_data = nsscc_port1_3_rx_data,
			.num_parents = ARRAY_SIZE(nsscc_port1_3_rx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_srds1_ch1_rx_clk = {
	.halt_reg = 0x128,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x128,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_srds1_ch1_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_srds1_ch1_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_tx_clk = {
	.halt_reg = 0x12c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x12c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_gephy1_tx_clk = {
	.halt_reg = 0x130,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x130,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_gephy1_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_srds1_ch1_xgmii_rx_clk = {
	.halt_reg = 0x134,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x134,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_srds1_ch1_xgmii_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_srds1_ch1_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC2 RX Clock */
static struct clk_rcg2 nsscc_mac2_rx_clk_src = {
	.cmd_rcgr = 0x13c,
	.hid_width = 5,
	.parent_map = nsscc_port1_3_rx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac2_rx_clk_src",
		.parent_data = nsscc_port1_3_rx_data,
		.num_parents = ARRAY_SIZE(nsscc_port1_3_rx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac2_rx_div_clk_src = {
	.reg = 0x144,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_rx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_rx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac2_srds1_ch1_xgmii_tx_div_clk_src = {
	.reg = 0x14c,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_srds1_ch1_xgmii_tx_div_clk_src",
			.parent_data = nsscc_port1_3_tx_data,
			.num_parents = ARRAY_SIZE(nsscc_port1_3_tx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_srds1_ch1_tx_clk = {
	.halt_reg = 0x150,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x150,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_srds1_ch1_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_srds1_ch1_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_rx_clk = {
	.halt_reg = 0x154,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x154,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_gephy1_rx_clk = {
	.halt_reg = 0x158,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x158,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_gephy1_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac2_srds1_ch1_xgmii_tx_clk = {
	.halt_reg = 0x15c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x15c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac2_srds1_ch1_xgmii_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac2_srds1_ch1_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC3 TX Clock - UNIPHY1 312.5MHz */
static struct clk_rcg2 nsscc_mac3_tx_clk_src = {
	.cmd_rcgr = 0x164,
	.hid_width = 5,
	.parent_map = nsscc_port1_3_tx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac3_tx_clk_src",
		.parent_data = nsscc_port1_3_tx_data,
		.num_parents = ARRAY_SIZE(nsscc_port1_3_tx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac3_tx_div_clk_src = {
	.reg = 0x16c,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_tx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_tx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac3_srds1_ch2_xgmii_rx_div_clk_src = {
	.reg = 0x174,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_srds1_ch2_xgmii_rx_div_clk_src",
			.parent_data = nsscc_port1_3_rx_data,
			.num_parents = ARRAY_SIZE(nsscc_port1_3_rx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_srds1_ch2_rx_clk = {
	.halt_reg = 0x17c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x17c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_srds1_ch2_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_srds1_ch2_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_tx_clk = {
	.halt_reg = 0x180,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x180,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_gephy2_tx_clk = {
	.halt_reg = 0x184,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x184,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_gephy2_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_srds1_ch2_xgmii_rx_clk = {
	.halt_reg = 0x188,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x188,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_srds1_ch2_xgmii_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_srds1_ch2_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC3 RX Clock */
static struct clk_rcg2 nsscc_mac3_rx_clk_src = {
	.cmd_rcgr = 0x190,
	.hid_width = 5,
	.parent_map = nsscc_port1_3_rx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac3_rx_clk_src",
		.parent_data = nsscc_port1_3_rx_data,
		.num_parents = ARRAY_SIZE(nsscc_port1_3_rx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac3_rx_div_clk_src = {
	.reg = 0x198,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_rx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_rx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac3_srds1_ch2_xgmii_tx_div_clk_src = {
	.reg = 0x1a0,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_srds1_ch2_xgmii_tx_div_clk_src",
			.parent_data = nsscc_port1_3_tx_data,
			.num_parents = ARRAY_SIZE(nsscc_port1_3_tx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_srds1_ch2_tx_clk = {
	.halt_reg = 0x1a8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1a8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_srds1_ch2_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_srds1_ch2_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_rx_clk = {
	.halt_reg = 0x1ac,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1ac,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_gephy2_rx_clk = {
	.halt_reg = 0x1b0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1b0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_gephy2_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac3_srds1_ch2_xgmii_tx_clk = {
	.halt_reg = 0x1b4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1b4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac3_srds1_ch2_xgmii_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac3_srds1_ch2_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC4 TX Clock - Multiple UNIPHY options */
static struct clk_rcg2 nsscc_mac4_tx_clk_src = {
	.cmd_rcgr = 0x1bc,
	.hid_width = 5,
	.parent_map = nsscc_port4_tx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac4_tx_clk_src",
		.parent_data = nsscc_port4_tx_data,
		.num_parents = ARRAY_SIZE(nsscc_port4_tx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac4_tx_div_clk_src = {
	.reg = 0x1c4,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_tx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_tx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac4_srds1_ch3_xgmii_rx_div_clk_src = {
	.reg = 0x1cc,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_srds1_ch3_xgmii_rx_div_clk_src",
			.parent_data = nsscc_port4_rx_data,
			.num_parents = ARRAY_SIZE(nsscc_port4_rx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_srds1_ch3_rx_clk = {
	.halt_reg = 0x1d0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1d0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_srds1_ch3_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_srds1_ch3_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_tx_clk = {
	.halt_reg = 0x1d4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1d4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_gephy3_tx_clk = {
	.halt_reg = 0x1d8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1d8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_gephy3_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_srds1_ch3_xgmii_rx_clk = {
	.halt_reg = 0x1dc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1dc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_srds1_ch3_xgmii_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_srds1_ch3_xgmii_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC4 RX Clock */
static struct clk_rcg2 nsscc_mac4_rx_clk_src = {
	.cmd_rcgr = 0x1e4,
	.hid_width = 5,
	.parent_map = nsscc_port4_rx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac4_rx_clk_src",
		.parent_data = nsscc_port4_rx_data,
		.num_parents = ARRAY_SIZE(nsscc_port4_rx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac4_rx_div_clk_src = {
	.reg = 0x1ec,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_rx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_rx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_regmap_div nsscc_mac4_srds1_ch3_xgmii_tx_div_clk_src = {
	.reg = 0x1f4,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_srds1_ch3_xgmii_tx_div_clk_src",
			.parent_data = nsscc_port4_tx_data,
			.num_parents = ARRAY_SIZE(nsscc_port4_tx_data),
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_srds1_ch3_tx_clk = {
	.halt_reg = 0x1fc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x1fc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_srds1_ch3_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_srds1_ch3_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_rx_clk = {
	.halt_reg = 0x200,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x200,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_gephy3_rx_clk = {
	.halt_reg = 0x204,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x204,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_gephy3_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac4_srds1_ch3_xgmii_tx_clk = {
	.halt_reg = 0x208,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x208,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac4_srds1_ch3_xgmii_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac4_srds1_ch3_xgmii_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC5 TX Clock - UNIPHY0 312.5MHz */
static struct clk_rcg2 nsscc_mac5_tx_clk_src = {
	.cmd_rcgr = 0x210,
	.hid_width = 5,
	.parent_map = nsscc_port5_tx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac5_tx_clk_src",
		.parent_data = nsscc_port5_tx_data,
		.num_parents = ARRAY_SIZE(nsscc_port5_tx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac5_tx_div_clk_src = {
	.reg = 0x218,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_tx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_tx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac5_tx_clk = {
	.halt_reg = 0x220,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x220,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_tx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac5_tx_srds0_clk = {
	.halt_reg = 0x224,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x224,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_tx_srds0_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac5_tx_srds0_ch0_xgmii_clk = {
	.halt_reg = 0x228,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x228,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_tx_srds0_ch0_xgmii_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_tx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* MAC5 RX Clock */
static struct clk_rcg2 nsscc_mac5_rx_clk_src = {
	.cmd_rcgr = 0x230,
	.hid_width = 5,
	.parent_map = nsscc_port5_rx_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_mac5_rx_clk_src",
		.parent_data = nsscc_port5_rx_data,
		.num_parents = ARRAY_SIZE(nsscc_port5_rx_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_mac5_rx_div_clk_src = {
	.reg = 0x238,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_rx_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_rx_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_mac5_rx_clk = {
	.halt_reg = 0x23c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x23c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_rx_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac5_rx_srds0_clk = {
	.halt_reg = 0x244,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x244,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_rx_srds0_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mac5_rx_srds0_ch0_xgmii_clk = {
	.halt_reg = 0x248,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x248,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mac5_rx_srds0_ch0_xgmii_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_mac5_rx_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* AHB Clock - UNIPHY1 TX 312.5MHz */
static struct clk_rcg2 nsscc_ahb_clk_src = {
	.cmd_rcgr = 0x250,
	.hid_width = 5,
	.parent_map = nsscc_ahb_sys_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_ahb_clk_src",
		.parent_data = nsscc_ahb_sys_data,
		.num_parents = ARRAY_SIZE(nsscc_ahb_sys_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch nsscc_ahb_clk = {
	.halt_reg = 0x258,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x258,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* AHB-related clocks */
static struct clk_branch nsscc_sec_ctrl_ahb_clk = {
	.halt_reg = 0x25c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x25c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_sec_ctrl_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_tlmm_clk = {
	.halt_reg = 0x260,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x260,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_tlmm_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_tlmm_ahb_clk = {
	.halt_reg = 0x264,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x264,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_tlmm_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_cnoc_ahb_clk = {
	.halt_reg = 0x268,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x268,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_cnoc_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mdio_ahb_clk = {
	.halt_reg = 0x26c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x26c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mdio_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_mdio_master_ahb_clk = {
	.halt_reg = 0x270,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x270,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_mdio_master_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_tsens_ahb_clk = {
	.halt_reg = 0x274,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x274,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_tsens_ahb_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* System Clock - UNIPHY1 TX 312.5MHz */
static struct clk_rcg2 nsscc_sys_clk_src = {
	.cmd_rcgr = 0x278,
	.hid_width = 5,
	.parent_map = nsscc_ahb_sys_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_sys_clk_src",
		.parent_data = nsscc_ahb_sys_data,
		.num_parents = ARRAY_SIZE(nsscc_ahb_sys_data),
		.ops = &clk_rcg2_ops,
	},
};

/* SerDes System Clocks */
static struct clk_branch nsscc_srds0_sys_clk = {
	.halt_reg = 0x280,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x280,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_srds0_sys_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_srds1_sys_clk = {
	.halt_reg = 0x284,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x284,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_srds1_sys_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* GEPHY System Clocks */
static struct clk_branch nsscc_gephy0_sys_clk = {
	.halt_reg = 0x288,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x288,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_gephy0_sys_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_gephy1_sys_clk = {
	.halt_reg = 0x28c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x28c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_gephy1_sys_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_gephy2_sys_clk = {
	.halt_reg = 0x290,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x290,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_gephy2_sys_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_gephy3_sys_clk = {
	.halt_reg = 0x294,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x294,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_gephy3_sys_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Additional clocks */
static struct clk_branch nsscc_kdf_clk = {
	.halt_reg = 0x298,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x298,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_kdf_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_tsens_ext_clk = {
	.halt_reg = 0x2a0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x2a0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_tsens_ext_clk",
			.parent_data = nsscc_xo_data,
			.num_parents = ARRAY_SIZE(nsscc_xo_data),
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Security Control Clock - XO */
static struct clk_rcg2 nsscc_sec_ctrl_clk_src = {
	.cmd_rcgr = 0x2a8,
	.hid_width = 5,
	.parent_map = nsscc_xo_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_sec_ctrl_clk_src",
		.parent_data = nsscc_xo_data,
		.num_parents = ARRAY_SIZE(nsscc_xo_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch nsscc_sec_ctrl_clk = {
	.halt_reg = 0x2b0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x2b0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_sec_ctrl_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sec_ctrl_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_sec_ctrl_sense_clk = {
	.halt_reg = 0x2b8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x2b8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_sec_ctrl_sense_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sec_ctrl_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Sleep Clock - XO */
static struct clk_rcg2 nsscc_sleep_clk_src = {
	.cmd_rcgr = 0x2bc,
	.hid_width = 5,
	.parent_map = nsscc_xo_map,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "nsscc_sleep_clk_src",
		.parent_data = nsscc_xo_data,
		.num_parents = ARRAY_SIZE(nsscc_xo_data),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div nsscc_sleep_div_clk_src = {
	.reg = 0x2c4,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_sleep_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sleep_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_sleep_clk = {
	.halt_reg = 0x2cc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x2cc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_sleep_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sleep_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_branch nsscc_ts_sleep_clk = {
	.halt_reg = 0x2d0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x2d0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_ts_sleep_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sleep_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

/* Debug Clock */
static struct clk_regmap_div nsscc_debug_div_clk_src = {
	.reg = 0x2d8,
	.shift = 0,
	.width = 4,
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_debug_div_clk_src",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_sys_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_regmap_div_ops,
		},
	},
};

static struct clk_branch nsscc_debug_clk = {
	.halt_reg = 0x2e0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x2e0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "nsscc_debug_clk",
			.parent_hws = (const struct clk_hw *[]) {
				&nsscc_debug_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_prepare_ops,
		},
	},
};

static struct clk_regmap *nsscc_qce2204_clocks[] = {
	[NSSCC_SWITCH_CORE_CLK_SRC] = &nsscc_switch_core_clk_src.clkr,
	[NSSCC_SWITCH_CORE_CLK] = &nsscc_switch_core_clk.clkr,
	[NSSCC_SWITCH_IPE_CLK] = &nsscc_switch_ipe_clk.clkr,
	[NSSCC_SWITCH_BTQ_CLK] = &nsscc_switch_btq_clk.clkr,
	[NSSCC_SWITCH_CFG_CLK] = &nsscc_switch_cfg_clk.clkr,
	[NSSCC_SWITCH_MAC0_CLK] = &nsscc_switch_mac0_clk.clkr,
	[NSSCC_SWITCH_MAC1_CLK] = &nsscc_switch_mac1_clk.clkr,
	[NSSCC_SWITCH_MAC2_CLK] = &nsscc_switch_mac2_clk.clkr,
	[NSSCC_SWITCH_MAC3_CLK] = &nsscc_switch_mac3_clk.clkr,
	[NSSCC_SWITCH_MAC4_CLK] = &nsscc_switch_mac4_clk.clkr,
	[NSSCC_SWITCH_MAC5_CLK] = &nsscc_switch_mac5_clk.clkr,
	[NSSCC_XGMAC0_PTP_REF_CLK] = &nsscc_xgmac0_ptp_ref_clk.clkr,
	[NSSCC_XGMAC1_PTP_REF_CLK] = &nsscc_xgmac1_ptp_ref_clk.clkr,
	[NSSCC_APB_BRIDGE_CLK] = &nsscc_apb_bridge_clk.clkr,
	[NSSCC_MAC0_TX_CLK_SRC] = &nsscc_mac0_tx_clk_src.clkr,
	[NSSCC_MAC0_TX_DIV_CLK_SRC] = &nsscc_mac0_tx_div_clk_src.clkr,
	[NSSCC_MAC0_TX_CLK] = &nsscc_mac0_tx_clk.clkr,
	[NSSCC_MAC0_TX_SRDS1_CLK] = &nsscc_mac0_tx_srds1_clk.clkr,
	[NSSCC_MAC0_RX_CLK_SRC] = &nsscc_mac0_rx_clk_src.clkr,
	[NSSCC_MAC0_RX_DIV_CLK_SRC] = &nsscc_mac0_rx_div_clk_src.clkr,
	[NSSCC_MAC0_RX_CLK] = &nsscc_mac0_rx_clk.clkr,
	[NSSCC_MAC0_RX_SRDS1_CLK] = &nsscc_mac0_rx_srds1_clk.clkr,
	[NSSCC_MAC1_TX_CLK_SRC] = &nsscc_mac1_tx_clk_src.clkr,
	[NSSCC_MAC1_TX_DIV_CLK_SRC] = &nsscc_mac1_tx_div_clk_src.clkr,
	[NSSCC_MAC1_SRDS1_CH0_XGMII_RX_DIV_CLK_SRC] = &nsscc_mac1_srds1_ch0_xgmii_rx_div_clk_src.clkr,
	[NSSCC_MAC1_SRDS1_CH0_RX_CLK] = &nsscc_mac1_srds1_ch0_rx_clk.clkr,
	[NSSCC_MAC1_TX_CLK] = &nsscc_mac1_tx_clk.clkr,
	[NSSCC_MAC1_GEPHY0_TX_CLK] = &nsscc_mac1_gephy0_tx_clk.clkr,
	[NSSCC_MAC1_SRDS1_CH0_XGMII_RX_CLK] = &nsscc_mac1_srds1_ch0_xgmii_rx_clk.clkr,
	[NSSCC_MAC1_RX_CLK_SRC] = &nsscc_mac1_rx_clk_src.clkr,
	[NSSCC_MAC1_RX_DIV_CLK_SRC] = &nsscc_mac1_rx_div_clk_src.clkr,
	[NSSCC_MAC1_SRDS1_CH0_XGMII_TX_DIV_CLK_SRC] = &nsscc_mac1_srds1_ch0_xgmii_tx_div_clk_src.clkr,
	[NSSCC_MAC1_SRDS1_CH0_TX_CLK] = &nsscc_mac1_srds1_ch0_tx_clk.clkr,
	[NSSCC_MAC1_RX_CLK] = &nsscc_mac1_rx_clk.clkr,
	[NSSCC_MAC1_GEPHY0_RX_CLK] = &nsscc_mac1_gephy0_rx_clk.clkr,
	[NSSCC_MAC1_SRDS1_CH0_XGMII_TX_CLK] = &nsscc_mac1_srds1_ch0_xgmii_tx_clk.clkr,
	[NSSCC_MAC2_TX_CLK_SRC] = &nsscc_mac2_tx_clk_src.clkr,
	[NSSCC_MAC2_TX_DIV_CLK_SRC] = &nsscc_mac2_tx_div_clk_src.clkr,
	[NSSCC_MAC2_SRDS1_CH1_XGMII_RX_DIV_CLK_SRC] = &nsscc_mac2_srds1_ch1_xgmii_rx_div_clk_src.clkr,
	[NSSCC_MAC2_SRDS1_CH1_RX_CLK] = &nsscc_mac2_srds1_ch1_rx_clk.clkr,
	[NSSCC_MAC2_TX_CLK] = &nsscc_mac2_tx_clk.clkr,
	[NSSCC_MAC2_GEPHY1_TX_CLK] = &nsscc_mac2_gephy1_tx_clk.clkr,
	[NSSCC_MAC2_SRDS1_CH1_XGMII_RX_CLK] = &nsscc_mac2_srds1_ch1_xgmii_rx_clk.clkr,
	[NSSCC_MAC2_RX_CLK_SRC] = &nsscc_mac2_rx_clk_src.clkr,
	[NSSCC_MAC2_RX_DIV_CLK_SRC] = &nsscc_mac2_rx_div_clk_src.clkr,
	[NSSCC_MAC2_SRDS1_CH1_XGMII_TX_DIV_CLK_SRC] = &nsscc_mac2_srds1_ch1_xgmii_tx_div_clk_src.clkr,
	[NSSCC_MAC2_SRDS1_CH1_TX_CLK] = &nsscc_mac2_srds1_ch1_tx_clk.clkr,
	[NSSCC_MAC2_RX_CLK] = &nsscc_mac2_rx_clk.clkr,
	[NSSCC_MAC2_GEPHY1_RX_CLK] = &nsscc_mac2_gephy1_rx_clk.clkr,
	[NSSCC_MAC2_SRDS1_CH1_XGMII_TX_CLK] = &nsscc_mac2_srds1_ch1_xgmii_tx_clk.clkr,
	[NSSCC_MAC3_TX_CLK_SRC] = &nsscc_mac3_tx_clk_src.clkr,
	[NSSCC_MAC3_TX_DIV_CLK_SRC] = &nsscc_mac3_tx_div_clk_src.clkr,
	[NSSCC_MAC3_SRDS1_CH2_XGMII_RX_DIV_CLK_SRC] = &nsscc_mac3_srds1_ch2_xgmii_rx_div_clk_src.clkr,
	[NSSCC_MAC3_SRDS1_CH2_RX_CLK] = &nsscc_mac3_srds1_ch2_rx_clk.clkr,
	[NSSCC_MAC3_TX_CLK] = &nsscc_mac3_tx_clk.clkr,
	[NSSCC_MAC3_GEPHY2_TX_CLK] = &nsscc_mac3_gephy2_tx_clk.clkr,
	[NSSCC_MAC3_SRDS1_CH2_XGMII_RX_CLK] = &nsscc_mac3_srds1_ch2_xgmii_rx_clk.clkr,
	[NSSCC_MAC3_RX_CLK_SRC] = &nsscc_mac3_rx_clk_src.clkr,
	[NSSCC_MAC3_RX_DIV_CLK_SRC] = &nsscc_mac3_rx_div_clk_src.clkr,
	[NSSCC_MAC3_SRDS1_CH2_XGMII_TX_DIV_CLK_SRC] = &nsscc_mac3_srds1_ch2_xgmii_tx_div_clk_src.clkr,
	[NSSCC_MAC3_SRDS1_CH2_TX_CLK] = &nsscc_mac3_srds1_ch2_tx_clk.clkr,
	[NSSCC_MAC3_RX_CLK] = &nsscc_mac3_rx_clk.clkr,
	[NSSCC_MAC3_GEPHY2_RX_CLK] = &nsscc_mac3_gephy2_rx_clk.clkr,
	[NSSCC_MAC3_SRDS1_CH2_XGMII_TX_CLK] = &nsscc_mac3_srds1_ch2_xgmii_tx_clk.clkr,
	[NSSCC_MAC4_TX_CLK_SRC] = &nsscc_mac4_tx_clk_src.clkr,
	[NSSCC_MAC4_TX_DIV_CLK_SRC] = &nsscc_mac4_tx_div_clk_src.clkr,
	[NSSCC_MAC4_SRDS1_CH3_XGMII_RX_DIV_CLK_SRC] = &nsscc_mac4_srds1_ch3_xgmii_rx_div_clk_src.clkr,
	[NSSCC_MAC4_SRDS1_CH3_RX_CLK] = &nsscc_mac4_srds1_ch3_rx_clk.clkr,
	[NSSCC_MAC4_TX_CLK] = &nsscc_mac4_tx_clk.clkr,
	[NSSCC_MAC4_GEPHY3_TX_CLK] = &nsscc_mac4_gephy3_tx_clk.clkr,
	[NSSCC_MAC4_SRDS1_CH3_XGMII_RX_CLK] = &nsscc_mac4_srds1_ch3_xgmii_rx_clk.clkr,
	[NSSCC_MAC4_RX_CLK_SRC] = &nsscc_mac4_rx_clk_src.clkr,
	[NSSCC_MAC4_RX_DIV_CLK_SRC] = &nsscc_mac4_rx_div_clk_src.clkr,
	[NSSCC_MAC4_SRDS1_CH3_XGMII_TX_DIV_CLK_SRC] = &nsscc_mac4_srds1_ch3_xgmii_tx_div_clk_src.clkr,
	[NSSCC_MAC4_SRDS1_CH3_TX_CLK] = &nsscc_mac4_srds1_ch3_tx_clk.clkr,
	[NSSCC_MAC4_RX_CLK] = &nsscc_mac4_rx_clk.clkr,
	[NSSCC_MAC4_GEPHY3_RX_CLK] = &nsscc_mac4_gephy3_rx_clk.clkr,
	[NSSCC_MAC4_SRDS1_CH3_XGMII_TX_CLK] = &nsscc_mac4_srds1_ch3_xgmii_tx_clk.clkr,
	[NSSCC_MAC5_TX_CLK_SRC] = &nsscc_mac5_tx_clk_src.clkr,
	[NSSCC_MAC5_TX_DIV_CLK_SRC] = &nsscc_mac5_tx_div_clk_src.clkr,
	[NSSCC_MAC5_TX_CLK] = &nsscc_mac5_tx_clk.clkr,
	[NSSCC_MAC5_TX_SRDS0_CLK] = &nsscc_mac5_tx_srds0_clk.clkr,
	[NSSCC_MAC5_TX_SRDS0_CH0_XGMII_CLK] = &nsscc_mac5_tx_srds0_ch0_xgmii_clk.clkr,
	[NSSCC_MAC5_RX_CLK_SRC] = &nsscc_mac5_rx_clk_src.clkr,
	[NSSCC_MAC5_RX_DIV_CLK_SRC] = &nsscc_mac5_rx_div_clk_src.clkr,
	[NSSCC_MAC5_RX_CLK] = &nsscc_mac5_rx_clk.clkr,
	[NSSCC_MAC5_RX_SRDS0_CLK] = &nsscc_mac5_rx_srds0_clk.clkr,
	[NSSCC_MAC5_RX_SRDS0_CH0_XGMII_CLK] = &nsscc_mac5_rx_srds0_ch0_xgmii_clk.clkr,
	[NSSCC_AHB_CLK_SRC] = &nsscc_ahb_clk_src.clkr,
	[NSSCC_AHB_CLK] = &nsscc_ahb_clk.clkr,
	[NSSCC_SEC_CTRL_AHB_CLK] = &nsscc_sec_ctrl_ahb_clk.clkr,
	[NSSCC_TLMM_CLK] = &nsscc_tlmm_clk.clkr,
	[NSSCC_TLMM_AHB_CLK] = &nsscc_tlmm_ahb_clk.clkr,
	[NSSCC_CNOC_AHB_CLK] = &nsscc_cnoc_ahb_clk.clkr,
	[NSSCC_MDIO_AHB_CLK] = &nsscc_mdio_ahb_clk.clkr,
	[NSSCC_MDIO_MASTER_AHB_CLK] = &nsscc_mdio_master_ahb_clk.clkr,
	[NSSCC_TSENS_AHB_CLK] = &nsscc_tsens_ahb_clk.clkr,
	[NSSCC_SYS_CLK_SRC] = &nsscc_sys_clk_src.clkr,
	[NSSCC_SRDS0_SYS_CLK] = &nsscc_srds0_sys_clk.clkr,
	[NSSCC_SRDS1_SYS_CLK] = &nsscc_srds1_sys_clk.clkr,
	[NSSCC_GEPHY0_SYS_CLK] = &nsscc_gephy0_sys_clk.clkr,
	[NSSCC_GEPHY1_SYS_CLK] = &nsscc_gephy1_sys_clk.clkr,
	[NSSCC_GEPHY2_SYS_CLK] = &nsscc_gephy2_sys_clk.clkr,
	[NSSCC_GEPHY3_SYS_CLK] = &nsscc_gephy3_sys_clk.clkr,
	[NSSCC_KDF_CLK] = &nsscc_kdf_clk.clkr,
	[NSSCC_TSENS_EXT_CLK] = &nsscc_tsens_ext_clk.clkr,
	[NSSCC_SEC_CTRL_CLK_SRC] = &nsscc_sec_ctrl_clk_src.clkr,
	[NSSCC_SEC_CTRL_CLK] = &nsscc_sec_ctrl_clk.clkr,
	[NSSCC_SEC_CTRL_SENSE_CLK] = &nsscc_sec_ctrl_sense_clk.clkr,
	[NSSCC_SLEEP_CLK_SRC] = &nsscc_sleep_clk_src.clkr,
	[NSSCC_SLEEP_DIV_CLK_SRC] = &nsscc_sleep_div_clk_src.clkr,
	[NSSCC_SLEEP_CLK] = &nsscc_sleep_clk.clkr,
	[NSSCC_TS_SLEEP_CLK] = &nsscc_ts_sleep_clk.clkr,
	[NSSCC_DEBUG_DIV_CLK_SRC] = &nsscc_debug_div_clk_src.clkr,
	[NSSCC_DEBUG_CLK] = &nsscc_debug_clk.clkr,
};

static const struct qcom_reset_map nsscc_qce2204_resets[] = {
	[NSSCC_SWITCH_CORE_BCR] = { 0x314, 0 },
	[NSSCC_SWITCH_CORE_ARES] = { 0x14, 2 },
	[NSSCC_SWITCH_IPE_ARES] = { 0x18, 2 },
	[NSSCC_SWITCH_BTQ_ARES] = { 0x20, 2 },
	[NSSCC_SWITCH_MAC0_ARES] = { 0x34, 2 },
	[NSSCC_SWITCH_MAC1_ARES] = { 0x3c, 2 },
	[NSSCC_SWITCH_MAC2_ARES] = { 0x44, 2 },
	[NSSCC_SWITCH_MAC3_ARES] = { 0x4c, 2 },
	[NSSCC_SWITCH_MAC4_ARES] = { 0x54, 2 },
	[NSSCC_SWITCH_MAC5_ARES] = { 0x5c, 2 },
	[NSSCC_MAC0_TX_ARES] = { 0x88, 2 },
	[NSSCC_MAC0_TX_SRDS1_ARES] = { 0x8c, 2 },
	[NSSCC_MAC0_RX_ARES] = { 0xa8, 2 },
	[NSSCC_MAC0_RX_SRDS1_ARES] = { 0xac, 2 },
	[NSSCC_MAC1_TX_ARES] = { 0xd0, 2 },
	[NSSCC_MAC1_GEPHY0_TX_ARES] = { 0xd4, 2 },
	[NSSCC_MAC1_SRDS1_CH0_RX_ARES] = { 0xcc, 2 },
	[NSSCC_MAC1_SRDS1_CH0_XGMII_RX_ARES] = { 0xd8, 2 },
	[NSSCC_MAC1_RX_ARES] = { 0xfc, 2 },
	[NSSCC_MAC1_GEPHY0_RX_ARES] = { 0x104, 2 },
	[NSSCC_MAC1_SRDS1_CH0_TX_ARES] = { 0xf8, 2 },
	[NSSCC_MAC1_SRDS1_CH0_XGMII_TX_ARES] = { 0x108, 2 },
	[NSSCC_MAC2_TX_ARES] = { 0x12c, 2 },
	[NSSCC_MAC2_GEPHY1_TX_ARES] = { 0x130, 2 },
	[NSSCC_MAC2_SRDS1_CH1_RX_ARES] = { 0x128, 2 },
	[NSSCC_MAC2_SRDS1_CH1_XGMII_RX_ARES] = { 0x134, 2 },
	[NSSCC_MAC2_RX_ARES] = { 0x154, 2 },
	[NSSCC_MAC2_GEPHY1_RX_ARES] = { 0x158, 2 },
	[NSSCC_MAC2_SRDS1_CH1_TX_ARES] = { 0x150, 2 },
	[NSSCC_MAC2_SRDS1_CH1_XGMII_TX_ARES] = { 0x15c, 2 },
	[NSSCC_MAC3_TX_ARES] = { 0x180, 2 },
	[NSSCC_MAC3_GEPHY2_TX_ARES] = { 0x184, 2 },
	[NSSCC_MAC3_SRDS1_CH2_RX_ARES] = { 0x17c, 2 },
	[NSSCC_MAC3_SRDS1_CH2_XGMII_RX_ARES] = { 0x188, 2 },
	[NSSCC_MAC3_RX_ARES] = { 0x1ac, 2 },
	[NSSCC_MAC3_GEPHY2_RX_ARES] = { 0x1b0, 2 },
	[NSSCC_MAC3_SRDS1_CH2_TX_ARES] = { 0x1a8, 2 },
	[NSSCC_MAC3_SRDS1_CH2_XGMII_TX_ARES] = { 0x1b4, 2 },
	[NSSCC_MAC4_TX_ARES] = { 0x1d4, 2 },
	[NSSCC_MAC4_GEPHY3_TX_ARES] = { 0x1d8, 2 },
	[NSSCC_MAC4_SRDS1_CH3_RX_ARES] = { 0x1d0, 2 },
	[NSSCC_MAC4_SRDS1_CH3_XGMII_RX_ARES] = { 0x1dc, 2 },
	[NSSCC_MAC4_RX_ARES] = { 0x200, 2 },
	[NSSCC_MAC4_GEPHY3_RX_ARES] = { 0x204, 2 },
	[NSSCC_MAC4_SRDS1_CH3_TX_ARES] = { 0x1fc, 2 },
	[NSSCC_MAC4_SRDS1_CH3_XGMII_TX_ARES] = { 0x208, 2 },
	[NSSCC_MAC5_TX_ARES] = { 0x220, 2 },
	[NSSCC_MAC5_TX_SRDS0_ARES] = { 0x224, 2 },
	[NSSCC_MAC5_TX_SRDS0_CH0_XGMII_ARES] = { 0x228, 2 },
	[NSSCC_MAC5_RX_ARES] = { 0x240, 2 },
	[NSSCC_MAC5_RX_SRDS0_ARES] = { 0x244, 2 },
	[NSSCC_MAC5_RX_SRDS0_CH0_XGMII_ARES] = { 0x248, 2 },
	[NSSCC_AHB_ARES] = { 0x258, 2 },
	[NSSCC_SEC_CTRL_AHB_ARES] = { 0x25c, 2 },
	[NSSCC_TLMM_ARES] = { 0x260, 2 },
	[NSSCC_TLMM_AHB_ARES] = { 0x264, 2 },
	[NSSCC_CNOC_AHB_ARES] = { 0x268, 2 },
	[NSSCC_MDIO_AHB_ARES] = { 0x26c, 2 },
	[NSSCC_MDIO_MASTER_AHB_ARES] = { 0x270, 2 },
	[NSSCC_TSENS_AHB_ARES] = { 0x274, 2 },
	[NSSCC_SRDS0_SYS_ARES] = { 0x280, 2 },
	[NSSCC_SRDS1_SYS_ARES] = { 0x284, 2 },
	[NSSCC_GEPHY0_SYS_ARES] = { 0x288, 2 },
	[NSSCC_GEPHY1_SYS_ARES] = { 0x28c, 2 },
	[NSSCC_GEPHY2_SYS_ARES] = { 0x290, 2 },
	[NSSCC_GEPHY3_SYS_ARES] = { 0x294, 2 },
	[NSSCC_SEC_CTRL_ARES] = { 0x2b4, 2 },
	[NSSCC_SEC_CTRL_SENSE_ARES] = { 0x2b8, 2 },
	[NSSCC_SLEEP_ARES] = { 0x2cc, 2 },
	[NSSCC_TS_SLEEP_ARES] = { 0x2d0, 2 },
	[NSSCC_DEBUG_ARES] = { 0x2e0, 2 },
};

static const struct qcom_cc_desc nsscc_qce2204_desc = {
	.config = &nsscc_qce2204_regmap_config,
	.clks = nsscc_qce2204_clocks,
	.num_clks = ARRAY_SIZE(nsscc_qce2204_clocks),
	.resets = nsscc_qce2204_resets,
	.num_resets = ARRAY_SIZE(nsscc_qce2204_resets),
};

static int nsscc_qce2204_probe(struct mdio_device *mdiodev)
{
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init(&mdiodev->dev, NULL, mdiodev, &nsscc_qce2204_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&mdiodev->dev, PTR_ERR(regmap), "Failed to init regmap\n");

	ret = qcom_cc_really_probe(&mdiodev->dev, &nsscc_qce2204_desc, regmap);
	if (ret)
		return dev_err_probe(&mdiodev->dev, ret, "Failed to register clocks\n");

	dev_info(&mdiodev->dev, "QCE2204 NSSCC registered\n");
	return 0;
}

static const struct of_device_id nsscc_qce2204_match_table[] = {
	{ .compatible = "qcom,qce2204-nsscc" },
	{ }
};
MODULE_DEVICE_TABLE(of, nsscc_qce2204_match_table);

static struct mdio_driver nsscc_qce2204_driver = {
	.mdiodrv.driver = {
		.name = "qcom,qce2204-nsscc",
		.of_match_table = nsscc_qce2204_match_table,
	},
	.probe = nsscc_qce2204_probe,
};

mdio_module_driver(nsscc_qce2204_driver);

MODULE_DESCRIPTION("QCE2204 Network SubSystem Clock Controller");
MODULE_LICENSE("GPL v2");
