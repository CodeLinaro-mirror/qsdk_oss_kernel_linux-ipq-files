/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CONNTRACK_PROTO_ESP_H
#define _CONNTRACK_PROTO_ESP_H
#include <asm/byteorder.h>
#include <net/netfilter/nf_conntrack_tuple.h>

/* ESP PROTOCOL HEADER */

struct esphdr {
	__u32 spi;
};

struct nf_ct_esp {
	__u32 l_spi, r_spi;
};

void nf_ct_esp_pernet_flush(struct net *net);

void destroy_esp_conntrack_entry(struct nf_conn *ct);

bool esp_pkt_to_tuple(const struct sk_buff *skb, unsigned int dataoff,
		      struct net *net, struct nf_conntrack_tuple *tuple);

bool nf_ct_esp_manip_packet(struct sk_buff *skb, const struct nf_conntrack_tuple *tuple, int maniptype);
#endif /* _CONNTRACK_PROTO_ESP_H */
