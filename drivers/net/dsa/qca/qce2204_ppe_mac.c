/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/regmap.h>
#include <linux/phylink.h>
#include <linux/pcs/pcs-qce2204.h>
#include <net/dsa.h>

#include "qce2204.h"
#include "qce2204_ppe_regs.h"

/* QCE2204 port MAC max frame size which including 4bytes FCS */
#define QCE2204_PORT_MAC_MAX_FRAME_SIZE		0x3000

/* Poll interval to prevent GMAC 32-bit counter overflow at line rate (120 seconds) */
#define QCE2204_GMIB_POLL_INTERVAL_MS		120000

/* GMAC MIB statistics type */
enum qce2204_gmib_stats_type {
	gmib_rx_broadcast,
	gmib_rx_pause,
	gmib_rx_multicast,
	gmib_rx_fcserr,
	gmib_rx_alignerr,
	gmib_rx_runt,
	gmib_rx_frag,
	gmib_rx_jumbofcserr,
	gmib_rx_jumboalignerr,
	gmib_rx_pkt64,
	gmib_rx_pkt65to127,
	gmib_rx_pkt128to255,
	gmib_rx_pkt256to511,
	gmib_rx_pkt512to1023,
	gmib_rx_pkt1024to1518,
	gmib_rx_pkt1519tomax,
	gmib_rx_toolong,
	gmib_rx_bytes_g,
	gmib_rx_bytes_b,
	gmib_rx_unicast,
	gmib_tx_broadcast,
	gmib_tx_pause,
	gmib_tx_multicast,
	gmib_tx_underrun,
	gmib_tx_pkt64,
	gmib_tx_pkt65to127,
	gmib_tx_pkt128to255,
	gmib_tx_pkt256to511,
	gmib_tx_pkt512to1023,
	gmib_tx_pkt1024to1518,
	gmib_tx_pkt1519tomax,
	gmib_tx_bytes,
	gmib_tx_collisions,
	gmib_tx_abortcol,
	gmib_tx_multicol,
	gmib_tx_singlecol,
	gmib_tx_excdeffer,
	gmib_tx_deffer,
	gmib_tx_latecol,
	gmib_tx_unicast,
};

/* XGMAC MIB statistics type */
enum qce2204_xgmib_stats_type {
	xgmib_tx_bytes,
	xgmib_tx_frames,
	xgmib_tx_broadcast_g,
	xgmib_tx_multicast_g,
	xgmib_tx_pkt64,
	xgmib_tx_pkt65to127,
	xgmib_tx_pkt128to255,
	xgmib_tx_pkt256to511,
	xgmib_tx_pkt512to1023,
	xgmib_tx_pkt1024tomax,
	xgmib_tx_unicast,
	xgmib_tx_multicast,
	xgmib_tx_broadcast,
	xgmib_tx_underflow_err,
	xgmib_tx_bytes_g,
	xgmib_tx_frames_g,
	xgmib_tx_pause,
	xgmib_tx_vlan_g,
	xgmib_tx_lpi_usec,
	xgmib_tx_lpi_tran,
	xgmib_rx_frames,
	xgmib_rx_bytes,
	xgmib_rx_bytes_g,
	xgmib_rx_broadcast_g,
	xgmib_rx_multicast_g,
	xgmib_rx_crc_err,
	xgmib_rx_frag_err,
	xgmib_rx_jabber_err,
	xgmib_rx_undersize_g,
	xgmib_rx_oversize_g,
	xgmib_rx_pkt64,
	xgmib_rx_pkt65to127,
	xgmib_rx_pkt128to255,
	xgmib_rx_pkt256to511,
	xgmib_rx_pkt512to1023,
	xgmib_rx_pkt1024tomax,
	xgmib_rx_unicast_g,
	xgmib_rx_len_err,
	xgmib_rx_outofrange_err,
	xgmib_rx_pause,
	xgmib_rx_fifo_overflow,
	xgmib_rx_vlan,
	xgmib_rx_wdog_err,
	xgmib_rx_lpi_usec,
	xgmib_rx_lpi_tran,
	xgmib_rx_drop_frames,
	xgmib_rx_drop_bytes,
};

/* MIB descriptor macro */
#define QCE2204_MAC_MIB_DESC(_s, _o, _n)	\
	{					\
		.size = (_s),			\
		.offset = (_o),			\
		.name = (_n),			\
	}

/* MAC MIB description structure */
struct qce2204_mac_mib_info {
	u32 size;
	u32 offset;
	const char *name;
};

/* GMAC MIB statistics description */
static const struct qce2204_mac_mib_info gmib_info[] = {
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXBROAD_ADDR, "rx_broadcast"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPAUSE_ADDR, "rx_pause"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXMULTI_ADDR, "rx_multicast"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXFCSERR_ADDR, "rx_fcserr"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXALIGNERR_ADDR, "rx_alignerr"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXRUNT_ADDR, "rx_runt"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXFRAG_ADDR, "rx_frag"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXJUMBOFCSERR_ADDR, "rx_jumbofcserr"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXJUMBOALIGNERR_ADDR, "rx_jumboalignerr"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT64_ADDR, "rx_pkt64"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT65TO127_ADDR, "rx_pkt65to127"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT128TO255_ADDR, "rx_pkt128to255"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT256TO511_ADDR, "rx_pkt256to511"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT512TO1023_ADDR, "rx_pkt512to1023"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT1024TO1518_ADDR, "rx_pkt1024to1518"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXPKT1519TOX_ADDR, "rx_pkt1519tomax"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXTOOLONG_ADDR, "rx_toolong"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_GMAC_RXBYTE_G_ADDR, "rx_bytes_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_GMAC_RXBYTE_B_ADDR, "rx_bytes_b"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_RXUNI_ADDR, "rx_unicast"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXBROAD_ADDR, "tx_broadcast"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPAUSE_ADDR, "tx_pause"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXMULTI_ADDR, "tx_multicast"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXUNDERRUN_ADDR, "tx_underrun"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT64_ADDR, "tx_pkt64"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT65TO127_ADDR, "tx_pkt65to127"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT128TO255_ADDR, "tx_pkt128to255"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT256TO511_ADDR, "tx_pkt256to511"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT512TO1023_ADDR, "tx_pkt512to1023"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT1024TO1518_ADDR, "tx_pkt1024to1518"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXPKT1519TOX_ADDR, "tx_pkt1519tomax"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_GMAC_TXBYTE_ADDR, "tx_bytes"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXCOLLISIONS_ADDR, "tx_collisions"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXABORTCOL_ADDR, "tx_abortcol"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXMULTICOL_ADDR, "tx_multicol"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXSINGLECOL_ADDR, "tx_singlecol"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXEXCESSIVEDEFER_ADDR, "tx_excdeffer"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXDEFER_ADDR, "tx_deffer"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXLATECOL_ADDR, "tx_latecol"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_GMAC_TXUNI_ADDR, "tx_unicast"),
};

/* XGMAC MIB statistics description */
static const struct qce2204_mac_mib_info xgmib_info[] = {
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXBYTE_GB_ADDR, "tx_bytes"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT_GB_ADDR, "tx_frames"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXBROAD_G_ADDR, "tx_broadcast_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXMULTI_G_ADDR, "tx_multicast_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT64_GB_ADDR, "tx_pkt64"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT65TO127_GB_ADDR, "tx_pkt65to127"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT128TO255_GB_ADDR, "tx_pkt128to255"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT256TO511_GB_ADDR, "tx_pkt256to511"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT512TO1023_GB_ADDR, "tx_pkt512to1023"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT1024TOMAX_GB_ADDR, "tx_pkt1024tomax"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXUNI_GB_ADDR, "tx_unicast"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXMULTI_GB_ADDR, "tx_multicast"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXBROAD_GB_ADDR, "tx_broadcast"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXUNDERFLOW_ERR_ADDR, "tx_underflow_err"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXBYTE_G_ADDR, "tx_bytes_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPKT_G_ADDR, "tx_frames_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXPAUSE_ADDR, "tx_pause"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_TXVLAN_G_ADDR, "tx_vlan_g"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_TXLPI_USEC_ADDR, "tx_lpi_usec"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_TXLPI_TRAN_ADDR, "tx_lpi_tran"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT_GB_ADDR, "rx_frames"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXBYTE_GB_ADDR, "rx_bytes"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXBYTE_G_ADDR, "rx_bytes_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXBROAD_G_ADDR, "rx_broadcast_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXMULTI_G_ADDR, "rx_multicast_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXCRC_ERR_ADDR, "rx_crc_err"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXFRAG_ERR_ADDR, "rx_frag_err"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXJABBER_ERR_ADDR, "rx_jabber_err"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXUNDERSIZE_G_ADDR, "rx_undersize_g"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXOVERSIZE_G_ADDR, "rx_oversize_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT64_GB_ADDR, "rx_pkt64"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT65TO127_GB_ADDR, "rx_pkt65to127"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT128TO255_GB_ADDR, "rx_pkt128to255"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT256TO511_GB_ADDR, "rx_pkt256to511"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT512TO1023_GB_ADDR, "rx_pkt512to1023"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPKT1024TOMAX_GB_ADDR, "rx_pkt1024tomax"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXUNI_G_ADDR, "rx_unicast_g"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXLEN_ERR_ADDR, "rx_len_err"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXOUTOFRANGE_ADDR, "rx_outofrange_err"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXPAUSE_ADDR, "rx_pause"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXFIFOOVERFLOW_ADDR, "rx_fifo_overflow"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXVLAN_GB_ADDR, "rx_vlan"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXWATCHDOG_ERR_ADDR, "rx_wdog_err"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXLPI_USEC_ADDR, "rx_lpi_usec"),
	QCE2204_MAC_MIB_DESC(4, QCE2204_PPE_XGMAC_RXLPI_TRAN_ADDR, "rx_lpi_tran"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXDISCARD_GB_ADDR, "rx_drop_frames"),
	QCE2204_MAC_MIB_DESC(8, QCE2204_PPE_XGMAC_RXDISCARDBYTE_GB_ADDR, "rx_drop_bytes"),
};

/* Read GMAC MIB counters and accumulate to stats array */
static void qce2204_port_gmib_update(struct qce2204_priv *priv, int port)
{
	const struct qce2204_mac_mib_info *mib;
	u32 reg, val;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(gmib_info); i++) {
		mib = &gmib_info[i];
		reg = QCE2204_PPE_GMAC_ADDR(port) + mib->offset;

		ret = regmap_read(priv->regmap, reg, &val);
		if (ret) {
			dev_warn(priv->dev, "Port %d GMIB read fail at 0x%x: %d\n",
				 port, reg, ret);
			continue;
		}

		priv->ppe_port[port].gmib_stats[i] += val;

		if (mib->size == 8) {
			/* Read high 32-bit for 8-byte counter */
			ret = regmap_read(priv->regmap, reg + 4, &val);
			if (ret) {
				dev_warn(priv->dev,
					 "Port %d GMIB read fail at 0x%x: %d\n",
					 port, reg + 4, ret);
				continue;
			}

			priv->ppe_port[port].gmib_stats[i] += (u64)val << 32;
		}
	}
}

/* Polling task to read GMIB statistics to avoid 32-bit register overflow */
static void qce2204_port_gmib_stats_poll(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct qce2204_ppe_port *ppe_port;
	struct qce2204_priv *priv;
	int port;

	/* Get ppe_port from delayed_work using container_of */
	ppe_port = container_of(dwork, struct qce2204_ppe_port, gmib_read);
	port = ppe_port->port;

	/* Get priv from ppe_port */
	priv = ppe_port->priv;

	/* Protect GMIB update with mutex to prevent race with ethtool stats read */
	mutex_lock(&ppe_port->gmib_stats_lock);
	qce2204_port_gmib_update(priv, port);
	mutex_unlock(&ppe_port->gmib_stats_lock);

	schedule_delayed_work(&ppe_port->gmib_read,
			      msecs_to_jiffies(QCE2204_GMIB_POLL_INTERVAL_MS));
}

/**
 * qce2204_port_gmib_work_start() - Start GMAC MIB statistics polling work
 * @priv: QCE2204 private data
 * @port: Port number
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_gmib_work_start(struct qce2204_priv *priv, int port)
{
	if (!priv->ppe_port[port].gmib_stats) {
		/* Allocate array memory to store GMIB statistics */
		priv->ppe_port[port].gmib_stats = devm_kzalloc(priv->dev,
						      array_size(ARRAY_SIZE(gmib_info),
								 sizeof(u64)),
						      GFP_KERNEL);
		if (!priv->ppe_port[port].gmib_stats)
			return -ENOMEM;

		/* Init GMIB statistics polling work */
		mutex_init(&priv->ppe_port[port].gmib_stats_lock);
		INIT_DELAYED_WORK(&priv->ppe_port[port].gmib_read, qce2204_port_gmib_stats_poll);
	}

	/* Start GMIB statistics polling work */
	schedule_delayed_work(&priv->ppe_port[port].gmib_read, 0);

	return 0;
}

/**
 * qce2204_port_gmib_work_stop() - Stop GMAC MIB statistics polling work
 * @priv: QCE2204 private data
 * @port: Port number
 */
static void qce2204_port_gmib_work_stop(struct qce2204_priv *priv, int port)
{
	if (priv->ppe_port[port].gmib_stats) {
		/* Stop GMIB statistics polling work */
		cancel_delayed_work_sync(&priv->ppe_port[port].gmib_read);
	}
}

/* Get XGMAC MIB counter */
static u64 qce2204_port_xgmib_get(struct qce2204_priv *priv, int port,
				  enum qce2204_xgmib_stats_type xgmib_type)
{
	const struct qce2204_mac_mib_info *mib;
	u32 reg, val;
	u64 data = 0;
	int ret;

	mib = &xgmib_info[xgmib_type];

	if (port == 0)
		reg = QCE2204_PPE_XGMAC_ADDR(0);
	else if (port == 5)
		reg = QCE2204_PPE_XGMAC_ADDR(1);
	else
		return 0;

	reg +=  mib->offset;

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret) {
		dev_warn(priv->dev, "Port %d XGMIB read fail at 0x%x: %d\n",
			 port, reg, ret);
		goto data_return;
	}

	data = val;
	if (mib->size == 8) {
		/* Read high 32-bit for 8-byte counter */
		ret = regmap_read(priv->regmap, reg + 4, &val);
		if (ret) {
			dev_warn(priv->dev, "Port %d XGMIB read fail at 0x%x: %d\n",
				 port, reg + 4, ret);
			goto data_return;
		}

		data |= (u64)val << 32;
	}

data_return:
	return data;
}

/**
 * qce2204_get_sset_count() - Get statistics string count
 * @ds: DSA switch
 * @port: Port number
 * @sset: String set ID
 *
 * Return: The count of the statistics string
 */
int qce2204_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	/* port0 and port5: use XGMAC format (can switch between GMAC and XGMAC) */
	if (port == 0 || port == 5)
		return ARRAY_SIZE(xgmib_info);

	/* port1 to port4 lan ports: use GMAC format only */
	return ARRAY_SIZE(gmib_info);
}

/**
 * qce2204_get_strings() - Get statistics strings
 * @ds: DSA switch
 * @port: Port number
 * @stringset: String set ID
 * @data: Pointer to statistics strings
 */
void qce2204_get_strings(struct dsa_switch *ds, int port, u32 stringset, u8 *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	/* port0 and port5: use XGMAC format */
	if (port == 0 || port == 5) {
		for (i = 0; i < ARRAY_SIZE(xgmib_info); i++)
			strscpy(data + i * ETH_GSTRING_LEN, xgmib_info[i].name,
				ETH_GSTRING_LEN);
		return;
	}

	/* port1 to port4 lan ports: use GMAC format */
	for (i = 0; i < ARRAY_SIZE(gmib_info); i++)
		strscpy(data + i * ETH_GSTRING_LEN, gmib_info[i].name,
			ETH_GSTRING_LEN);
}

/**
 * qce2204_get_ethtool_stats() - Get ethtool statistics
 * @ds: DSA switch
 * @port: Port number
 * @data: Pointer to statistics data
 */
void qce2204_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data)
{
	struct qce2204_priv *priv = ds->priv;
	int i;

	/* port0 and port5: use XGMAC format, merge GMAC and XGMAC statistics */
	if (port == 0 || port == 5) {
		/* Get XGMAC statistics */
		for (i = 0; i < ARRAY_SIZE(xgmib_info); i++)
			data[i] = qce2204_port_xgmib_get(priv, port, i);

		/* Merge GMIB statistics into XGMIB statistics */
		if (priv->ppe_port[port].gmib_stats) {
			u64 *gsrc = priv->ppe_port[port].gmib_stats;

			mutex_lock(&priv->ppe_port[port].gmib_stats_lock);

			qce2204_port_gmib_update(priv, port);

			/* Merge GMIB into XGMIB format */
			data[xgmib_tx_bytes] += gsrc[gmib_tx_bytes];
			data[xgmib_tx_frames] += gsrc[gmib_tx_broadcast];
			data[xgmib_tx_frames] += gsrc[gmib_tx_multicast];
			data[xgmib_tx_frames] += gsrc[gmib_tx_unicast];
			data[xgmib_tx_broadcast_g] += gsrc[gmib_tx_broadcast];
			data[xgmib_tx_multicast_g] += gsrc[gmib_tx_multicast];
			data[xgmib_tx_pkt64] += gsrc[gmib_tx_pkt64];
			data[xgmib_tx_pkt65to127] += gsrc[gmib_tx_pkt65to127];
			data[xgmib_tx_pkt128to255] += gsrc[gmib_tx_pkt128to255];
			data[xgmib_tx_pkt256to511] += gsrc[gmib_tx_pkt256to511];
			data[xgmib_tx_pkt512to1023] += gsrc[gmib_tx_pkt512to1023];
			data[xgmib_tx_pkt1024tomax] += gsrc[gmib_tx_pkt1024to1518];
			data[xgmib_tx_pkt1024tomax] += gsrc[gmib_tx_pkt1519tomax];
			data[xgmib_tx_unicast] += gsrc[gmib_tx_unicast];
			data[xgmib_tx_multicast] += gsrc[gmib_tx_multicast];
			data[xgmib_tx_broadcast] += gsrc[gmib_tx_broadcast];
			data[xgmib_tx_underflow_err] += gsrc[gmib_tx_underrun];
			data[xgmib_tx_bytes_g] += gsrc[gmib_tx_bytes];
			data[xgmib_tx_frames_g] += gsrc[gmib_tx_broadcast];
			data[xgmib_tx_frames_g] += gsrc[gmib_tx_multicast];
			data[xgmib_tx_frames_g] += gsrc[gmib_tx_unicast];
			data[xgmib_tx_pause] += gsrc[gmib_tx_pause];

			data[xgmib_rx_frames] += gsrc[gmib_rx_broadcast];
			data[xgmib_rx_frames] += gsrc[gmib_rx_multicast];
			data[xgmib_rx_frames] += gsrc[gmib_rx_unicast];
			data[xgmib_rx_bytes] += gsrc[gmib_rx_bytes_g];
			data[xgmib_rx_bytes] += gsrc[gmib_rx_bytes_b];
			data[xgmib_rx_bytes_g] += gsrc[gmib_rx_bytes_g];
			data[xgmib_rx_broadcast_g] += gsrc[gmib_rx_broadcast];
			data[xgmib_rx_multicast_g] += gsrc[gmib_rx_multicast];
			data[xgmib_rx_crc_err] += gsrc[gmib_rx_fcserr];
			data[xgmib_rx_crc_err] += gsrc[gmib_rx_frag];
			data[xgmib_rx_frag_err] += gsrc[gmib_rx_frag];
			data[xgmib_rx_pkt64] += gsrc[gmib_rx_pkt64];
			data[xgmib_rx_pkt65to127] += gsrc[gmib_rx_pkt65to127];
			data[xgmib_rx_pkt128to255] += gsrc[gmib_rx_pkt128to255];
			data[xgmib_rx_pkt256to511] += gsrc[gmib_rx_pkt256to511];
			data[xgmib_rx_pkt512to1023] += gsrc[gmib_rx_pkt512to1023];
			data[xgmib_rx_pkt1024tomax] += gsrc[gmib_rx_pkt1024to1518];
			data[xgmib_rx_pkt1024tomax] += gsrc[gmib_rx_pkt1519tomax];
			data[xgmib_rx_unicast_g] += gsrc[gmib_rx_unicast];
			data[xgmib_rx_pause] += gsrc[gmib_rx_pause];

			mutex_unlock(&priv->ppe_port[port].gmib_stats_lock);
		}
		return;
	}

	/* port1 to port4 lan ports: use GMAC format only, directly return GMIB statistics */
	if (priv->ppe_port[port].gmib_stats) {
		mutex_lock(&priv->ppe_port[port].gmib_stats_lock);
		qce2204_port_gmib_update(priv, port);
		memcpy(data, priv->ppe_port[port].gmib_stats, ARRAY_SIZE(gmib_info) * sizeof(u64));
		mutex_unlock(&priv->ppe_port[port].gmib_stats_lock);
	} else {
		memset(data, 0, ARRAY_SIZE(gmib_info) * sizeof(u64));
	}
}

/**
 * qce2204_phylink_mac_select_pcs() - Select PCS for phylink
 * @ds: DSA switch
 * @port: Port number
 * @interface: PHY interface mode
 *
 * Return: Pointer to phylink_pcs for port 0 and 5, NULL for other ports
 */
struct phylink_pcs *
qce2204_phylink_mac_select_pcs(struct dsa_switch *ds, int port,
			       phy_interface_t interface)
{
	struct qce2204_priv *priv = ds->priv;
	struct phylink_pcs *pcs = NULL;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		switch (port) {
		case 0:
			pcs = priv->pcs[0];
			break;
		case 5:
			pcs = priv->pcs[1];
			break;
		}
		break;

	default:
		break;
	}

	return pcs;
}

/**
 * qce2204_phylink_mac_config() - Configure MAC for phylink
 * @ds: DSA switch
 * @port: Port number
 * @mode: Phylink mode
 * @state: Link state
 *
 * Configure MAC type (GMAC or XGMAC) based on interface mode for port 0 and 5.
 */
void qce2204_phylink_mac_config(struct dsa_switch *ds, int port,
				unsigned int mode,
				const struct phylink_link_state *state)
{
	struct qce2204_priv *priv = ds->priv;
	enum qce2204_port_mac_type mac_type;
	u32 mask, val;
	int ret;

	/* Only configure for port 0 and port 5 */
	if (port != 0 && port != 5)
		return;

	/* Determine MAC type based on interface mode */
	switch (state->interface) {
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		/* 10GBASER/USXGMII mode: select XGMAC */
		mac_type = QCE2204_PORT_MAC_TYPE_XGMAC;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_GMII:
	default:
		/* Other modes: select GMAC */
		mac_type = QCE2204_PORT_MAC_TYPE_GMAC;
		break;
	}

	/* Configure port MUX to select GMAC or XGMAC
	 * Port 0: BIT(8) - QCE2204_PPE_PORT0_SEL_XGMAC
	 * Port 5: BIT(12) - QCE2204_PPE_PORT5_SEL_XGMAC
	 */
	mask = (port == 0) ? QCE2204_PPE_PORT0_SEL_XGMAC : QCE2204_PPE_PORT5_SEL_XGMAC;

	val = (mac_type == QCE2204_PORT_MAC_TYPE_XGMAC) ? mask : 0;

	ret = regmap_update_bits(priv->regmap, QCE2204_PPE_PORT_MUX_CTRL_ADDR,
				 mask, val);
	if (ret) {
		dev_err(priv->dev, "Failed to configure MAC MUX for port %d: %d\n",
			port, ret);
		return;
	}

	/* Store the MAC type configuration in ppe_port structure */
	priv->ppe_port[port].mac_type = mac_type;

	dev_info(priv->dev, "Port %d: interface=%s, MAC type=%s\n",
		port, phy_modes(state->interface),
		mac_type == QCE2204_PORT_MAC_TYPE_XGMAC ? "XGMAC" : "GMAC");
}

/**
 * qce2204_port_gmac_link_down() - Configure GMAC link down
 * @priv: QCE2204 private data
 * @port: Port number
 *
 * Disable GMAC RX and TX.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_gmac_link_down(struct qce2204_priv *priv, int port)
{
	u32 reg;
	int ret;

	/* Stop GMIB polling work for GMAC ports */
	qce2204_port_gmib_work_stop(priv, port);

	reg = QCE2204_PPE_GMAC_ADDR(port);

	/* Disable GMAC RX and TX */
	ret = regmap_clear_bits(priv->regmap, reg + QCE2204_PPE_GMAC_ENABLE_ADDR,
				QCE2204_PPE_GMAC_TRXEN);
	if (ret)
		return ret;

	dev_info(priv->dev, "Port %d GMAC link down\n", port);
	return 0;
}

/**
 * qce2204_port_xgmac_link_down() - Configure XGMAC link down
 * @priv: QCE2204 private data
 * @port: Port number (0 or 5)
 *
 * Disable XGMAC RX and TX.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_xgmac_link_down(struct qce2204_priv *priv, int port)
{
	u32 reg;
	int ret;

	if (port == 0)
		reg = QCE2204_PPE_XGMAC_ADDR(0);
	else if (port == 5)
		reg = QCE2204_PPE_XGMAC_ADDR(1);
	else
		return -EINVAL;

	/* Disable XGMAC RX and TX */
	ret = regmap_clear_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_RX_CONFIG_ADDR,
				QCE2204_PPE_XGMAC_RXEN);
	if (ret)
		return ret;

	ret = regmap_clear_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_TX_CONFIG_ADDR,
				QCE2204_PPE_XGMAC_TXEN);
	if (ret)
		return ret;

	dev_info(priv->dev, "Port %d XGMAC link down\n", port);
	return 0;
}

/**
 * qce2204_phylink_mac_link_down() - Handle phylink MAC link down
 * @ds: DSA switch
 * @port: Port number
 * @mode: Phylink mode
 * @interface: PHY interface mode
 *
 * Disable MAC and stop statistics polling.
 */
void qce2204_phylink_mac_link_down(struct dsa_switch *ds, int port,
				   unsigned int mode,
				   phy_interface_t interface)
{
	struct qce2204_priv *priv = ds->priv;
	enum qce2204_port_mac_type mac_type;
	u32 reg;
	int ret;

	/* Disable PPE port bridge TX MAC */
	reg = QCE2204_PPE_PORT_BRIDGE_CTRL_ADDR +
		QCE2204_PPE_PORT_BRIDGE_CTRL_INC * port;
	ret = regmap_clear_bits(priv->regmap, reg, QCE2204_PPE_PORT_BRIDGE_TXMAC_EN);
	if (ret) {
		dev_err(priv->dev, "Failed to disable bridge TX MAC for port %d: %d\n",
			port, ret);
		return;
	}

	/* Determine MAC type for this port */
	mac_type = priv->ppe_port[port].mac_type;

	/* Configure MAC link down based on MAC type */
	if (mac_type == QCE2204_PORT_MAC_TYPE_GMAC) {
		ret = qce2204_port_gmac_link_down(priv, port);
	} else {
		ret = qce2204_port_xgmac_link_down(priv, port);
	}

	if (ret) {
		dev_err(priv->dev, "Failed to configure port %d link down: %d\n", port, ret);
		return;
	}

	dev_info(priv->dev, "Port %d link down completed successfully\n", port);
}

/**
 * qce2204_port_gmac_link_up() - Configure GMAC link up
 * @priv: QCE2204 private data
 * @port: Port number
 * @speed: Link speed
 * @duplex: Duplex mode
 * @tx_pause: TX pause enabled
 * @rx_pause: RX pause enabled
 *
 * Configure GMAC speed, duplex, flow control and enable MAC.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_gmac_link_up(struct qce2204_priv *priv, int port,
				     int speed, int duplex,
				     bool tx_pause, bool rx_pause)
{
	u32 reg, val;
	int ret;

	ret = qce2204_port_gmib_work_start(priv, port);
	if (ret) {
		dev_err(priv->dev, "Failed to start GMIB polling for port %d: %d\n",
			port, ret);
		return ret;
	}

	reg = QCE2204_PPE_GMAC_ADDR(port);

	/* Set GMAC speed */
	switch (speed) {
	case SPEED_2500:
	case SPEED_1000:
		val = QCE2204_PPE_GMAC_SPEED_1000;
		break;
	case SPEED_100:
		val = QCE2204_PPE_GMAC_SPEED_100;
		break;
	case SPEED_10:
		val = QCE2204_PPE_GMAC_SPEED_10;
		break;
	default:
		dev_err(priv->dev, "Invalid GMAC speed %d for port %d\n", speed, port);
		return -EINVAL;
	}

	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_GMAC_SPEED_ADDR,
				 QCE2204_PPE_GMAC_SPEED_M, val);
	if (ret)
		return ret;

	/* Set duplex, flow control and enable GMAC */
	val = QCE2204_PPE_GMAC_TXEN | QCE2204_PPE_GMAC_RXEN;
	if (duplex == DUPLEX_FULL)
		val |= QCE2204_PPE_GMAC_DUPLEX_FULL;
	if (tx_pause)
		val |= QCE2204_PPE_GMAC_TXFCEN;
	if (rx_pause)
		val |= QCE2204_PPE_GMAC_RXFCEN;

	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_GMAC_ENABLE_ADDR,
				 QCE2204_PPE_GMAC_ENABLE_ALL, val);
	if (ret)
		return ret;

	dev_info(priv->dev, "Port %d GMAC link up: speed=%d, duplex=%s, tx_pause=%d, rx_pause=%d\n",
		port, speed, duplex == DUPLEX_FULL ? "full" : "half", tx_pause, rx_pause);

	return 0;
}

/**
 * qce2204_port_xgmac_link_up() - Configure XGMAC link up
 * @priv: QCE2204 private data
 * @port: Port number (0 or 5)
 * @interface: PHY interface mode
 * @speed: Link speed
 * @duplex: Duplex mode
 * @tx_pause: TX pause enabled
 * @rx_pause: RX pause enabled
 *
 * Configure XGMAC speed, flow control and enable MAC.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_xgmac_link_up(struct qce2204_priv *priv, int port,
				      phy_interface_t interface,
				      int speed, int duplex,
				      bool tx_pause, bool rx_pause)
{
	u32 reg, val;
	int ret;

	if (port == 0)
		reg = QCE2204_PPE_XGMAC_ADDR(0);
	else if (port == 5)
		reg = QCE2204_PPE_XGMAC_ADDR(1);
	else
		return -EINVAL;

	/* Set XGMAC TX speed and enable TX */
	switch (speed) {
	case SPEED_10000:
		if (interface == PHY_INTERFACE_MODE_USXGMII)
			val = QCE2204_PPE_XGMAC_SPEED_10000_USXGMII;
		else
			val = QCE2204_PPE_XGMAC_SPEED_10000;
		break;
	case SPEED_5000:
		val = QCE2204_PPE_XGMAC_SPEED_5000;
		break;
	case SPEED_2500:
		if (interface == PHY_INTERFACE_MODE_USXGMII)
			val = QCE2204_PPE_XGMAC_SPEED_2500_USXGMII;
		else
			val = QCE2204_PPE_XGMAC_SPEED_2500;
		break;
	case SPEED_1000:
		val = QCE2204_PPE_XGMAC_SPEED_1000;
		break;
	case SPEED_100:
		val = QCE2204_PPE_XGMAC_SPEED_100;
		break;
	case SPEED_10:
		val = QCE2204_PPE_XGMAC_SPEED_10;
		break;
	default:
		dev_err(priv->dev, "Invalid XGMAC speed %d for port %d\n", speed, port);
		return -EINVAL;
	}

	val |= QCE2204_PPE_XGMAC_TXEN;
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_TX_CONFIG_ADDR,
				 QCE2204_PPE_XGMAC_SPEED_M | QCE2204_PPE_XGMAC_TXEN,
				 val);
	if (ret)
		return ret;

	/* Set XGMAC TX flow control */
	val = FIELD_PREP(QCE2204_PPE_XGMAC_PAUSE_TIME_M,
			 FIELD_MAX(QCE2204_PPE_XGMAC_PAUSE_TIME_M));
	val |= tx_pause ? QCE2204_PPE_XGMAC_TXFCEN : 0;
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_TX_FLOW_CTRL_ADDR,
				 QCE2204_PPE_XGMAC_PAUSE_TIME_M | QCE2204_PPE_XGMAC_TXFCEN,
				 val);
	if (ret)
		return ret;

	/* Set XGMAC RX flow control */
	val = rx_pause ? QCE2204_PPE_XGMAC_RXFCEN : 0;
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_RX_FLOW_CTRL_ADDR,
				 QCE2204_PPE_XGMAC_RXFCEN, val);
	if (ret)
		return ret;

	/* Enable XGMAC RX */
	ret = regmap_set_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_RX_CONFIG_ADDR,
			      QCE2204_PPE_XGMAC_RXEN);
	if (ret)
		return ret;

	dev_info(priv->dev, "Port %d XGMAC link up: speed=%d, duplex=%s, tx_pause=%d, rx_pause=%d\n",
		port, speed, duplex == DUPLEX_FULL ? "full" : "half", tx_pause, rx_pause);

	return 0;
}

/**
 * qce2204_phylink_mac_link_up() - Handle phylink MAC link up
 * @ds: DSA switch
 * @port: Port number
 * @mode: Phylink mode
 * @interface: PHY interface mode
 * @phydev: PHY device
 * @speed: Link speed
 * @duplex: Duplex mode
 * @tx_pause: TX pause enabled
 * @rx_pause: RX pause enabled
 *
 * Configure MAC and enable statistics polling.
 */
void qce2204_phylink_mac_link_up(struct dsa_switch *ds, int port,
				 unsigned int mode,
				 phy_interface_t interface,
				 struct phy_device *phydev,
				 int speed, int duplex,
				 bool tx_pause, bool rx_pause)
{
	struct qce2204_priv *priv = ds->priv;
	enum qce2204_port_mac_type mac_type;
	u32 reg, val;
	int ret;

	/* Set speed clock for port0 10G force speed */
	if (port == 0 && speed == SPEED_10000) {
		/* Set TX and RX clock rate to 312.5MHz for 10G speed */
		if (priv->port_clks[port].tx_clk && !IS_ERR(priv->port_clks[port].tx_clk)) {
			ret = clk_set_rate(priv->port_clks[port].tx_clk, 312500000);
			if (ret) {
				dev_warn(priv->dev, "Failed to set TX clock rate for port %d: %d\n",
					port, ret);
			} else {
				dev_dbg(priv->dev, "Port %d: Set TX clock rate to 312.5MHz\n", port);
			}
		}

		if (priv->port_clks[port].rx_clk && !IS_ERR(priv->port_clks[port].rx_clk)) {
			ret = clk_set_rate(priv->port_clks[port].rx_clk, 312500000);
			if (ret) {
				dev_warn(priv->dev, "Failed to set RX clock rate for port %d: %d\n",
					port, ret);
			} else {
				dev_dbg(priv->dev, "Port %d: Set RX clock rate to 312.5MHz\n", port);
			}
		}
	}

	/* Determine MAC type for this port */
	mac_type = priv->ppe_port[port].mac_type;

	/* Configure MAC link up based on MAC type */
	if (mac_type == QCE2204_PORT_MAC_TYPE_GMAC) {
		ret = qce2204_port_gmac_link_up(priv, port, speed, duplex,
						tx_pause, rx_pause);
	} else {
		ret = qce2204_port_xgmac_link_up(priv, port, interface, speed,
						 duplex, tx_pause, rx_pause);
	}

	if (ret) {
		dev_err(priv->dev, "Failed to configure port %d link up: %d\n", port, ret);
		return;
	}

	/* Set PPE port BM flow control
	 * BM port mapping: port 0 -> BM port 7, port 1-5 -> BM port 8-12
	 */
	reg = QCE2204_PPE_BM_PORT_FC_MODE_ADDR +
		QCE2204_PPE_BM_PORT_FC_MODE_INC * (port + 7);
	val = tx_pause ? QCE2204_PPE_BM_PORT_FC_MODE_EN : 0;
	ret = regmap_update_bits(priv->regmap, reg,
				 QCE2204_PPE_BM_PORT_FC_MODE_EN, val);
	if (ret) {
		dev_err(priv->dev, "Failed to set BM flow control for port %d: %d\n",
			port, ret);
		return;
	}

	/* Enable PPE port bridge TX MAC */
	reg = QCE2204_PPE_PORT_BRIDGE_CTRL_ADDR +
		QCE2204_PPE_PORT_BRIDGE_CTRL_INC * port;
	ret = regmap_set_bits(priv->regmap, reg, QCE2204_PPE_PORT_BRIDGE_TXMAC_EN);
	if (ret) {
		dev_err(priv->dev, "Failed to enable bridge TX MAC for port %d: %d\n",
			port, ret);
		return;
	}

	dev_info(priv->dev, "Port %d link up completed successfully\n", port);
}

/**
 * qce2204_phylink_get_caps() - Get phylink capabilities
 * @ds: DSA switch
 * @port: Port number
 * @config: Phylink configuration
 *
 * Set supported interfaces and MAC capabilities for each port.
 */
void qce2204_phylink_get_caps(struct dsa_switch *ds, int port,
			      struct phylink_config *config)
{
	/* Common interfaces supported by all ports */
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);

	/* Common MAC capabilities for all ports */
	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000FD |
				   MAC_2500FD;

	if (port == 0 || port == 5) {
		/* Port 0 and 5: No INTERNAL interface, but support 10GBASER/USXGMII and 10G MAC */
		__set_bit(PHY_INTERFACE_MODE_10GBASER, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
		config->mac_capabilities |= MAC_5000FD | MAC_10000FD;
	} else {
		/* User ports: Support INTERNAL interface, no 10GBASER/USXGMII or 10G MAC */
		__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	}
}

/**
 * qce2204_port_gmac_hw_init() - Initialize GMAC hardware
 * @priv: QCE2204 private data
 * @port: Port number
 *
 * Initialize GMAC hardware registers including frame size, MIB counters, etc.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_gmac_hw_init(struct qce2204_priv *priv, int port)
{
	u32 reg, val;
	int ret;

	reg = QCE2204_PPE_GMAC_ADDR(port);

	/* GMAC RX and TX are initialized as disabled */
	ret = regmap_clear_bits(priv->regmap, reg + QCE2204_PPE_GMAC_ENABLE_ADDR,
				QCE2204_PPE_GMAC_TRXEN);
	if (ret)
		return ret;

	/* GMAC jumbo frame size configuration */
	val = FIELD_PREP(QCE2204_PPE_GMAC_JUMBO_SIZE_M, QCE2204_PORT_MAC_MAX_FRAME_SIZE);
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_GMAC_JUMBO_SIZE_ADDR,
				 QCE2204_PPE_GMAC_JUMBO_SIZE_M, val);
	if (ret)
		return ret;

	/* GMAC max frame size and TX threshold configuration */
	val = FIELD_PREP(QCE2204_PPE_GMAC_MAXFRAME_SIZE_M, QCE2204_PORT_MAC_MAX_FRAME_SIZE);
	val |= FIELD_PREP(QCE2204_PPE_GMAC_TX_THD_M, QCE2204_PPE_GMAC_TX_THD_DEFAULT);
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_GMAC_CTRL0_ADDR,
				 QCE2204_PPE_GMAC_CTRL_MASK, val);
	if (ret)
		return ret;

	/* GMAC high IPG configuration */
	val = FIELD_PREP(QCE2204_PPE_GMAC_HIGH_IPG_M, QCE2204_PPE_GMAC_HIGH_IPG_DEFAULT);
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_GMAC_CTRL1_ADDR,
				 QCE2204_PPE_GMAC_HIGH_IPG_M, val);
	if (ret)
		return ret;

	/* Enable and reset GMAC MIB counters and set as read clear mode */
	ret = regmap_set_bits(priv->regmap, reg + QCE2204_PPE_GMAC_MIB_CTRL_ADDR,
			      QCE2204_PPE_GMAC_MIB_CTRL_MASK);
	if (ret)
		return ret;

	ret = regmap_clear_bits(priv->regmap, reg + QCE2204_PPE_GMAC_MIB_CTRL_ADDR,
				QCE2204_PPE_GMAC_MIB_RST);
	if (ret)
		return ret;

	return 0;
}

/**
 * qce2204_port_xgmac_hw_init() - Initialize XGMAC hardware
 * @priv: QCE2204 private data
 * @port: Port number (0 or 5)
 *
 * Initialize XGMAC hardware registers including frame size, MIB counters, etc.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_port_xgmac_hw_init(struct qce2204_priv *priv, int port)
{
	u32 reg, val;
	int ret;

	if (port == 0)
		/* Port 0 is first XGMAC ID */
		reg = QCE2204_PPE_XGMAC_ADDR(0);
	else if (port == 5)
		/* Port 5 is second XGMAC ID */
		reg = QCE2204_PPE_XGMAC_ADDR(1);
	else
		return -EINVAL;

	/* XGMAC TX disabled and jumbo disable configuration */
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_TX_CONFIG_ADDR,
				 QCE2204_PPE_XGMAC_TXEN | QCE2204_PPE_XGMAC_JD,
				 QCE2204_PPE_XGMAC_JD);
	if (ret)
		return ret;

	/* XGMAC RX configuration with max frame size */
	val = FIELD_PREP(QCE2204_PPE_XGMAC_GPSL_M, QCE2204_PORT_MAC_MAX_FRAME_SIZE);
	val |= QCE2204_PPE_XGMAC_GPSLEN;
	val |= QCE2204_PPE_XGMAC_CST;
	val |= QCE2204_PPE_XGMAC_ACS;
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_RX_CONFIG_ADDR,
				 QCE2204_PPE_XGMAC_RX_CONFIG_MASK, val);
	if (ret)
		return ret;

	/* XGMAC watchdog timeout configuration */
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_WD_TIMEOUT_ADDR,
				 QCE2204_PPE_XGMAC_WD_TIMEOUT_MASK,
				 QCE2204_PPE_XGMAC_WD_TIMEOUT_VAL);
	if (ret)
		return ret;

	/* XGMAC packet filter configuration */
	ret = regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_PKT_FILTER_ADDR,
				 QCE2204_PPE_XGMAC_PKT_FILTER_MASK,
				 QCE2204_PPE_XGMAC_PKT_FILTER_VAL);
	if (ret)
		return ret;

	/* Enable and reset XGMAC MIB counters */
	return regmap_update_bits(priv->regmap, reg + QCE2204_PPE_XGMAC_MMC_CTRL_ADDR,
				  QCE2204_PPE_XGMAC_MCF | QCE2204_PPE_XGMAC_CNTRST,
				  QCE2204_PPE_XGMAC_CNTRST);
}

/**
 * qce2204_pcs_create() - Create PCS instance from device tree
 * @dp_node: Device tree node of the DSA port
 *
 * Helper function to create PCS instance from pcsphy-handle property.
 *
 * Return: Pointer to phylink_pcs on success, ERR_PTR on failure
 */
static struct phylink_pcs *qce2204_pcs_create(struct device_node *dp_node)
{
	struct device_node *node;

	node = of_parse_phandle(dp_node, "pcsphy-handle", 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	return qce2204_pcs_create_fwnode(of_fwnode_handle(node));
}

/**
 * qce2204_setup_pcs() - Setup PCS for port 0 and port 5
 * @priv: QCE2204 private data
 * @port: Port number (0 or 5)
 *
 * Setup PCS instance for the specified port.
 *
 * Return: 0 on success, negative error code on failure
 */
static int qce2204_setup_pcs(struct qce2204_priv *priv, int port)
{
	struct dsa_switch *ds = priv->ds;
	struct dsa_port *dp;
	int pcs_index;

	/* Determine PCS index based on port */
	if (port == 0) {
		pcs_index = 0;
	} else if (port == 5) {
		pcs_index = 1;
	} else {
		dev_err(priv->dev, "PCS setup not supported for port %d\n", port);
		return -EINVAL;
	}

	/* Get DSA port */
	dp = dsa_to_port(ds, port);
	if (!dp) {
		dev_err(priv->dev, "Failed to get DSA port %d\n", port);
		return -ENODEV;
	}

	/* Create PCS instance */
	priv->pcs[pcs_index] = qce2204_pcs_create(dp->dn);
	if (IS_ERR(priv->pcs[pcs_index])) {
		dev_err(priv->dev, "Port %d: PCS not available\n", port);
		return PTR_ERR(priv->pcs[pcs_index]);
	} else {
		dev_info(priv->dev, "Port %d: PCS created successfully\n", port);
	}

	return 0;
}

/**
 * qce2204_port_mac_init() - Initialize DSA ports MAC configuration
 * @priv: QCE2204 private data
 *
 * Initialize MAC configuration for all available DSA ports.
 * For port0 and port5 ports, initialize both GMAC and XGMAC.
 * For port1 to port4 lan ports, initialize only GMAC.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_port_mac_init(struct qce2204_priv *priv)
{
	struct dsa_switch *ds = priv->ds;
	struct dsa_port *dp;
	int ret;

	dev_info(priv->dev, "Initializing DSA port MAC configurations\n");

	dsa_switch_for_each_available_port(dp, ds) {
		int port = dp->index;

		/* Initialize ppe_port structure */
		priv->ppe_port[port].port = port;
		priv->ppe_port[port].priv = priv;

		dev_info(priv->dev, "Initializing port %d MAC\n", port);

		if (port == 0 || port == 5) {
			/* Initialize both GMAC and XGMAC */
			dev_info(priv->dev, "Port %d: initializing GMAC and XGMAC\n", port);

			ret = qce2204_port_gmac_hw_init(priv, port);
			if (ret) {
				dev_err(priv->dev,
					"Failed to initialize GMAC for port %d: %d\n",
					port, ret);
				return ret;
			}

			ret = qce2204_port_xgmac_hw_init(priv, port);
			if (ret) {
				dev_err(priv->dev,
					"Failed to initialize XGMAC for port %d: %d\n",
					port, ret);
				return ret;
			}

			/* Store MAC type configuration (default to GMAC) */
			priv->ppe_port[port].mac_type = QCE2204_PORT_MAC_TYPE_GMAC;
			dev_info(priv->dev, "Port %d MAC type set to GMAC\n", port);

			/* Setup PCS for port 0 and port 5 */
			ret = qce2204_setup_pcs(priv, port);
			if (ret) {
				dev_err(priv->dev, "Failed to setup PCS for port %d: %d\n", port, ret);
				return ret;
			}
		} else {
			/* Initialize only GMAC */
			dev_info(priv->dev, "User port %d: initializing GMAC only\n", port);

			ret = qce2204_port_gmac_hw_init(priv, port);
			if (ret) {
				dev_err(priv->dev,
					"Failed to initialize GMAC for user port %d: %d\n",
					port, ret);
				return ret;
			}

			/* Lan ports always use GMAC */
			priv->ppe_port[port].mac_type = QCE2204_PORT_MAC_TYPE_GMAC;
		}
	}

	dev_info(priv->dev, "DSA port MAC initialization completed successfully\n");
	return 0;
}

/**
 * qce2204_port_mac_deinit() - Cleanup DSA ports MAC configuration
 * @priv: QCE2204 private data
 *
 * Destroy PCS instances for port 0 and 5.
 */
void qce2204_port_mac_deinit(struct qce2204_priv *priv)
{
	/* Destroy PCS instances */
	if (priv->pcs[0]) {
		qce2204_pcs_destroy(priv->pcs[0]);
		priv->pcs[0] = NULL;
		dev_info(priv->dev, "Port 0: PCS destroyed\n");
	}

	if (priv->pcs[1]) {
		qce2204_pcs_destroy(priv->pcs[1]);
		priv->pcs[1] = NULL;
		dev_info(priv->dev, "Port 5: PCS destroyed\n");
	}

	dev_info(priv->dev, "DSA port MAC cleanup completed successfully\n");
}
