/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/module.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phylink.h>
#include <linux/mdio.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>
#include <net/dsa.h>
#include <linux/dsa/8021q.h>

#include "qce2204.h"
#include "qce2204_ppe.h"
#include "qce2204_ppe_regs.h"
#include "qce2204_ppe_mac.h"

/**
 * qce2204_port_alloc_ppe_resources - Allocate PPE resources for a port
 * @priv: QCE2204 private data
 * @port: Port number
 *
 * Allocates VLAN translation index and calculates standalone VID for the port.
 * For user ports (1-4): allocates both user port RX index and CPU port TX index
 * CPU port doesn't need resource allocation (uses per-user-port cpu_in_vlan_xlt_idx)
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_port_alloc_ppe_resources(struct qce2204_priv *priv, int port)
{
	struct qce2204_port_ppe_res *res;

	if (port >= QCE2204_NUM_PORTS)
		return -EINVAL;

	/* CPU port doesn't need resource allocation */
	if (port == QCE2204_CPU_PORT_ID) {
		dev_dbg(priv->dev, "CPU port doesn't need PPE resource allocation\n");
		return 0;
	}

	res = &priv->port_ppe_res[port];

	/* Skip if already allocated */
	if (res->allocated) {
		dev_dbg(priv->dev, "Port %d PPE resources already allocated\n", port);
		return 0;
	}

	/* Allocate user port RX VLAN translation index */
	res->in_vlan_xlt_idx = QCE2204_VLAN_XLT_BASE_USER_PORT + ((port - 1) * 2);

	/* Allocate CPU port TX VLAN translation index for this user port */
	res->cpu_in_vlan_xlt_idx = QCE2204_VLAN_XLT_BASE_CPU_PORT + (port - 1);

	/* Calculate standalone VID for this port */
	res->standalone_vid = dsa_tag_8021q_standalone_vid(dsa_to_port(priv->ds, port));

	res->allocated = true;

	dev_info(priv->dev, "Port %d: allocated user RX XLT index %d, CPU TX XLT index %d, standalone VID 0x%x\n",
		 port, res->in_vlan_xlt_idx, res->cpu_in_vlan_xlt_idx, res->standalone_vid);

	return 0;
}

/**
 * qce2204_port_free_ppe_resources - Free PPE resources for a port
 * @priv: QCE2204 private data
 * @port: Port number
 */
void qce2204_port_free_ppe_resources(struct qce2204_priv *priv, int port)
{
	struct qce2204_port_ppe_res *res;

	if (port >= QCE2204_NUM_PORTS)
		return;

	res = &priv->port_ppe_res[port];

	if (!res->allocated)
		return;

	dev_info(priv->dev, "Port %d: freeing VLAN XLT index %d\n",
		 port, res->in_vlan_xlt_idx);

	res->in_vlan_xlt_idx = 0;
	res->standalone_vid = 0;
	res->allocated = false;
}


/* MDIO register access */
static inline void qce2204_addr_split(u32 regaddr, u16 *reg_low, u16 *reg_mid, u16 *reg_high)
{
	*reg_low = FIELD_GET(GENMASK(3, 0), regaddr);
	*reg_low &= 0xc;
	*reg_low <<= 1;
	*reg_mid = FIELD_GET(GENMASK(19, 4), regaddr);
	*reg_high = FIELD_GET(GENMASK(23, 20), regaddr);
	*reg_high <<= 1;
	*reg_high |= BIT(0);
}

static int qce2204_mdio_read(struct mii_bus *bus, int addr, u32 reg, u32 *val)
{
	u16 reg_low, reg_mid, reg_high;
	int ret, data;

	qce2204_addr_split(reg, &reg_low, &reg_mid, &reg_high);

	mutex_lock(&bus->mdio_lock);

	ret = __mdiobus_write(bus, addr, reg_high & 0x1f, reg_mid);
	if (ret)
		return ret;

	usleep_range(100, 200);
	ret = __mdiobus_read(bus, addr, reg_low);
	if (ret >= 0) {
		data = ret;
		ret = __mdiobus_read(bus, addr, (reg_low | BIT(2)));
		if (ret >= 0)
			*val = data | ret << 16;
	}

	mutex_unlock(&bus->mdio_lock);

	return ret < 0 ? ret : 0;
}

static int qce2204_mdio_write(struct mii_bus *bus, int addr, u32 reg, u32 val)
{
	u16 reg_low, reg_mid, reg_high;
	int ret;

	qce2204_addr_split(reg, &reg_low, &reg_mid, &reg_high);

	mutex_lock(&bus->mdio_lock);

	ret = __mdiobus_write(bus, addr, reg_high & 0x1f, reg_mid);
	if (ret)
		return ret;

	usleep_range(100, 200);
	ret = __mdiobus_write(bus, addr, reg_low, lower_16_bits(val));
	if (!ret)
		ret = __mdiobus_write(bus, addr, (reg_low | BIT(2)), upper_16_bits(val));

	mutex_unlock(&bus->mdio_lock);

	return ret;
}

/* Regmap bus implementation */
static int qce2204_regmap_read(void *context, unsigned int reg, unsigned int *val)
{
	struct qce2204_priv *priv = context;
	int ret;

	if (!priv->bus || !priv->addr)
		return -ENODEV;

	/* Add base offset firstly */
	reg += priv->reg_base_offset;

	ret = qce2204_mdio_read(priv->bus, priv->addr, reg, val);
	if (ret)
		dev_err(priv->dev, "Register read failed: reg=0x%x, ret=%d\n", reg, ret);

	return ret;
}

static int qce2204_regmap_write(void *context, unsigned int reg, unsigned int val)
{
	struct qce2204_priv *priv = context;
	int ret;

	if (!priv->bus || !priv->addr)
		return -ENODEV;

	/* Add base offset firstly */
	reg += priv->reg_base_offset;

	ret = qce2204_mdio_write(priv->bus, priv->addr, reg, val);
	if (ret)
		dev_err(priv->dev, "Register write failed: reg=0x%x, val=0x%x, ret=%d\n", reg, val, ret);

	return ret;
}

static const struct regmap_bus qce2204_regmap_bus = {
	.reg_write = qce2204_regmap_write,
	.reg_read = qce2204_regmap_read,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

/* Port management - placeholder */
void qce2204_port_set_status(struct qce2204_priv *priv, int port, int enable)
{
	dev_dbg(priv->dev, "Port %d: %s\n", port, enable ? "enabled" : "disabled");
}

/* DSA operations - placeholder implementations */
static int qce2204_setup(struct dsa_switch *ds)
{
	struct qce2204_priv *priv = ds->priv;
	struct dsa_port *dp;
	int ret;

	dev_info(priv->dev, "Setting up QCE2204 switch\n");

	ret = qce2204_ppe_hw_init(priv);
	if (ret) {
		dev_err(priv->dev, "PPE init failed: %d\n", ret);
		return ret;
	}

	ret = qce2204_port_mac_init(priv);
	if (ret) {
		dev_err(priv->dev, "Port MAC init failed: %d\n", ret);
		return ret;
	}

	dsa_switch_for_each_user_port(dp, ds) {
		qce2204_port_set_status(priv, dp->index, 0);

		/* Allocate PPE resources for each user port
		 * This also allocates CPU port TX index for this user port
		 */
		ret = qce2204_port_alloc_ppe_resources(priv, dp->index);
		if (ret) {
			dev_err(priv->dev, "Failed to allocate PPE resources for port %d: %d\n",
				dp->index, ret);
			return ret;
		}
	}

	dsa_switch_for_each_cpu_port(dp, ds) {
		priv->cpu_port = dp->index;
		dev_info(priv->dev, "CPU port: %d\n", dp->index);
	}

	dev_info(priv->dev, "Setup completed.\n");
	return 0;
}

static enum dsa_tag_protocol qce2204_get_tag_protocol(struct dsa_switch *ds, int port,
							       enum dsa_tag_protocol mp)
{
	struct qce2204_priv *priv = ds->priv;

	/* Initialize tag protocol to default */
	priv->tag_protocol = DSA_TAG_PROTO_NONE;

	return DSA_TAG_PROTO_NONE;
}

static int qce2204_change_tag_protocol(struct dsa_switch *ds, enum dsa_tag_protocol proto)
{
	struct qce2204_priv *priv = ds->priv;
	struct dsa_port *dp;
	int ret;

	/* Check if protocol is supported */
	switch (proto) {
	case DSA_TAG_PROTO_NONE:
	case DSA_TAG_PROTO_QCA:
	case DSA_TAG_PROTO_QCA_8021Q:
	case DSA_TAG_PROTO_4B_QCA:
		break;
	default:
		dev_err(priv->dev, "Unsupported tag protocol: %d\n", proto);
		return -EOPNOTSUPP;
	}

	/* If already using this protocol, nothing to do */
	if (priv->tag_protocol == proto) {
		dev_dbg(priv->dev, "Already using tag protocol %d\n", proto);
		return 0;
	}

	dev_info(priv->dev, "Changing tag protocol from %d to %d\n", priv->tag_protocol, proto);

	/* Teardown old protocol configuration */
	if (priv->tag_protocol == DSA_TAG_PROTO_QCA_8021Q) {
		qce2204_teardown_8021q_global(priv);
		dsa_switch_for_each_user_port(dp, ds) {
			ret = qce2204_teardown_8021q_tagging(priv, dp->index);
			if (ret) {
				dev_err(priv->dev, "Failed to teardown 8021Q tagging for port %d: %d\n",
					dp->index, ret);
				return ret;
			}
		}
	} else if (priv->tag_protocol == DSA_TAG_PROTO_4B_QCA) {
		/* Restore ATH tag configuration if needed */
		ret = qce2204_teardown_cpu_port_athtag(priv);
		if (ret) {
			dev_err(priv->dev, "Failed to restore ATH tag: %d\n", ret);
			return ret;
		}
	} else if (priv->tag_protocol == DSA_TAG_PROTO_NONE) {
		/* Teardown none tag VSI configuration */
		ret = qce2204_teardown_none_tag_vsi(priv);
		if (ret) {
			dev_err(priv->dev, "Failed to teardown none tag VSI: %d\n", ret);
			return ret;
		}
	}

	/* Setup new protocol configuration */
	if (proto == DSA_TAG_PROTO_QCA_8021Q) {
		qce2204_setup_8021q_global(priv);
		dsa_switch_for_each_user_port(dp, ds) {
			ret = qce2204_setup_8021q_tagging(priv, dp->index);
			if (ret) {
				dev_err(priv->dev, "Failed to setup 8021Q tagging for port %d: %d\n",
					dp->index, ret);
				return ret;
			}
		}
	} else if (proto == DSA_TAG_PROTO_4B_QCA) {
		/* Configure ATH tag */
		ret = qce2204_setup_cpu_port_athtag(priv);
		if (ret) {
			dev_err(priv->dev, "Failed to config ATH tag: %d\n", ret);
			return ret;
		}
	} else if (proto == DSA_TAG_PROTO_NONE) {
		/* Setup none tag VSI configuration */
		ret = qce2204_setup_none_tag_vsi(priv);
		if (ret) {
			dev_err(priv->dev, "Failed to setup none tag VSI: %d\n", ret);
			return ret;
		}
	}

	priv->tag_protocol = proto;
	dev_info(priv->dev, "Tag protocol changed to %d successfully\n", proto);
	return 0;
}

static int qce2204_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy)
{
	struct qce2204_priv *priv = ds->priv;
	qce2204_port_set_status(priv, port, 1);
	priv->port_enabled_map |= BIT(port);
	return 0;
}

static void qce2204_port_disable(struct dsa_switch *ds, int port)
{
	struct qce2204_priv *priv = ds->priv;
	qce2204_port_set_status(priv, port, 0);
	priv->port_enabled_map &= ~BIT(port);
}

static int qce2204_port_vlan_filtering(struct dsa_switch *ds, int port,
					       bool vlan_filtering,
					       struct netlink_ext_ack *extack)
{
	return 0;
}

static int qce2204_port_vlan_add(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_port_vlan *vlan,
				 struct netlink_ext_ack *extack)
{
	return 0;
}

static int qce2204_port_vlan_del(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_port_vlan *vlan)
{
	return 0;
}

static int qce2204_port_fdb_add(struct dsa_switch *ds, int port,
				const unsigned char *addr, u16 vid,
				struct dsa_db db)
{
	return 0;
}

static int qce2204_port_fdb_del(struct dsa_switch *ds, int port,
				const unsigned char *addr, u16 vid,
				struct dsa_db db)
{
	return 0;
}

static int qce2204_port_fdb_dump(struct dsa_switch *ds, int port,
				 dsa_fdb_dump_cb_t *cb, void *data)
{
	return 0;
}

static int qce2204_port_bridge_join(struct dsa_switch *ds, int port,
				    struct dsa_bridge bridge,
				    bool *tx_fwd_offload,
				    struct netlink_ext_ack *extack)
{
	return 0;
}

static void qce2204_port_bridge_leave(struct dsa_switch *ds, int port,
				      struct dsa_bridge bridge)
{
}

static void qce2204_port_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct qce2204_priv *priv = ds->priv;
	struct qce2204_ppe_stp_state_cfg stp_cfg;
	int ret;

	/* Skip CPU port */
	if (dsa_is_cpu_port(ds, port))
		return;

	/* Configure MTU with drop action for oversized packets */
	stp_cfg.stp_state = state;

	ret = qce2204_ppe_stp_state_set(priv, port, &stp_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set STP for port %d: %d\n", port, ret);
		return;
	}

	dev_dbg(priv->dev, "Port %d STP set to %d\n", port, state);
	return;
}

static void qce2204_port_fast_age(struct dsa_switch *ds, int port)
{
}

static int qce2204_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct qce2204_priv *priv = ds->priv;
	struct qce2204_ppe_port_mtu_cfg mtu_cfg;
	struct qce2204_ppe_port_mru_cfg mru_cfg;
	struct dsa_port *cpu_dp;
	int ret, mtu_val;

	/* Skip CPU port */
	if (dsa_is_cpu_port(ds, port))
		return 0;

	/* Align with PPE port's MTU, dsa_slave_change_mtu/ppe_drv_port_mtu_mru_set */
	cpu_dp = dsa_to_port(ds, QCE2204_CPU_PORT_ID);
	mtu_val = new_mtu + ETH_HLEN +
				cpu_dp->tag_ops->needed_headroom +
				cpu_dp->tag_ops->needed_tailroom;

	/* Configure MTU with rdt cpu action for oversized packets */
	mtu_cfg.mtu = mtu_val;
	mtu_cfg.mtu_cmd = QCE2204_PPE_ACTION_REDIRECT_TO_CPU;

	ret = qce2204_ppe_port_mtu_set(priv, port, &mtu_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set MTU for port %d: %d\n", port, ret);
		return ret;
	}

	/* Configure MRU with rdt cpu action for oversized packets */
	mru_cfg.mru = mtu_val;
	mru_cfg.mru_cmd = QCE2204_PPE_ACTION_REDIRECT_TO_CPU;

	ret = qce2204_ppe_port_mru_set(priv, port, &mru_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set MRU for port %d: %d\n", port, ret);
		return ret;
	}

	dev_dbg(priv->dev, "Port %d MTU/MRU set to %d\n", port, mtu_val);
	return 0;
}

static int qce2204_port_max_mtu(struct dsa_switch *ds, int port)
{
	return QCE2204_MAX_FRAME_SIZE - ETH_HLEN - VLAN_HLEN - ETH_FCS_LEN;
}

static int qce2204_set_ageing_time(struct dsa_switch *ds, unsigned int ageing_time)
{
	return 0;
}

static int qce2204_set_mac_eee(struct dsa_switch *ds, int port, struct ethtool_eee *e)
{
	return 0;
}

static const struct dsa_switch_ops qce2204_switch_ops = {
	.phylink_get_caps	= qce2204_phylink_get_caps,
	.phylink_mac_select_pcs	= qce2204_phylink_mac_select_pcs,
	.phylink_mac_config	= qce2204_phylink_mac_config,
	.phylink_mac_link_down	= qce2204_phylink_mac_link_down,
	.phylink_mac_link_up	= qce2204_phylink_mac_link_up,
	.get_tag_protocol	= qce2204_get_tag_protocol,
	.change_tag_protocol	= qce2204_change_tag_protocol,
	.setup			= qce2204_setup,
	.port_enable		= qce2204_port_enable,
	.port_disable		= qce2204_port_disable,
	.port_vlan_filtering	= qce2204_port_vlan_filtering,
	.port_vlan_add		= qce2204_port_vlan_add,
	.port_vlan_del		= qce2204_port_vlan_del,
	.port_fdb_add		= qce2204_port_fdb_add,
	.port_fdb_del		= qce2204_port_fdb_del,
	.port_fdb_dump		= qce2204_port_fdb_dump,
	.port_bridge_join	= qce2204_port_bridge_join,
	.port_bridge_leave	= qce2204_port_bridge_leave,
	.port_stp_state_set	= qce2204_port_stp_state_set,
	.port_fast_age		= qce2204_port_fast_age,
	.get_strings		= qce2204_get_strings,
	.get_ethtool_stats	= qce2204_get_ethtool_stats,
	.get_sset_count		= qce2204_get_sset_count,
	.port_change_mtu	= qce2204_port_change_mtu,
	.port_max_mtu		= qce2204_port_max_mtu,
	.set_ageing_time	= qce2204_set_ageing_time,
	.set_mac_eee		= qce2204_set_mac_eee,
};

/* Regmap configuration */

static const struct regmap_range qce2204_readable_ranges[] = {
	/* All registers use relative addresses, offset added in regmap_read/write */

	/* NSS_SW_GLB_REG - Global control and chip ID */
	regmap_reg_range(0x00000000, 0x00000FFF),

	/* GMAC0-5 - Gigabit MAC modules */
	regmap_reg_range(0x00001000, 0x00001200), /* GMAC0 */
	regmap_reg_range(0x00001200, 0x00001400), /* GMAC1 */
	regmap_reg_range(0x00001400, 0x00001600), /* GMAC2 */
	regmap_reg_range(0x00001600, 0x00001800), /* GMAC3 */
	regmap_reg_range(0x00001800, 0x00001A00), /* GMAC4 */
	regmap_reg_range(0x00001A00, 0x00001C00), /* GMAC5 */

	/* PRX_CSR - Packet RX processing */
	regmap_reg_range(0x0000B000, 0x0000E960),

	/* IPE_IV_REG - IV processing */
	regmap_reg_range(0x0000F000, 0x0001A000),

	/* IPO_CSR - IPO processing */
	regmap_reg_range(0x000B0000, 0x000BFFFF),

	/* IPR_CSR - IPR processing */
	regmap_reg_range(0x001E0000, 0x001EFFFF),

	/* IPE_L3_CSR - L3 packet processing */
	regmap_reg_range(0x00200828, 0x00206000),

	/* TM_REG - Traffic manager */
	regmap_reg_range(0x00400000, 0x0047A800),

	/* XGMAC0-1 - 10G MAC modules */
	regmap_reg_range(0x00500000, 0x00504000), /* XGMAC0 */
	regmap_reg_range(0x00504000, 0x00508000), /* XGMAC1 */

	/* IPE_L2_CSR - L2 packet processing */
	regmap_reg_range(0x00540000, 0x00581100),

	/* PTX_CSR - Packet TX processing */
	regmap_reg_range(0x00600000, 0x00629000),

	/* IPE_PC_REG - PC processing */
	regmap_reg_range(0x00700000, 0x0070FFFF),

	/* BM_CSR - Buffer manager */
	regmap_reg_range(0x00800000, 0x0080FFFF),

	/* QM_REG - Queue manager */
	regmap_reg_range(0x00A00000, 0x00BF8000),
};

static const struct regmap_access_table qce2204_readable_table = {
	.yes_ranges = qce2204_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(qce2204_readable_ranges),
};

static const struct regmap_config qce2204_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x00BF8000,  /* Relative address, offset added in regmap_read/write */
	.rd_table = &qce2204_readable_table,
	.wr_table = &qce2204_readable_table,
	.cache_type = REGCACHE_NONE,
	.fast_io = false,
};

static int qce2204_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct qce2204_priv *priv;
	struct dsa_switch *ds;
	u32 val;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->bus = mdiodev->bus;
	priv->addr = mdiodev->addr;
	mutex_init(&priv->reg_mutex);

	priv->reg_base_offset = QCE2204_MDIO_REG_BASE_OFFSET;

	dev_info(dev, "QCE2204 MDIO device at address %d\n", mdiodev->addr);

	ret = qce2204_ahb_clk_set_rate(priv, QCE2204_AHB_CLK_RATE_104M);
	if (ret) {
		dev_err(dev, "Failed to set ahb clock to 104Mhz: %d\n", ret);
		return ret;
	}

	ret = qce2204_init_switch_clocks_resets(priv);
	if (ret) {
		dev_err(dev, "Failed to init switch clocks/resets: %d\n", ret);
		return ret;
	}

	/* Initialize per-port clocks and resets */
	ret = qce2204_init_port_clocks_resets(priv);
	if (ret) {
		dev_err(dev, "Failed to init port clocks/resets: %d\n", ret);
		goto err_disable_clocks;
	}

	priv->regmap = devm_regmap_init(dev, &qce2204_regmap_bus, priv, &qce2204_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(dev, "Failed to init regmap: %d\n", ret);
		goto err_disable_clocks;
	}

	ret = regmap_read(priv->regmap, QCE2204_PPE_SWITCH_ID_ADDR, &val);
	if (ret) {
		dev_err(dev, "Failed to read chip ID from 0x%08x\n", QCE2204_PPE_SWITCH_ID_ADDR);
		goto err_disable_clocks;
	}

	priv->switch_id = QCE2204_PPE_SWITCH_ID_GET_DEVICE_ID(val);
	priv->switch_revision = QCE2204_PPE_SWITCH_ID_GET_REV_ID(val);

	if (priv->switch_id != QCE2204_CHIP_ID) {
		dev_err(dev, "Unsupported chip ID: 0x%02x\n", priv->switch_id);
		ret = -ENODEV;
		goto err_disable_clocks;
	}

	dev_info(dev, "QCE2204 detected, ID 0x%02x, rev 0x%02x\n",
		priv->switch_id, priv->switch_revision);

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds) {
		ret = -ENOMEM;
		goto err_disable_clocks;
	}

	ds->dev = dev;
	ds->num_ports = QCE2204_NUM_PORTS;
	ds->priv = priv;
	ds->ops = &qce2204_switch_ops;
	priv->ds = ds;

	dev_set_drvdata(dev, priv);

	ret = dsa_register_switch(ds);
	if (ret) {
		dev_err(dev, "Failed to register DSA switch: %d\n", ret);
		goto err_disable_clocks;
	}

	dev_info(dev, "QCE2204 registered successfully\n");
	return 0;

err_disable_clocks:
	qce2204_cleanup_port_clocks_resets(priv);
	qce2204_cleanup_switch_clocks_resets(priv);

	return ret;
}

static void qce2204_mdio_remove(struct mdio_device *mdiodev)
{
	struct qce2204_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (priv && priv->ds) {
		dsa_unregister_switch(priv->ds);
		qce2204_port_mac_deinit(priv);
	}

	qce2204_cleanup_port_clocks_resets(priv);
	qce2204_cleanup_switch_clocks_resets(priv);
}

static const struct of_device_id qce2204_of_match[] = {
	{ .compatible = "qcom,qce2204" },
	{ }
};
MODULE_DEVICE_TABLE(of, qce2204_of_match);

static struct mdio_driver qce2204_mdio_driver = {
	.probe = qce2204_mdio_probe,
	.remove = qce2204_mdio_remove,
	.mdiodrv.driver = {
		.name = "qce2204-switch",
		.of_match_table = qce2204_of_match,
	},
};

mdio_module_driver(qce2204_mdio_driver);

MODULE_AUTHOR("Qualcomm Innovation Center, Inc.");
MODULE_DESCRIPTION("Qualcomm QCE2204 Ethernet Switch Driver");
MODULE_LICENSE("GPL");
