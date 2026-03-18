// SPDX-License-Identifier: GPL-2.0
/*
 * <:copyright-gpl
 * Copyright 2008 Broadcom Corp. All Rights Reserved.
 * Copyright (C) 2021 Allied Telesis Labs NZ
 *
 * This program is free software; you can distribute it and/or modify it
 * under the terms of the GNU General Public License (Version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.
 * :>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/in.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <net/dst.h>
#if IS_ENABLED(CONFIG_XFRM)
#include <net/xfrm.h>
#endif
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_l4proto.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_timeout.h>
#include <net/netfilter/nf_conntrack_proto_esp.h>
#include <net/netns/hash.h>
#include <linux/rhashtable.h>
#include <net/ipv6.h>
#if IS_ENABLED(CONFIG_NF_NAT)
#include <net/netfilter/nf_nat.h>
#endif

#include "nf_internals.h"

#if IS_ENABLED(CONFIG_NF_CT_NETLINK)

#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#endif

/* External sysctl to enable/disable ESP conntrack */
extern int nf_ct_esp_enabled;

/* esp_id of 0 is left for unassigned values */
#define TEMP_SPI_START 1
#define TEMP_SPI_MAX   (TEMP_SPI_START + 1024 - 1)

struct _esp_entry {
	/* linked list node for per net lookup via esp_id */
	struct list_head net_node;

       /* Hash table nodes for each required lookup
	* lnode: net->hash_mix, l_spi, l_ip, r_ip
	* rnode: net->hash_mix, r_spi, r_ip
	* incmpl_rlist: net->hash_mix, r_ip
	*/
	struct rhash_head lnode;
	struct rhash_head rnode;
	struct rhlist_head incmpl_rlist;

	u16 esp_id;

	u16 l3num;

	u32 l_spi;
	u32 r_spi;

	union nf_inet_addr l_ip;
	union nf_inet_addr r_ip;

	u32 alloc_time_jiffies;
	bool nat_done;
	struct net *net;
};

struct _esp_hkey {
	u8 l3num;
	union nf_inet_addr src_ip;
	union nf_inet_addr dst_ip;
	u32 net_hmix;
	u32 spi;
};

extern unsigned int nf_conntrack_net_id;

static struct rhashtable ltable;
static struct rhashtable rtable;
static struct rhltable incmpl_rtable;
static unsigned int esp_timeouts[ESP_CT_MAX] = {
	[ESP_CT_UNREPLIED] = 20 * HZ,
	[ESP_CT_REPLIED] = 20 * HZ,
};

static void esp_ip_addr_copy(int af, union nf_inet_addr *dst,
			     const union nf_inet_addr *src)
{
	if (af == AF_INET6)
		dst->in6 = src->in6;
	else
		dst->ip = src->ip;
}

static int esp_ip_addr_equal(int af, const union nf_inet_addr *a,
			     const union nf_inet_addr *b)
{
	if (af == AF_INET6)
		return ipv6_addr_equal(&a->in6, &b->in6);
	return a->ip == b->ip;
}

static inline struct nf_esp_net *esp_pernet(struct net *net)
{
	return &net->ct.nf_ct_proto.esp;
}

static inline void calculate_key(const u32 net_hmix, const u32 spi,
				 const u8 l3num,
				 const union nf_inet_addr *src_ip,
				 const union nf_inet_addr *dst_ip,
				 struct _esp_hkey *key)
{
	key->net_hmix = net_hmix;
	key->spi = spi;
	key->l3num = l3num;
	esp_ip_addr_copy(l3num, &key->src_ip, src_ip);
	esp_ip_addr_copy(l3num, &key->dst_ip, dst_ip);
}

static inline u32 calculate_hash(const void *data, u32 len, u32 seed)
{
	return jhash(data, len, seed);
}

static int ltable_obj_cmpfn(struct rhashtable_compare_arg *arg, const void *obj)
{
	struct _esp_hkey obj_key = {};
	const struct _esp_hkey *key = (const struct _esp_hkey *)arg->key;
	const struct _esp_entry *eobj = (const struct _esp_entry *)obj;
	u32 net_hmix = net_hash_mix(eobj->net);

	calculate_key(net_hmix, eobj->l_spi, eobj->l3num, &eobj->l_ip,
		      &eobj->r_ip, &obj_key);
	return memcmp(key, &obj_key, sizeof(struct _esp_hkey));
}

static int rtable_obj_cmpfn(struct rhashtable_compare_arg *arg, const void *obj)
{
	const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
	struct _esp_hkey obj_key = {};
	const struct _esp_hkey *key = (const struct _esp_hkey *)arg->key;
	const struct _esp_entry *eobj = (const struct _esp_entry *)obj;
	u32 net_hmix = net_hash_mix(eobj->net);

	calculate_key(net_hmix, eobj->r_spi, eobj->l3num, &any, &eobj->r_ip,
		      &obj_key);
	return memcmp(key, &obj_key, sizeof(struct _esp_hkey));
}

static int incmpl_table_obj_cmpfn(struct rhashtable_compare_arg *arg, const void *obj)
{
	const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
	struct _esp_hkey obj_key = {};
	const struct _esp_hkey *key = (const struct _esp_hkey *)arg->key;
	const struct _esp_entry *eobj = (const struct _esp_entry *)obj;
	u32 net_hmix = net_hash_mix(eobj->net);

	/* Skip the non NATed entries */
	if (!eobj->nat_done)
		return 1;

	calculate_key(net_hmix, 0, eobj->l3num, &any, &eobj->r_ip, &obj_key);

	return memcmp(key, &obj_key, sizeof(struct _esp_hkey));
}

static u32 ltable_obj_hashfn(const void *data, u32 len, u32 seed)
{
	struct _esp_hkey key = {};
	const struct _esp_entry *eobj = (const struct _esp_entry *)data;
	u32 net_hmix = net_hash_mix(eobj->net);

	calculate_key(net_hmix, eobj->l_spi, eobj->l3num, &eobj->l_ip,
		      &eobj->r_ip, &key);
	return calculate_hash(&key, len, seed);
}

static u32 rtable_obj_hashfn(const void *data, u32 len, u32 seed)
{
	const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
	struct _esp_hkey key = {};
	const struct _esp_entry *eobj = (const struct _esp_entry *)data;
	u32 net_hmix = net_hash_mix(eobj->net);

	calculate_key(net_hmix, eobj->r_spi, eobj->l3num, &any, &eobj->r_ip, &key);
	return calculate_hash(&key, len, seed);
}

static u32 incmpl_table_obj_hashfn(const void *data, u32 len, u32 seed)
{
	const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
	struct _esp_hkey key = {};
	const struct _esp_entry *eobj = (const struct _esp_entry *)data;
	u32 net_hmix = net_hash_mix(eobj->net);

	calculate_key(net_hmix, 0, eobj->l3num, &any, &eobj->r_ip, &key);
	return calculate_hash(&key, len, seed);
}

static const struct rhashtable_params ltable_params = {
	.key_len     = sizeof(struct _esp_hkey),
	.head_offset = offsetof(struct _esp_entry, lnode),
	.hashfn      = calculate_hash,
	.obj_hashfn = ltable_obj_hashfn,
	.obj_cmpfn   = ltable_obj_cmpfn,
};

static const struct rhashtable_params rtable_params = {
	.key_len     = sizeof(struct _esp_hkey),
	.head_offset = offsetof(struct _esp_entry, rnode),
	.hashfn      = calculate_hash,
	.obj_hashfn = rtable_obj_hashfn,
	.obj_cmpfn   = rtable_obj_cmpfn,
};

static const struct rhashtable_params incmpl_rtable_params = {
	.key_len     = sizeof(struct _esp_hkey),
	.head_offset = offsetof(struct _esp_entry, incmpl_rlist),
	.hashfn      = calculate_hash,
	.obj_hashfn = incmpl_table_obj_hashfn,
	.obj_cmpfn   = incmpl_table_obj_cmpfn,
};

int nf_conntrack_esp_init(void)
{
	int ret;

	ret = rhashtable_init(&ltable, &ltable_params);
	if (ret)
		return ret;

	ret = rhashtable_init(&rtable, &rtable_params);
	if (ret)
		goto err_free_ltable;

	ret = rhltable_init(&incmpl_rtable, &incmpl_rtable_params);
	if (ret)
		goto err_free_rtable;

	return ret;

err_free_rtable:
	rhashtable_destroy(&rtable);
err_free_ltable:
	rhashtable_destroy(&ltable);

	return ret;
}

void nf_conntrack_esp_init_net(struct net *net)
{
	int i;
	struct nf_esp_net *net_esp = esp_pernet(net);

	spin_lock_init(&net_esp->id_list_lock);
	INIT_LIST_HEAD(&net_esp->id_list);

	for (i = 0; i < ESP_CT_MAX; i++)
		net_esp->esp_timeouts[i] = esp_timeouts[i];
}

static struct _esp_entry *find_esp_entry_by_id(struct nf_esp_net *esp_net, int esp_id)
{
	struct list_head *pos, *head;
	struct _esp_entry *esp_entry;

	head = &esp_net->id_list;
	list_for_each(pos, head) {
		esp_entry = list_entry(pos, struct _esp_entry, net_node);
		if (esp_entry->esp_id == esp_id)
			return esp_entry;
	}
	return NULL;
}

static void free_esp_entry(struct nf_conntrack_net *cnet, struct _esp_entry *esp_entry)
{
	if (esp_entry) {
		/* Remove from all the hash tables */
		list_del(&esp_entry->net_node);
		rhashtable_remove_fast(&ltable, &esp_entry->lnode, ltable_params);
		rhashtable_remove_fast(&rtable, &esp_entry->rnode, rtable_params);
		rhltable_remove(&incmpl_rtable, &esp_entry->incmpl_rlist, incmpl_rtable_params);
		/* Clear the bitmap bit AFTER removing from hash tables to prevent ID reuse race */
		clear_bit(esp_entry->esp_id - TEMP_SPI_START, cnet->esp_id_map);
		kfree(esp_entry);
	}
}

/* Free an entry referred to by esp_id.
 *
 * NOTE:
 * Per net linked list locking and unlocking is the responsibility of the calling function.
 * Range checking is the responsibility of the calling function.
 */
static void free_esp_entry_by_id(struct net *net, int esp_id)
{
	struct nf_esp_net *esp_net = esp_pernet(net);
	struct nf_conntrack_net *cnet = net_generic(net, nf_conntrack_net_id);
	struct _esp_entry *esp_entry = find_esp_entry_by_id(esp_net, esp_id);

	free_esp_entry(cnet, esp_entry);
}

/* Allocate the first available IPSEC table entry.
 * NOTE: This function may block on per net list lock.
 */
struct _esp_entry *alloc_esp_entry(struct net *net)
{
	struct nf_conntrack_net *cnet = net_generic(net, nf_conntrack_net_id);
	struct nf_esp_net *esp_net = esp_pernet(net);
	struct _esp_entry *esp_entry;
	int id;

again:
	id = find_first_zero_bit(cnet->esp_id_map, 1024);
	if (id >= 1024)
		return NULL;

	if (test_and_set_bit(id, cnet->esp_id_map))
		goto again; /* raced */

	esp_entry = kmalloc(sizeof(*esp_entry), GFP_ATOMIC);
	if (!esp_entry) {
		clear_bit(id, cnet->esp_id_map);
		return NULL;
	}

	esp_entry->esp_id = id + TEMP_SPI_START;
	esp_entry->alloc_time_jiffies = nfct_time_stamp;
	esp_entry->net = net;

	spin_lock(&esp_net->id_list_lock);
	list_add(&esp_entry->net_node, &esp_net->id_list);
	spin_unlock(&esp_net->id_list_lock);

	esp_entry->nat_done = false;

	return esp_entry;
}

/* Search for an ESP entry in the initial state based on the IP address of
 * the remote peer.
 */
static struct _esp_entry *search_esp_entry_init_remote(struct net *net,
						       u16 l3num,
						       const union nf_inet_addr *src_ip,
						const union nf_inet_addr *dest_ip)
{
	const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
	u32 net_hmix = net_hash_mix(net);
	struct _esp_entry *first_esp_entry = NULL;
	struct _esp_entry *esp_entry;
	struct _esp_hkey key = {};
	struct rhlist_head *pos, *list;

	calculate_key(net_hmix, 0, l3num, &any, src_ip, &key);
	list = rhltable_lookup(&incmpl_rtable, (const void *)&key, incmpl_rtable_params);
	rhl_for_each_entry_rcu(esp_entry, pos, list, incmpl_rlist) {
		if (net_eq(net, esp_entry->net) &&
		    l3num == esp_entry->l3num &&
		    esp_ip_addr_equal(l3num, src_ip, &esp_entry->r_ip)) {
			if (!first_esp_entry) {
				first_esp_entry = esp_entry;
			} else if (first_esp_entry->alloc_time_jiffies - esp_entry->alloc_time_jiffies <= 0) {
				/* This entry is older than the last one found so treat this
				 * as a better match.
				 */
				first_esp_entry = esp_entry;
			}
		}
	}

	if (first_esp_entry) {
		if (first_esp_entry->l3num == AF_INET) {
			pr_debug("esp_entry %p Matches incmpl_rtable entry %x with l_spi %x r_ip %pI4\n",
				 esp_entry, first_esp_entry->esp_id, first_esp_entry->l_spi,
				 &first_esp_entry->r_ip.in);
		} else {
			pr_debug("esp_entry %p Matches incmpl_rtable entry %x with l_spi %x r_ip %pI6\n",
				 esp_entry, first_esp_entry->esp_id, first_esp_entry->l_spi,
				 &first_esp_entry->r_ip.in6);
		}
	}

	return first_esp_entry;
}

/* Search for an ESP entry by SPI, source and destination IP addresses.
 * NOTE: This function may block on per net list lock.
 */
static struct _esp_entry *search_esp_entry_by_spi(struct net *net, const __u32 spi,
						  u16 l3num,
						  const union nf_inet_addr *src_ip,
						  const union nf_inet_addr *dst_ip)
{
	const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
	u32 net_hmix = net_hash_mix(net);
	struct _esp_entry *esp_entry;
	struct _esp_hkey key = {};

	/* Check for matching established session or repeated initial LAN side */
	/* LAN side first */
	calculate_key(net_hmix, spi, l3num, src_ip, dst_ip, &key);
	esp_entry = rhashtable_lookup_fast(&ltable, (const void *)&key, ltable_params);
	if (esp_entry) {
		/* When r_spi is set this is an established session. When not set it's
		 * a repeated initial packet from LAN side. But both cases are treated
		 * the same.
		 */
		if (esp_entry->l3num == AF_INET) {
			pr_debug("esp_entry %p Matches ltable entry %x with l_spi %x l_ip %pI4 r_ip %pI4\n",
				 esp_entry, esp_entry->esp_id, esp_entry->l_spi,
				 &esp_entry->l_ip.in, &esp_entry->r_ip.in);
		} else {
			pr_debug("esp_entry %p Matches ltable entry %x with l_spi %x l_ip %pI6 r_ip %pI6\n",
				 esp_entry, esp_entry->esp_id, esp_entry->l_spi,
				 &esp_entry->l_ip.in6, &esp_entry->r_ip.in6);
		}
		return esp_entry;
	}

	/* Established remote side */
	calculate_key(net_hmix, spi, l3num, &any, src_ip, &key);
	esp_entry = rhashtable_lookup_fast(&rtable, (const void *)&key, rtable_params);
	if (esp_entry) {
		if (esp_entry->l3num == AF_INET) {
			pr_debug("esp_entry %p Matches rtable entry %x with l_spi %x r_spi %x l_ip %pI4 r_ip %pI4\n",
				 esp_entry, esp_entry->esp_id, esp_entry->l_spi, esp_entry->r_spi,
				 &esp_entry->l_ip.in, &esp_entry->r_ip.in);
		} else {
			pr_debug("esp_entry %p Matches rtable entry %x with l_spi %x r_spi %x l_ip %pI6 r_ip %pI6\n",
				 esp_entry, esp_entry->esp_id, esp_entry->l_spi, esp_entry->r_spi,
				 &esp_entry->l_ip.in6, &esp_entry->r_ip.in6);
		}
		return esp_entry;
	}

	/* Incomplete remote side, check if packet has a missing r_spi */
	esp_entry = search_esp_entry_init_remote(net, l3num, src_ip, dst_ip);
	if (esp_entry) {
		int err;

		esp_entry->r_spi = spi;
		/* Remove entry from incmpl_rtable and add to rtable */
		rhltable_remove(&incmpl_rtable, &esp_entry->incmpl_rlist, incmpl_rtable_params);
		/* Error will not be due to duplicate as established remote side lookup
		 * above would have found it. Delete entry.
		 */
		err = rhashtable_insert_fast(&rtable, &esp_entry->rnode, rtable_params);
		if (err) {
			struct nf_esp_net *esp_net = esp_pernet(net);
			spin_lock(&esp_net->id_list_lock);
			free_esp_entry_by_id(net, esp_entry->esp_id);
			spin_unlock(&esp_net->id_list_lock);
			return NULL;
		}
		return esp_entry;
	}

	if (l3num == AF_INET) {
		pr_debug("No entry matches for spi %x src_ip %pI4 dst_ip %pI4\n",
			 spi, &src_ip->in, &dst_ip->in);
	} else {
		pr_debug("No entry matches for spi %x src_ip %pI6 dst_ip %pI6\n",
			 spi, &src_ip->in6, &dst_ip->in6);
	}
	return NULL;
}

/* invert esp part of tuple */
bool nf_conntrack_invert_esp_tuple(struct nf_conntrack_tuple *tuple,
				   const struct nf_conntrack_tuple *orig)
{
	/* Check if ESP conntrack is enabled */
	if (!nf_ct_esp_enabled) {
		/* Behave like generic conntrack */
		tuple->src.u.all = orig->dst.u.all;
		tuple->dst.u.all = orig->src.u.all;

		return true;
	}

	tuple->dst.u.esp.id = orig->dst.u.esp.id;
	tuple->src.u.esp.id = orig->src.u.esp.id;
	return true;
}

bool nf_ct_esp_manip_packet(struct sk_buff *skb, const struct nf_conntrack_tuple *tuple, int maniptype)
{
	u16 esp_id = 0;
	struct _esp_entry *esp_entry;
	struct nf_conn *ct;
	struct nf_esp_net *esp_net;
	enum ip_conntrack_info ctinfo;

	/* Check if ESP conntrack is enabled */
	if (!nf_ct_esp_enabled) {
		return true;
	}

	ct = nf_ct_get(skb, &ctinfo);
	esp_net = esp_pernet(nf_ct_net(ct));

	esp_id = tuple->src.u.esp.id;

	spin_lock(&esp_net->id_list_lock);
	esp_entry = find_esp_entry_by_id(esp_net, esp_id);
	if (!esp_entry) {
		spin_unlock(&esp_net->id_list_lock);
		return true;
	}

#if IS_ENABLED(CONFIG_NF_NAT)
	if (maniptype == NF_NAT_MANIP_SRC) {
		esp_entry->nat_done = true;
	}
#endif

	spin_unlock(&esp_net->id_list_lock);

	return true;
}
EXPORT_SYMBOL(nf_ct_esp_manip_packet);

/* esp hdr info to tuple */
bool esp_pkt_to_tuple(const struct sk_buff *skb, unsigned int dataoff,
		      struct net *net, struct nf_conntrack_tuple *tuple)
{
	struct esphdr _esphdr, *esphdr;
	struct _esp_entry *esp_entry;
#if IS_ENABLED(CONFIG_XFRM)
	struct xfrm_state *x = NULL;
#endif
	u32 spi;

	esphdr = skb_header_pointer(skb, dataoff, sizeof(_esphdr), &_esphdr);
	if (!esphdr) {
		/* try to behave like "nf_conntrack_proto_generic" */
		tuple->src.u.all = 0;
		tuple->dst.u.all = 0;
		return true;
	}
	spi = ntohl(esphdr->spi);

#if IS_ENABLED(CONFIG_XFRM)
	/* xfrm_state_lookup_byspi expects SPI in network byte order */
	x = xfrm_state_lookup_byspi(net, esphdr->spi, AF_INET);

	if (x) {
		xfrm_state_put(x);
		return true;
	}
#endif

	/* Check if esphdr already associated with a pre-existing connection:
	 *   if no, create a new connection, missing the r_spi;
	 *   if yes, check if we have seen the source IP:
	 *             if no, fill in r_spi in the pre-existing connection.
	 */
	esp_entry = search_esp_entry_by_spi(net, spi, tuple->src.l3num,
					    &tuple->src.u3, &tuple->dst.u3);
	if (!esp_entry) {
		struct _esp_hkey key = {};
		const union nf_inet_addr any = { .in6 = IN6ADDR_ANY_INIT };
		u32 net_hmix = net_hash_mix(net);
		struct nf_esp_net *esp_net = esp_pernet(net);
		struct _esp_entry *esp_entry_old;
		int err;

		esp_entry = alloc_esp_entry(net);
		if (!esp_entry) {
			pr_debug("All esp connection slots in use\n");
			return false;
		}
		esp_entry->l_spi = spi;
		esp_entry->l3num = tuple->src.l3num;
		esp_ip_addr_copy(esp_entry->l3num, &esp_entry->l_ip, &tuple->src.u3);
		esp_ip_addr_copy(esp_entry->l3num, &esp_entry->r_ip, &tuple->dst.u3);

		/* Add entries to the hash tables */

		calculate_key(net_hmix, esp_entry->l_spi, esp_entry->l3num, &esp_entry->l_ip,
			      &esp_entry->r_ip, &key);
		esp_entry_old = rhashtable_lookup_get_insert_key(&ltable, &key, &esp_entry->lnode,
								 ltable_params);
		if (esp_entry_old) {
			spin_lock(&esp_net->id_list_lock);

			if (IS_ERR(esp_entry_old)) {
				free_esp_entry_by_id(net, esp_entry->esp_id);
				spin_unlock(&esp_net->id_list_lock);
				return false;
			}

			free_esp_entry_by_id(net, esp_entry->esp_id);
			spin_unlock(&esp_net->id_list_lock);

			/* insertion raced, use existing entry */
			esp_entry = esp_entry_old;
		}
		/* esp_entry_old == NULL -- insertion successful */

		pr_debug("Inserting into rhtable espentry %p net_hmix %d esp_entry->l3num %d any %pI4 esp_entry rip %pI4 %p skb\n", esp_entry, net_hmix, esp_entry->l3num, &any, &esp_entry->r_ip.in, skb);

		calculate_key(net_hmix, 0, esp_entry->l3num, &any, &esp_entry->r_ip, &key);
		err = rhltable_insert_key(&incmpl_rtable, (const void *)&key,
					  &esp_entry->incmpl_rlist, incmpl_rtable_params);
		if (err) {
			spin_lock(&esp_net->id_list_lock);
			free_esp_entry_by_id(net, esp_entry->esp_id);
			spin_unlock(&esp_net->id_list_lock);
			return false;
		}

		if (esp_entry->l3num == AF_INET) {
			pr_debug("esp entry %p New entry %x with l_spi %x l_ip %pI4 r_ip %pI4 %p skb\n",
				 esp_entry, esp_entry->esp_id, esp_entry->l_spi,
				 &esp_entry->l_ip.in, &esp_entry->r_ip.in, skb);
		} else {
			pr_debug("esp entry %p New entry %x with l_spi %x l_ip %pI6 r_ip %pI6\n",
				 esp_entry, esp_entry->esp_id, esp_entry->l_spi,
				 &esp_entry->l_ip.in6, &esp_entry->r_ip.in6);
		}
	}

	tuple->dst.u.esp.id = esp_entry->esp_id;
	tuple->src.u.esp.id = esp_entry->esp_id;

	return true;
}

#ifdef CONFIG_NF_CONNTRACK_PROCFS
/* print private data for conntrack */
static void esp_print_conntrack(struct seq_file *s, struct nf_conn *ct)
{
	seq_printf(s, "l_spi=%x, r_spi=%x ", ct->proto.esp.l_spi, ct->proto.esp.r_spi);
}
#endif

/* Returns verdict for packet, and may modify conntrack */
int nf_conntrack_esp_packet(struct nf_conn *ct, struct sk_buff *skb,
			    unsigned int dataoff,
			    enum ip_conntrack_info ctinfo,
			    const struct nf_hook_state *state)
{
	int esp_id;
	struct nf_conntrack_tuple *tuple;
	unsigned int *timeouts = nf_ct_timeout_lookup(ct);
	struct nf_esp_net *esp_net = esp_pernet(nf_ct_net(ct));

	if (!timeouts)
		timeouts = esp_net->esp_timeouts;

	/* If we've seen traffic both ways, this is some kind of ESP
	 * stream.  Extend timeout.
	 */
	if (test_bit(IPS_SEEN_REPLY_BIT, &ct->status)) {
		nf_ct_refresh_acct(ct, ctinfo, skb, timeouts[ESP_CT_REPLIED]);
		/* Also, more likely to be important, and not a probe */
		if (!test_and_set_bit(IPS_ASSURED_BIT, &ct->status)) {
			/* Was originally IPCT_STATUS but this is no longer an option.
			 * GRE uses assured for same purpose
			 */
			nf_conntrack_event_cache(IPCT_ASSURED, ct);

			/* Retrieve SPIs of original and reply from esp_entry.
			 * Both directions should contain the same esp_entry,
			 * so just check the first one.
			 */
			tuple = nf_ct_tuple(ct, IP_CT_DIR_ORIGINAL);

			esp_id = tuple->src.u.esp.id;
			if (esp_id >= TEMP_SPI_START && esp_id <= TEMP_SPI_MAX) {
				struct _esp_entry *esp_entry;

				spin_lock(&esp_net->id_list_lock);
				esp_entry = find_esp_entry_by_id(esp_net, esp_id);
				spin_unlock(&esp_net->id_list_lock);

				if (esp_entry) {
					ct->proto.esp.l_spi = esp_entry->l_spi;
					ct->proto.esp.r_spi = esp_entry->r_spi;
				}
			}
		}
	} else {
		nf_ct_refresh_acct(ct, ctinfo, skb, timeouts[ESP_CT_UNREPLIED]);
	}

	return NF_ACCEPT;
}

void nf_ct_esp_pernet_flush(struct net *net)
{
	struct nf_conntrack_net *cnet = net_generic(net, nf_conntrack_net_id);
	struct nf_esp_net *esp_net = esp_pernet(net);
	struct list_head *pos, *tmp, *head = &esp_net->id_list;
	struct _esp_entry *esp_entry;

	spin_lock(&esp_net->id_list_lock);
	list_for_each_safe(pos, tmp, head) {
		esp_entry = list_entry(pos, struct _esp_entry, net_node);
		free_esp_entry(cnet, esp_entry);
	}
	spin_unlock(&esp_net->id_list_lock);
}

/* Called when a conntrack entry has already been removed from the hashes
 * and is about to be deleted from memory.
 * At this point, ESP entries should already be marked as pending_free.
 */
void destroy_esp_conntrack_entry(struct nf_conn *ct)
{
	struct nf_conntrack_tuple *tuple;
	enum ip_conntrack_dir dir;
	int esp_id;
	struct net *net = nf_ct_net(ct);
	struct nf_esp_net *esp_net = esp_pernet(net);

	/* Probably all the ESP entries referenced in this connection are the same,
	 * but the free function handles repeated frees, so best to do them all.
	 */
	for (dir = IP_CT_DIR_ORIGINAL; dir < IP_CT_DIR_MAX; dir++) {
		tuple = nf_ct_tuple(ct, dir);

		spin_lock(&esp_net->id_list_lock);

		esp_id = tuple->src.u.esp.id;
		if (esp_id >= TEMP_SPI_START && esp_id <= TEMP_SPI_MAX) {
			pr_debug("deleting esp entry %d ct %p\n", esp_id, ct);
			free_esp_entry_by_id(net, esp_id);
		}
		tuple->src.u.esp.id = 0;

		esp_id = tuple->dst.u.esp.id;
		if (esp_id >= TEMP_SPI_START && esp_id <= TEMP_SPI_MAX) {
			pr_debug("deleting esp entry %d ct %p\n", esp_id, ct);
			free_esp_entry_by_id(net, esp_id);
		}
		tuple->dst.u.esp.id = 0;

		spin_unlock(&esp_net->id_list_lock);
	}
}

#if IS_ENABLED(CONFIG_NF_CT_NETLINK)

static int esp_tuple_to_nlattr(struct sk_buff *skb,
			       const struct nf_conntrack_tuple *t)
{
	if (nla_put_be16(skb, CTA_PROTO_SRC_ESP_ID, t->src.u.esp.id) ||
	    nla_put_be16(skb, CTA_PROTO_DST_ESP_ID, t->dst.u.esp.id))
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -1;
}

static const struct nla_policy esp_nla_policy[CTA_PROTO_MAX + 1] = {
	[CTA_PROTO_SRC_ESP_ID] = { .type = NLA_U16 },
	[CTA_PROTO_DST_ESP_ID] = { .type = NLA_U16 },
};

static int esp_nlattr_to_tuple(struct nlattr *tb[],
			       struct nf_conntrack_tuple *t,
			       u32 flags)
{
	if (flags & CTA_FILTER_FLAG(CTA_PROTO_SRC_ESP_ID)) {
		if (!tb[CTA_PROTO_SRC_ESP_ID])
			return -EINVAL;

		t->src.u.esp.id = nla_get_be16(tb[CTA_PROTO_SRC_ESP_ID]);
	}

	if (flags & CTA_FILTER_FLAG(CTA_PROTO_DST_ESP_ID)) {
		if (!tb[CTA_PROTO_DST_ESP_ID])
			return -EINVAL;

		t->dst.u.esp.id = nla_get_be16(tb[CTA_PROTO_DST_ESP_ID]);
	}

	return 0;
}

static unsigned int esp_nlattr_tuple_size(void)
{
	return nla_policy_len(esp_nla_policy, CTA_PROTO_MAX + 1);
}
#endif

/* protocol helper struct */
const struct nf_conntrack_l4proto nf_conntrack_l4proto_esp = {
	.l4proto = IPPROTO_ESP,
#ifdef CONFIG_NF_CONNTRACK_PROCFS
	.print_conntrack = esp_print_conntrack,
#endif
#if IS_ENABLED(CONFIG_NF_CT_NETLINK)
	.tuple_to_nlattr = esp_tuple_to_nlattr,
	.nlattr_tuple_size = esp_nlattr_tuple_size,
	.nlattr_to_tuple = esp_nlattr_to_tuple,
	.nla_policy = esp_nla_policy,
#endif
};
