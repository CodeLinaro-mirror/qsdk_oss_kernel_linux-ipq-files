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
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
#include <net/addrconf.h>
#endif

#include "br_private.h"
#include "br_mcast_offload.h"

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
			if ((tmp->vid == vid) && (tmp->ifindex == ifindex) && ether_addr_equal(tmp->mac, mac))
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
			if ((tmp->vid == vid) && (tmp->ifindex == ifindex) && ether_addr_equal(tmp->mac, mac))
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
	struct net_bridge_ip_to_mac *entry = from_timer(entry, t, timer);
	struct net_bridge_mcast *brmctx = entry->brmctx;
	struct net_bridge *br;

	if (!brmctx || !brmctx->br)
		return;

	br = brmctx->br;

	spin_lock_bh(&br->multicast_lock);

	/* Check if entry is still in hash table */
	if (hlist_unhashed(&entry->hlist)) {
		spin_unlock_bh(&br->multicast_lock);
		return;
	}

	/* Remove entry from hash table and schedule deferred cleanup */
	hlist_del_init_rcu(&entry->hlist);
	call_rcu(&entry->rcu, br_mcast_offload_map_deferred_cleanup);

	spin_unlock_bh(&br->multicast_lock);
}

/*
 * br_multicast_ip_mac_map_add
 *	Store the host ip and corresponding host mac address in the db
 */
int br_mcast_offload_map_add(struct net_bridge_mcast *brmctx,
				    const struct br_ip *host,
				    const unsigned char *mac,
				    int ifindex,
				    uint16_t vid)
{
	struct net_bridge_ip_to_mac *ip_mac_entry, *tmp;
	unsigned int hash;

	spin_lock_bh(&brmctx->br->multicast_lock);

	/* Calculate hash based on IP address */
	hash = br_mcast_offload_map_hash(host);

	hlist_for_each_entry_rcu(tmp, &brmctx->ip_mac_map[hash], hlist) {
		if (tmp->ip_proto != host->proto)
			continue;

		switch (host->proto) {
		case htons(ETH_P_IP):
			if ((tmp->vid == vid) &&
			    (tmp->addr.ip == host->src.ip4) &&
			    (tmp->ifindex == ifindex)) {
				/* Entry exists - restart the timer */
				mod_timer(&tmp->timer,
					  jiffies + br_multicast_gmi(brmctx));
				spin_unlock_bh(&brmctx->br->multicast_lock);
				return 0;
			}
			break;
#if IS_ENABLED(CONFIG_IPV6)
		case htons(ETH_P_IPV6):
			if ((tmp->vid == vid) &&
			    ipv6_addr_equal(&tmp->addr.in6, &host->src.ip6) &&
			    (tmp->ifindex == ifindex)) {
				/* Entry exists - restart the timer */
				mod_timer(&tmp->timer,
					  jiffies + br_multicast_gmi(brmctx));
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

	timer_setup(&ip_mac_entry->timer, br_mcast_offload_map_timer, 0);
	mod_timer(&ip_mac_entry->timer, jiffies + br_multicast_gmi(brmctx));

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
			if ((ip_mac_entry->vid == vid) &&
			    (ip_mac_entry->addr.ip == host->src.ip4) &&
			    (ip_mac_entry->ifindex == ifindex)) {
				ether_addr_copy(mac_out, ip_mac_entry->mac);
				*shared_mac = ip_mac_entry->shared_mac;
				ret = 0;
				goto exit;
			}
			break;
#if IS_ENABLED(CONFIG_IPV6)
		case htons(ETH_P_IPV6):
			if ((ip_mac_entry->vid == vid) &&
			    ipv6_addr_equal(&ip_mac_entry->addr.in6, &host->src.ip6) &&
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
	memset(host, 0, sizeof(*host));

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
	unsigned int i;

	for (i = 0; i < BR_IP_MAC_HASH_SIZE; i++) {
next:
		spin_lock_bh(&brmctx->br->multicast_lock);

		/* Always take the first entry in the bucket */
		ip_mac_entry = hlist_entry_safe(
				brmctx->ip_mac_map[i].first,
				struct net_bridge_ip_to_mac, hlist);

		if (ip_mac_entry) {
			/*
			 * Remove from hash FIRST while holding lock.
			 * This makes hlist_unhashed() return true,
			 * so timer callback will early-return safely.
			 */
			hlist_del_init_rcu(&ip_mac_entry->hlist);
			spin_unlock_bh(&brmctx->br->multicast_lock);

			/*
			 * Shutdown timer without lock.
			 * Timer callback can no longer access this entry
			 * because hlist_unhashed() now returns true.
			 */
			timer_shutdown_sync(&ip_mac_entry->timer);

			/*
			 * Schedule RCU cleanup via offload helper.
			 * Safe because timer is dead and entry is unhashed.
			 */
			br_mcast_offload_free_entry(ip_mac_entry);

			/*
			 * Process next entry in this bucket
			 */
			goto next;
		}

		spin_unlock_bh(&brmctx->br->multicast_lock);
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
}
