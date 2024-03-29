/*
 * Kernel iptables module to track stats for packets based on user tags.
 *
 * (C) 2011 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * NOTE: I hacked in a few functions so that this would compile as a kernel
 * ipv6_find_hdr
 */

/* #define DEBUG */
/* #define IDEBUG */
/* #define MDEBUG */
/* #define RDEBUG */
/* #define CDEBUG */

/* Iface handling */
#ifdef IDEBUG
#define IF_DEBUG(...) pr_debug(__VA_ARGS__)
#else
#define IF_DEBUG(...) no_printk(__VA_ARGS__)
#endif
/* Iptable Matching */
#ifdef MDEBUG
#define MT_DEBUG(...) pr_debug(__VA_ARGS__)
#else
#define MT_DEBUG(...) no_printk(__VA_ARGS__)
#endif
/* Red-black tree handling */
#ifdef RDEBUG
#define RB_DEBUG(...) pr_debug(__VA_ARGS__)
#else
#define RB_DEBUG(...) no_printk(__VA_ARGS__)
#endif
/* procfs ctrl/stats handling */
#ifdef CDEBUG
#define CT_DEBUG(...) pr_debug(__VA_ARGS__)
#else
#define CT_DEBUG(...) no_printk(__VA_ARGS__)
#endif

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/in6.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/ipv6.h>
#include <net/ipv6.h>
#include <linux/icmpv6.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include <linux/file.h>
#include <linux/inetdevice.h>
#include <linux/netfilter/x_tables.h>
#include <linux/workqueue.h>

#include <net/ndisc.h>
#include <net/protocol.h>
#include <net/transp_v6.h>
#include <net/ip6_route.h>
#include <net/raw.h>
#include <net/tcp_states.h>
#include <net/ip6_checksum.h>
#include <net/xfrm.h>

#include <net/addrconf.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/netfilter/nf_tproxy_core.h>
#include <net/inet6_hashtables.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/icmp.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/netfilter/xt_socket.h>

#include "xt_qtaguid.h"
#include "xt_qtaguid_printk.h"

/*
 * We only use the xt_socket funcs within a similar context to avoid unexpected
 * return values.
 */
#define XT_SOCKET_SUPPORTED_HOOKS \
	((1 << NF_INET_PRE_ROUTING) | (1 << NF_INET_LOCAL_IN))


static struct proc_dir_entry *xt_qtaguid_procdir;

static unsigned int proc_iface_perms = S_IRUGO;
module_param_named(iface_perms, proc_iface_perms, uint, S_IRUGO | S_IWUSR);

static struct proc_dir_entry *xt_qtaguid_stats_file;
static unsigned int proc_stats_perms = S_IRUGO;
module_param_named(stats_perms, proc_stats_perms, uint, S_IRUGO | S_IWUSR);

static struct proc_dir_entry *xt_qtaguid_ctrl_file;
#ifdef CONFIG_ANDROID_PARANOID_NETWORK
static unsigned int proc_ctrl_perms = S_IRUGO | S_IWUGO;
#else
static unsigned int proc_ctrl_perms = S_IRUGO | S_IWUSR;
#endif
module_param_named(ctrl_perms, proc_ctrl_perms, uint, S_IRUGO | S_IWUSR);

#ifdef CONFIG_ANDROID_PARANOID_NETWORK
#include <linux/android_aid.h>
static gid_t proc_stats_readall_gid = 3006; /* AID_NET_BW_STATS */
static gid_t proc_ctrl_write_gid = 3007; /* AID_NET_BW_ACCT */
#else
/* 0 means, don't limit anybody */
static gid_t proc_stats_readall_gid;
static gid_t proc_ctrl_write_gid;
#endif
module_param_named(stats_readall_gid, proc_stats_readall_gid, uint,
		   S_IRUGO | S_IWUSR);
module_param_named(ctrl_write_gid, proc_ctrl_write_gid, uint,
		   S_IRUGO | S_IWUSR);


/******************************************************
 * nf_tproxy_core.h
 */
#define NFT_LOOKUP_ANY         0
#define NFT_LOOKUP_LISTENER    1
#define NFT_LOOKUP_ESTABLISHED 2

struct sock *
nf_tproxy_get_sock_v4(struct net *net, const u8 protocol,
		      const __be32 saddr, const __be32 daddr,
		      const __be16 sport, const __be16 dport,
		      const struct net_device *in, bool listening_only)
{
	struct sock *sk;

	/* look up socket */
	switch (protocol) {
	case IPPROTO_TCP:
		if (listening_only)
			sk = __inet_lookup_listener(net, &tcp_hashinfo,
						    daddr, ntohs(dport),
						    in->ifindex);
		else
			sk = __inet_lookup(net, &tcp_hashinfo,
					   saddr, sport, daddr, dport,
					   in->ifindex);
		break;
	case IPPROTO_UDP:
		sk = udp4_lib_lookup(net, saddr, sport, daddr, dport,
				     in->ifindex);
		break;
	default:
		WARN_ON(1);
		sk = NULL;
	}

	pr_debug("tproxy socket lookup: proto %u %08x:%u -> %08x:%u, listener only: %d, sock %p\n",
		 protocol, ntohl(saddr), ntohs(sport), ntohl(daddr), ntohs(dport), listening_only, sk);

	return sk;
}

static inline struct sock *
nf_tproxy_get_sock_v6(struct net *net, const u8 protocol,
		      const struct in6_addr *saddr, const struct in6_addr *daddr,
		      const __be16 sport, const __be16 dport,
		      const struct net_device *in, int lookup_type)
{
	struct sock *sk;

	/* look up socket */
	switch (protocol) {
	case IPPROTO_TCP:
		switch (lookup_type) {
		case NFT_LOOKUP_ANY:
			sk = inet6_lookup(net, &tcp_hashinfo,
					  saddr, sport, daddr, dport,
					  in->ifindex);
			break;
		case NFT_LOOKUP_LISTENER:
			sk = inet6_lookup_listener(net, &tcp_hashinfo,
						   daddr, ntohs(dport),
						   in->ifindex);

			/* NOTE: we return listeners even if bound to
			 * 0.0.0.0, those are filtered out in
			 * xt_socket, since xt_TPROXY needs 0 bound
			 * listeners too */

			break;
		case NFT_LOOKUP_ESTABLISHED:
			sk = __inet6_lookup_established(net, &tcp_hashinfo,
							saddr, sport, daddr, ntohs(dport),
							in->ifindex);
			break;
		default:
			WARN_ON(1);
			sk = NULL;
			break;
		}
		break;
	case IPPROTO_UDP:
                /* FIXME
		sk = udp6_lib_lookup(net, saddr, sport, daddr, dport, in->ifindex);
                */
		WARN_ON(1); /* HACK */
		if (sk && lookup_type != NFT_LOOKUP_ANY) {
			/* NOTE: we return listeners even if bound to
			 * 0.0.0.0, those are filtered out in
			 * xt_socket, since xt_TPROXY needs 0 bound
			 * listeners too */
			int connected = (sk->sk_state == TCP_ESTABLISHED);
			int wildcard = ipv6_addr_any(&inet6_sk(sk)->rcv_saddr);
			if ((lookup_type == NFT_LOOKUP_ESTABLISHED && (!connected || wildcard)) ||
			    (lookup_type == NFT_LOOKUP_LISTENER && connected)) {
				sock_put(sk);
				sk = NULL;
			}
		}
		break;
	default:
		WARN_ON(1);
		sk = NULL;
	}

	pr_debug("tproxy socket lookup: proto %u %pI6:%u -> %pI6:%u, lookup type: %d, sock %p\n",
		 protocol, saddr, ntohs(sport), daddr, ntohs(dport), lookup_type, sk);

	return sk;
}

/**************************************************************
 * xt_socket.c missing functions 
 */
static int
extract_icmp4_fields(const struct sk_buff *skb,
		    u8 *protocol,
		    __be32 *raddr,
		    __be32 *laddr,
		    __be16 *rport,
		    __be16 *lport)
{
	unsigned int outside_hdrlen = ip_hdrlen(skb);
	struct iphdr *inside_iph, _inside_iph;
	struct icmphdr *icmph, _icmph;
	__be16 *ports, _ports[2];

	icmph = skb_header_pointer(skb, outside_hdrlen,
				   sizeof(_icmph), &_icmph);
	if (icmph == NULL)
		return 1;

	switch (icmph->type) {
	case ICMP_DEST_UNREACH:
	case ICMP_SOURCE_QUENCH:
	case ICMP_REDIRECT:
	case ICMP_TIME_EXCEEDED:
	case ICMP_PARAMETERPROB:
		break;
	default:
		return 1;
	}

	inside_iph = skb_header_pointer(skb, outside_hdrlen +
					sizeof(struct icmphdr),
					sizeof(_inside_iph), &_inside_iph);
	if (inside_iph == NULL)
		return 1;

	if (inside_iph->protocol != IPPROTO_TCP &&
	    inside_iph->protocol != IPPROTO_UDP)
		return 1;

	ports = skb_header_pointer(skb, outside_hdrlen +
				   sizeof(struct icmphdr) +
				   (inside_iph->ihl << 2),
				   sizeof(_ports), &_ports);
	if (ports == NULL)
		return 1;

	/* the inside IP packet is the one quoted from our side, thus
	 * its saddr is the local address */
	*protocol = inside_iph->protocol;
	*laddr = inside_iph->saddr;
	*lport = ports[0];
	*raddr = inside_iph->daddr;
	*rport = ports[1];

	return 0;
}

static int
extract_icmp6_fields(const struct sk_buff *skb,
		     unsigned int outside_hdrlen,
		     int *protocol,
		     struct in6_addr **raddr,
		     struct in6_addr **laddr,
		     __be16 *rport,
		     __be16 *lport)
{
	struct ipv6hdr *inside_iph, _inside_iph;
	struct icmp6hdr *icmph, _icmph;
	__be16 *ports, _ports[2];
	u8 inside_nexthdr;
	int inside_hdrlen;

	icmph = skb_header_pointer(skb, outside_hdrlen,
				   sizeof(_icmph), &_icmph);
	if (icmph == NULL)
		return 1;

	if (icmph->icmp6_type & ICMPV6_INFOMSG_MASK)
		return 1;

	inside_iph = skb_header_pointer(skb, outside_hdrlen + sizeof(_icmph), sizeof(_inside_iph), &_inside_iph);
	if (inside_iph == NULL)
		return 1;
	inside_nexthdr = inside_iph->nexthdr;

	inside_hdrlen = ipv6_skip_exthdr(skb, outside_hdrlen + sizeof(_icmph) + sizeof(_inside_iph), &inside_nexthdr);
	if (inside_hdrlen < 0)
		return 1; /* hjm: Packet has no/incomplete transport layer headers. */

	if (inside_nexthdr != IPPROTO_TCP &&
	    inside_nexthdr != IPPROTO_UDP)
		return 1;

	ports = skb_header_pointer(skb, inside_hdrlen,
				   sizeof(_ports), &_ports);
	if (ports == NULL)
		return 1;

	/* the inside IP packet is the one quoted from our side, thus
	 * its saddr is the local address */
	*protocol = inside_nexthdr;
	*laddr = &inside_iph->saddr;
	*lport = ports[0];
	*raddr = &inside_iph->daddr;
	*rport = ports[1];

	return 0;
}
void
xt_socket_put_sk(struct sock *sk)
{
	if (sk->sk_state == TCP_TIME_WAIT)
		inet_twsk_put(inet_twsk(sk));
	else
		sock_put(sk);
}

struct sock*
xt_socket_get4_sk(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct iphdr *iph = ip_hdr(skb);
	struct udphdr _hdr, *hp = NULL;
	struct sock *sk;
	__be32 daddr, saddr;
	__be16 dport, sport;
	u8 protocol;
#ifdef XT_SOCKET_HAVE_CONNTRACK
	struct nf_conn const *ct;
	enum ip_conntrack_info ctinfo;
#endif

	if (iph->protocol == IPPROTO_UDP || iph->protocol == IPPROTO_TCP) {
		hp = skb_header_pointer(skb, ip_hdrlen(skb),
					sizeof(_hdr), &_hdr);
		if (hp == NULL)
			return NULL;

		protocol = iph->protocol;
		saddr = iph->saddr;
		sport = hp->source;
		daddr = iph->daddr;
		dport = hp->dest;

	} else if (iph->protocol == IPPROTO_ICMP) {
		if (extract_icmp4_fields(skb, &protocol, &saddr, &daddr,
					&sport, &dport))
			return NULL;
	} else {
		return NULL;
	}

#ifdef XT_SOCKET_HAVE_CONNTRACK
	/* Do the lookup with the original socket address in case this is a
	 * reply packet of an established SNAT-ted connection. */

	ct = nf_ct_get(skb, &ctinfo);
	if (ct && !nf_ct_is_untracked(ct) &&
	    ((iph->protocol != IPPROTO_ICMP &&
	      ctinfo == IP_CT_ESTABLISHED_REPLY) ||
	     (iph->protocol == IPPROTO_ICMP &&
	      ctinfo == IP_CT_RELATED_REPLY)) &&
	    (ct->status & IPS_SRC_NAT_DONE)) {

		daddr = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip;
		dport = (iph->protocol == IPPROTO_TCP) ?
			ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.tcp.port :
			ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.udp.port;
	}
#endif

	sk = nf_tproxy_get_sock_v4(dev_net(skb->dev), protocol,
				   saddr, daddr, sport, dport, par->in, NFT_LOOKUP_ANY);

	pr_debug("proto %hhu %pI4:%hu -> %pI4:%hu (orig %pI4:%hu) sock %p\n",
		 protocol, &saddr, ntohs(sport),
		 &daddr, ntohs(dport),
		 &iph->daddr, hp ? ntohs(hp->dest) : 0, sk);

	return sk;
}

struct sock*
xt_socket_get6_sk(const struct sk_buff *skb, struct xt_action_param *par)
{
	struct ipv6hdr *iph = ipv6_hdr(skb);
	struct udphdr _hdr, *hp = NULL;
	struct sock *sk;
	struct in6_addr *daddr, *saddr;
	__be16 dport, sport;
	int thoff, tproto;

	/* FIXME: tproto = ipv6_find_hdr(skb, &thoff, -1, NULL); */
	if (tproto < 0) {
		pr_debug("unable to find transport header in IPv6 packet, dropping\n");
		return NF_DROP;
	}

	if (tproto == IPPROTO_UDP || tproto == IPPROTO_TCP) {
		hp = skb_header_pointer(skb, thoff,
					sizeof(_hdr), &_hdr);
		if (hp == NULL)
			return NULL;

		saddr = &iph->saddr;
		sport = hp->source;
		daddr = &iph->daddr;
		dport = hp->dest;

	} else if (tproto == IPPROTO_ICMPV6) {
		if (extract_icmp6_fields(skb, thoff, &tproto, &saddr, &daddr,
					 &sport, &dport))
			return NULL;
	} else {
		return NULL;
	}

	sk = nf_tproxy_get_sock_v6(dev_net(skb->dev), tproto,
				   saddr, daddr, sport, dport, par->in, NFT_LOOKUP_ANY);
	pr_debug("proto %hhd %pI6:%hu -> %pI6:%hu "
		 "(orig %pI6:%hu) sock %p\n",
		 tproto, saddr, ntohs(sport),
		 daddr, ntohs(dport),
		 &iph->daddr, hp ? ntohs(hp->dest) : 0, sk);
	return sk;
}


/*
 * After the kernel has initiallized this module, it is still possible
 * to make it passive:
 *  - do not register it via iptables.
 *   the matching code will not be invoked.
 *  - set passive to 0
 *   the iface stats handling will not be act on notifications.
 * This is mostly usefull when a bug is suspected.
 */
static bool module_passive;
module_param_named(passive, module_passive, bool, S_IRUGO | S_IWUSR);

/*---------------------------------------------------------------------------*/
/*
 * Tags:
 *
 * They represent what the data usage counters will be tracked against.
 * By default a tag is just based on the UID.
 * The UID is used as the base for policying, and can not be ignored.
 * So a tag will always at least represent a UID (uid_tag).
 *
 * A tag can be augmented with an "accounting tag" which is associated
 * with a UID.
 * User space can set the acct_tag portion of the tag which is then used
 * with sockets: all data belong to that socket will be counted against the
 * tag. The policing is then based on the tag's uid_tag portion,
 * and stats are collected for the acct_tag portion seperately.
 *
 * There could be
 * a:  {acct_tag=1, uid_tag=10003}
 * b:  {acct_tag=2, uid_tag=10003}
 * c:  {acct_tag=3, uid_tag=10003}
 * d:  {acct_tag=0, uid_tag=10003}
 * (a, b, and c represent tags associated with specific sockets.
 * d is for the totals for that uid, including all untagged traffic.
 * Typically d is used with policing/quota rules.
 *
 * We want tag_t big enough to distinguish uid_t and acct_tag.
 * It might become a struct if needed.
 * Nothing should be using it as an int.
 */
typedef uint64_t tag_t;  /* Only used via accessors */

static const char *iface_stat_procdirname = "iface_stat";
static struct proc_dir_entry *iface_stat_procdir;


/*
 * For now we only track 2 sets of counters.
 * The default set is 0.
 * Userspace can activate another set for a given uid being tracked.
 */
#define IFS_MAX_COUNTER_SETS 2

enum ifs_tx_rx {
	IFS_TX,
	IFS_RX,
	IFS_MAX_DIRECTIONS
};

/* For now, TCP, UDP, the rest */
enum ifs_proto {
	IFS_TCP,
	IFS_UDP,
	IFS_PROTO_OTHER,
	IFS_MAX_PROTOS
};

struct byte_packet_counters {
	uint64_t bytes;
	uint64_t packets;
};

struct data_counters {
	struct byte_packet_counters bpc[IFS_MAX_COUNTER_SETS][IFS_MAX_DIRECTIONS][IFS_MAX_PROTOS];
};

/* Generic tag based node used as a base for rb_tree ops. */
struct tag_node {
	struct rb_node node;
	tag_t tag;
};

struct tag_stat {
	struct tag_node tn;
	struct data_counters counters;
	/*
	 * If this tag is acct_tag based, we need to count against the
	 * matching parent uid_tag.
	 */
	struct data_counters *parent_counters;
};

struct iface_stat {
	struct list_head list;
	char *ifname;
	uint64_t rx_bytes;
	uint64_t rx_packets;
	uint64_t tx_bytes;
	uint64_t tx_packets;
	bool active;
	struct proc_dir_entry *proc_ptr;

	struct rb_root tag_stat_tree;
	spinlock_t tag_stat_list_lock;
};

static LIST_HEAD(iface_stat_list);
static DEFINE_SPINLOCK(iface_stat_list_lock);

/* This is needed to create proc_dir_entries from atomic context. */
struct iface_stat_work {
	struct work_struct iface_work;
	struct iface_stat *iface_entry;
};

/*
 * Track tag that this socket is transferring data for, and not necessarily
 * the uid that owns the socket.
 * This is the tag against which tag_stat.counters will be billed.
 */
struct sock_tag {
	struct rb_node sock_node;
	struct sock *sk;  /* Only used as a number, never dereferenced */
	/* The socket is needed for sockfd_put() */
	struct socket *socket;

	tag_t tag;
};

struct qtaguid_event_counts {
	/* Various successful events */
	atomic64_t sockets_tagged;
	atomic64_t sockets_untagged;
	atomic64_t counter_set_changes;
	atomic64_t delete_cmds;
	atomic64_t iface_events;  /* Number of NETDEV_* events handled */
	/*
	 * match_found_sk_*: numbers related to the netfilter matching
	 * function finding a sock for the sk_buff.
	 */
	atomic64_t match_found_sk;   /* An sk was already in the sk_buff. */
	/* The connection tracker had the sk. */
	atomic64_t match_found_sk_in_ct;
	/*
	 * No sk could be found. No apparent owner. Could happen with
	 * unsolicited traffic.
	 */
	atomic64_t match_found_sk_none;
};
static struct qtaguid_event_counts qtu_events;

static struct rb_root sock_tag_tree = RB_ROOT;
static DEFINE_SPINLOCK(sock_tag_list_lock);

/* Track the set active_set for the given tag. */
struct tag_counter_set {
	struct tag_node tn;
	int active_set;
};

static struct rb_root tag_counter_set_tree = RB_ROOT;
static DEFINE_SPINLOCK(tag_counter_set_list_lock);

static bool qtaguid_mt(const struct sk_buff *skb, struct xt_action_param *par);

/*----------------------------------------------*/
static inline int tag_compare(tag_t t1, tag_t t2)
{
	return t1 < t2 ? -1 : t1 == t2 ? 0 : 1;
}

static inline tag_t combine_atag_with_uid(tag_t acct_tag, uid_t uid)
{
	return acct_tag | uid;
}
static inline tag_t make_tag_from_uid(uid_t uid)
{
	return uid;
}
static inline uid_t get_uid_from_tag(tag_t tag)
{
	return tag & 0xFFFFFFFFULL;
}
static inline tag_t get_utag_from_tag(tag_t tag)
{
	return tag & 0xFFFFFFFFULL;
}
static inline tag_t get_atag_from_tag(tag_t tag)
{
	return tag & ~0xFFFFFFFFULL;
}

static inline bool valid_atag(tag_t tag)
{
	return !(tag & 0xFFFFFFFFULL);
}

static inline void dc_add_byte_packets(struct data_counters *counters, int set,
				  enum ifs_tx_rx direction,
				  enum ifs_proto ifs_proto,
				  int bytes,
				  int packets)
{
	counters->bpc[set][direction][ifs_proto].bytes += bytes;
	counters->bpc[set][direction][ifs_proto].packets += packets;
}

static inline uint64_t dc_sum_bytes(struct data_counters *counters,
				    int set,
				    enum ifs_tx_rx direction)
{
	return counters->bpc[set][direction][IFS_TCP].bytes
		+ counters->bpc[set][direction][IFS_UDP].bytes
		+ counters->bpc[set][direction][IFS_PROTO_OTHER].bytes;
}

static inline uint64_t dc_sum_packets(struct data_counters *counters,
				      int set,
				      enum ifs_tx_rx direction)
{
	return counters->bpc[set][direction][IFS_TCP].packets
		+ counters->bpc[set][direction][IFS_UDP].packets
		+ counters->bpc[set][direction][IFS_PROTO_OTHER].packets;
}

static struct tag_node *tag_node_tree_search(struct rb_root *root, tag_t tag)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct tag_node *data = rb_entry(node, struct tag_node, node);
		int result = tag_compare(tag, data->tag);
		RB_DEBUG("qtaguid: tag_node_tree_search(): tag=0x%llx"
			 " (uid=%d)\n",
			 data->tag,
			 get_uid_from_tag(data->tag));

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static void tag_node_tree_insert(struct tag_node *data, struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct tag_node *this = rb_entry(*new, struct tag_node,
						 node);
		int result = tag_compare(data->tag, this->tag);
		RB_DEBUG("qtaguid: tag_node_tree_insert(): tag=0x%llx"
			 " (uid=%d)\n",
			 this->tag,
			 get_uid_from_tag(this->tag));
		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			BUG();
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);
}

static void tag_stat_tree_insert(struct tag_stat *data, struct rb_root *root)
{
	tag_node_tree_insert(&data->tn, root);
}

static struct tag_stat *tag_stat_tree_search(struct rb_root *root, tag_t tag)
{
	struct tag_node *node = tag_node_tree_search(root, tag);
	if (!node)
		return NULL;
	return rb_entry(&node->node, struct tag_stat, tn.node);
}

static void tag_counter_set_tree_insert(struct tag_counter_set *data,
					struct rb_root *root)
{
	tag_node_tree_insert(&data->tn, root);
}

static struct tag_counter_set *tag_counter_set_tree_search(struct rb_root *root,
							   tag_t tag)
{
	struct tag_node *node = tag_node_tree_search(root, tag);
	if (!node)
		return NULL;
	return rb_entry(&node->node, struct tag_counter_set, tn.node);

}

static struct sock_tag *sock_tag_tree_search(struct rb_root *root,
					     const struct sock *sk)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct sock_tag *data = rb_entry(node, struct sock_tag,
						 sock_node);
		ptrdiff_t result = sk - data->sk;
		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static void sock_tag_tree_insert(struct sock_tag *data, struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct sock_tag *this = rb_entry(*new, struct sock_tag,
						 sock_node);
		ptrdiff_t result = data->sk - this->sk;
		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			BUG();
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->sock_node, parent, new);
	rb_insert_color(&data->sock_node, root);
}

static int read_proc_u64(char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	int len;
	uint64_t value;
	char *p = page;
	uint64_t *iface_entry = data;

	if (!data)
		return 0;

	value = *iface_entry;
	p += sprintf(p, "%llu\n", value);
	len = (p - page) - off;
	*eof = (len <= count) ? 1 : 0;
	*start = page + off;
	return len;
}

static int read_proc_bool(char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	int len;
	bool value;
	char *p = page;
	bool *bool_entry = data;

	if (!data)
		return 0;

	value = *bool_entry;
	p += sprintf(p, "%u\n", value);
	len = (p - page) - off;
	*eof = (len <= count) ? 1 : 0;
	*start = page + off;
	return len;
}

static int get_active_counter_set(tag_t tag)
{
	int active_set = 0;
	struct tag_counter_set *tcs;

	MT_DEBUG("qtaguid: get_active_counter_set(tag=0x%llx)"
		 " (uid=%u)\n",
		 tag, get_uid_from_tag(tag));
	/* For now we only handle UID tags for active sets */
	tag = get_utag_from_tag(tag);
	spin_lock_bh(&tag_counter_set_list_lock);
	tcs = tag_counter_set_tree_search(&tag_counter_set_tree, tag);
	if (tcs)
		active_set = tcs->active_set;
	spin_unlock_bh(&tag_counter_set_list_lock);
	return active_set;
}

/*
 * Find the entry for tracking the specified interface.
 * Caller must hold iface_stat_list_lock
 */
static struct iface_stat *get_iface_entry(const char *ifname)
{
	struct iface_stat *iface_entry;

	/* Find the entry for tracking the specified tag within the interface */
	if (ifname == NULL) {
		pr_info("qtaguid: iface_stat: get() NULL device name\n");
		return NULL;
	}

	/* Iterate over interfaces */
	list_for_each_entry(iface_entry, &iface_stat_list, list) {
		if (!strcmp(ifname, iface_entry->ifname))
			goto done;
	}
	iface_entry = NULL;
done:
	return iface_entry;
}

static void iface_create_proc_worker(struct work_struct *work)
{
	struct proc_dir_entry *proc_entry;
	struct iface_stat_work *isw = container_of(work, struct iface_stat_work,
						   iface_work);
	struct iface_stat *new_iface  = isw->iface_entry;

	/* iface_entries are not deleted, so safe to manipulate. */
	proc_entry = proc_mkdir(new_iface->ifname, iface_stat_procdir);
	if (IS_ERR_OR_NULL(proc_entry)) {
		pr_err("qtaguid: iface_stat: create_proc(): alloc failed.\n");
		kfree(isw);
		return;
	}

	new_iface->proc_ptr = proc_entry;

	create_proc_read_entry("tx_bytes", proc_iface_perms, proc_entry,
			read_proc_u64, &new_iface->tx_bytes);
	create_proc_read_entry("rx_bytes", proc_iface_perms, proc_entry,
			read_proc_u64, &new_iface->rx_bytes);
	create_proc_read_entry("tx_packets", proc_iface_perms, proc_entry,
			read_proc_u64, &new_iface->tx_packets);
	create_proc_read_entry("rx_packets", proc_iface_perms, proc_entry,
			read_proc_u64, &new_iface->rx_packets);
	create_proc_read_entry("active", proc_iface_perms, proc_entry,
			read_proc_bool, &new_iface->active);

	IF_DEBUG("qtaguid: iface_stat: create_proc(): done "
		 "entry=%p dev=%s\n", new_iface, new_iface->ifname);
	kfree(isw);
}

/* Caller must hold iface_stat_list_lock */
static struct iface_stat *iface_alloc(const char *ifname)
{
	struct iface_stat *new_iface;
	struct iface_stat_work *isw;

	new_iface = kzalloc(sizeof(*new_iface), GFP_ATOMIC);
	if (new_iface == NULL) {
		pr_err("qtaguid: iface_stat: create(%s): "
		       "iface_stat alloc failed\n", ifname);
		return NULL;
	}
	new_iface->ifname = kstrdup(ifname, GFP_ATOMIC);
	if (new_iface->ifname == NULL) {
		pr_err("qtaguid: iface_stat: create(%s): "
		       "ifname alloc failed\n", ifname);
		kfree(new_iface);
		return NULL;
	}
	spin_lock_init(&new_iface->tag_stat_list_lock);
	new_iface->active = true;
	new_iface->tag_stat_tree = RB_ROOT;

	/*
	 * ipv6 notifier chains are atomic :(
	 * No create_proc_read_entry() for you!
	 */
	isw = kmalloc(sizeof(*isw), GFP_ATOMIC);
	if (!isw) {
		pr_err("qtaguid: iface_stat: create(%s): "
		       "work alloc failed\n", new_iface->ifname);
		kfree(new_iface->ifname);
		kfree(new_iface);
		return NULL;
	}
	isw->iface_entry = new_iface;
	INIT_WORK(&isw->iface_work, iface_create_proc_worker);
	schedule_work(&isw->iface_work);
	list_add(&new_iface->list, &iface_stat_list);
	return new_iface;
}

/*
 * Create a new entry for tracking the specified interface.
 * Do nothing if the entry already exists.
 * Called when an interface is configured with a valid IP address.
 */
void iface_stat_create(const struct net_device *net_dev,
		       struct in_ifaddr *ifa)
{
	struct in_device *in_dev = NULL;
	const char *ifname;
	struct iface_stat *entry;
	__be32 ipaddr = 0;
	struct iface_stat *new_iface;

	IF_DEBUG("qtaguid: iface_stat: create(%s): ifa=%p netdev=%p\n",
		 net_dev ? net_dev->name : "?",
		 ifa, net_dev);
	if (!net_dev) {
		pr_err("qtaguid: iface_stat: create(): no net dev\n");
		return;
	}

	ifname = net_dev->name;
	if (!ifa) {
		in_dev = in_dev_get(net_dev);
		if (!in_dev) {
			pr_err("qtaguid: iface_stat: create(%s): no inet dev\n",
			       ifname);
			return;
		}
		IF_DEBUG("qtaguid: iface_stat: create(%s): in_dev=%p\n",
			 ifname, in_dev);
		for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
			IF_DEBUG("qtaguid: iface_stat: create(%s): "
				 "ifa=%p ifa_label=%s\n",
				 ifname, ifa,
				 ifa->ifa_label ? ifa->ifa_label : "(null)");
			if (ifa->ifa_label && !strcmp(ifname, ifa->ifa_label))
				break;
		}
	}

	if (!ifa) {
		IF_DEBUG("qtaguid: iface_stat: create(%s): no matching IP\n",
			 ifname);
		goto done_put;
	}
	ipaddr = ifa->ifa_local;

	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(ifname);
	if (entry != NULL) {
		IF_DEBUG("qtaguid: iface_stat: create(%s): entry=%p\n",
			 ifname, entry);
		if (ipv4_is_loopback(ipaddr)) {
			entry->active = false;
			IF_DEBUG("qtaguid: iface_stat: create(%s): "
				 "disable tracking of loopback dev\n",
				 ifname);
		} else {
			entry->active = true;
			IF_DEBUG("qtaguid: iface_stat: create(%s): "
				 "enable tracking. ip=%pI4\n",
				 ifname, &ipaddr);
		}
		goto done_unlock_put;
	} else if (ipv4_is_loopback(ipaddr)) {
		IF_DEBUG("qtaguid: iface_stat: create(%s): "
			 "ignore loopback dev. ip=%pI4\n", ifname, &ipaddr);
		goto done_unlock_put;
	}

	new_iface = iface_alloc(ifname);
	IF_DEBUG("qtaguid: iface_stat: create(%s): done "
		 "entry=%p ip=%pI4\n", ifname, new_iface, &ipaddr);

done_unlock_put:
	spin_unlock_bh(&iface_stat_list_lock);
done_put:
	if (in_dev)
		in_dev_put(in_dev);
}

void iface_stat_create_ipv6(const struct net_device *net_dev,
			    struct inet6_ifaddr *ifa)
{
	struct in_device *in_dev;
	const char *ifname;
	struct iface_stat *entry;
	struct iface_stat *new_iface;
	int addr_type;

	IF_DEBUG("qtaguid: iface_stat: create6(): ifa=%p netdev=%p->name=%s\n",
		 ifa, net_dev, net_dev ? net_dev->name : "");
	if (!net_dev) {
		pr_err("qtaguid: iface_stat: create6(): no net dev!\n");
		return;
	}
	ifname = net_dev->name;

	in_dev = in_dev_get(net_dev);
	if (!in_dev) {
		pr_err("qtaguid: iface_stat: create6(%s): no inet dev\n",
		       ifname);
		return;
	}

	IF_DEBUG("qtaguid: iface_stat: create6(%s): in_dev=%p\n",
		 ifname, in_dev);

	if (!ifa) {
		IF_DEBUG("qtaguid: iface_stat: create6(%s): no matching IP\n",
			 ifname);
		goto done_put;
	}
	addr_type = ipv6_addr_type(&ifa->addr);

	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(ifname);
	if (entry != NULL) {
		IF_DEBUG("qtaguid: iface_stat: create6(%s): entry=%p\n",
			 ifname, entry);
		if (addr_type & IPV6_ADDR_LOOPBACK) {
			entry->active = false;
			IF_DEBUG("qtaguid: iface_stat: create6(%s): "
				 "disable tracking of loopback dev\n",
				 ifname);
		} else {
			entry->active = true;
			IF_DEBUG("qtaguid: iface_stat: create6(%s): "
				 "enable tracking. ip=%pI6c\n",
				 ifname, &ifa->addr);
		}
		goto done_unlock_put;
	} else if (addr_type & IPV6_ADDR_LOOPBACK) {
		IF_DEBUG("qtaguid: iface_stat: create6(%s): "
			 "ignore loopback dev. ip=%pI6c\n",
			 ifname, &ifa->addr);
		goto done_unlock_put;
	}

	new_iface = iface_alloc(ifname);
	IF_DEBUG("qtaguid: iface_stat: create6(%s): done "
		 "entry=%p ip=%pI6c\n", ifname, new_iface, &ifa->addr);

done_unlock_put:
	spin_unlock_bh(&iface_stat_list_lock);
done_put:
	in_dev_put(in_dev);
}

static struct sock_tag *get_sock_stat_nl(const struct sock *sk)
{
	MT_DEBUG("qtaguid: get_sock_stat_nl(sk=%p)\n", sk);
	return sock_tag_tree_search(&sock_tag_tree, sk);
}

static struct sock_tag *get_sock_stat(const struct sock *sk)
{
	struct sock_tag *sock_tag_entry;
	MT_DEBUG("qtaguid: get_sock_stat(sk=%p)\n", sk);
	if (!sk)
		return NULL;
	spin_lock_bh(&sock_tag_list_lock);
	sock_tag_entry = get_sock_stat_nl(sk);
	spin_unlock_bh(&sock_tag_list_lock);
	return sock_tag_entry;
}

static void
data_counters_update(struct data_counters *dc, int set,
		     enum ifs_tx_rx direction, int proto, int bytes)
{
	switch (proto) {
	case IPPROTO_TCP:
		dc_add_byte_packets(dc, set, direction, IFS_TCP, bytes, 1);
		break;
	case IPPROTO_UDP:
		dc_add_byte_packets(dc, set, direction, IFS_UDP, bytes, 1);
		break;
	case IPPROTO_IP:
	default:
		dc_add_byte_packets(dc, set, direction, IFS_PROTO_OTHER, bytes,
				    1);
		break;
	}
}

/*
 * Update stats for the specified interface. Do nothing if the entry
 * does not exist (when a device was never configured with an IP address).
 * Called when an device is being unregistered.
 */
static void iface_stat_update(struct net_device *dev)
{
	struct rtnl_link_stats64 *stats;
	struct iface_stat *entry;

	stats = dev_get_stats(dev);
	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(dev->name);
	if (entry == NULL) {
		IF_DEBUG("qtaguid: iface_stat: update(%s): not tracked\n",
			 dev->name);
		spin_unlock_bh(&iface_stat_list_lock);
		return;
	}
	IF_DEBUG("qtaguid: iface_stat: update(%s): entry=%p\n",
		 dev->name, entry);
	if (entry->active) {
		entry->tx_bytes += stats->tx_bytes;
		entry->tx_packets += stats->tx_packets;
		entry->rx_bytes += stats->rx_bytes;
		entry->rx_packets += stats->rx_packets;
		entry->active = false;
		IF_DEBUG("qtaguid: iface_stat: update(%s): "
			 " disable tracking. rx/tx=%llu/%llu\n",
			 dev->name, stats->rx_bytes, stats->tx_bytes);
	} else {
		IF_DEBUG("qtaguid: iface_stat: update(%s): disabled\n",
			dev->name);
	}
	spin_unlock_bh(&iface_stat_list_lock);
}

static void tag_stat_update(struct tag_stat *tag_entry,
			enum ifs_tx_rx direction, int proto, int bytes)
{
	int active_set;
	active_set = get_active_counter_set(tag_entry->tn.tag);
	MT_DEBUG("qtaguid: tag_stat_update(tag=0x%llx (uid=%u) set=%d "
		 "dir=%d proto=%d bytes=%d)\n",
		 tag_entry->tn.tag, get_uid_from_tag(tag_entry->tn.tag),
		 active_set, direction, proto, bytes);
	data_counters_update(&tag_entry->counters, active_set, direction,
			     proto, bytes);
	if (tag_entry->parent_counters)
		data_counters_update(tag_entry->parent_counters, active_set,
				     direction, proto, bytes);
}

/*
 * Create a new entry for tracking the specified {acct_tag,uid_tag} within
 * the interface.
 * iface_entry->tag_stat_list_lock should be held.
 */
static struct tag_stat *create_if_tag_stat(struct iface_stat *iface_entry,
					   tag_t tag)
{
	struct tag_stat *new_tag_stat_entry = NULL;
	IF_DEBUG("qtaguid: iface_stat: create_if_tag_stat(): ife=%p tag=0x%llx"
		 " (uid=%u)\n",
		 iface_entry, tag, get_uid_from_tag(tag));
	new_tag_stat_entry = kzalloc(sizeof(*new_tag_stat_entry), GFP_ATOMIC);
	if (!new_tag_stat_entry) {
		pr_err("qtaguid: iface_stat: tag stat alloc failed\n");
		goto done;
	}
	new_tag_stat_entry->tn.tag = tag;
	tag_stat_tree_insert(new_tag_stat_entry, &iface_entry->tag_stat_tree);
done:
	return new_tag_stat_entry;
}

static void if_tag_stat_update(const char *ifname, uid_t uid,
			       const struct sock *sk, enum ifs_tx_rx direction,
			       int proto, int bytes)
{
	struct tag_stat *tag_stat_entry;
	tag_t tag, acct_tag;
	tag_t uid_tag;
	struct data_counters *uid_tag_counters;
	struct sock_tag *sock_tag_entry;
	struct iface_stat *iface_entry;
	struct tag_stat *new_tag_stat;
	MT_DEBUG("qtaguid: if_tag_stat_update(ifname=%s "
		"uid=%u sk=%p dir=%d proto=%d bytes=%d)\n",
		 ifname, uid, sk, direction, proto, bytes);


	iface_entry = get_iface_entry(ifname);
	if (!iface_entry) {
		pr_err("qtaguid: iface_stat: stat_update() %s not found\n",
		       ifname);
		return;
	}
	/* It is ok to process data when an iface_entry is inactive */

	MT_DEBUG("qtaguid: iface_stat: stat_update() dev=%s entry=%p\n",
		 ifname, iface_entry);

	/*
	 * Look for a tagged sock.
	 * It will have an acct_uid.
	 */
	sock_tag_entry = get_sock_stat(sk);
	if (sock_tag_entry) {
		tag = sock_tag_entry->tag;
		acct_tag = get_atag_from_tag(tag);
		uid_tag = get_utag_from_tag(tag);
	} else {
		uid_tag = make_tag_from_uid(uid);
		acct_tag = 0;
		tag = combine_atag_with_uid(acct_tag, uid);
	}
	MT_DEBUG("qtaguid: iface_stat: stat_update(): "
		 " looking for tag=0x%llx (uid=%u) in ife=%p\n",
		 tag, get_uid_from_tag(tag), iface_entry);
	/* Loop over tag list under this interface for {acct_tag,uid_tag} */
	spin_lock_bh(&iface_entry->tag_stat_list_lock);

	tag_stat_entry = tag_stat_tree_search(&iface_entry->tag_stat_tree,
					      tag);
	if (tag_stat_entry) {
		/*
		 * Updating the {acct_tag, uid_tag} entry handles both stats:
		 * {0, uid_tag} will also get updated.
		 */
		tag_stat_update(tag_stat_entry, direction, proto, bytes);
		spin_unlock_bh(&iface_entry->tag_stat_list_lock);
		return;
	}

	/* Loop over tag list under this interface for {0,uid_tag} */
	tag_stat_entry = tag_stat_tree_search(&iface_entry->tag_stat_tree,
					      uid_tag);
	if (!tag_stat_entry) {
		/* Here: the base uid_tag did not exist */
		/*
		 * No parent counters. So
		 *  - No {0, uid_tag} stats and no {acc_tag, uid_tag} stats.
		 */
		new_tag_stat = create_if_tag_stat(iface_entry, uid_tag);
		uid_tag_counters = &new_tag_stat->counters;
	} else {
		uid_tag_counters = &tag_stat_entry->counters;
	}

	if (acct_tag) {
		new_tag_stat = create_if_tag_stat(iface_entry, tag);
		new_tag_stat->parent_counters = uid_tag_counters;
	}
	spin_unlock_bh(&iface_entry->tag_stat_list_lock);
	tag_stat_update(new_tag_stat, direction, proto, bytes);
}

static int iface_netdev_event_handler(struct notifier_block *nb,
				      unsigned long event, void *ptr) {
	struct net_device *dev = ptr;

	if (unlikely(module_passive))
		return NOTIFY_DONE;

	IF_DEBUG("qtaguid: iface_stat: netdev_event(): "
		 "ev=0x%lx netdev=%p->name=%s\n",
		 event, dev, dev ? dev->name : "");

	switch (event) {
	case NETDEV_UP:
		iface_stat_create(dev, NULL);
		break;
	case NETDEV_DOWN:
		iface_stat_update(dev);
		break;
	}
	return NOTIFY_DONE;
}

static int iface_inet6addr_event_handler(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct inet6_ifaddr *ifa = ptr;
	struct net_device *dev;

	if (unlikely(module_passive))
		return NOTIFY_DONE;

	IF_DEBUG("qtaguid: iface_stat: inet6addr_event(): "
		 "ev=0x%lx ifa=%p\n",
		 event, ifa);

	switch (event) {
	case NETDEV_UP:
		BUG_ON(!ifa || !ifa->idev);
		dev = (struct net_device *)ifa->idev->dev;
		iface_stat_create_ipv6(dev, ifa);
		atomic64_inc(&qtu_events.iface_events);
		break;
	case NETDEV_DOWN:
		BUG_ON(!ifa || !ifa->idev);
		dev = (struct net_device *)ifa->idev->dev;
		iface_stat_update(dev);
		atomic64_inc(&qtu_events.iface_events);
		break;
	}
	return NOTIFY_DONE;
}

static int iface_inetaddr_event_handler(struct notifier_block *nb,
					unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct net_device *dev;

	if (unlikely(module_passive))
		return NOTIFY_DONE;

	IF_DEBUG("qtaguid: iface_stat: inetaddr_event(): "
		 "ev=0x%lx ifa=%p\n",
		 event, ifa);

	switch (event) {
	case NETDEV_UP:
		BUG_ON(!ifa || !ifa->ifa_dev);
		dev = ifa->ifa_dev->dev;
		iface_stat_create(dev, ifa);
		atomic64_inc(&qtu_events.iface_events);
		break;
	case NETDEV_DOWN:
		BUG_ON(!ifa || !ifa->ifa_dev);
		dev = ifa->ifa_dev->dev;
		iface_stat_update(dev);
		atomic64_inc(&qtu_events.iface_events);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block iface_netdev_notifier_blk = {
	.notifier_call = iface_netdev_event_handler,
};

static struct notifier_block iface_inetaddr_notifier_blk = {
	.notifier_call = iface_inetaddr_event_handler,
};

static struct notifier_block iface_inet6addr_notifier_blk = {
	.notifier_call = iface_inet6addr_event_handler,
};

static int __init iface_stat_init(struct proc_dir_entry *parent_procdir)
{
	int err;

	iface_stat_procdir = proc_mkdir(iface_stat_procdirname, parent_procdir);
	if (!iface_stat_procdir) {
		pr_err("qtaguid: iface_stat: init failed to create proc entry\n");
		err = -1;
		goto err;
	}
	err = register_netdevice_notifier(&iface_netdev_notifier_blk);
	if (err) {
		pr_err("qtaguid: iface_stat: init "
		       "failed to register dev event handler\n");
		goto err_zap_entry;
	}
	err = register_inetaddr_notifier(&iface_inetaddr_notifier_blk);
	if (err) {
		pr_err("qtaguid: iface_stat: init "
		       "failed to register ipv4 dev event handler\n");
		goto err_unreg_nd;
	}

	err = register_inet6addr_notifier(&iface_inet6addr_notifier_blk);
	if (err) {
		pr_err("qtaguid: iface_stat: init "
		       "failed to register ipv6 dev event handler\n");
		goto err_unreg_ip4_addr;
	}
	return 0;

err_unreg_ip4_addr:
	unregister_inetaddr_notifier(&iface_inetaddr_notifier_blk);
err_unreg_nd:
	unregister_netdevice_notifier(&iface_netdev_notifier_blk);
err_zap_entry:
	remove_proc_entry(iface_stat_procdirname, parent_procdir);
err:
	return err;
}

static struct sock *qtaguid_find_sk(const struct sk_buff *skb,
				    struct xt_action_param *par)
{
	struct sock *sk;
	unsigned int hook_mask = (1 << par->hooknum);

	MT_DEBUG("qtaguid: find_sk(skb=%p) hooknum=%d family=%d\n", skb,
		 par->hooknum, par->family);

	/*
	 * Let's not abuse the the xt_socket_get*_sk(), or else it will
	 * return garbage SKs.
	 */
	if (!(hook_mask & XT_SOCKET_SUPPORTED_HOOKS))
		return NULL;

	switch (par->family) {
	case NFPROTO_IPV6:
		sk = xt_socket_get6_sk(skb, par);
		break;
	case NFPROTO_IPV4:
		sk = xt_socket_get4_sk(skb, par);
		break;
	default:
		return NULL;
	}

	/*
	 * Seems to be issues on the file ptr for TCP_TIME_WAIT SKs.
	 * http://kerneltrap.org/mailarchive/linux-netdev/2010/10/21/6287959
	 * Not fixed in 3.0-r3 :(
	 */
	if (sk) {
		MT_DEBUG("qtaguid: %p->sk_proto=%u "
			 "->sk_state=%d\n", sk, sk->sk_protocol, sk->sk_state);
		if (sk->sk_state  == TCP_TIME_WAIT) {
			xt_socket_put_sk(sk);
			sk = NULL;
		}
	}
	return sk;
}

static void account_for_uid(const struct sk_buff *skb,
			    const struct sock *alternate_sk, uid_t uid,
			    struct xt_action_param *par)
{
	const struct net_device *el_dev;

	if (!skb->dev) {
		MT_DEBUG("qtaguid[%d]: no skb->dev\n", par->hooknum);
		el_dev = par->in ? : par->out;
	} else {
		const struct net_device *other_dev;
		el_dev = skb->dev;
		other_dev = par->in ? : par->out;
		if (el_dev != other_dev) {
			MT_DEBUG("qtaguid[%d]: skb->dev=%p %s vs "
				"par->(in/out)=%p %s\n",
				par->hooknum, el_dev, el_dev->name, other_dev,
				other_dev->name);
		}
	}

	if (unlikely(!el_dev)) {
		pr_info("qtaguid[%d]: no par->in/out?!!\n", par->hooknum);
	} else if (unlikely(!el_dev->name)) {
		pr_info("qtaguid[%d]: no dev->name?!!\n", par->hooknum);
	} else {
		MT_DEBUG("qtaguid[%d]: dev name=%s type=%d\n",
			 par->hooknum,
			 el_dev->name,
			 el_dev->type);

		if_tag_stat_update(el_dev->name, uid,
				skb->sk ? skb->sk : alternate_sk,
				par->in ? IFS_RX : IFS_TX,
				ip_hdr(skb)->protocol, skb->len);
	}
}

static bool qtaguid_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_qtaguid_match_info *info = par->matchinfo;
	const struct file *filp;
	bool got_sock = false;
	struct sock *sk;
	uid_t sock_uid;
	bool res;

	if (unlikely(module_passive))
		return (info->match ^ info->invert) == 0;

	MT_DEBUG("qtaguid[%d]: entered skb=%p par->in=%p/out=%p fam=%d\n",
		 par->hooknum, skb, par->in, par->out, par->family);

	if (skb == NULL) {
		res = (info->match ^ info->invert) == 0;
		goto ret_res;
	}

	sk = skb->sk;

	if (sk == NULL) {
		/*
		 * A missing sk->sk_socket happens when packets are in-flight
		 * and the matching socket is already closed and gone.
		 */
		sk = qtaguid_find_sk(skb, par);
		/*
		 * If we got the socket from the find_sk(), we will need to put
		 * it back, as nf_tproxy_get_sock_v4() got it.
		 */
		got_sock = sk;
		if (sk)
			atomic64_inc(&qtu_events.match_found_sk_in_ct);
	} else {
		atomic64_inc(&qtu_events.match_found_sk);
	}
	MT_DEBUG("qtaguid[%d]: sk=%p got_sock=%d proto=%d\n",
		par->hooknum, sk, got_sock, ip_hdr(skb)->protocol);
	if (sk != NULL) {
		MT_DEBUG("qtaguid[%d]: sk=%p->sk_socket=%p->file=%p\n",
			par->hooknum, sk, sk->sk_socket,
			sk->sk_socket ? sk->sk_socket->file : (void *)-1LL);
		filp = sk->sk_socket ? sk->sk_socket->file : NULL;
		MT_DEBUG("qtaguid[%d]: filp...uid=%u\n",
			par->hooknum, filp ? filp->f_cred->fsuid : -1);
	}

	if (sk == NULL || sk->sk_socket == NULL) {
		/*
		 * Here, the qtaguid_find_sk() using connection tracking
		 * couldn't find the owner, so for now we just count them
		 * against the system.
		 */
		/*
		 * TODO: unhack how to force just accounting.
		 * For now we only do iface stats when the uid-owner is not
		 * requested.
		 */
		if (!(info->match & XT_QTAGUID_UID))
			account_for_uid(skb, sk, 0, par);
		MT_DEBUG("qtaguid[%d]: leaving (sk?sk->sk_socket)=%p\n",
			par->hooknum,
			sk ? sk->sk_socket : NULL);
		res = (info->match ^ info->invert) == 0;
		atomic64_inc(&qtu_events.match_found_sk_none);
		goto put_sock_ret_res;
	} else if (info->match & info->invert & XT_QTAGUID_SOCKET) {
		res = false;
		goto put_sock_ret_res;
	}
	filp = sk->sk_socket->file;
	if (filp == NULL) {
		MT_DEBUG("qtaguid[%d]: leaving filp=NULL\n", par->hooknum);
		res = ((info->match ^ info->invert) &
			(XT_QTAGUID_UID | XT_QTAGUID_GID)) == 0;
		goto put_sock_ret_res;
	}
	sock_uid = filp->f_cred->fsuid;
	/*
	 * TODO: unhack how to force just accounting.
	 * For now we only do iface stats when the uid-owner is not requested
	 */
	if (!(info->match & XT_QTAGUID_UID))
		account_for_uid(skb, sk, sock_uid, par);

	/*
	 * The following two tests fail the match when:
	 *    id not in range AND no inverted condition requested
	 * or id     in range AND    inverted condition requested
	 * Thus (!a && b) || (a && !b) == a ^ b
	 */
	if (info->match & XT_QTAGUID_UID)
		if ((filp->f_cred->fsuid >= info->uid_min &&
		     filp->f_cred->fsuid <= info->uid_max) ^
		    !(info->invert & XT_QTAGUID_UID)) {
			MT_DEBUG("qtaguid[%d]: leaving uid not matching\n",
				 par->hooknum);
			res = false;
			goto put_sock_ret_res;
		}
	if (info->match & XT_QTAGUID_GID)
		if ((filp->f_cred->fsgid >= info->gid_min &&
				filp->f_cred->fsgid <= info->gid_max) ^
			!(info->invert & XT_QTAGUID_GID)) {
			MT_DEBUG("qtaguid[%d]: leaving gid not matching\n",
				par->hooknum);
			res = false;
			goto put_sock_ret_res;
		}

	MT_DEBUG("qtaguid[%d]: leaving matched\n", par->hooknum);
	res = true;

put_sock_ret_res:
	if (got_sock)
		xt_socket_put_sk(sk);
ret_res:
	MT_DEBUG("qtaguid[%d]: left %d\n", par->hooknum, res);
	return res;
}

/*
 * Procfs reader to get all active socket tags using style "1)" as described in
 * fs/proc/generic.c
 */
static int qtaguid_ctrl_proc_read(char *page, char **num_items_returned,
				  off_t items_to_skip, int char_count, int *eof,
				  void *data)
{
	char *outp = page;
	int len;
	uid_t uid;
	struct sock_tag *sock_tag_entry;
	struct rb_node *node;
	int item_index = 0;

	if (unlikely(module_passive)) {
		*eof = 1;
		return 0;
	}

	/* TODO: support skipping num_items_returned on entry. */
	CT_DEBUG("qtaguid: proc ctrl page=%p off=%ld char_count=%d *eof=%d\n",
		page, items_to_skip, char_count, *eof);

	if (*eof)
		return 0;

	spin_lock_bh(&sock_tag_list_lock);
	for (node = rb_first(&sock_tag_tree);
	     node;
	     node = rb_next(node)) {
		if (item_index++ < items_to_skip)
			continue;
		sock_tag_entry = rb_entry(node, struct sock_tag, sock_node);
		uid = get_uid_from_tag(sock_tag_entry->tag);
		CT_DEBUG("qtaguid: proc_read(): sk=%p tag=0x%llx (uid=%u)\n",
			 sock_tag_entry->sk,
			 sock_tag_entry->tag,
			 uid
			);
		len = snprintf(outp, char_count,
			       "sock=%p tag=0x%llx (uid=%u)\n",
			       sock_tag_entry->sk, sock_tag_entry->tag, uid);
		if (len >= char_count) {
			spin_unlock_bh(&sock_tag_list_lock);
			*outp = '\0';
			return outp - page;
		}
		outp += len;
		char_count -= len;
		(*num_items_returned)++;
	}
	spin_unlock_bh(&sock_tag_list_lock);

	if (item_index++ >= items_to_skip) {
		len = snprintf(outp, char_count,
			       "events: sockets_tagged=%llu "
			       "sockets_untagged=%llu "
			       "counter_set_changes=%llu "
			       "delete_cmds=%llu "
			       "iface_events=%llu "
			       "match_found_sk=%llu "
			       "match_found_sk_in_ct=%llu "
			       "match_found_sk_none=%llu\n",
			       atomic64_read(&qtu_events.sockets_tagged),
			       atomic64_read(&qtu_events.sockets_untagged),
			       atomic64_read(&qtu_events.counter_set_changes),
			       atomic64_read(&qtu_events.delete_cmds),
			       atomic64_read(&qtu_events.iface_events),
			       atomic64_read(&qtu_events.match_found_sk),
			       atomic64_read(&qtu_events.match_found_sk_in_ct),
			       atomic64_read(&qtu_events.match_found_sk_none));
		if (len >= char_count) {
			*outp = '\0';
			return outp - page;
		}
		outp += len;
		char_count -= len;
		(*num_items_returned)++;
	}

	*eof = 1;
	return outp - page;
}

static bool can_manipulate_uids(void)
{
	/* root pwnd */
	return unlikely(!current_fsuid()) || unlikely(!proc_ctrl_write_gid)
		|| in_egroup_p(proc_ctrl_write_gid);
}

static bool can_impersonate_uid(uid_t uid)
{
	return uid == current_fsuid() || can_manipulate_uids();
}

static bool can_read_other_uid_stats(uid_t uid)
{
	/* root pwnd */
	return unlikely(!current_fsuid()) || uid == current_fsuid()
		|| unlikely(!proc_stats_readall_gid)
		|| in_egroup_p(proc_stats_readall_gid);
}

/*
 * Delete socket tags, and stat tags associated with a given
 * accouting tag and uid.
 */
static int ctrl_cmd_delete(const char *input)
{
	char cmd;
	uid_t uid;
	uid_t entry_uid;
	tag_t acct_tag;
	tag_t tag;
	int res, argc;
	struct iface_stat *iface_entry;
	struct rb_node *node;
	struct sock_tag *st_entry;
	struct rb_root st_to_free_tree = RB_ROOT;
	struct tag_stat *ts_entry;
	struct tag_counter_set *tcs_entry;

	argc = sscanf(input, "%c %llu %u", &cmd, &acct_tag, &uid);
	CT_DEBUG("qtaguid: ctrl_delete(%s): argc=%d cmd=%c "
		 "user_tag=0x%llx uid=%u\n", input, argc, cmd,
		 acct_tag, uid);
	if (argc < 2) {
		res = -EINVAL;
		goto err;
	}
	if (!valid_atag(acct_tag)) {
		pr_info("qtaguid: ctrl_delete(%s): invalid tag\n", input);
		res = -EINVAL;
		goto err;
	}
	if (argc < 3) {
		uid = current_fsuid();
	} else if (!can_impersonate_uid(uid)) {
		pr_info("qtaguid: ctrl_delete(%s): "
			"insufficient priv from pid=%u uid=%u\n",
			input, current->pid, current_fsuid());
		res = -EPERM;
		goto err;
	}

	/* Delete socket tags */
	spin_lock_bh(&sock_tag_list_lock);
	node = rb_first(&sock_tag_tree);
	while (node) {
		st_entry = rb_entry(node, struct sock_tag, sock_node);
		entry_uid = get_uid_from_tag(st_entry->tag);
		node = rb_next(node);
		if (entry_uid != uid)
			continue;

		if (!acct_tag || st_entry->tag == tag) {
			rb_erase(&st_entry->sock_node, &sock_tag_tree);
			/* Can't sockfd_put() within spinlock, do it later. */
			sock_tag_tree_insert(st_entry, &st_to_free_tree);
		}
	}
	spin_unlock_bh(&sock_tag_list_lock);

	node = rb_first(&st_to_free_tree);
	while (node) {
		st_entry = rb_entry(node, struct sock_tag, sock_node);
		node = rb_next(node);
		CT_DEBUG("qtaguid: ctrl_delete(): "
			 "erase st: sk=%p tag=0x%llx (uid=%u)\n",
			 st_entry->sk,
			 st_entry->tag,
			 entry_uid);
		rb_erase(&st_entry->sock_node, &st_to_free_tree);
		sockfd_put(st_entry->socket);
		kfree(st_entry);
	}

	tag = combine_atag_with_uid(acct_tag, uid);

	/* Delete tag counter-sets */
	spin_lock_bh(&tag_counter_set_list_lock);
	tcs_entry = tag_counter_set_tree_search(&tag_counter_set_tree, tag);
	if (tcs_entry) {
		CT_DEBUG("qtaguid: ctrl_delete(): "
			 "erase tcs: tag=0x%llx (uid=%u) set=%d\n",
			 tcs_entry->tn.tag,
			 get_uid_from_tag(tcs_entry->tn.tag),
			 tcs_entry->active_set);
		rb_erase(&tcs_entry->tn.node, &tag_counter_set_tree);
		kfree(tcs_entry);
	}
	spin_unlock_bh(&tag_counter_set_list_lock);

	/*
	 * If acct_tag is 0, then all entries belonging to uid are
	 * erased.
	 */
	spin_lock_bh(&iface_stat_list_lock);
	list_for_each_entry(iface_entry, &iface_stat_list, list) {
		spin_lock_bh(&iface_entry->tag_stat_list_lock);
		node = rb_first(&iface_entry->tag_stat_tree);
		while (node) {
			ts_entry = rb_entry(node, struct tag_stat, tn.node);
			entry_uid = get_uid_from_tag(ts_entry->tn.tag);
			node = rb_next(node);
			if (entry_uid != uid)
				continue;
			if (!acct_tag || ts_entry->tn.tag == tag) {
				CT_DEBUG("qtaguid: ctrl_delete(): "
					 "erase ts: %s 0x%llx %u\n",
					 iface_entry->ifname,
					 get_atag_from_tag(ts_entry->tn.tag),
					 entry_uid);
				rb_erase(&ts_entry->tn.node,
					 &iface_entry->tag_stat_tree);
				kfree(ts_entry);
			}
		}
		spin_unlock_bh(&iface_entry->tag_stat_list_lock);
	}
	spin_unlock_bh(&iface_stat_list_lock);
	atomic64_inc(&qtu_events.delete_cmds);
	res = 0;

err:
	return res;
}

static int ctrl_cmd_counter_set(const char *input)
{
	char cmd;
	uid_t uid = 0;
	tag_t tag;
	int res, argc;
	struct tag_counter_set *tcs;
	int counter_set;

	argc = sscanf(input, "%c %d %u", &cmd, &counter_set, &uid);
	CT_DEBUG("qtaguid: ctrl_counterset(%s): argc=%d cmd=%c "
		 "set=%d uid=%u\n", input, argc, cmd,
		 counter_set, uid);
	if (argc != 3) {
		res = -EINVAL;
		goto err;
	}
	if (counter_set < 0 || counter_set >= IFS_MAX_COUNTER_SETS) {
		pr_info("qtaguid: ctrl_counterset(%s): invalid counter_set range\n",
			input);
		res = -EINVAL;
		goto err;
	}
	if (!can_manipulate_uids()) {
		pr_info("qtaguid: ctrl_counterset(%s): "
			"insufficient priv from pid=%u uid=%u\n",
			input, current->pid, current_fsuid());
		res = -EPERM;
		goto err;
	}

	tag = make_tag_from_uid(uid);
	spin_lock_bh(&tag_counter_set_list_lock);
	tcs = tag_counter_set_tree_search(&tag_counter_set_tree, tag);
	if (!tcs) {
		tcs = kzalloc(sizeof(*tcs), GFP_ATOMIC);
		if (!tcs) {
			spin_unlock_bh(&tag_counter_set_list_lock);
			pr_err("qtaguid: ctrl_counterset(%s): "
			       "failed to alloc counter set\n",
			       input);
			res = -ENOMEM;
			goto err;
		}
		tcs->tn.tag = tag;
		tag_counter_set_tree_insert(tcs, &tag_counter_set_tree);
		CT_DEBUG("qtaguid: ctrl_counterset(%s): added tcs tag=0x%llx "
			 "(uid=%u) set=%d\n",
			 input, tag, get_uid_from_tag(tag), counter_set);
	}
	tcs->active_set = counter_set;
	spin_unlock_bh(&tag_counter_set_list_lock);
	atomic64_inc(&qtu_events.counter_set_changes);
	res = 0;

err:
	return res;
}

static int ctrl_cmd_tag(const char *input)
{
	char cmd;
	int sock_fd = 0;
	uid_t uid = 0;
	tag_t acct_tag = 0;
	struct socket *el_socket;
	int refcnt = -1;
	int res, argc;
	struct sock_tag *sock_tag_entry;

	/* Unassigned args will get defaulted later. */
	argc = sscanf(input, "%c %d %llu %u", &cmd, &sock_fd, &acct_tag, &uid);
	CT_DEBUG("qtaguid: ctrl_tag(%s): argc=%d cmd=%c sock_fd=%d "
		 "acct_tag=0x%llx uid=%u\n", input, argc, cmd, sock_fd,
		 acct_tag, uid);
	if (argc < 2) {
		res = -EINVAL;
		goto err;
	}
	el_socket = sockfd_lookup(sock_fd, &res);  /* This locks the file */
	if (!el_socket) {
		pr_info("qtaguid: ctrl_tag(%s): failed to lookup"
			" sock_fd=%d err=%d\n", input, sock_fd, res);
		goto err;
	}
	refcnt = atomic_read(&el_socket->file->f_count);
	CT_DEBUG("qtaguid: ctrl_tag(%s): socket->...->f_count=%d\n",
		 input, refcnt);
	if (argc < 3) {
		acct_tag = 0;
	} else if (!valid_atag(acct_tag)) {
		pr_info("qtaguid: ctrl_tag(%s): invalid tag\n", input);
		res = -EINVAL;
		goto err_put;
	}
	CT_DEBUG("qtaguid: ctrl_tag(%s): "
		 "uid=%u euid=%u fsuid=%u "
		 "in_group=%d in_egroup=%d\n",
		 input, current_uid(), current_euid(), current_fsuid(),
		 in_group_p(proc_stats_readall_gid),
		 in_egroup_p(proc_stats_readall_gid));
	if (argc < 4) {
		uid = current_fsuid();
	} else if (!can_impersonate_uid(uid)) {
		pr_info("qtaguid: ctrl_tag(%s): "
			"insufficient priv from pid=%u uid=%u\n",
			input, current->pid, current_fsuid());
		res = -EPERM;
		goto err_put;
	}

	spin_lock_bh(&sock_tag_list_lock);
	sock_tag_entry = get_sock_stat_nl(el_socket->sk);
	if (sock_tag_entry) {
		/*
		 * This is a re-tagging, so release the sock_fd that was
		 * locked at the time of the 1st tagging.
		 */
		sockfd_put(sock_tag_entry->socket);
		refcnt--;
		sock_tag_entry->tag = combine_atag_with_uid(acct_tag,
							    uid);
	} else {
		sock_tag_entry = kzalloc(sizeof(*sock_tag_entry),
					 GFP_ATOMIC);
		if (!sock_tag_entry) {
			pr_err("qtaguid: ctrl_tag(%s): "
			       "socket tag alloc failed\n",
			       input);
			spin_unlock_bh(&sock_tag_list_lock);
			res = -ENOMEM;
			goto err_put;
		}
		sock_tag_entry->sk = el_socket->sk;
		sock_tag_entry->socket = el_socket;
		sock_tag_entry->tag = combine_atag_with_uid(acct_tag,
							    uid);
		sock_tag_tree_insert(sock_tag_entry, &sock_tag_tree);
		atomic64_inc(&qtu_events.sockets_tagged);
	}
	spin_unlock_bh(&sock_tag_list_lock);
	/* We keep the ref to the socket (file) until it is untagged */
	CT_DEBUG("qtaguid: ctrl_tag(%s): done. socket->...->f_count=%d\n",
		 input,
		 el_socket ? atomic_read(&el_socket->file->f_count) : -1);
	return 0;

err_put:
	/* Release the sock_fd that was grabbed by sockfd_lookup(). */
	sockfd_put(el_socket);
	refcnt--;
err:
	CT_DEBUG("qtaguid: ctrl_tag(%s): done. socket->...->f_count=%d\n",
		 input, refcnt);
	return res;
}

static int ctrl_cmd_untag(const char *input)
{
	char cmd;
	int sock_fd = 0;
	struct socket *el_socket;
	int refcnt = -1;
	int res, argc;
	struct sock_tag *sock_tag_entry;

	argc = sscanf(input, "%c %d", &cmd, &sock_fd);
	CT_DEBUG("qtaguid: ctrl_untag(%s): argc=%d cmd=%c sock_fd=%d\n",
		 input, argc, cmd, sock_fd);
	if (argc < 2) {
		res = -EINVAL;
		goto err;
	}
	el_socket = sockfd_lookup(sock_fd, &res);  /* This locks the file */
	if (!el_socket) {
		pr_info("qtaguid: ctrl_untag(%s): failed to lookup"
			" sock_fd=%d err=%d\n", input, sock_fd, res);
		goto err;
	}
	refcnt = atomic_read(&el_socket->file->f_count);
	CT_DEBUG("qtaguid: ctrl_untag(%s): socket->...->f_count=%d\n",
		 input, refcnt);
	spin_lock_bh(&sock_tag_list_lock);
	sock_tag_entry = get_sock_stat_nl(el_socket->sk);
	if (!sock_tag_entry) {
		spin_unlock_bh(&sock_tag_list_lock);
		res = -EINVAL;
		goto err_put;
	}
	/*
	 * The socket already belongs to the current process
	 * so it can do whatever it wants to it.
	 */
	rb_erase(&sock_tag_entry->sock_node, &sock_tag_tree);

	/*
	 * Release the sock_fd that was grabbed at tag time,
	 * and once more for the sockfd_lookup() here.
	 */
	sockfd_put(sock_tag_entry->socket);
	spin_unlock_bh(&sock_tag_list_lock);
	sockfd_put(el_socket);
	refcnt -= 2;
	kfree(sock_tag_entry);
	atomic64_inc(&qtu_events.sockets_untagged);
	CT_DEBUG("qtaguid: ctrl_untag(%s): done. socket->...->f_count=%d\n",
		 input, refcnt);

	return 0;

err_put:
	/* Release the sock_fd that was grabbed by sockfd_lookup(). */
	sockfd_put(el_socket);
	refcnt--;
err:
	CT_DEBUG("qtaguid: ctrl_untag(%s): done. socket->...->f_count=%d\n",
		 input, refcnt);
	return res;
}

static int qtaguid_ctrl_parse(const char *input, int count)
{
	char cmd;
	int res;

	cmd = input[0];
	/* Collect params for commands */
	switch (cmd) {
	case 'd':
		res = ctrl_cmd_delete(input);
		break;

	case 's':
		res = ctrl_cmd_counter_set(input);
		break;

	case 't':
		res = ctrl_cmd_tag(input);
		break;

	case 'u':
		res = ctrl_cmd_untag(input);
		break;

	default:
		res = -EINVAL;
		goto err;
	}
	if (!res)
		res = count;
err:
	CT_DEBUG("qtaguid: ctrl(%s): res=%d\n", input, res);
	return res;
}

#define MAX_QTAGUID_CTRL_INPUT_LEN 255
static int qtaguid_ctrl_proc_write(struct file *file, const char __user *buffer,
			unsigned long count, void *data)
{
	char input_buf[MAX_QTAGUID_CTRL_INPUT_LEN];

	if (unlikely(module_passive))
		return count;

	if (count >= MAX_QTAGUID_CTRL_INPUT_LEN)
		return -EINVAL;

	if (copy_from_user(input_buf, buffer, count))
		return -EFAULT;

	input_buf[count] = '\0';
	return qtaguid_ctrl_parse(input_buf, count);
}

struct proc_print_info {
	char *outp;
	char **num_items_returned;
	struct iface_stat *iface_entry;
	struct tag_stat *ts_entry;
	int item_index;
	int char_count;
};

static int pp_stats_line(struct proc_print_info *ppi, int cnt_set)
{
	int len;
	struct data_counters *cnts;
	if (!ppi->item_index) {
		len = snprintf(ppi->outp, ppi->char_count,
			       "idx iface acct_tag_hex uid_tag_int cnt_set "
			       "rx_bytes rx_packets "
			       "tx_bytes tx_packets "
			       "rx_tcp_packets rx_tcp_bytes "
			       "rx_udp_packets rx_udp_bytes "
			       "rx_other_packets rx_other_bytes "
			       "tx_tcp_packets tx_tcp_bytes "
			       "tx_udp_packets tx_udp_bytes "
			       "tx_other_packets tx_other_bytes\n");
	} else {
		tag_t tag = ppi->ts_entry->tn.tag;
		uid_t stat_uid = get_uid_from_tag(tag);
		if (!can_read_other_uid_stats(stat_uid)) {
			CT_DEBUG("qtaguid: stats line: "
				 "%s 0x%llx %u: "
				 "insufficient priv from pid=%u uid=%u\n",
				 ppi->iface_entry->ifname,
				 get_atag_from_tag(tag), stat_uid,
				 current->pid, current_fsuid());
			return 0;
		}
		cnts = &ppi->ts_entry->counters;
		len = snprintf(
			ppi->outp, ppi->char_count,
			"%d %s 0x%llx %u %u "
			"%llu %llu "
			"%llu %llu "
			"%llu %llu "
			"%llu %llu "
			"%llu %llu "
			"%llu %llu "
			"%llu %llu "
			"%llu %llu\n",
			ppi->item_index,
			ppi->iface_entry->ifname,
			get_atag_from_tag(tag),
			stat_uid,
			cnt_set,
			dc_sum_bytes(cnts, cnt_set, IFS_RX),
			dc_sum_packets(cnts, cnt_set, IFS_RX),
			dc_sum_bytes(cnts, cnt_set, IFS_TX),
			dc_sum_packets(cnts, cnt_set, IFS_TX),
			cnts->bpc[cnt_set][IFS_RX][IFS_TCP].bytes,
			cnts->bpc[cnt_set][IFS_RX][IFS_TCP].packets,
			cnts->bpc[cnt_set][IFS_RX][IFS_UDP].bytes,
			cnts->bpc[cnt_set][IFS_RX][IFS_UDP].packets,
			cnts->bpc[cnt_set][IFS_RX][IFS_PROTO_OTHER].bytes,
			cnts->bpc[cnt_set][IFS_RX][IFS_PROTO_OTHER].packets,
			cnts->bpc[cnt_set][IFS_TX][IFS_TCP].bytes,
			cnts->bpc[cnt_set][IFS_TX][IFS_TCP].packets,
			cnts->bpc[cnt_set][IFS_TX][IFS_UDP].bytes,
			cnts->bpc[cnt_set][IFS_TX][IFS_UDP].packets,
			cnts->bpc[cnt_set][IFS_TX][IFS_PROTO_OTHER].bytes,
			cnts->bpc[cnt_set][IFS_TX][IFS_PROTO_OTHER].packets);
	}
	return len;
}

bool pp_sets(struct proc_print_info *ppi)
{
	int len;
	int counter_set;
	for (counter_set = 0; counter_set < IFS_MAX_COUNTER_SETS;
	     counter_set++) {
		len = pp_stats_line(ppi, counter_set);
		if (len >= ppi->char_count) {
			*ppi->outp = '\0';
			return false;
		}
		if (len) {
			ppi->outp += len;
			ppi->char_count -= len;
			(*ppi->num_items_returned)++;
		}
	}
	return true;
}

/*
 * Procfs reader to get all tag stats using style "1)" as described in
 * fs/proc/generic.c
 * Groups all protocols tx/rx bytes.
 */
static int qtaguid_stats_proc_read(char *page, char **num_items_returned,
				off_t items_to_skip, int char_count, int *eof,
				void *data)
{
	struct proc_print_info ppi;
	int len;

	ppi.outp = page;
	ppi.item_index = 0;
	ppi.char_count = char_count;
	ppi.num_items_returned = num_items_returned;

	if (unlikely(module_passive)) {
		len = pp_stats_line(&ppi, 0);
		/* The header should always be shorter than the buffer. */
		WARN_ON(len >= ppi.char_count);
		*eof = 1;
		return len;
	}

	CT_DEBUG("qtaguid:proc stats page=%p *num_items_returned=%p off=%ld "
		"char_count=%d *eof=%d\n", page, *num_items_returned,
		items_to_skip, char_count, *eof);

	if (*eof)
		return 0;

	if (!items_to_skip) {
		/* The idx is there to help debug when things go belly up. */
		len = pp_stats_line(&ppi, 0);
		/* Don't advance the outp unless the whole line was printed */
		if (len >= ppi.char_count) {
			*ppi.outp = '\0';
			return ppi.outp - page;
		}
		ppi.outp += len;
		ppi.char_count -= len;
	}

	spin_lock_bh(&iface_stat_list_lock);
	list_for_each_entry(ppi.iface_entry, &iface_stat_list, list) {
		struct rb_node *node;
		spin_lock_bh(&ppi.iface_entry->tag_stat_list_lock);
		for (node = rb_first(&ppi.iface_entry->tag_stat_tree);
		     node;
		     node = rb_next(node)) {
			ppi.ts_entry = rb_entry(node, struct tag_stat, tn.node);
			if (ppi.item_index++ < items_to_skip)
				continue;
			if (!pp_sets(&ppi)) {
				spin_unlock_bh(
					&ppi.iface_entry->tag_stat_list_lock);
				spin_unlock_bh(&iface_stat_list_lock);
				return ppi.outp - page;
			}
		}
		spin_unlock_bh(&ppi.iface_entry->tag_stat_list_lock);
	}
	spin_unlock_bh(&iface_stat_list_lock);

	*eof = 1;
	return ppi.outp - page;
}

/*------------------------------------------*/
static int __init qtaguid_proc_register(struct proc_dir_entry **res_procdir)
{
	int ret;
	*res_procdir = proc_mkdir("xt_qtaguid", init_net.proc_net);
	if (!*res_procdir) {
		pr_err("qtaguid: failed to create proc/.../xt_qtaguid\n");
		ret = -ENOMEM;
		goto no_dir;
	}

	xt_qtaguid_ctrl_file = create_proc_entry("ctrl", proc_ctrl_perms,
						*res_procdir);
	if (!xt_qtaguid_ctrl_file) {
		pr_err("qtaguid: failed to create xt_qtaguid/ctrl "
			" file\n");
		ret = -ENOMEM;
		goto no_ctrl_entry;
	}
	xt_qtaguid_ctrl_file->read_proc = qtaguid_ctrl_proc_read;
	xt_qtaguid_ctrl_file->write_proc = qtaguid_ctrl_proc_write;

	xt_qtaguid_stats_file = create_proc_entry("stats", proc_stats_perms,
						*res_procdir);
	if (!xt_qtaguid_stats_file) {
		pr_err("qtaguid: failed to create xt_qtaguid/stats "
			"file\n");
		ret = -ENOMEM;
		goto no_stats_entry;
	}
	xt_qtaguid_stats_file->read_proc = qtaguid_stats_proc_read;
	/*
	 * TODO: add support counter hacking
	 * xt_qtaguid_stats_file->write_proc = qtaguid_stats_proc_write;
	 */
	return 0;

no_stats_entry:
	remove_proc_entry("ctrl", *res_procdir);
no_ctrl_entry:
	remove_proc_entry("xt_qtaguid", NULL);
no_dir:
	return ret;
}

static struct xt_match qtaguid_mt_reg __read_mostly = {
	/*
	 * This module masquerades as the "owner" module so that iptables
	 * tools can deal with it.
	 */
	.name       = "owner",
	.revision   = 1,
	.family     = NFPROTO_UNSPEC,
	.match      = qtaguid_mt,
	.matchsize  = sizeof(struct xt_qtaguid_match_info),
	.me         = THIS_MODULE,
};

static int __init qtaguid_mt_init(void)
{
	if (qtaguid_proc_register(&xt_qtaguid_procdir)
	    || iface_stat_init(xt_qtaguid_procdir)
	    || xt_register_match(&qtaguid_mt_reg)) {
		return -1;
	} else {
        }
}

/*
 * TODO: allow unloading of the module.
 * For now stats are permanent.
 * Kconfig forces'y/n' and never an 'm'.
 */

module_init(qtaguid_mt_init);
MODULE_AUTHOR("jpa <jpa@google.com>");
MODULE_DESCRIPTION("Xtables: socket owner+tag matching and associated stats");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_owner");
MODULE_ALIAS("ip6t_owner");
MODULE_ALIAS("ipt_qtaguid");
MODULE_ALIAS("ip6t_qtaguid");
