/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __QCE2204_H
#define __QCE2204_H

#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/phylink.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>
#include <net/dsa.h>

#define QCE2204_ATHTAG_TYPE				0xaaaa
#define QCE2204_PPE_RSTP_ACL				0	/* RSTP reserved ACL index */
#define QCE2204_PPE_RSTP_VP					64	/* RSTP reserved virtual port which starts from 64 */
#define QCE2204_RSTP_ATHTAG_TYPE			0xfefe	/* RSTP Atheros header type */
#define QCE2204_NUM_PORTS				6
#define QCE2204_NUM_CPU_PORTS				2
#define QCE2204_MDIO_PPE_REG_BASE_OFFSET	0x07000000

#define PHY_ID_QCE2204					0x004dd190
#define QCE2204_CHIP_ID					0x50

/* Default VSI for none tag mode */
#define QCE2204_DEFAULT_VSI				0

/* VLAN translation index allocation */
#define QCE2204_VLAN_XLT_BASE_USER_PORT		0
#define QCE2204_VLAN_XLT_BASE_CPU_PORT		16

/* Maximum frame size from device tree */
#define QCE2204_MAX_FRAME_SIZE				9728

/* Number of clocks per port */
#define QCE2204_NUM_PORT_CLOCKS				2
#define QCE2204_NUM_PORT_RESETS				2

/* AHB clk rate for QCE2204 */
#define QCE2204_AHB_CLK_RATE_104M			104000000

/* AHB clock name */
#define QCE2204_CLK_AHB				"ahb"

/* Switch-level clock names */
#define QCE2204_CLK_CORE				"core"
#define QCE2204_CLK_IPE					"ipe"
#define QCE2204_CLK_BTQ					"btq"
#define QCE2204_CLK_CFG					"cfg"
#define QCE2204_CLK_APB					"apb"
#define QCE2204_NUM_SWITCH_CLOCKS			5

/* Per-port clock names */
#define QCE2204_PORT_CLK_TX				"tx"
#define QCE2204_PORT_CLK_RX				"rx"

/* Reset names */
#define QCE2204_RESET_CORE				"core"
#define QCE2204_RESET_CORE_CLK				"core_clk_reset"
#define QCE2204_RESET_CFG_CLK				"cfg_clk_reset"
#define QCE2204_PORT_RESET_TX				"tx"
#define QCE2204_PORT_RESET_RX				"rx"

/* QCE2204 port MAC types */
enum qce2204_port_mac_type {
	QCE2204_PORT_MAC_TYPE_GMAC = 0,
	QCE2204_PORT_MAC_TYPE_XGMAC = 1,
};

/**
 * enum qce2204_bp_mode - Cross-chip backpressure mode
 * @QCE2204_BP_QUEUE: Queue-based backpressure (default)
 * @QCE2204_BP_EDMA: EDMA-based backpressure (for IPQ5424/IPQ5332)
 */
enum qce2204_bp_mode {
	QCE2204_BP_QUEUE = 0,
	QCE2204_BP_EDMA  = 1,
};

/* QCE2204 Hardware Module Base Addresses */
#define QCE2204_NSS_SW_GLB_REG_BASE			0x07000000
#define QCE2204_NSS_GMAC0_BASE				0x07001000
#define QCE2204_NSS_GMAC1_BASE				0x07001200
#define QCE2204_NSS_GMAC2_BASE				0x07001400
#define QCE2204_NSS_GMAC3_BASE				0x07001600
#define QCE2204_NSS_GMAC4_BASE				0x07001800
#define QCE2204_NSS_GMAC5_BASE				0x07001A00
#define QCE2204_NSS_PRX_CSR_BASE			0x0700B000
#define QCE2204_NSS_IPE_IV_REG_BASE			0x0700F000
#define QCE2204_NSS_IPO_CSR_BASE			0x070B0000
#define QCE2204_NSS_IPR_CSR_BASE			0x071E0000
#define QCE2204_NSS_IPE_L3_CSR_BASE			0x07200828
#define QCE2204_NSS_IPE_TL_REG_BASE			0x07300800
#define QCE2204_NSS_TM_REG_BASE				0x07400000
#define QCE2204_NSS_XGMAC0_BASE				0x07500000
#define QCE2204_NSS_XGMAC1_BASE				0x07504000
#define QCE2204_NSS_IPE_L2_CSR_BASE			0x07540000
#define QCE2204_NSS_PTX_CSR_BASE			0x07600000
#define QCE2204_NSS_IPE_PC_REG_BASE			0x07700000
#define QCE2204_NSS_BM_CSR_BASE				0x07800000
#define QCE2204_NSS_QM_REG_BASE				0x07A00000

/* QCE2204 Hardware Module Sizes */
#define QCE2204_NSS_SW_GLB_REG_SIZE			0x1000
#define QCE2204_NSS_GMAC_SIZE				0x200
#define QCE2204_NSS_PRX_CSR_SIZE			0x1000
#define QCE2204_NSS_IPE_IV_REG_SIZE			0x1000
#define QCE2204_NSS_IPO_CSR_SIZE			0x10000
#define QCE2204_NSS_IPR_CSR_SIZE			0x10000
#define QCE2204_NSS_IPE_L3_CSR_SIZE			0x1000
#define QCE2204_NSS_IPE_TL_REG_SIZE			0x1000
#define QCE2204_NSS_TM_REG_SIZE				0x10000
#define QCE2204_NSS_XGMAC_SIZE				0x4000
#define QCE2204_NSS_IPE_L2_CSR_SIZE			0x10000
#define QCE2204_NSS_PTX_CSR_SIZE			0x10000
#define QCE2204_NSS_IPE_PC_REG_SIZE			0x10000
#define QCE2204_NSS_BM_CSR_SIZE				0x10000
#define QCE2204_NSS_QM_REG_SIZE				0x20000

/**
 * struct qce2204_port_clk - Per-port clock resources
 * @tx_clk: TX clock
 * @rx_clk: RX clock
 */
struct qce2204_port_clk {
	struct clk *tx_clk;
	struct clk *rx_clk;
};

/**
 * struct qce2204_port_reset - Per-port reset resources
 * @tx_reset: TX reset controller
 * @rx_reset: RX reset controller
 */
struct qce2204_port_reset {
	struct reset_control *tx_reset;
	struct reset_control *rx_reset;
};

/**
 * struct qce2204_port_ppe_res - Per-port PPE resources for DSA 8021Q tagging
 * @in_vlan_xlt_idx: Ingress VLAN translation table index (for user port RX)
 * @cpu_in_vlan_xlt_idx: CPU port VLAN translation index (for CPU port TX to this user port)
 * @standalone_vid: DSA 8021Q standalone VID for this port
 * @allocated: Resource allocation status
 */
struct qce2204_port_ppe_res {
	u32 in_vlan_xlt_idx;
	u32 cpu_in_vlan_xlt_idx;
	u16 standalone_vid;
	bool allocated;
};

/**
 * struct qce2204_ppe_port - Per-port PPE configuration and statistics
 * @port: Port number
 * @priv: Pointer to QCE2204 private data
 * @mac_type: MAC type (GMAC or XGMAC) for this port
 * @gmib_read: Delayed work for GMIB statistics polling
 * @gmib_stats: GMIB statistics array (for GMAC ports)
 * @gmib_stats_lock: Mutex to protect GMIB statistics
 */
struct qce2204_ppe_port {
	int port;
	struct qce2204_priv *priv;
	enum qce2204_port_mac_type mac_type;
	struct delayed_work gmib_read;
	u64 *gmib_stats;
	struct mutex gmib_stats_lock;
};

/**
 * struct qce2204_priv - QCE2204 switch private data
 * @ds: DSA switch structure
 * @dev: Device pointer
 * @regmap: Register map for switch access
 * @bus: MDIO bus
 * @addr: MDIO address
 * @pcs: Array of PCS handles from independent PCS driver
 * @reg_mutex: Mutex for register access protection
 * @port_enabled_map: Bitmap of enabled ports
 * @cpu_port: CPU port number
 * @switch_id: Switch chip ID
 * @switch_revision: Switch chip revision
 * @reset_gpio: GPIO for hardware reset
 * @ahb_clk: AHB clock
 * @core_clk: Switch core clock
 * @ipe_clk: Switch IPE clock
 * @btq_clk: Switch BTQ clock
 * @cfg_clk: Switch CFG clock
 * @apb_clk: APB bridge clock
 * @core_reset: Switch core BCR reset controller (GCC_SWITCH_CORE_BCR)
 * @core_clk_reset: Switch core clock reset (GCC_SWITCH_CORE_ARES)
 * @cfg_clk_reset: Switch cfg clock reset (GCC_SWITCH_CFG_ARES)
 * @port_clks: Per-port clock resources
 * @port_resets: Per-port reset resources
 * @ppe_port: Per-port PPE configuration and statistics
 * @pcs: Array of PCS handles from independent PCS driver
 * @ppe_offload: PPE hardware offload enabled
 * @max_frame_size: Maximum frame size from device tree
 * @reg_base_offset: Base offset for register addresses
 */
struct qce2204_priv {
	struct dsa_switch *ds;
	struct device *dev;
	struct regmap *regmap;
	struct mii_bus *bus;
	int addr;
	struct mutex reg_mutex;
	u32 port_enabled_map;
	u8 cpu_port;
	u32 switch_id;
	u8 switch_revision;

	/* Reset resources */
	struct gpio_desc *reset_gpio;
	struct reset_control *core_reset;		/* GCC_SWITCH_CORE_BCR */
	struct reset_control *core_clk_reset;		/* GCC_SWITCH_CORE_ARES */
	struct reset_control *cfg_clk_reset;		/* GCC_SWITCH_CFG_ARES */

	/* AHB clock */
	struct clk *ahb_clk;

	/* Switch-level clocks */
	struct clk *core_clk;
	struct clk *ipe_clk;
	struct clk *btq_clk;
	struct clk *cfg_clk;
	struct clk *apb_clk;

	/* Per-port resources */
	struct qce2204_port_clk port_clks[QCE2204_NUM_PORTS];
	struct qce2204_port_reset port_resets[QCE2204_NUM_PORTS];
	struct qce2204_port_ppe_res port_ppe_res[QCE2204_NUM_PORTS];

	/* Per-port PPE configuration and statistics */
	struct qce2204_ppe_port ppe_port[QCE2204_NUM_PORTS];

	/* PCS instances for CPU ports */
	struct phylink_pcs *pcs[QCE2204_NUM_CPU_PORTS];

	/* Device tree properties */
	bool ppe_offload;
	u32 max_frame_size;

	/* Register base offset */
	u32 reg_base_offset;

	/* DSA tag protocol */
	enum dsa_tag_protocol tag_protocol;

	/* Cross-chip backpressure mode */
	enum qce2204_bp_mode bp_mode;
};

/* Clock/Reset management functions */
int qce2204_init_switch_clocks_resets(struct qce2204_priv *priv);
void qce2204_cleanup_switch_clocks_resets(struct qce2204_priv *priv);
int qce2204_init_port_clocks_resets(struct qce2204_priv *priv);
void qce2204_cleanup_port_clocks_resets(struct qce2204_priv *priv);
int qce2204_ahb_clk_set_rate(struct qce2204_priv *priv, unsigned long rate);

/* Internal clock and reset management functions (used by qce2204_clk.c) */
int qce2204_get_clocks(struct qce2204_priv *priv);
int qce2204_enable_clocks(struct qce2204_priv *priv);
void qce2204_disable_clocks(struct qce2204_priv *priv);
int qce2204_get_port_clocks(struct qce2204_priv *priv, int port,
			    struct device_node *port_np);
int qce2204_enable_port_clocks(struct qce2204_priv *priv, int port);
void qce2204_disable_port_clocks(struct qce2204_priv *priv, int port);
int qce2204_get_resets(struct qce2204_priv *priv);
int qce2204_get_port_resets(struct qce2204_priv *priv, int port,
			    struct device_node *port_np);
int qce2204_reset_switch(struct qce2204_priv *priv);
int qce2204_reset_port(struct qce2204_priv *priv, int port);

/* QCE2204 register access functions */
int qce2204_mdio_read(struct mii_bus *bus, int addr, u32 reg, u32 *val);
int qce2204_mdio_write(struct mii_bus *bus, int addr, u32 reg, u32 val);

/* Port management functions */
void qce2204_port_set_status(struct qce2204_priv *priv, int port, int enable);
int qce2204_port_mac_init(struct qce2204_priv *priv);

/* PCS functions */
struct phylink_pcs *pcs_qce2204_create(struct device *dev, void __iomem *base, int port);
void pcs_qce2204_destroy(struct phylink_pcs *pcs);

#endif /* __QCE2204_H */
