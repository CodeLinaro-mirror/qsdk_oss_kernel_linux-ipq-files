/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Bridge multicast offload support.
 *
 * This file contains helpers for offloading multicast traffic
 */

#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <net/ip.h>
#include <linux/igmp.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
#include <net/addrconf.h>
#include <net/mld.h>
#endif
#include <linux/notifier.h>
#include <linux/if_bridge.h>

#include "br_private.h"
#include "br_mcast_offload.h"
#include <net/netlink.h>
#include <net/rtnetlink.h>
#include "br_private_mcast_eht.h"

#define BR_MCAST_OFFLOAD_MAP_TIMER_INTVL (10 * HZ)

/*
 * Bridge multicast offload notifier API
 */
ATOMIC_NOTIFIER_HEAD(br_mcast_offload_mdb_notifier_list);

/**
 * br_mcast_offload_mdb_register_notify - Register a notifier for multicast group events
 * @nb: notifier block to register
 *
 * Register a notifier that will be called when multicast groups are added
 * or removed from the bridge. The notifier receives BR_MDB_EVENT_ADD or
 * BR_MDB_EVENT_DEL events with a pointer to a br_mdb_event structure
 * containing the device and group address information.
 */
void br_mcast_offload_mdb_register_notify(struct notifier_block *nb)
{
	atomic_notifier_chain_register(&br_mcast_offload_mdb_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(br_mcast_offload_mdb_register_notify);

/**
 * br_mcast_offload_mdb_unregister_notify - Unregister a multicast group event notifier
 * @nb: notifier block to unregister
 *
 * Unregister a notifier previously registered with br_mcast_offload_mdb_register_notify.
 */

void br_mcast_offload_mdb_unregister_notify(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&br_mcast_offload_mdb_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(br_mcast_offload_mdb_unregister_notify);

/*
 * br_mcast_offload_mdb_send_event_notify()
 *	Sends the Bridge MDB atomic notifications.
 */
int br_mcast_offload_mdb_send_event_notify(struct net_device *dev, struct net_bridge_mdb_entry *mp, int type)
{
	struct br_mdb_event mdb_event;

	if (!dev || !mp) {
		pr_debug("bridge: Invalid parameters to %s\n", __func__);
		return -EINVAL;
	}

	memset(&mdb_event, 0, sizeof(mdb_event));

	switch (mp->addr.proto) {
	case htons(ETH_P_IP):
		mdb_event.group.ip = mp->addr.dst.ip4;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		mdb_event.group.in6 = mp->addr.dst.ip6;
		break;
#endif
	default:
		pr_debug("bridge: Unkown protocol to %s\n", __func__);
		return -EINVAL;
	}

	mdb_event.dev = dev;
	mdb_event.proto = mp->addr.proto;

	/*
	 * This Notifier is invoked inside a spinlock.
	 * The Receiver of this notification is expected to not call sleeping functions.
	 */
	atomic_notifier_call_chain(&br_mcast_offload_mdb_notifier_list, type, (void *)&mdb_event);
	return 0;
}

/*
 * Bridge multicast EHT event notifier API
 */
ATOMIC_NOTIFIER_HEAD(br_mcast_event_notifier_list);

/**
 * br_mcast_offload_event_notifier_register - Register a notifier for Enhanced host tracking updates
 * @nb: notifier block to register
 *
 * Register a notifier that will be called when there is an update to the
 * EHT_HOST_TREE or EHT_SET_TREE list for a given port group.
 */
void br_mcast_offload_event_notifier_register(struct notifier_block *nb)
{
	atomic_notifier_chain_register(&br_mcast_event_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(br_mcast_offload_event_notifier_register);

/**
 * br_mcast_offload_event_notifier_unregister - Unregister a notifier for Enhanced host tracking updates
 * @nb: notifier block to unregister
 *
 * Unregister a notifier previously registered with br_mcast_offload_event_notifier_register.
 */
void br_mcast_offload_event_notifier_unregister(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&br_mcast_event_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(br_mcast_offload_event_notifier_unregister);



/*
 * br_mcast_offload_eht_snapshot_free
 *     API to free the EHT snapshot
 */
void br_mcast_offload_eht_snapshot_free(struct eht_snapshot *snapshot)
{
	u32 i;

	if (!snapshot)
		return;

	if (snapshot->hosts) {
		for (i = 0; i < snapshot->num_hosts; i++)
			kfree(snapshot->hosts[i].src_entries);
		kfree(snapshot->hosts);
	}
	kfree(snapshot);
}

/*
 * br_mcast_offload_eht_collect_snapshot
 *
 *	Collect EHT snapshot in a single pass under spinlock.
 * Counts, allocates, and copies data in one loop iteration.
 */
struct eht_snapshot *br_mcast_offload_eht_collect_snapshot(struct net_bridge_port_group *pg, __be16 proto)
{
	struct net_bridge *br = pg->key.port->br;
	struct net_bridge_group_eht_host *eht_host;
	struct net_bridge_group_eht_set_entry *set_h;
	struct eht_host_snapshot *host_snap;
	struct net_bridge_mcast *brmctx;
	struct eht_snapshot *snapshot;
	struct rb_node *node;
	struct br_ip host_ip;
	bool shared_mac;
	u32 host_idx, src_idx;

	spin_lock_bh(&br->multicast_lock);

	if (RB_EMPTY_ROOT(&pg->eht_host_tree)) {
		spin_unlock_bh(&br->multicast_lock);
		return NULL;
	}

	snapshot = kzalloc(sizeof(*snapshot), GFP_ATOMIC);
	if (!snapshot) {
		spin_unlock_bh(&br->multicast_lock);
		return NULL;
	}

	snapshot->proto = proto;
	brmctx = &pg->key.port->br->multicast_ctx;

	snapshot->num_hosts = 0;
	for (node = rb_first(&pg->eht_host_tree); node; node = rb_next(node))
		snapshot->num_hosts++;

	if (!snapshot->num_hosts) {
		spin_unlock_bh(&br->multicast_lock);
		kfree(snapshot);
		return NULL;
	}

	snapshot->hosts = kcalloc(snapshot->num_hosts,
			sizeof(struct eht_host_snapshot),
			GFP_ATOMIC);
	if (!snapshot->hosts) {
		spin_unlock_bh(&br->multicast_lock);
		kfree(snapshot);
		return NULL;
	}

	host_idx = 0;
	for (node = rb_first(&pg->eht_host_tree); node; node = rb_next(node)) {
		if (host_idx >= snapshot->num_hosts)
			break;

		eht_host = rb_entry(node, struct net_bridge_group_eht_host,
				rb_node);
		host_snap = &snapshot->hosts[host_idx];

		host_snap->h_addr = eht_host->h_addr;
		host_snap->filter_mode = eht_host->filter_mode;
		host_snap->num_entries = eht_host->num_entries;

		if (host_snap->num_entries > 0) {
			host_snap->src_entries =
				kcalloc(host_snap->num_entries,
						sizeof(struct eht_src_entry_snapshot),
						GFP_ATOMIC);
			if (!host_snap->src_entries) {
				/*
				 * Allocation failed. Cleanup to hosts initialized so far.
				 */
				snapshot->num_hosts = host_idx;
				br_mcast_offload_eht_snapshot_free(snapshot);
				spin_unlock_bh(&br->multicast_lock);
				return NULL;
			}

			src_idx = 0;
			hlist_for_each_entry(set_h, &eht_host->set_entries,
					host_list) {
				/*
				 * Copy source entries, skipping 0.0.0.0 entries
				 */
				if (proto == htons(ETH_P_IP) &&
				    set_h->eht_set->src_addr.ip4 == 0)
					continue;
#if IS_ENABLED(CONFIG_IPV6)
				if (proto == htons(ETH_P_IPV6) &&
				    ipv6_addr_any(&set_h->eht_set->src_addr.ip6))
					continue;
#endif
				if (src_idx >= host_snap->num_entries)
					break;

				host_snap->src_entries[src_idx].src_addr =
					set_h->eht_set->src_addr;
				host_snap->src_entries[src_idx].timer_val =
					br_timer_value(&set_h->timer);
				src_idx++;
			}
			host_snap->num_entries = src_idx;
		}

		memset(&host_ip, 0, sizeof(host_ip));
		host_ip.proto = proto;
		host_snap->mac_valid = false;

		if (proto == htons(ETH_P_IP)) {
			host_ip.src.ip4 = eht_host->h_addr.ip4;
#if IS_ENABLED(CONFIG_IPV6)
		} else if (proto == htons(ETH_P_IPV6)) {
			host_ip.src.ip6 = eht_host->h_addr.ip6;
#endif
		}

		if (proto == htons(ETH_P_IP) || proto == htons(ETH_P_IPV6)) {
			if (!br_mcast_offload_ip_map_lookup(brmctx, &host_ip,
						pg->key.port->dev->ifindex,
						pg->key.addr.vid,
						host_snap->mac_addr,
						&shared_mac)) {
				host_snap->mac_valid = true;
			}
		}

		host_idx++;
	}

	spin_unlock_bh(&br->multicast_lock);

	snapshot->num_hosts = host_idx;

	if (!host_idx) {
		br_mcast_offload_eht_snapshot_free(snapshot);
		return NULL;
	}

	return snapshot;
}

/*
 * br_mcast_offload_mdb_fill_eht_hosts_from_snapshot
 *	Build netlink message from snapshot (no locks held)
 */
int br_mcast_offload_mdb_fill_eht_hosts_from_snapshot(struct sk_buff *skb,
		struct eht_snapshot *snapshot)
{
	struct nlattr *nest, *host_nest, *src_nest, *src_entry_nest;
	u32 i, j;
	int addr_size;

	if (!snapshot || !snapshot->num_hosts)
		return 0;

	switch (snapshot->proto) {
	case htons(ETH_P_IP):
		addr_size = sizeof(__be32);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		addr_size = sizeof(struct in6_addr);
		break;
#endif
	default:
		addr_size = ETH_ALEN;
		break;
	}

	nest = nla_nest_start(skb, MDBA_MDB_EATTR_EHT_HOSTS);
	if (!nest)
		return -EMSGSIZE;

	for (i = 0; i < snapshot->num_hosts; i++) {
		struct eht_host_snapshot *host = &snapshot->hosts[i];

		host_nest = nla_nest_start(skb, MDBA_MDB_EATTR_EHT_HOST_ENTRY);
		if (!host_nest)
			goto out_cancel;

		if (nla_put(skb, MDBA_MDB_EATTR_EHT_HOST_IP_ADDR,
					addr_size, &host->h_addr))
			goto out_cancel_host;

		if (nla_put_u8(skb, MDBA_MDB_EATTR_EHT_HOST_MODE,
					host->filter_mode) ||
				nla_put_u32(skb, MDBA_MDB_EATTR_EHT_HOST_NSRCS,
					host->num_entries))
			goto out_cancel_host;

		if (host->mac_valid) {
			if (nla_put(skb, MDBA_MDB_EATTR_EHT_HOST_MAC,
						ETH_ALEN, host->mac_addr))
				goto out_cancel_host;
		}

		if (host->num_entries && host->src_entries) {
			src_nest = nla_nest_start(skb, MDBA_MDB_EATTR_EHT_HOST_SRCS);
			if (!src_nest)
				goto out_cancel_host;

			for (j = 0; j < host->num_entries; j++) {
				struct eht_src_entry_snapshot *src = &host->src_entries[j];

				src_entry_nest = nla_nest_start(skb,
						MDBA_MDB_EATTR_EHT_HOST_SRC_ENTRY);
				if (!src_entry_nest)
					goto out_cancel_src;

				if (nla_put(skb, MDBA_MDB_EATTR_EHT_HOST_SRC_ADDR,
							addr_size, &src->src_addr) ||
						nla_put_u32(skb, MDBA_MDB_EATTR_EHT_HOST_SRC_TIMER,
							src->timer_val)) {
					goto out_cancel_src;
				}

				nla_nest_end(skb, src_entry_nest);
			}

			nla_nest_end(skb, src_nest);
		}

		nla_nest_end(skb, host_nest);
	}

	nla_nest_end(skb, nest);
	return 0;

out_cancel_src:
	nla_nest_cancel(skb, src_nest);
out_cancel_host:
	nla_nest_cancel(skb, host_nest);
out_cancel:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

#if IS_ENABLED(CONFIG_IPV6)
/*
 * br_mcast_offload_ip6_should_snoop_group - Wrapper to decide IPv6 multicast snooping
 *
 * Avoid snooping for permanent IPv6 multicast addresses by default.
 * Snoop the addresses when ignore transient bit is enabled on the bridge via
 * ip link utility, or when the IPv6 multicast address has the transient bit set.
 *
 * Returns:
 *   true  -> perform snooping for this group
 *   false -> skip snooping for this group
 */
bool br_mcast_offload_ip6_should_snoop_group(const struct net_bridge_mcast *brmctx,
					     const struct in6_addr *group)
{
	bool ignore_t_bit = br_opt_get(brmctx->br, BROPT_MCAST_IGNORE_T_BIT);
	bool transient_bit = ((group->s6_addr[1] & 0x10) &&
			      (group->s6_addr[12] & 0x80));

	return ignore_t_bit || transient_bit;
}

/*
 * br_mcast_offload_ip6_should_mark_mrouters_only - Wrapper for mrouters_only decision
 *
 * Equivalent logic previously embedded in br_multicast_ipv6_rcv:
 * - Determine if destination IPv6 multicast address is link-local all-nodes
 * - Determine transient bit status and whether bridge is configured to ignore it
 * - Mark mrouters_only for frames that are not link-local and not permanent groups
 *
 * Returns:
 *   true  -> set BR_INPUT_SKB_CB(skb)->mrouters_only = 1
 *   false -> do not set mrouters_only
 */
bool br_mcast_offload_ip6_should_mark_mrouters_only(const struct net_bridge_mcast *brmctx,
						    const struct in6_addr *dst)
{
	bool ignore_t_bit, transient_bit, link_local, permanent;

	ignore_t_bit = br_opt_get(brmctx->br, BROPT_MCAST_IGNORE_T_BIT);
	transient_bit = ((dst->s6_addr[1] & 0x10) && (dst->s6_addr[12] & 0x80));

	link_local = ipv6_addr_is_ll_all_nodes(dst);
	permanent = !ignore_t_bit && !transient_bit;

	return (!link_local && !permanent);
}
#endif

 /*
 * br_mcast_offload_shared_mac_state_hash
 *	Calculate hash for shared MAC state lookup
 *
 * Returns the hash bucket index (0 to BR_SHARED_MAC_STATE_HASH_SIZE - 1)
 */
static inline unsigned int br_mcast_offload_shared_mac_state_hash(const unsigned char *mac, __be16 proto,
								const union nf_inet_addr *grp_ip, u16 vid, u32 ifindex)
{
	u32 hash = jhash(mac, ETH_ALEN, 0);
	hash = jhash(&proto, sizeof(proto), hash);
	hash = jhash(&vid, sizeof(vid), hash);
	hash = jhash(&ifindex, sizeof(ifindex), hash);

	if (proto == htons(ETH_P_IP)) {
		hash = jhash(&grp_ip->ip, sizeof(grp_ip->ip), hash);
#if IS_ENABLED(CONFIG_IPV6)
	} else if (proto == htons(ETH_P_IPV6)) {
		hash = jhash(&grp_ip->in6, sizeof(grp_ip->in6), hash);
#endif
	}

	return hash & (BR_SHARED_MAC_STATE_HASH_SIZE - 1);
}

/*
 * br_mcast_offload_shared_mac_state_lookup
 *	Find existing shared MAC state
 */
static struct br_mcast_shared_mac_state *br_mcast_offload_shared_mac_state_lookup(struct net_bridge_mcast *brmctx, const unsigned char *mac, __be16 proto,
										const union nf_inet_addr *grp_ip, uint16_t vid, uint32_t ifindex)
{
	struct br_mcast_shared_mac_state *state;
	unsigned int hash;

	hash = br_mcast_offload_shared_mac_state_hash(mac, proto, grp_ip, vid, ifindex);
	hlist_for_each_entry_rcu(state, &brmctx->shared_mac_state[hash], hlist) {
		if (!ether_addr_equal(state->mac, mac))
			continue;

		if (state->proto != proto)
			continue;

		if (state->vid != vid)
			continue;

		if (state->ifindex != ifindex)
			continue;

		if (proto == htons(ETH_P_IP)) {
			if (state->grp_ip.ip == grp_ip->ip)
				return state;
#if IS_ENABLED(CONFIG_IPV6)
		} else if (proto == htons(ETH_P_IPV6)) {
			if (ipv6_addr_equal(&state->grp_ip.in6, &grp_ip->in6))
				return state;
#endif
		}
	}

	return NULL;
}

/*
 * br_mcast_offload_get_all_ips_for_mac
 *	Get all IP addresses associated with a MAC address
 */
static int br_mcast_offload_get_all_ips_for_mac(struct net_bridge_mcast *brmctx, const unsigned char *mac, __be16 proto,
						uint16_t vid, uint32_t ifindex, struct br_ip *ip_list, u32 max_ips)
{
	struct net_bridge_ip_to_mac *entry;
	unsigned int i;
	uint32_t count = 0;

	/* Search all hash buckets */
	for (i = 0; i < BR_IP_MAC_HASH_SIZE && count < max_ips; i++) {
		hlist_for_each_entry_rcu(entry, &brmctx->ip_mac_map[i], hlist) {
			if (!ether_addr_equal(entry->mac, mac))
				continue;

			if (entry->ip_proto != proto)
				continue;

			if (entry->vid != vid)
				continue;

			if (entry->ifindex != ifindex)
				continue;

			/* Found matching entry */
			ip_list[count].proto = proto;
			ip_list[count].vid = vid;

			if (proto == htons(ETH_P_IP)) {
				ip_list[count].src.ip4 = entry->addr.ip;
#if IS_ENABLED(CONFIG_IPV6)
			} else if (proto == htons(ETH_P_IPV6)) {
				ip_list[count].src.ip6 = entry->addr.in6;
#endif
			}

			count++;
		}
	}

	return count;
}

/*
 * br_mcast_offload_src_equal
 *	Compare two source addresses
 */
static bool br_mcast_offload_src_equal(const union nf_inet_addr *src1, const union nf_inet_addr *src2, __be16 proto)
{
	if (proto == htons(ETH_P_IP)) {
		return src1->ip == src2->ip;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (proto == htons(ETH_P_IPV6)) {
		return ipv6_addr_equal(&src1->in6, &src2->in6);
#endif
	}

	return false;
}

/*
 * br_mcast_offload_states_equal
 *	Compare two consolidated states
 */
static bool br_mcast_offload_states_equal( enum br_mcast_filter mode1, const union nf_inet_addr *srcs1, u32 cnt1,
			enum br_mcast_filter mode2, const union nf_inet_addr *srcs2, u32 cnt2, __be16 proto)
{
	uint32_t i, j;

	if (mode1 != mode2)
		return false;

	if (cnt1 != cnt2)
		return false;

	/* Check if all sources in srcs1 are in srcs2 */
	for (i = 0; i < cnt1; i++) {
		bool found = false;

		for (j = 0; j < cnt2; j++) {
			if (br_mcast_offload_src_equal(&srcs1[i], &srcs2[j], proto)) {
				found = true;
				break;
			}
		}

		if (!found)
			return false;

	}

	return true;
}

/*
 * br_mcast_offload_consolidate_filter_modes
 *	Consolidate multiple filter modes and source lists according to IGMPv3 rules
 *
 * Returns: Consolidated filter mode
 * Fills: consolidated_srcs with consolidated source list
 *	  consolidated_cnt with number of sources
 */
static enum br_mcast_filter br_mcast_offload_consolidate_filter_modes(struct br_mcast_host_info *hosts, u32 host_count,
						union nf_inet_addr *consolidated_srcs, u32 *consolidated_cnt, __be16 proto)
{
	bool has_exclude = false;
	bool has_include = false;
	uint32_t i, j, k;
	uint32_t src_count = 0;

	/* Check what modes we have */
	for (i = 0; i < host_count; i++) {
		if (hosts[i].filter_mode == BR_MCAST_SRCLIST_EXCLUDE)
			has_exclude = true;
		else if (hosts[i].filter_mode == BR_MCAST_SRCLIST_INCLUDE)
			has_include = true;
	}

	/* Rule 1: If any EXCLUDE, result is EXCLUDE */
	if (has_exclude) {
		if (!has_include) {
			/* All EXCLUDE: intersection of EXCLUDE lists */
			/* Start with first EXCLUDE host's sources */
			for (i = 0; i < host_count; i++) {
				if (hosts[i].filter_mode == BR_MCAST_SRCLIST_EXCLUDE) {
					for (j = 0; j < hosts[i].src_cnt && src_count < BR_MCAST_SRC_ENT_LIMIT; j++) {
						bool in_all_exclude = true;

						/* Check if this source is in ALL EXCLUDE lists */
						for (k = 0; k < host_count; k++) {
							if (hosts[k].filter_mode != BR_MCAST_SRCLIST_EXCLUDE)
								continue;

							bool found = false;
							uint32_t m;

							for (m = 0; m < hosts[k].src_cnt; m++) {
								if (br_mcast_offload_src_equal(&hosts[i].src_list[j], &hosts[k].src_list[m], proto)) {
									found = true;
									break;
								}
							}

							if (!found) {
								in_all_exclude = false;
								break;
							}
						}

						/* Add to consolidated list if in all EXCLUDE lists */
						if (in_all_exclude) {
							consolidated_srcs[src_count++] = hosts[i].src_list[j];
						}
					}

					break;  /* Only need to iterate once */
				}
			}
		} else {
			/* Mixed INCLUDE and EXCLUDE */
			/* Result = EXCLUDE with (EXCLUDE sources - INCLUDE sources) */
			/* First, collect all EXCLUDE sources */
			union nf_inet_addr exclude_srcs[BR_MCAST_SRC_ENT_LIMIT];
			uint32_t exclude_cnt = 0;

			for (i = 0; i < host_count; i++) {
				if (hosts[i].filter_mode == BR_MCAST_SRCLIST_EXCLUDE) {
					for (j = 0; j < hosts[i].src_cnt && exclude_cnt < BR_MCAST_SRC_ENT_LIMIT; j++) {
						/* Add if not already in list */
						bool found = false;

						for (k = 0; k < exclude_cnt; k++) {
							if (br_mcast_offload_src_equal(&hosts[i].src_list[j], &exclude_srcs[k], proto)) {
								found = true;
								break;
							}
						}

						if (!found) {
							exclude_srcs[exclude_cnt++] = hosts[i].src_list[j];
						}
					}
				}
			}

			/* Now remove any sources that are in INCLUDE lists */
			for (i = 0; i < exclude_cnt; i++) {
				bool in_include = false;

				for (j = 0; j < host_count; j++) {
					if (hosts[j].filter_mode == BR_MCAST_SRCLIST_INCLUDE) {
						for (k = 0; k < hosts[j].src_cnt; k++) {
							if (br_mcast_offload_src_equal(&exclude_srcs[i], &hosts[j].src_list[k], proto)) {
								in_include = true;
								break;
							}
						}

						if (in_include)
							break;
					}
				}

				/* Add to consolidated list if NOT in any INCLUDE list */
				if (!in_include && src_count < BR_MCAST_SRC_ENT_LIMIT) {
					consolidated_srcs[src_count++] = exclude_srcs[i];
				}
			}
		}

		*consolidated_cnt = src_count;
		return BR_MCAST_SRCLIST_EXCLUDE;
	}

	/* Rule 2: All INCLUDE: union of INCLUDE lists */
	if (has_include) {
		for (i = 0; i < host_count; i++) {
			if (hosts[i].filter_mode == BR_MCAST_SRCLIST_INCLUDE) {
				for (j = 0; j < hosts[i].src_cnt && src_count < BR_MCAST_SRC_ENT_LIMIT; j++) {
					/* Add if not already in consolidated list */
					bool found = false;

					for (k = 0; k < src_count; k++) {
						if (br_mcast_offload_src_equal(&hosts[i].src_list[j], &consolidated_srcs[k], proto)) {
							found = true;
							break;
						}
					}

					if (!found) {
						consolidated_srcs[src_count++] = hosts[i].src_list[j];
					}
				}
			}
		}

		*consolidated_cnt = src_count;
		return BR_MCAST_SRCLIST_INCLUDE;
	}

	/* No valid filter mode */
	*consolidated_cnt = 0;
	return BR_MCAST_SRCLIST_NONE;
}

/*
 * br_mcast_offload_get_host_info
 *	Get filter mode and source list for a specific host IP
 */
static int br_mcast_offload_get_host_info(struct net_bridge_port_group *pg, const struct br_ip *host_ip, struct br_mcast_host_info *info)
{
	struct net_bridge_group_eht_host *eht_host;
	struct net_bridge_group_eht_set_entry *entry;
	union net_bridge_eht_addr h_addr;
	uint32_t src_idx = 0;

	memset(info, 0, sizeof(*info));
	memset(&h_addr, 0, sizeof(h_addr));

	if (host_ip->proto == htons(ETH_P_IP)) {
		h_addr.ip4 = host_ip->src.ip4;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (host_ip->proto == htons(ETH_P_IPV6)) {
		h_addr.ip6 = host_ip->src.ip6;
#endif
	} else {
		return -EINVAL;
	}

	eht_host = br_multicast_eht_host_lookup(pg, &h_addr);
	if (!eht_host)
		return -ENOENT;

	/* Get filter mode */
	switch (eht_host->filter_mode) {
	case MCAST_INCLUDE:
		info->filter_mode = BR_MCAST_SRCLIST_INCLUDE;
		break;
	case MCAST_EXCLUDE:
		info->filter_mode = BR_MCAST_SRCLIST_EXCLUDE;
		break;
	default:
		return -EINVAL;
	}

	/* Get source list */
	hlist_for_each_entry(entry, &eht_host->set_entries, host_list) {
		if (src_idx >= BR_MCAST_SRC_ENT_LIMIT)
			break;

		/* Skip zero sources */
		if (host_ip->proto == htons(ETH_P_IP)) {
			if (entry->eht_set->src_addr.ip4 == 0)
				continue;

			info->src_list[src_idx].ip = entry->eht_set->src_addr.ip4;
#if IS_ENABLED(CONFIG_IPV6)
		} else if (host_ip->proto == htons(ETH_P_IPV6)) {
			if (ipv6_addr_any(&entry->eht_set->src_addr.ip6))
				continue;

			info->src_list[src_idx].in6 = entry->eht_set->src_addr.ip6;
#endif
		}

		src_idx++;
	}

	info->src_cnt = src_idx;
	return 0;
}

/*
 * br_mcast_offload_update_shared_mac_state
 *	Update or create shared MAC state and send notification if changed
 */
static int br_mcast_offload_update_shared_mac_state(struct net_bridge_port_group *pg, const unsigned char *mac, const union nf_inet_addr *grp_ip,
						__be16 proto, uint16_t vid, uint32_t ifindex, enum br_mcast_event_type *event_type)
{
	struct net_bridge *br = pg->key.port->br;
	struct net_bridge_mcast *brmctx = &br->multicast_ctx;
	struct br_mcast_shared_mac_state *state;
	struct br_ip ip_list[BR_MCAST_SRC_ENT_LIMIT];
	struct br_mcast_host_info hosts[BR_MCAST_SRC_ENT_LIMIT];
	union nf_inet_addr consolidated_srcs[BR_MCAST_SRC_ENT_LIMIT];
	enum br_mcast_filter consolidated_mode;
	uint32_t consolidated_cnt, ip_count, host_count = 0, i;
	int ret;

	/* Get all IPs for this MAC */
	ip_count = br_mcast_offload_get_all_ips_for_mac(brmctx, mac, proto, vid, ifindex, ip_list, BR_MCAST_SRC_ENT_LIMIT);
	if (!ip_count) {
		/* No hosts for this MAC, Though shared_mac is TRUE */
		*event_type = BR_MCAST_EVENT_NONE;
		pr_debug("Shared MAC: Cannot find Hosts for the given MAC=%pM\n", mac);
		return -EINVAL;
	}

	/* Get host info for each IP */
	for (i = 0; i < ip_count; i++) {
		ret = br_mcast_offload_get_host_info(pg, &ip_list[i], &hosts[host_count]);
		if (!ret)
			host_count++;
	}

	if (!host_count) {
		/* No valid hosts, should send DEL */
		*event_type = BR_MCAST_EVENT_DEL;
		return 0;
	}

	/* Consolidate filter modes and source lists */
	pr_debug("update_shared_mac_state: HOST Count = %d\n", host_count);
	consolidated_mode = br_mcast_offload_consolidate_filter_modes(hosts, host_count, consolidated_srcs, &consolidated_cnt, proto);

	/* Look up existing state */
	state = br_mcast_offload_shared_mac_state_lookup(brmctx, mac, proto, grp_ip, vid, ifindex);
	if (!state) {
		/* New state - allocate and add */
		unsigned int hash;

		state = kzalloc(sizeof(*state), GFP_ATOMIC);
		if (!state) {
			pr_debug("Shared MAC: Failed to allocate memory for consolidated state\n");
			return -ENOMEM;
		}

		ether_addr_copy(state->mac, mac);
		state->proto = proto;
		state->grp_ip = *grp_ip;
		state->vid = vid;
		state->ifindex = ifindex;
		state->filter_mode = consolidated_mode;
		state->src_cnt = consolidated_cnt;
		memcpy(state->src_list, consolidated_srcs, consolidated_cnt * sizeof(union nf_inet_addr));
		state->host_count = host_count;

		hash = br_mcast_offload_shared_mac_state_hash(mac, proto, grp_ip, vid, ifindex);
		hlist_add_head_rcu(&state->hlist, &brmctx->shared_mac_state[hash]);

		*event_type = (host_count > 1) ? BR_MCAST_EVENT_UPDATE : BR_MCAST_EVENT_ADD;

		return 0;
	}

	/* Existing state - check if changed */
	if (br_mcast_offload_states_equal(state->filter_mode, state->src_list, state->src_cnt, consolidated_mode,
					consolidated_srcs, consolidated_cnt, proto)) {
		/* No change */
		state->host_count = host_count;
		*event_type = BR_MCAST_EVENT_NONE;
		return 0;
	}

	/* State changed - update and notify */
	state->filter_mode = consolidated_mode;
	state->src_cnt = consolidated_cnt;
	memcpy(state->src_list, consolidated_srcs, consolidated_cnt * sizeof(union nf_inet_addr));
	state->host_count = host_count;
	*event_type = BR_MCAST_EVENT_UPDATE;
	return 0;
}

/**
 * br_ip_mac_hash - Calculate hash bucket index for IP address
 * @ip: The IP address structure containing protocol and address
 *
 * Returns the hash bucket index (0 to BR_IP_MAC_HASH_SIZE-1)
 */
static inline unsigned int br_mcast_offload_map_hash(const struct br_ip *ip)
{
	u32 hash;

	switch (ip->proto) {
	case htons(ETH_P_IP):
		hash = jhash_1word((__force u32)ip->src.ip4, 0);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		hash = jhash2((__force u32 *)&ip->src.ip6, 4, 0);
		break;
#endif
	default:
		hash = 0;
		break;
	}

	return hash & (BR_IP_MAC_HASH_SIZE - 1);
}

/*
 * br_multicast_recompute_shared_mac
 *      Recompute the shared mac value when entries are added / deleted
 */
static void br_mcast_offload_recompute_shared_mac(struct net_bridge_mcast *brmctx,
						  const unsigned char *mac, uint16_t vid, int ifindex,
						  bool is_add)
{
	struct net_bridge_ip_to_mac *tmp;
	int matches = 0;
	unsigned int i;
	bool is_shared;

	/* Count total matches */
	for (i = 0; i < BR_IP_MAC_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(tmp, &brmctx->ip_mac_map[i], hlist) {
			if (ether_addr_equal(tmp->mac, mac) && (tmp->vid == vid) && (tmp->ifindex == ifindex))
				matches++;
		}
	}

	is_shared = matches > 1;

	if (is_add) {
		/* For additions, if there's only 1 match after addition,
		 * the MAC is not shared hence early return
		 */
		if (matches == 1)
			return;
	} else {
		/* For deletions, if there are 0 or > 1 matches after deletion,
		 * no update needed hence early return
		 */
		if (!matches || matches > 1)
			return;
	}

	/* Update all matching entries */
	for (i = 0; i < BR_IP_MAC_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(tmp, &brmctx->ip_mac_map[i], hlist) {
			if (ether_addr_equal(tmp->mac, mac) && (tmp->vid == vid) && (tmp->ifindex == ifindex))
				tmp->shared_mac = is_shared;
		}
	}
}

/*
 * br_multicast_ip_mac_deferred_cleanup - Deferred cleanup callback for IP-MAC entries
 * @rcu: RCU head for deferred cleanup
 *
 * This function is called via call_rcu() to perform deferred cleanup of IP-MAC
 * mapping entries after RCU grace period, ensuring all readers have finished.
 */
static void br_mcast_offload_map_deferred_cleanup(struct rcu_head *rcu)
{
	struct net_bridge_ip_to_mac *entry =
		container_of(rcu, struct net_bridge_ip_to_mac, rcu);
	struct net_bridge_mcast *brmctx = entry->brmctx;
	unsigned char deleted_mac[ETH_ALEN];
	uint16_t vid = entry->vid;
	int ifindex = entry->ifindex;

	/* Save MAC address for shared_mac recomputation */
	ether_addr_copy(deleted_mac, entry->mac);

	/* Recompute shared_mac flag for remaining entries with same MAC */
	if (brmctx && brmctx->br) {
		spin_lock_bh(&brmctx->br->multicast_lock);
		br_mcast_offload_recompute_shared_mac(brmctx, deleted_mac, vid, ifindex, false);
		spin_unlock_bh(&brmctx->br->multicast_lock);
	}

	/* Free the entry */
	kfree(entry);

	pr_debug("Mac entry %pM removed from ip mac map\n", deleted_mac);
}

/*
 * br_multicast_ip_mac_map_timer - Timer callback for IP-MAC map entry expiration
 * @t: Timer that expired
 *
 * This function is called when an IP-MAC mapping entry expires.
 * It removes the entry from the hash table using RCU and schedules deferred cleanup.
 */
static void br_mcast_offload_map_timer(struct timer_list *t)
{
	struct net_bridge_mcast *brmctx = from_timer(brmctx, t, ip_mac_map_timer);
	struct net_bridge *br;
	struct net_bridge_ip_to_mac *entry;
	struct hlist_node *tmp;
	unsigned int i;
	bool has_entries = false;

	if (!brmctx || !brmctx->br)
		return;

	br = brmctx->br;

	spin_lock_bh(&br->multicast_lock);

	for (i = 0; i < BR_IP_MAC_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(entry, tmp, &brmctx->ip_mac_map[i], hlist) {
			if (hlist_unhashed(&entry->hlist))
				continue;

			if (time_after_eq(jiffies, entry->time + br_multicast_gmi(brmctx))) {
				hlist_del_init_rcu(&entry->hlist);
				br_mcast_offload_free_entry(entry);
			} else {
				has_entries = true;
			}
		}
	}

	spin_unlock_bh(&br->multicast_lock);

	if (has_entries)
		mod_timer(&brmctx->ip_mac_map_timer, jiffies + BR_MCAST_OFFLOAD_MAP_TIMER_INTVL);
}

/*
 * br_mcast_offload_ip_mac_map_add
 *	Store the host ip and corresponding host mac address in the db
 */
int br_mcast_offload_ip_mac_map_add(struct net_bridge_mcast *brmctx,
				    const struct br_ip *host,
				    const unsigned char *mac,
				    int ifindex,
				    uint16_t vid)
{
	struct net_bridge_ip_to_mac *ip_mac_entry, *tmp;
	unsigned int hash;

	/* Calculate hash based on IP address */
	hash = br_mcast_offload_map_hash(host);

	spin_lock_bh(&brmctx->br->multicast_lock);

	if (!netif_running(brmctx->br->dev)) {
		spin_unlock_bh(&brmctx->br->multicast_lock);
		return -EINVAL;
	}

	hlist_for_each_entry_rcu(tmp, &brmctx->ip_mac_map[hash], hlist) {
		if (tmp->ip_proto != host->proto)
			continue;

		switch (host->proto) {
		case htons(ETH_P_IP):
			if ((tmp->addr.ip == host->src.ip4) && (tmp->vid == vid) &&
			    (tmp->ifindex == ifindex)) {
				/* Entry exists - update the entry time */
				tmp->time = jiffies;
				if (!timer_pending(&brmctx->ip_mac_map_timer))
					mod_timer(&brmctx->ip_mac_map_timer, jiffies + BR_MCAST_OFFLOAD_MAP_TIMER_INTVL);
				spin_unlock_bh(&brmctx->br->multicast_lock);
				return 0;
			}
			break;
#if IS_ENABLED(CONFIG_IPV6)
		case htons(ETH_P_IPV6):
			if (ipv6_addr_equal(&tmp->addr.in6, &host->src.ip6) &&
			    (tmp->vid == vid) &&
			    (tmp->ifindex == ifindex)) {
				/* Entry exists - update the entry time */
				tmp->time = jiffies;
				if (!timer_pending(&brmctx->ip_mac_map_timer))
					mod_timer(&brmctx->ip_mac_map_timer, jiffies + BR_MCAST_OFFLOAD_MAP_TIMER_INTVL);
				spin_unlock_bh(&brmctx->br->multicast_lock);
				return 0;
			}
			break;
#endif
		default:
			pr_debug("%px:Invalid ethernet protocol\n", brmctx);
			spin_unlock_bh(&brmctx->br->multicast_lock);
			return -EINVAL;
		}
	}

	/*
	 * Insert a new IP to MAC mapping
	 */
	ip_mac_entry = kzalloc(sizeof(*ip_mac_entry), GFP_ATOMIC);
	if (!ip_mac_entry) {
		pr_debug("%px:Failed to allocate memory for ip mac entry\n", brmctx);
		spin_unlock_bh(&brmctx->br->multicast_lock);
		return -ENOMEM;
	}

	switch (host->proto) {
	case htons(ETH_P_IP):
		ip_mac_entry->addr.ip = host->src.ip4;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		ip_mac_entry->addr.in6 = host->src.ip6;
		break;
#endif
	default:
		pr_debug("%px:Invalid ethernet protocol\n", brmctx);
		kfree(ip_mac_entry);
		spin_unlock_bh(&brmctx->br->multicast_lock);
		return -EINVAL;
	}

	ip_mac_entry->ip_proto = host->proto;
	ether_addr_copy(ip_mac_entry->mac, mac);
	ip_mac_entry->vid = vid;
	ip_mac_entry->ifindex = ifindex;
	ip_mac_entry->brmctx = brmctx;
	ip_mac_entry->shared_mac = false;

	ip_mac_entry->time = jiffies;
	if (!timer_pending(&brmctx->ip_mac_map_timer))
		mod_timer(&brmctx->ip_mac_map_timer, jiffies + BR_MCAST_OFFLOAD_MAP_TIMER_INTVL);

	hlist_add_head_rcu(&ip_mac_entry->hlist, &brmctx->ip_mac_map[hash]);

	/*
	 * Check if this MAC is shared with other IPs
	 */
	br_mcast_offload_recompute_shared_mac(brmctx, ip_mac_entry->mac,
					      ip_mac_entry->vid, ip_mac_entry->ifindex, true);

	spin_unlock_bh(&brmctx->br->multicast_lock);
	return 0;
}

/*
 * br_multicast_ip_mac_map_lookup
 * 	Lookup the host mac address from the given host ip
 */
int br_mcast_offload_ip_map_lookup(struct net_bridge_mcast *brmctx,
				       const struct br_ip *host,
				       int ifindex, uint16_t vid,
				       unsigned char *mac_out, bool *shared_mac)
{
	struct net_bridge_ip_to_mac *ip_mac_entry;
	unsigned int hash;
	int ret = -ENOENT;

	if (!host || !mac_out) {
		pr_debug("%px:Invalid host/ mac\n", brmctx);
		return -EINVAL;
	}

	/* Calculate hash idx based on IP address */
	hash = br_mcast_offload_map_hash(host);

	hlist_for_each_entry_rcu(ip_mac_entry, &brmctx->ip_mac_map[hash], hlist) {
		if (ip_mac_entry->ip_proto != host->proto)
			continue;

		switch (host->proto) {
		case htons(ETH_P_IP):
			if ((ip_mac_entry->addr.ip == host->src.ip4) &&
			    (ip_mac_entry->vid == vid) &&
			    (ip_mac_entry->ifindex == ifindex)) {
				ether_addr_copy(mac_out, ip_mac_entry->mac);
				*shared_mac = ip_mac_entry->shared_mac;
				ret = 0;
				goto exit;
			}
			break;
#if IS_ENABLED(CONFIG_IPV6)
		case htons(ETH_P_IPV6):
			if (ipv6_addr_equal(&ip_mac_entry->addr.in6, &host->src.ip6) &&
			    (ip_mac_entry->vid == vid) &&
			    (ip_mac_entry->ifindex == ifindex)) {
				ether_addr_copy(mac_out, ip_mac_entry->mac);
				*shared_mac = ip_mac_entry->shared_mac;
				ret = 0;
				goto exit;
			}
			break;
#endif
		default:
			break;
		}
	}

exit:
	return ret;
}

/*
 * br_multicast_get_br_ip
 *	API to fetch the ip address from skb
 */
void br_mcast_offload_get_br_ip(const struct sk_buff *skb, struct br_ip *host)
{
	if (skb->protocol == htons(ETH_P_IP)) {
		host->proto = htons(ETH_P_IP);
		host->src.ip4 = ip_hdr(skb)->saddr;
	}
#if IS_ENABLED(CONFIG_IPV6)
	else if (skb->protocol == htons(ETH_P_IPV6)) {
		host->proto = htons(ETH_P_IPV6);
		host->src.ip6 = ipv6_hdr(skb)->saddr;
	}
#endif
}

/*
 * br_mcast_offload_map_add
 *	Host ip and mac address mapping
 */
void br_mcast_offload_map_add(struct net_bridge_mcast_port *pmctx, struct net_bridge_mcast *brmctx,
		struct sk_buff *skb,
		const unsigned char *mac,
		uint16_t vid)
{
	struct net_bridge *br = brmctx->br;
	struct br_ip host = {0};
	int ifindex, ret = 0;

	if ((pmctx) && (pmctx->port->flags & BR_MCAST_MCUC_HW_OFFLOAD)) {
		ifindex = pmctx->port->dev->ifindex;
		br_mcast_offload_get_br_ip(skb, &host);
		ret = br_mcast_offload_ip_mac_map_add(&br->multicast_ctx, &host, mac, ifindex, vid);
		if (ret) {
			pr_debug("br_mcast_offload: Failed to add IP-MAC mapping: %d\n", ret);
		}
	}

	return;
}

/*
 * br_mcast_offload_free_entry - schedule deferred free for an IP-MAC entry
 */
void br_mcast_offload_free_entry(struct net_bridge_ip_to_mac *entry)
{
	call_rcu(&entry->rcu, br_mcast_offload_map_deferred_cleanup);
}

/*
 * br_mcast_offload_cleanup_ip_mac_map - cleanup all IP-MAC entries in map
 * Implements the restart approach to avoid use-after-free races and
 * processes one entry at a time to avoid stale 'next' pointers.
 */
void br_mcast_offload_cleanup_map(struct net_bridge_mcast *brmctx)
{
	struct net_bridge_ip_to_mac *ip_mac_entry;
	struct hlist_node *tmp;
	unsigned int i;

	timer_shutdown_sync(&brmctx->ip_mac_map_timer);

	spin_lock_bh(&brmctx->br->multicast_lock);

	for (i = 0; i < BR_IP_MAC_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(ip_mac_entry, tmp, &brmctx->ip_mac_map[i], hlist) {
			hlist_del_init_rcu(&ip_mac_entry->hlist);
			br_mcast_offload_free_entry(ip_mac_entry);
		}
	}

	spin_unlock_bh(&brmctx->br->multicast_lock);
}

/*
 * br_mcast_offload_cleanup_shared_mac_states
 *	Cleanup all shared MAC states
 */
void br_mcast_offload_cleanup_shared_mac_states(struct net_bridge_mcast *brmctx)
{
	struct br_mcast_shared_mac_state *state;
	struct hlist_node *tmp;
	unsigned int i;

	for (i = 0; i < BR_SHARED_MAC_STATE_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(state, tmp, &brmctx->shared_mac_state[i], hlist) {
			hlist_del_rcu(&state->hlist);
			kfree_rcu(state, rcu);
		}
	}
 }

/*
 * br_mcast_offload_init_ip_mac_map - initialize IP-MAC map buckets
 */
void br_mcast_offload_init_map(struct net_bridge_mcast *brmctx)
{
	unsigned int i;

	for (i = 0; i < BR_IP_MAC_HASH_SIZE; i++) {
		INIT_HLIST_HEAD(&brmctx->ip_mac_map[i]);
	}
	timer_setup(&brmctx->ip_mac_map_timer, br_mcast_offload_map_timer, 0);
}

/*
 * br_mcast_offload_init_shared_mac_states
 *	Initialize shared MAC state hash table
 */
void br_mcast_offload_init_shared_mac_states(struct net_bridge_mcast *brmctx)
{
	unsigned int i;

	for (i = 0; i < BR_SHARED_MAC_STATE_HASH_SIZE; i++) {
		INIT_HLIST_HEAD(&brmctx->shared_mac_state[i]);
	}
}

/*
 * br_mcast_offload_send_del_event
 *	Sends Bridge MCAST Host Delete Event.
 */
static int br_mcast_offload_send_del_event(void *port_data, void *host_addr)
{
	struct net_bridge_port_group *pg = (struct net_bridge_port_group *)port_data;
	union net_bridge_eht_addr *h_addr = (union net_bridge_eht_addr *)host_addr;
	struct net_bridge *br;
	struct br_mcast_event grp_event;
	struct net_bridge_mcast *brmctx;
	struct br_ip host;
	bool shared_mac;
	enum br_mcast_event_type actual_event;
	int event = 0, ret;

	memset(&grp_event, 0, sizeof(grp_event));
	memset(&host, 0, sizeof(struct br_ip));

	if (!pg || !pg->key.port || !h_addr) {
		pr_debug("Delete Send: Invalid parameters in send_del_event\n");
		return -EINVAL;
	}

	br = pg->key.port->br;
	if (!br) {
		pr_debug("Delete Send : Unable to fetch the net_bridge pointer\n");
		return -EINVAL;
	}

	/*
	 * Dont send the DEL Event if FLUSH ALL Event
	 * is already triggered for this port.
	 */
	if (pg->key.port->mcast_flush_all) {
		pr_debug("Delete Send: Flush ALL event already triggered for this PORT\n");
		return 0;
	}

	brmctx = &br->multicast_ctx;
	host.proto = pg->key.addr.proto;
	memcpy(&host.src, h_addr, sizeof(host.src));

	/*
	 * Fetch the Host's MAC address.
	 */
	if (br_mcast_offload_ip_map_lookup(brmctx, &host,
					   pg->key.port->dev->ifindex,
					   pg->key.addr.vid,
					   grp_event.host_mac, &shared_mac)) {
		pr_debug("Delete Send: Unable to look up the HOST MAC address from the db\n");
		return -EINVAL;
	}

	grp_event.ifindex = pg->key.port->dev->ifindex;
	if (pg->key.addr.proto == htons(ETH_P_IP)) {
		grp_event.is_v4 = true;
		grp_event.grp_ip.ip = pg->key.addr.dst.ip4;
#if IS_ENABLED(CONFIG_IPV6)
	} else {
		grp_event.is_v4 = false;
		grp_event.grp_ip.in6 = pg->key.addr.dst.ip6;
#endif
	}

	if (shared_mac) {
		/* Handle shared MAC - consolidate all hosts */
		struct br_mcast_shared_mac_state *state;

		pr_debug("Delete Send: Shared MAC is true\n");
		ret = br_mcast_offload_update_shared_mac_state(pg, grp_event.host_mac, &grp_event.grp_ip,
								pg->key.addr.proto, pg->key.addr.vid,
								grp_event.ifindex, &actual_event);
		if (ret)
			return ret;

		if (actual_event == BR_MCAST_EVENT_NONE) {
			pr_debug("Delete Send: Don't send notification; No change to the consolidated state\n");
			return 0;	/* No change, don't send notification */
                }

		/* Get the consolidated state */
		state = br_mcast_offload_shared_mac_state_lookup(brmctx, grp_event.host_mac, pg->key.addr.proto,
							&grp_event.grp_ip, pg->key.addr.vid, grp_event.ifindex);

		if (!state && actual_event != BR_MCAST_EVENT_DEL) {
			pr_debug("Delete Send: Failed to find shared MAC state\n");
			return -ENOENT;
		}

		if (actual_event == BR_MCAST_EVENT_DEL) {
			/* All hosts left - send DEL */
			grp_event.src_filter = BR_MCAST_SRCLIST_INCLUDE;
			grp_event.src_cnt = 0;
			event = BR_MCAST_EVENT_DEL;
		} else {
			/* Send consolidated state */
			if ((state->filter_mode == BR_MCAST_SRCLIST_EXCLUDE) && (!state->src_cnt)) {
				grp_event.src_filter = BR_MCAST_SRCLIST_IGNORE;
				grp_event.src_cnt = 0;
			} else {
				grp_event.src_filter = state->filter_mode;
				grp_event.src_cnt = state->src_cnt;
				memcpy(grp_event.src_list, state->src_list, state->src_cnt * sizeof(union nf_inet_addr));
			}

			event = actual_event;
		}
	} else {
		/* Single host - use existing logic */
		grp_event.src_filter = BR_MCAST_SRCLIST_INCLUDE;
		grp_event.src_cnt = 0;
		event = pg->eht_event;
	}

	/*
	 * This Notifier is invoked inside a spinlock.
	 * The Receiver of this notification is expected to not call sleeping functions.
	 */
	atomic_notifier_call_chain(&br_mcast_event_notifier_list, event, (void *)&grp_event);
	return 0;
}

/*
 * br_mcast_offload_send_update_event
 *	Sends Bridge MCAST Host Add/Update Event with shared MAC support
 */
static int br_mcast_offload_send_update_event(void *port_data, void *host_addr)
{
	struct net_bridge_port_group *pg = (struct net_bridge_port_group *)port_data;
	union net_bridge_eht_addr *h_addr = (union net_bridge_eht_addr *)host_addr;
	struct net_bridge *br;
	struct br_mcast_event grp_event;
	struct net_bridge_mcast *brmctx;
	struct br_ip host;
	bool shared_mac = false;
	enum br_mcast_event_type actual_event;
	int event = 0, ret;

	memset(&grp_event, 0, sizeof(grp_event));
	memset(&host, 0, sizeof(struct br_ip));

	if (!pg || !pg->key.port || !h_addr) {
		pr_debug("Update Send: Invalid parameters in send_update_event\n");
		return -EINVAL;
	}

	br = pg->key.port->br;
	if (!br) {
		pr_debug("%px: Update Send: Unable to fetch the net_bridge pointer\n", pg);
		return -EINVAL;
	}

	brmctx = &br->multicast_ctx;
	host.proto = pg->key.addr.proto;
	memcpy(&host.src, h_addr, sizeof(host.src));

	/* Fetch the Host's MAC address */
	if (br_mcast_offload_ip_map_lookup(brmctx, &host, pg->key.port->dev->ifindex, pg->key.addr.vid,
						grp_event.host_mac, &shared_mac)) {
		pr_debug("Update Send: Unable to look up the HOST MAC address from the db\n");
		return -EINVAL;
	}

	grp_event.ifindex = pg->key.port->dev->ifindex;
	if (pg->key.addr.proto == htons(ETH_P_IP)) {
		grp_event.is_v4 = true;
		grp_event.grp_ip.ip = pg->key.addr.dst.ip4;
#if IS_ENABLED(CONFIG_IPV6)
	} else {
		grp_event.is_v4 = false;
		grp_event.grp_ip.in6 = pg->key.addr.dst.ip6;
#endif
	}

	if (shared_mac) {
		/* Handle shared MAC - consolidate all hosts */
		struct br_mcast_shared_mac_state *state;

		pr_debug("Update Send: Shared MAC is true\n");
		ret = br_mcast_offload_update_shared_mac_state(pg, grp_event.host_mac, &grp_event.grp_ip,
								pg->key.addr.proto, pg->key.addr.vid,
								grp_event.ifindex, &actual_event);
		if (ret)
			return ret;

		if (actual_event == BR_MCAST_EVENT_NONE) {
			pr_debug("Update Send: Don't send notification; No change to the consolidated state\n");
			return 0;	/* No change, don't send notification */
		}

		/* Get the consolidated state */
		state = br_mcast_offload_shared_mac_state_lookup(brmctx, grp_event.host_mac, pg->key.addr.proto,
							&grp_event.grp_ip, pg->key.addr.vid, grp_event.ifindex);

		if (!state && actual_event != BR_MCAST_EVENT_DEL) {
			pr_debug("Update Send: Failed to find shared MAC state\n");
			return -ENOENT;
		}

		if (actual_event == BR_MCAST_EVENT_DEL) {
			/* All hosts left - Invalid scenario */
			pr_debug("Update Send: Invalid Event type generated for Host Add/Update operations\n");
			return -EINVAL;
		} else {
			/* Send consolidated state */
			if ((state->filter_mode == BR_MCAST_SRCLIST_EXCLUDE) && (!state->src_cnt)) {
				grp_event.src_filter = BR_MCAST_SRCLIST_IGNORE;
				grp_event.src_cnt = 0;
			} else {
				grp_event.src_filter = state->filter_mode;
				grp_event.src_cnt = state->src_cnt;
				memcpy(grp_event.src_list, state->src_list, state->src_cnt * sizeof(union nf_inet_addr));
			}

			event = actual_event;
		}
	} else {
		/* Single host - use existing logic */
		struct net_bridge_group_eht_host *eht_host;
		struct net_bridge_group_eht_set_entry *entry;
		int i = 0;

		eht_host = br_multicast_eht_host_lookup(pg, h_addr);
		if (!eht_host) {
			pr_debug("Update Send: Failed to Fetch the Host entry\n");
			return -EINVAL;
		}

		if (eht_host->num_entries >= BR_MCAST_SRC_ENT_LIMIT) {
			pr_debug("Update send: Sources list for a host Exceeds Maximum Limit\n");
			return -EINVAL;
		}

		grp_event.src_cnt = eht_host->num_entries;
		switch (eht_host->filter_mode) {
		case MCAST_INCLUDE:
			grp_event.src_filter = BR_MCAST_SRCLIST_INCLUDE;
			break;
		case MCAST_EXCLUDE:
			grp_event.src_filter = eht_host->num_entries ? BR_MCAST_SRCLIST_EXCLUDE : BR_MCAST_SRCLIST_IGNORE;
			break;
		default:
			pr_debug("Update Send: Invalid Filter mode used by EHT host db\n");
			return -EINVAL;
		}

		if (pg->key.addr.proto == htons(ETH_P_IP)) {
			hlist_for_each_entry(entry, &eht_host->set_entries, host_list) {
				if (entry->eht_set->src_addr.ip4) {
					grp_event.src_list[i].ip = entry->eht_set->src_addr.ip4;
					i++;
				}
			}
#if IS_ENABLED(CONFIG_IPV6)
		} else {
			hlist_for_each_entry(entry, &eht_host->set_entries, host_list) {
				if (ipv6_addr_any(&entry->eht_set->src_addr.ip6)) {
					grp_event.src_list[i].in6 = entry->eht_set->src_addr.ip6;
					i++;
				}
			}
#endif
		}

		event = pg->eht_event;
	}

	/*
	 * This Notifier is invoked inside a spinlock.
	 * The Receiver of this notification is expected to not call sleeping functions.
	 */
	atomic_notifier_call_chain(&br_mcast_event_notifier_list, event, (void *)&grp_event);
	return 0;
}

/*
 * br_mcast_offload_send_flush_all_event
 *	Sends notificaion to flush all Hosts on the given Port.
 */
static int br_mcast_offload_send_flush_all_event(void *port_data)
{
	struct net_bridge_port *port = (struct net_bridge_port *)port_data;
	struct br_mcast_event grp_event;
	int event;

	if (!port || !port->dev) {
		pr_debug("br_mcast: Invalid port in flush_all_event\n");
		return -EINVAL;
	}

	memset(&grp_event, 0, sizeof(grp_event));
	grp_event.ifindex = port->dev->ifindex;

	event = BR_MCAST_EVENT_FLUSH_ALL;

	/*
	 * This Notifier is invoked inside a spinlock.
	 * The Receiver of this notification is expected to not call sleeping functions.
	 */
	atomic_notifier_call_chain(&br_mcast_event_notifier_list, event, (void *)&grp_event);
	return 0;
}

/*
 * br_mcast_offload_send_event
 * 	Send notification to wifi driver
 */
void br_mcast_offload_send_event(void *port_data, void *host_addr, enum br_mcast_event_type event)
{
	int err = 0;

	switch (event) {
	case BR_MCAST_EVENT_NONE:
		break;
	case BR_MCAST_EVENT_DEL:
		err = br_mcast_offload_send_del_event(port_data, host_addr);
		break;
	case BR_MCAST_EVENT_ADD:
	case BR_MCAST_EVENT_UPDATE:
		err = br_mcast_offload_send_update_event(port_data, host_addr);
		break;
	case BR_MCAST_EVENT_FLUSH_ALL:
		err = br_mcast_offload_send_flush_all_event(port_data);
		break;
	default:
		pr_debug("br_mcast: Unknown event type %d\n", event);
		err = -EINVAL;
		break;
	}

	if (err) {
		pr_debug("br_multicast: Failed to Send Bridge MCAST notification(%d)\n", err);
	}

	return;
}

int br_mcast_offload_handle_igmpv2_report(struct net_bridge_mcast *brmctx,
					  struct net_bridge_mcast_port *pmctx,
					  struct sk_buff *skb,
					  u16 vid)
{
	struct net_bridge_mdb_entry *mdst;
	struct net_bridge_port_group *pg;
	const unsigned char *src;
	struct igmphdr *ih;
	struct br_ip br_group = {};
	bool changed = false;
	int err = 0;

	if (!pmctx)
		return 0;

	ih = igmp_hdr(skb);
	src = eth_hdr(skb)->h_source;

	/* Add group via core helper to avoid duplicating join logic */
	err = br_ip4_multicast_add_group(brmctx, pmctx, ih->group, vid, src, true);
	if (err)
		return err;

	/* EHT-based processing for IGMPv2 semantics */
	spin_lock(&brmctx->br->multicast_lock);
	if (!br_multicast_ctx_should_use(brmctx, pmctx))
		goto unlock;

	memset(&br_group, 0, sizeof(br_group));
	br_group.dst.ip4 = ih->group;
	br_group.proto = htons(ETH_P_IP);
	br_group.vid = vid;

	mdst = br_mdb_ip_get(brmctx->br, &br_group);
	if (!mdst)
		goto unlock;

	/* Inline match equivalent to br_port_group_equal */
	for (pg = mlock_dereference(mdst->ports, brmctx->br);
	     pg; pg = mlock_dereference(pg->next, brmctx->br)) {
		if (pg->key.port != pmctx->port)
			continue;
		if (!(pmctx->port->flags & BR_MULTICAST_TO_UNICAST) ||
		    ether_addr_equal(src, pg->eth_addr))
			break;
	}

	if (!pg || (pg->flags & MDB_PG_FLAGS_PERMANENT))
		goto unlock;

	if (br_multicast_eht_handle(brmctx, pg, &ip_hdr(skb)->saddr, NULL, 0,
				    sizeof(__be32), IGMPV3_MODE_IS_EXCLUDE))
		changed = true;

	if (changed)
		br_mdb_notify(brmctx->br->dev, mdst, pg, RTM_NEWMDB);

unlock:
	spin_unlock(&brmctx->br->multicast_lock);
	return err;
}

void br_mcast_offload_handle_igmpv2_leave(struct net_bridge_mcast *brmctx,
					  struct net_bridge_mcast_port *pmctx,
					  struct sk_buff *skb,
					  __be32 group,
					  __u16 vid,
					  const unsigned char *src)
{
	struct net_bridge_mdb_entry *mdst;
	struct net_bridge_port_group *pg;
	struct br_ip br_group = {};
	bool changed = false;

	if (!pmctx)
		return;

	/* Use core leave helper to avoid duplicating leave logic */
	br_ip4_multicast_leave_group(brmctx, pmctx, group, vid, src);

	spin_lock(&brmctx->br->multicast_lock);
	if (!br_multicast_ctx_should_use(brmctx, pmctx))
		goto unlock;

	memset(&br_group, 0, sizeof(br_group));
	br_group.dst.ip4 = group;
	br_group.proto = htons(ETH_P_IP);
	br_group.vid = vid;

	mdst = br_mdb_ip_get(brmctx->br, &br_group);
	if (!mdst)
		goto unlock;

	for (pg = mlock_dereference(mdst->ports, brmctx->br);
	     pg; pg = mlock_dereference(pg->next, brmctx->br)) {
		if (pg->key.port != pmctx->port)
			continue;
		if (!(pmctx->port->flags & BR_MULTICAST_TO_UNICAST) ||
		    ether_addr_equal(src, pg->eth_addr))
			break;
	}

	if (!pg || (pg->flags & MDB_PG_FLAGS_PERMANENT))
		goto unlock;

	/* Simulate IGMPv3 CHANGE_TO_INCLUDE {} for IGMPv2 leave */
	if (br_multicast_eht_handle(brmctx, pg, &ip_hdr(skb)->saddr, NULL, 0,
				    sizeof(__be32), IGMPV3_CHANGE_TO_INCLUDE))
		changed = true;

	if (changed)
		br_mdb_notify(brmctx->br->dev, mdst, pg, RTM_NEWMDB);

unlock:
	spin_unlock(&brmctx->br->multicast_lock);
}

#if IS_ENABLED(CONFIG_IPV6)
int br_mcast_offload_handle_mldv1_report(struct net_bridge_mcast *brmctx,
					 struct net_bridge_mcast_port *pmctx,
					 struct sk_buff *skb,
					 u16 vid)
{
	struct net_bridge_mdb_entry *mdst;
	struct net_bridge_port_group *pg;
	const unsigned char *src;
	struct mld_msg *mld;
	struct br_ip br_group = {};
	bool changed = false;
	int err = 0;

	if (!pmctx)
		return 0;

	mld = (struct mld_msg *)skb_transport_header(skb);
	src = eth_hdr(skb)->h_source;

	/* Add group via core helper to avoid duplicating join logic */
	err = br_ip6_multicast_add_group(brmctx, pmctx, &mld->mld_mca, vid, src, true);
	if (err)
		return err;

	spin_lock(&brmctx->br->multicast_lock);
	if (!br_multicast_ctx_should_use(brmctx, pmctx))
		goto unlock;

	memset(&br_group, 0, sizeof(br_group));
	br_group.dst.ip6 = mld->mld_mca;
	br_group.proto = htons(ETH_P_IPV6);
	br_group.vid = vid;

	mdst = br_mdb_ip_get(brmctx->br, &br_group);
	if (!mdst)
		goto unlock;

	for (pg = mlock_dereference(mdst->ports, brmctx->br);
	     pg; pg = mlock_dereference(pg->next, brmctx->br)) {
		if (pg->key.port != pmctx->port)
			continue;
		if (!(pmctx->port->flags & BR_MULTICAST_TO_UNICAST) ||
		    ether_addr_equal(src, pg->eth_addr))
			break;
	}

	if (!pg || (pg->flags & MDB_PG_FLAGS_PERMANENT))
		goto unlock;

	if (br_multicast_eht_handle(brmctx, pg, &ipv6_hdr(skb)->saddr, NULL, 0,
				    sizeof(struct in6_addr),
				    MLD2_MODE_IS_EXCLUDE))
		changed = true;

	if (changed)
		br_mdb_notify(brmctx->br->dev, mdst, pg, RTM_NEWMDB);

unlock:
	spin_unlock(&brmctx->br->multicast_lock);
	return 0;
}

void br_mcast_offload_handle_mldv1_leave(struct net_bridge_mcast *brmctx,
					 struct net_bridge_mcast_port *pmctx,
					 struct sk_buff *skb,
					 const struct in6_addr *group,
					 __u16 vid,
					 const unsigned char *src)
{
	struct net_bridge_mdb_entry *mdst;
	struct net_bridge_port_group *pg;
	struct br_ip br_group = {};
	bool changed = false;

	if (!pmctx)
		return;

	/* Use core leave helper to avoid duplicating leave logic */
	br_ip6_multicast_leave_group(brmctx, pmctx, group, vid, src);

	spin_lock(&brmctx->br->multicast_lock);
	if (!br_multicast_ctx_should_use(brmctx, pmctx))
		goto unlock;

	memset(&br_group, 0, sizeof(br_group));
	br_group.dst.ip6 = *group;
	br_group.proto = htons(ETH_P_IPV6);
	br_group.vid = vid;

	mdst = br_mdb_ip_get(brmctx->br, &br_group);
	if (!mdst)
		goto unlock;

	for (pg = mlock_dereference(mdst->ports, brmctx->br);
	     pg; pg = mlock_dereference(pg->next, brmctx->br)) {
		if (pg->key.port != pmctx->port)
			continue;
		if (!(pmctx->port->flags & BR_MULTICAST_TO_UNICAST) ||
		    ether_addr_equal(src, pg->eth_addr))
			break;
	}

	if (!pg || (pg->flags & MDB_PG_FLAGS_PERMANENT))
		goto unlock;

	/* Simulate MLDv2 CHANGE_TO_INCLUDE {} for MLDv1 leave */
	if (br_multicast_eht_handle(brmctx, pg, &ipv6_hdr(skb)->saddr, NULL, 0,
				    sizeof(struct in6_addr),
				    MLD2_CHANGE_TO_INCLUDE))
		changed = true;

	if (changed)
		br_mdb_notify(brmctx->br->dev, mdst, pg, RTM_NEWMDB);

unlock:
	spin_unlock(&brmctx->br->multicast_lock);
}
#endif /* IS_ENABLED(CONFIG_IPV6) */

static void br_mcast_rule_free_rcu(struct rcu_head *rcu)
{
	struct br_mcast_rule *r = container_of(rcu, struct br_mcast_rule, rcu);

	kfree(r);
}

bool br_mcast_rule_check_ip4(struct net_bridge *br, __be32 group)
{
	struct br_mcast_rule *r;
	bool hit = false;

	rcu_read_lock();
	hlist_for_each_entry_rcu(r, &br->mcast_rule_list, hnode) {
		if (r->key.proto == htons(ETH_P_IP) &&
		    r->key.dst.ip4 == group &&
		    r->action == 1) {
			hit = true;
			break;
		}
	}
	rcu_read_unlock();

	return hit;
}

#if IS_ENABLED(CONFIG_IPV6)
bool br_mcast_rule_check_ip6(struct net_bridge *br, const struct in6_addr *group)
{
	struct br_mcast_rule *r;
	bool hit = false;

	rcu_read_lock();
	hlist_for_each_entry_rcu(r, &br->mcast_rule_list, hnode) {
		if (r->key.proto == htons(ETH_P_IPV6) &&
		    ipv6_addr_equal(&r->key.dst.ip6, group) &&
		    r->action == 1) {
			hit = true;
			break;
		}
	}
	rcu_read_unlock();

	return hit;
}
#endif

bool br_mcast_offload_should_force_flood(struct net_bridge *br, struct sk_buff *skb, struct net_bridge_mdb_entry *mdst)
{
	if (!mdst) {
		if (skb->protocol == htons(ETH_P_IP)) {
			if (br_mcast_rule_check_ip4(br, ip_hdr(skb)->daddr))
				return true;
#if IS_ENABLED(CONFIG_IPV6)
		} else if (skb->protocol == htons(ETH_P_IPV6)) {
			if (br_mcast_rule_check_ip6(br, &ipv6_hdr(skb)->daddr))
				return true;
#endif
		}
	}

	return false;
}

int br_mcast_rule_add(struct net_bridge *br, __be16 proto,
		      const void *group, size_t len, u8 action)
{
	struct br_mcast_rule *r, *iter;
	int ret = 0;

	if (action != 1)
		return -EOPNOTSUPP;

	/*
	 * Validate protocol and group length prior to allocation to avoid leaks
	 */
	if (proto == htons(ETH_P_IP)) {
		if (len != sizeof(__be32))
			return -EINVAL;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (proto == htons(ETH_P_IPV6)) {
		if (len != sizeof(struct in6_addr))
			return -EINVAL;
#endif
	} else {
		return -EINVAL;
	}

	spin_lock_bh(&br->multicast_lock);

	hlist_for_each_entry(iter, &br->mcast_rule_list, hnode) {
		if (iter->key.proto != proto)
			continue;

		if (proto == htons(ETH_P_IP)) {
			if (len == sizeof(__be32) &&
			    iter->key.dst.ip4 == *(__be32 *)group) {
				iter->action = action;
				goto out_unlock;
			}
		}
#if IS_ENABLED(CONFIG_IPV6)
		else if (proto == htons(ETH_P_IPV6)) {
			if (len == sizeof(struct in6_addr) &&
			    ipv6_addr_equal(&iter->key.dst.ip6,
					    (const struct in6_addr *)group)) {
				iter->action = action;
				goto out_unlock;
			}
		}
#endif
	}

	r = kzalloc(sizeof(*r), GFP_ATOMIC);
	if (!r) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	r->key.proto = proto;
	if (proto == htons(ETH_P_IP) && len == sizeof(__be32)) {
		r->key.dst.ip4 = *(__be32 *)group;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (proto == htons(ETH_P_IPV6) &&
		   len == sizeof(struct in6_addr)) {
		r->key.dst.ip6 = *(const struct in6_addr *)group;
#endif
	} else {
		kfree(r);
		ret = -EINVAL;
		goto out_unlock;
	}

	r->action = action;
	hlist_add_head_rcu(&r->hnode, &br->mcast_rule_list);

out_unlock:
	spin_unlock_bh(&br->multicast_lock);
	return ret;
}

int br_mcast_rule_del(struct net_bridge *br, __be16 proto,
		      const void *group, size_t len)
{
	struct br_mcast_rule *iter;
	int ret = -ENOENT;

	spin_lock_bh(&br->multicast_lock);

	hlist_for_each_entry(iter, &br->mcast_rule_list, hnode) {
		if (iter->key.proto != proto)
			continue;

		if (proto == htons(ETH_P_IP)) {
			if (len == sizeof(__be32) &&
			    iter->key.dst.ip4 == *(__be32 *)group) {
				hlist_del_rcu(&iter->hnode);
				call_rcu(&iter->rcu, br_mcast_rule_free_rcu);
				ret = 0;
				break;
			}
		}
#if IS_ENABLED(CONFIG_IPV6)
		else if (proto == htons(ETH_P_IPV6)) {
			if (len == sizeof(struct in6_addr) &&
			    ipv6_addr_equal(&iter->key.dst.ip6,
					    (const struct in6_addr *)group)) {
				hlist_del_rcu(&iter->hnode);
				call_rcu(&iter->rcu, br_mcast_rule_free_rcu);
				ret = 0;
				break;
			}
		}
#endif
	}

	spin_unlock_bh(&br->multicast_lock);
	return ret;
}

void br_mcast_rule_flush(struct net_bridge *br)
{
	struct br_mcast_rule *r;
	struct hlist_node *tmp;

	spin_lock_bh(&br->multicast_lock);
	hlist_for_each_entry_safe(r, tmp, &br->mcast_rule_list, hnode) {
		hlist_del_rcu(&r->hnode);
		call_rcu(&r->rcu, br_mcast_rule_free_rcu);
	}
	spin_unlock_bh(&br->multicast_lock);
}

int br_mcastrule_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct br_mcast_rule_msg *msg;
	struct net_device *dev;
	struct net_bridge *br;
	struct br_mcast_rule *r;
	__be16 proto;
	int idx = 0, s_idx = cb->args[0];
	int err = 0;
	struct nlmsghdr *nlh = NULL;
	struct nlattr *nest = NULL;

	if (cb->nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(*msg))
		return -EINVAL;

	msg = nlmsg_data(cb->nlh);

	switch (msg->family) {
	case AF_INET:
		proto = htons(ETH_P_IP);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		proto = htons(ETH_P_IPV6);
		break;
#endif
	case AF_UNSPEC:
		/* Dump all protocols when family is unspecified */
		proto = 0;
		break;
	case AF_BRIDGE:
		/* iproute2 'bridge mcast-rule show' uses AF_BRIDGE/PF_BRIDGE here */
		proto = 0;
		break;
	default:
		return -EAFNOSUPPORT;
	}

	{
		struct net *net = sock_net(cb->skb->sk);

		if (msg->ifindex) {
			/* Single device specified */
			dev = __dev_get_by_index(net, msg->ifindex);
			if (!dev)
				return -ENODEV;
			if (!netif_is_bridge_master(dev))
				return -EINVAL;

			br = netdev_priv(dev);

			rcu_read_lock();
			hlist_for_each_entry_rcu(r, &br->mcast_rule_list, hnode) {
				struct br_mcast_rule_msg *out;

				/* If proto filter is set, skip non-matching entries */
				if (proto && r->key.proto != proto)
					continue;

				if (idx++ < s_idx)
					continue;

				nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid,
						cb->nlh->nlmsg_seq, cb->nlh->nlmsg_type,
						sizeof(*out), NLM_F_MULTI);
				if (!nlh) {
					err = -EMSGSIZE;
					break;
				}

				out = nlmsg_data(nlh);
				memset(out, 0, sizeof(*out));
				/* Report family per entry when dumping all */
				if (r->key.proto == htons(ETH_P_IP))
					out->family = AF_INET;
#if IS_ENABLED(CONFIG_IPV6)
				else if (r->key.proto == htons(ETH_P_IPV6))
					out->family = AF_INET6;
#endif
				else
					out->family = msg->family;
				out->ifindex = dev->ifindex;

				nest = nla_nest_start_noflag(skb, BR_MCASTRULE_ENTRY);
				if (!nest)
					goto nla_err;

				if (r->key.proto == htons(ETH_P_IP)) {
					if (nla_put(skb, BR_MCASTRULE_ENTRY_GROUP,
						    sizeof(__be32), &r->key.dst.ip4))
						goto nla_err;
#if IS_ENABLED(CONFIG_IPV6)
				} else if (r->key.proto == htons(ETH_P_IPV6)) {
					if (nla_put(skb, BR_MCASTRULE_ENTRY_GROUP,
						    sizeof(struct in6_addr), &r->key.dst.ip6))
						goto nla_err;
#endif
				} else {
					goto nla_err;
				}

				if (nla_put_u8(skb, BR_MCASTRULE_ENTRY_ACTION, r->action))
					goto nla_err;

				nla_nest_end(skb, nest);
				nlmsg_end(skb, nlh);
			}
			rcu_read_unlock();
		} else {
			/* No device specified: iterate all bridge masters in netns */
			for_each_netdev(net, dev) {
				struct net_bridge *iter_br;

				if (!netif_is_bridge_master(dev))
					continue;

				iter_br = netdev_priv(dev);

				rcu_read_lock();
				hlist_for_each_entry_rcu(r, &iter_br->mcast_rule_list, hnode) {
					struct br_mcast_rule_msg *out;

					/* If proto filter is set, skip non-matching entries */
					if (proto && r->key.proto != proto)
						continue;

					if (idx++ < s_idx)
						continue;

					nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid,
							cb->nlh->nlmsg_seq, cb->nlh->nlmsg_type,
							sizeof(*out), NLM_F_MULTI);
					if (!nlh) {
						err = -EMSGSIZE;
						rcu_read_unlock();
						goto done_devices;
					}

					out = nlmsg_data(nlh);
					memset(out, 0, sizeof(*out));
					/* Report family per entry when dumping all */
					if (r->key.proto == htons(ETH_P_IP))
						out->family = AF_INET;
#if IS_ENABLED(CONFIG_IPV6)
					else if (r->key.proto == htons(ETH_P_IPV6))
						out->family = AF_INET6;
#endif
					else
						out->family = msg->family;
					out->ifindex = dev->ifindex;

					nest = nla_nest_start_noflag(skb, BR_MCASTRULE_ENTRY);
					if (!nest)
						goto nla_err;

					if (r->key.proto == htons(ETH_P_IP)) {
						if (nla_put(skb, BR_MCASTRULE_ENTRY_GROUP,
							    sizeof(__be32), &r->key.dst.ip4))
							goto nla_err;
#if IS_ENABLED(CONFIG_IPV6)
					} else if (r->key.proto == htons(ETH_P_IPV6)) {
						if (nla_put(skb, BR_MCASTRULE_ENTRY_GROUP,
							    sizeof(struct in6_addr), &r->key.dst.ip6))
							goto nla_err;
#endif
					} else {
						goto nla_err;
					}

					if (nla_put_u8(skb, BR_MCASTRULE_ENTRY_ACTION, r->action))
						goto nla_err;

					nla_nest_end(skb, nest);
					nlmsg_end(skb, nlh);
				}
				rcu_read_unlock();
			}
done_devices:
			;
		}
	}

	cb->args[0] = idx;

	return err;

nla_err:
	if (nest)
		nla_nest_cancel(skb, nest);
	if (nlh)
		nlmsg_cancel(skb, nlh);
	rcu_read_unlock();
	return -EMSGSIZE;
}
