/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _QCE2204_PPE_MAC_H_
#define _QCE2204_PPE_MAC_H_

#include <linux/phylink.h>
#include <net/dsa.h>

struct qce2204_priv;

/* MAC Init functions */
int qce2204_port_mac_init(struct qce2204_priv *priv);
void qce2204_port_mac_deinit(struct qce2204_priv *priv);

/* Phylink MAC operations */
void qce2204_phylink_get_caps(struct dsa_switch *ds, int port,
			      struct phylink_config *config);
struct phylink_pcs *qce2204_phylink_mac_select_pcs(struct dsa_switch *ds,
						   int port,
						   phy_interface_t interface);
void qce2204_phylink_mac_config(struct dsa_switch *ds, int port,
				unsigned int mode,
				const struct phylink_link_state *state);
void qce2204_phylink_mac_link_down(struct dsa_switch *ds, int port,
				   unsigned int mode,
				   phy_interface_t interface);
void qce2204_phylink_mac_link_up(struct dsa_switch *ds, int port,
				 unsigned int mode,
				 phy_interface_t interface,
				 struct phy_device *phydev,
				 int speed, int duplex,
				 bool tx_pause, bool rx_pause);

/* Ethtool statistics operations */
int qce2204_get_sset_count(struct dsa_switch *ds, int port, int sset);
void qce2204_get_strings(struct dsa_switch *ds, int port, u32 stringset, u8 *data);
void qce2204_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data);

#endif /* _QCE2204_PPE_MAC_H_ */
