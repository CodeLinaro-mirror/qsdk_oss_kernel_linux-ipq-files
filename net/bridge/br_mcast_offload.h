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
#include "br_private_mcast_eht.h"

/*
 * Hash size for IP->MAC map buckets
 */
#ifndef BR_IP_MAC_HASH_SIZE
#define BR_IP_MAC_HASH_SIZE 256
#endif

/* Hash table for shared MAC states */
#ifndef BR_SHARED_MAC_STATE_HASH_SIZE
#define BR_SHARED_MAC_STATE_HASH_SIZE 64
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

/* Snapshot structures for EHT data collection */
struct eht_src_entry_snapshot {
	union net_bridge_eht_addr	src_addr;
	unsigned long			timer_val;
};

struct eht_host_snapshot {
	union net_bridge_eht_addr	h_addr;
	u8				filter_mode;
	u32				num_entries;
	struct eht_src_entry_snapshot	*src_entries;
	unsigned char			mac_addr[ETH_ALEN];
	bool				mac_valid;
};

struct eht_snapshot {
	struct eht_host_snapshot	*hosts;
	u32				num_hosts;
	__be16				proto;
};

 /*
 * Consolidated state for a shared MAC address
 * Tracks the last notified state to detect changes
 */
struct br_mcast_shared_mac_state {
	struct hlist_node hlist;
	unsigned char mac[ETH_ALEN];
	__be16 proto;
	union nf_inet_addr grp_ip;
	uint16_t vid;
	uint32_t ifindex;

	/* Consolidated state */
	enum br_mcast_filter filter_mode;
	uint32_t src_cnt;
	union nf_inet_addr src_list[BR_MCAST_SRC_ENT_LIMIT];

	/* Reference counting */
	uint32_t host_count;		/* Number of hosts sharing this MAC */
	struct rcu_head rcu;
};

/*
 * Capture the Host information from the
 * corresponding EHT node.
 */
struct br_mcast_host_info {
	enum br_mcast_filter filter_mode;
	uint32_t src_cnt;
	union nf_inet_addr src_list[BR_MCAST_SRC_ENT_LIMIT];
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
void br_mcast_offload_cleanup_shared_mac_states(struct net_bridge_mcast *brmctx);
void br_mcast_offload_init_shared_mac_states(struct net_bridge_mcast *brmctx);


#if IS_ENABLED(CONFIG_IPV6)
/* Wrapper API to decide whether to snoop a given IPv6 multicast group */
bool br_mcast_offload_ip6_should_snoop_group(const struct net_bridge_mcast *brmctx,
					     const struct in6_addr *group);
/* Wrapper API to decide whether to mark mrouters_only for a given IPv6 dst group */
bool br_mcast_offload_ip6_should_mark_mrouters_only(const struct net_bridge_mcast *brmctx,
						    const struct in6_addr *dst);
#endif

struct eht_snapshot *br_mcast_offload_eht_collect_snapshot(struct net_bridge_port_group *pg,
							   __be16 proto);
void br_mcast_offload_eht_snapshot_free(struct eht_snapshot *snapshot);
int br_mcast_offload_mdb_fill_eht_hosts_from_snapshot(struct sk_buff *skb,
						      struct eht_snapshot *snapshot);

/* MDB atomic notifier */
void br_mcast_offload_mdb_register_notify(struct notifier_block *nb);
void br_mcast_offload_mdb_unregister_notify(struct notifier_block *nb);
int br_mcast_offload_mdb_send_event_notify(struct net_device *dev,
			     struct net_bridge_mdb_entry *mp,
			     int type);

/* EHT event notifier and send event */
void br_mcast_offload_event_notifier_register(struct notifier_block *nb);
void br_mcast_offload_event_notifier_unregister(struct notifier_block *nb);
void br_mcast_offload_send_event(void *port_data, void *host_addr, enum br_mcast_event_type event);

#endif /* _BR_MCAST_OFFLOAD_H */
