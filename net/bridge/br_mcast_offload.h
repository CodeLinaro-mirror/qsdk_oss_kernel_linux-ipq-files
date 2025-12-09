/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Bridge multicast offload support.
 */

#ifndef _BR_MCAST_OFFLOAD_H
#define _BR_MCAST_OFFLOAD_H

#include <linux/types.h>
#include <linux/if_ether.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/timer.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <linux/in6.h>
#endif
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include "br_private.h"

/*
 * Hash size for IP->MAC map buckets
 */
#ifndef BR_IP_MAC_HASH_SIZE
#define BR_IP_MAC_HASH_SIZE 256
#endif

/*
 * Per-host IP->MAC mapping entry for multicast MCUC offload
 */
struct net_bridge_ip_to_mac {
	struct hlist_node hlist;
	union nf_inet_addr addr;
	__be16 ip_proto;
	unsigned char	mac[ETH_ALEN];
	uint16_t vid;
	int ifindex;
	bool shared_mac;
	struct net_bridge_mcast *brmctx;
	struct timer_list timer;
	struct rcu_head rcu;
};

int br_mcast_offload_map_add(struct net_bridge_mcast *brmctx,
			     const struct br_ip *host,
			     const unsigned char *mac,
			     int ifindex,
			     uint16_t vid);
int br_mcast_offload_ip_map_lookup(struct net_bridge_mcast *brmctx,
				   const struct br_ip *host,
				   int ifindex, uint16_t vid,
				   unsigned char *mac_out, bool *shared_mac);
void br_mcast_offload_get_br_ip(const struct sk_buff *skb, struct br_ip *host);
void br_mcast_offload_free_entry(struct net_bridge_ip_to_mac *entry);
void br_mcast_offload_cleanup_map(struct net_bridge_mcast *brmctx);
void br_mcast_offload_init_map(struct net_bridge_mcast *brmctx);

#if IS_ENABLED(CONFIG_IPV6)
/* Wrapper API to decide whether to snoop a given IPv6 multicast group */
bool br_mcast_offload_ip6_should_snoop_group(const struct net_bridge_mcast *brmctx,
					     const struct in6_addr *group);
/* Wrapper API to decide whether to mark mrouters_only for a given IPv6 dst group */
bool br_mcast_offload_ip6_should_mark_mrouters_only(const struct net_bridge_mcast *brmctx,
						    const struct in6_addr *dst);
#endif

#endif /* _BR_MCAST_OFFLOAD_H */
