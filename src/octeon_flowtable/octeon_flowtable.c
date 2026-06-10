// SPDX-License-Identifier: GPL-2.0
/*
 * octeon_flowtable - nftables flow offload backend for the Cavium Octeon+
 * (CN50XX) packet complex, built on the mainline staging octeon_ethernet
 * driver.
 *
 * Clean-room reimplementation. Design: docs/design-rfc-octeon-flowtable.md.
 * NO code, struct layouts, or constants taken from the Ubiquiti binary module
 * or the Cavium SDK. The hardware model is derived from the GPL headers in
 * arch/mips/include/asm/octeon/ (see docs/blocks/).
 *
 * Copyright (c) 2026 (cavium-offload-port project)
 *
 * ============================ MILESTONE 2 ============================
 * First real hardware fast path. On FLOW_CLS_REPLACE we parse the offload
 * rule (5-tuple match + the generic MANGLE/REDIRECT action list) into a flow
 * entry in an rhashtable. A WQE-level RX hook (registered with the staging
 * driver) looks up forwarded packets by 5-tuple, replays the mangles in the
 * FPA packet buffer, decrements TTL, recomputes IP+L4 checksums, and transmits
 * via PKO (cvm_oct_transmit_qos) without ever building an skb. Misses fall
 * through to the normal Linux receive path.
 * ====================================================================
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <net/ip.h>
#include <net/checksum.h>
#include <net/flow_offload.h>
#include <net/netfilter/nf_flow_table.h>

#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx.h>
#include <asm/octeon/cvmx-wqe.h>
#include <asm/octeon/cvmx-packet.h>
#include <asm/octeon/cvmx-config.h>	/* cvmx_fau_reg_*_t enums (board FAU map) */
#include <asm/octeon/cvmx-fau.h>

/* ---- staging octeon_ethernet exports we consume (declared here; the symbols
 *      are EXPORT_SYMBOL'd by the patched staging driver) ---- */
typedef int (*cvm_oct_rx_hook_t)(struct cvmx_wqe *work);
void cvm_oct_register_rx_hook(cvm_oct_rx_hook_t hook);
void cvm_oct_unregister_rx_hook(void);
int cvm_oct_transmit_qos(struct net_device *dev, void *work_queue_entry,
			 int do_free, int qos);
int cvm_oct_get_port(const struct net_device *dev);
int cvm_oct_free_work(void *work_queue_entry);
extern int cvm_oct_flow_aqm_fau;	/* staging: per-port in-flight FAU base */

#define DRV_NAME "octeon_flowtable"
#define OCTEON_ETH_DRV_NAME "octeon_ethernet"

static bool octeon_ft_verbose;
module_param_named(verbose, octeon_ft_verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "log flow offload commands and stats");

/*
 * Disable the wildcard-VID fallback lookup. The fallback is REQUIRED for
 * bridged-VLAN (lower-device) offload rules — the kernel keys those untagged
 * while the wire packets carry tags — but it also lets a tagged packet with
 * ANY VID match an untagged-keyed flow on the same ingress port, i.e. the fast
 * path forwards frames that bridge VLAN ingress filtering / the stack would
 * drop. On configs with no bridged VLANs (plain routing and/or 802.1Q
 * subinterfaces only), set vlan_strict=1 to close that gap.
 */
static bool octeon_ft_vlan_strict;
module_param_named(vlan_strict, octeon_ft_vlan_strict, bool, 0644);
MODULE_PARM_DESC(vlan_strict,
		 "disable wildcard-VID fallback (breaks bridged-VLAN offload; tagged packets then match only VLAN-keyed flows)");

static bool octeon_ft_fau_stats;
module_param_named(fau_stats, octeon_ft_fau_stats, bool, 0644);
MODULE_PARM_DESC(fau_stats,
		 "accumulate global fast-path byte/packet totals in hardware FAU");

/*
 * Two free 64-bit FAU registers for global fast-path counters. The Octeon FAU
 * is only 2 KB (0..2047) — far too small for per-flow counters (the staging
 * driver itself uses the top, growing down from ~2040), so per-flow stats stay
 * in software (octeon_ft_flow.{packets,bytes}). These two are a HW-atomic global
 * aggregate: a global software atomic64 would bounce its cache line between the
 * two NPU cores, whereas an FAU add is a single fire-and-forget I/O store the
 * hardware serializes. Gated by fau_stats (off by default — zero hot-path cost).
 * Read back via the hw_tx_{bytes,packets} parameters.
 */
#define OCT_FT_FAU_BYTES	8
#define OCT_FT_FAU_PKTS		16

static inline void octeon_ft_fau_account(u32 bytes)
{
	cvmx_fau_atomic_add64(OCT_FT_FAU_BYTES, bytes);
	cvmx_fau_atomic_add64(OCT_FT_FAU_PKTS, 1);
}

/* Read-only params exposing the FAU counters (the register is carried in kp->arg
 * so one .get serves both). /sys/module/octeon_flowtable/parameters/hw_tx_*. */
static int octeon_ft_fau_show(char *buf, const struct kernel_param *kp)
{
	u64 reg = (uintptr_t)kp->arg;

	return sysfs_emit(buf, "%lld\n", (long long)cvmx_fau_fetch_and_add64(reg, 0));
}
static const struct kernel_param_ops octeon_ft_fau_ops = {
	.get = octeon_ft_fau_show,
};
module_param_cb(hw_tx_bytes, &octeon_ft_fau_ops,
		(void *)(uintptr_t)OCT_FT_FAU_BYTES, 0444);
module_param_cb(hw_tx_packets, &octeon_ft_fau_ops,
		(void *)(uintptr_t)OCT_FT_FAU_PKTS, 0444);

/* ---------------- PKO output-queue tail-drop AQM (#5) ----------------
 * Per-egress-port in-flight-bytes counter at OCT_FT_AQM_BASE + port*8: the hook
 * adds a packet's egress bytes when it queues to PKO, the patched
 * cvm_oct_transmit_qos arms PKO to subtract them on transmit (via reg0). So the
 * counter is bytes given to PKO but not yet on the wire; when it exceeds
 * aqm_limit the hook tail-drops, bounding worst-case queue latency.
 */
#define OCT_FT_AQM_BASE		24
#define OCT_FT_AQM_PORTS	8

static atomic_t octeon_ft_drops = ATOMIC_INIT(0);
static unsigned int octeon_ft_aqm_limit;	/* per-port byte cap; 0 = off */

static int octeon_ft_aqm_set(const char *val, const struct kernel_param *kp)
{
	unsigned int lim;
	int p, ret = kstrtouint(val, 0, &lim);

	if (ret)
		return ret;
	/* Ordering: the hook starts reserving bytes the moment aqm_limit goes
	 * non-zero, so the counters must be zeroed and PKO's subtract armed
	 * BEFORE the gate opens (the old set-limit-first order let concurrent
	 * reservations be wiped by the zeroing -> permanent counter drift).
	 */
	if (lim) {		/* arm: zero counters + PKO subtract, then gate */
		for (p = 0; p < OCT_FT_AQM_PORTS; p++)
			cvmx_fau_atomic_write64(OCT_FT_AQM_BASE + p * 8, 0);
		WRITE_ONCE(cvm_oct_flow_aqm_fau, OCT_FT_AQM_BASE);
		WRITE_ONCE(octeon_ft_aqm_limit, lim);
	} else {		/* disarm: close the gate first */
		WRITE_ONCE(octeon_ft_aqm_limit, 0);
		WRITE_ONCE(cvm_oct_flow_aqm_fau, 0);
	}
	return 0;
}
static const struct kernel_param_ops octeon_ft_aqm_ops = {
	.set = octeon_ft_aqm_set,
	.get = param_get_uint,
};
module_param_cb(aqm_limit, &octeon_ft_aqm_ops, &octeon_ft_aqm_limit, 0644);
MODULE_PARM_DESC(aqm_limit,
		 "per-egress-port in-flight byte cap (tail-drop AQM); 0 = off");

static int octeon_ft_drops_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&octeon_ft_drops));
}
static const struct kernel_param_ops octeon_ft_drops_ops = {
	.get = octeon_ft_drops_get,
};
module_param_cb(aqm_drops, &octeon_ft_drops_ops, NULL, 0444);

/* current in-flight bytes per port (read-only, for observing the AQM bound) */
static int octeon_ft_inflight_get(char *buf, const struct kernel_param *kp)
{
	int p, n = 0;

	for (p = 0; p < OCT_FT_AQM_PORTS; p++) {
		s64 v = cvmx_fau_fetch_and_add64(OCT_FT_AQM_BASE + p * 8, 0);

		if (v)
			n += sysfs_emit_at(buf, n, "port%d=%lld ", p, (long long)v);
	}
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static const struct kernel_param_ops octeon_ft_inflight_ops = {
	.get = octeon_ft_inflight_get,
};
module_param_cb(aqm_inflight, &octeon_ft_inflight_ops, NULL, 0444);

/* Tail-drop check: true => drop. On false (and AQM armed) the packet's @bytes
 * are reserved in the port's in-flight counter (PKO subtracts them on TX). */
static inline bool octeon_ft_aqm_overlimit(u8 eport, u32 bytes)
{
	u64 reg = OCT_FT_AQM_BASE + (u64)eport * 8;
	unsigned int lim = READ_ONCE(octeon_ft_aqm_limit);

	/* Only ports 0..OCT_FT_AQM_PORTS-1 have a reserved FAU slot. A higher
	 * PIP port (POW/loop/mgmt) would address registers belonging to the
	 * staging driver's FAU map — or past the 2 KB FAU — so it gets no AQM
	 * rather than a corrupted counter. (cvm_oct_transmit_qos arms the PKO
	 * subtract for any port, but it only ever sees reserved bytes that this
	 * gate admitted, so out-of-range ports never reserve and the matching
	 * subtract hits a counter we also never read.)
	 */
	if (!lim || eport >= OCT_FT_AQM_PORTS)
		return false;
	if (cvmx_fau_fetch_and_add64(reg, 0) > (s64)lim) {
		atomic_inc(&octeon_ft_drops);
		return true;
	}
	cvmx_fau_atomic_add64(reg, bytes);
	return false;
}

static inline void octeon_ft_aqm_unreserve(u8 eport, u32 bytes)
{
	if (READ_ONCE(octeon_ft_aqm_limit) && eport < OCT_FT_AQM_PORTS)
		cvmx_fau_atomic_add64(OCT_FT_AQM_BASE + (u64)eport * 8,
				      -(s64)bytes);
}

/* ---- flow table ---- */

struct octeon_ft_key {
	__be32	saddr;
	__be32	daddr;
	__be16	sport;
	__be16	dport;
	u8	l4proto;
	u8	pad;
	u16	vlan_id;	/* ingress OUTERMOST 802.1Q VID (host order); 0 =
				 * untagged. Distinguishes the same 5-tuple on
				 * different VLANs (router-on-a-stick). */
	u16	vlan_id2;	/* ingress INNER VID for QinQ; 0 if < 2 tags. */
	u16	iif_port;	/* ingress octeon PIP port — disambiguates the
				 * same 5-tuple+VLAN on different physical ports
				 * (multi-homing / policy routing). */
} __packed;

/* IPv6 5-tuple key (separate table; v6 is routed, never NAT'd, so no rewrite of
 * the addresses/ports — only MAC + hop limit change). */
struct octeon_ft_key6 {
	struct in6_addr	saddr;
	struct in6_addr	daddr;
	__be16		sport;
	__be16		dport;
	u8		l4proto;
	u8		pad;
	u16		vlan_id;
	u16		vlan_id2;
	u16		iif_port;	/* ingress octeon PIP port (see v4 key) */
} __packed;

/*
 * The decoded rewrite. We do NOT replay the flowtable's raw MANGLE words: those
 * are host-endian u32s encoded for little-endian HW-offload consumers (mlx5 &c.)
 * and place MAC/port bytes wrong on big-endian Octeon. Instead we decode each
 * mangle's *semantic* value at install time (explicit byte extraction, see
 * octeon_ft_decode_mangle) into these fields and apply them by hand.
 */
struct octeon_ft_rewrite {
	u8	dmac[ETH_ALEN];
	u8	smac[ETH_ALEN];
	__be32	saddr;
	__be32	daddr;
	__be16	sport;
	__be16	dport;
	u8	set_dmac:1, set_smac:1, set_saddr:1,
		set_daddr:1, set_sport:1, set_dport:1;
};

struct octeon_ft_flow {
	struct rhash_head	node;		/* in the v4 OR the v6 table */
	struct rhash_head	cookie_node;	/* keyed by cookie (control path) */
	struct octeon_ft_key	key;		/* used when !is_v6 */
	struct octeon_ft_key6	key6;		/* used when is_v6 */
	bool			is_v6;
	unsigned long		cookie;
	struct net_device	*out_dev;	/* physical octeon port for PKO;
						 * reference held (dev_hold) */
	u32			mtu;		/* egress (REDIRECT dev) MTU at
						 * install; oversize -> slow path
						 * so the stack can ICMP/frag */
	u32			csum_flags;
	struct octeon_ft_rewrite rw;
	/* Egress 802.1Q tag stack (router-on-a-stick / QinQ). The flowtable
	 * encodes the egress VLANs implicitly as the REDIRECT device being a
	 * (possibly stacked) subinterface, NOT as VLAN_PUSH actions (verified on
	 * hardware). The hook rewrites the ingress tag stack to this egress stack
	 * in the FPA buffer (retag / pop / push, any 0/1/2->0/1/2 combination).
	 */
	u8			n_eg;		/* egress tag count: 0, 1 or 2 */
	u8			eport;		/* egress octeon port (AQM counter idx) */
	u16			eg_vid[2];	/* wire order: [0]=outer, [1]=inner */
	/* per-flow accounting (fed back to nftables flowtable stats so offloaded
	 * traffic stays visible in `conntrack -L` and the flow is kept alive).
	 */
	atomic64_t		packets;
	atomic64_t		bytes;
	u64			last_packets;	/* last reported, for deltas */
	u64			last_bytes;
	unsigned long		lastused;	/* jiffies of last fast-path hit */
	unsigned long		last_polled;	/* jiffies of last FLOW_CLS_STATS */
	struct rcu_head		rcu;
};

static const struct rhashtable_params octeon_ft_rht_params = {
	.head_offset	= offsetof(struct octeon_ft_flow, node),
	.key_offset	= offsetof(struct octeon_ft_flow, key),
	.key_len	= sizeof(struct octeon_ft_key),
	.automatic_shrinking = true,
};

/* IPv6 data-path table; same `node` field (a flow lives in exactly one of the
 * two tables, chosen by is_v6). */
static const struct rhashtable_params octeon_ft_rht6_params = {
	.head_offset	= offsetof(struct octeon_ft_flow, node),
	.key_offset	= offsetof(struct octeon_ft_flow, key6),
	.key_len	= sizeof(struct octeon_ft_key6),
	.automatic_shrinking = true,
};

/* Secondary index by cookie, so FLOW_CLS_STATS/DESTROY are O(1) instead of an
 * O(N) walk of the 5-tuple table.
 */
static const struct rhashtable_params octeon_ft_cookie_params = {
	.head_offset	= offsetof(struct octeon_ft_flow, cookie_node),
	.key_offset	= offsetof(struct octeon_ft_flow, cookie),
	.key_len	= sizeof(unsigned long),
	.automatic_shrinking = true,
};

static struct rhashtable octeon_ft_table;
static struct rhashtable octeon_ft_table6;
static struct rhashtable octeon_ft_cookie_table;
static atomic_t octeon_ft_count = ATOMIC_INIT(0);
static atomic_t octeon_ft_hits = ATOMIC_INIT(0);
static atomic_t octeon_ft_tx_ok = ATOMIC_INIT(0);
static atomic_t octeon_ft_tx_fail = ATOMIC_INIT(0);

/* Read-only debug counters: installed-flow count + fast-path hit/tx stats.
 * `flows` vs conntrack's offloaded-flow count exposes leaked (zombie) entries.
 */
static int octeon_ft_atomic_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read((atomic_t *)kp->arg));
}

static const struct kernel_param_ops octeon_ft_atomic_ops = {
	.get = octeon_ft_atomic_get,
};

module_param_cb(flows, &octeon_ft_atomic_ops, &octeon_ft_count, 0444);
MODULE_PARM_DESC(flows, "installed hardware flow entries (both tables)");
module_param_cb(hits, &octeon_ft_atomic_ops, &octeon_ft_hits, 0444);
MODULE_PARM_DESC(hits, "fast-path lookup hits");
module_param_cb(tx_ok, &octeon_ft_atomic_ops, &octeon_ft_tx_ok, 0444);
MODULE_PARM_DESC(tx_ok, "fast-path successful PKO transmits");
module_param_cb(tx_fail, &octeon_ft_atomic_ops, &octeon_ft_tx_fail, 0444);
MODULE_PARM_DESC(tx_fail, "fast-path failed PKO transmits");

/* Reject-reason counters: why packets bypass the fast path (debug). */
static atomic_t octeon_ft_r_bufs = ATOMIC_INIT(0);
static atomic_t octeon_ft_r_wqe = ATOMIC_INIT(0);
static atomic_t octeon_ft_r_ipoff = ATOMIC_INIT(0);
static atomic_t octeon_ft_r_ctl = ATOMIC_INIT(0);
static atomic_t octeon_ft_r_miss = ATOMIC_INIT(0);
module_param_cb(r_bufs, &octeon_ft_atomic_ops, &octeon_ft_r_bufs, 0444);
MODULE_PARM_DESC(r_bufs, "rejects: multi-buffer WQE");
module_param_cb(r_wqe, &octeon_ft_atomic_ops, &octeon_ft_r_wqe, 0444);
MODULE_PARM_DESC(r_wqe, "rejects: rcv_error/not_IP/IP_exc/frag/!tcpudp");
module_param_cb(r_ipoff, &octeon_ft_atomic_ops, &octeon_ft_r_ipoff, 0444);
MODULE_PARM_DESC(r_ipoff, "rejects: unexpected ip_offset/tag stack");
module_param_cb(r_ctl, &octeon_ft_atomic_ops, &octeon_ft_r_ctl, 0444);
MODULE_PARM_DESC(r_ctl, "rejects: TCP syn/fin/rst punted");
module_param_cb(r_miss, &octeon_ft_atomic_ops, &octeon_ft_r_miss, 0444);
MODULE_PARM_DESC(r_miss, "rejects: flow table lookup miss");

/* ============================ data path ============================ */

static inline void *octeon_ft_pkt_data(struct cvmx_wqe *work)
{
	return cvmx_phys_to_ptr(work->packet_ptr.s.addr);
}

/* Apply the decoded rewrite to the packet headers (endian-correct, explicit). */
static inline void octeon_ft_apply_rewrite(const struct octeon_ft_rewrite *rw,
					   u8 *l2, struct iphdr *iph, u8 *l4)
{
	if (rw->set_dmac)
		ether_addr_copy(l2, rw->dmac);
	if (rw->set_smac)
		ether_addr_copy(l2 + ETH_ALEN, rw->smac);
	if (rw->set_saddr)
		iph->saddr = rw->saddr;
	if (rw->set_daddr)
		iph->daddr = rw->daddr;
	if (rw->set_sport)
		*(__be16 *)(l4 + 0) = rw->sport;
	if (rw->set_dport)
		*(__be16 *)(l4 + 2) = rw->dport;
}

/* 802.1Q tag length; Octeon cache line (units of buf_ptr.back). */
#define OCT_VLAN_HLEN	4
#define OCT_CACHE_LINE	128

/* Number of ingress 802.1Q tags PIP saw (0/1/2). vlan_stacked => 2. */
static inline int octeon_ft_n_ingress_tags(struct cvmx_wqe *work)
{
	if (work->word2.s.vlan_stacked)
		return 2;
	return work->word2.s.vlan_valid ? 1 : 0;
}

/*
 * Does the FPA buffer have @need bytes of headroom before addr (for a net tag
 * push that grows the frame)? Checked before we mutate the packet, so a no-room
 * case falls back to the slow path pristine. On this hardware the IPD
 * first-mbuff skip (184 B) puts addr ~192 B into the buffer (back>=1), so there
 * is always room; this guards the general case.
 */
static inline bool octeon_ft_has_headroom(struct cvmx_wqe *work, int need)
{
	u64 addr = work->packet_ptr.s.addr;

	return (int)(addr & (OCT_CACHE_LINE - 1)) >= need ||
	       work->packet_ptr.s.back >= 1;
}

/*
 * Rewrite the ingress 802.1Q tag stack (n_ig tags) to the egress stack
 * (n_eg tags, VIDs in eg_vid[] wire order). One mechanism for every
 * retag/pop/push/QinQ combination:
 *
 *   shift = (n_ig - n_eg) * 4    bytes the dst+src MAC moves (forward=pop net,
 *                                backward=push net); L3 and the ethertype never
 *                                move (ip_offset already points at L3).
 *
 * Then write the n_eg egress tags in the opened slot. The tag bytes are not
 * covered by the IP/L4 checksums, so nothing else changes. Finally fix the FPA
 * buffer pointer (addr/size/len) and, only if the ±shift crosses a cache line,
 * buf_ptr.back (wrong back corrupts the FPA pool). |shift| <= 8, so at most one
 * crossing. Caller guarantees headroom for a net push.
 */
static inline void octeon_ft_vlan_xform(struct cvmx_wqe *work, u8 *l2,
					int n_ig, int n_eg, const u16 *eg_vid)
{
	int shift = (n_ig - n_eg) * OCT_VLAN_HLEN;
	u8 *m = l2 + shift;
	u64 addr = work->packet_ptr.s.addr;
	int k;

	if (shift)
		memmove(m, l2, 2 * ETH_ALEN);
	for (k = 0; k < n_eg; k++) {
		u8 *t = m + 2 * ETH_ALEN + k * OCT_VLAN_HLEN;

		t[0] = (ETH_P_8021Q >> 8) & 0xff;	/* TPID 0x8100 */
		t[1] = ETH_P_8021Q & 0xff;
		t[2] = (eg_vid[k] >> 8) & 0x0f;		/* PCP/DEI 0 | VID[11:8] */
		t[3] = eg_vid[k] & 0xff;		/* VID[7:0] */
	}
	if (!shift)
		return;
	if (shift > 0) {	/* net pop: addr advances, frame shrinks */
		if ((addr & (OCT_CACHE_LINE - 1)) + shift >= OCT_CACHE_LINE)
			work->packet_ptr.s.back += 1;
	} else {		/* net push: addr backs up, frame grows */
		if ((int)(addr & (OCT_CACHE_LINE - 1)) < -shift)
			work->packet_ptr.s.back -= 1;
	}
	work->packet_ptr.s.addr = addr + shift;
	work->packet_ptr.s.size -= shift;
	work->word1.len -= shift;
}

/*
 * IPv6 fast path. v6 is routed, never NAT'd (the flowtable presents only eth
 * MAC mangles + REDIRECT — verified on hardware), so the rewrite is just dst/src
 * MAC + hop-limit decrement. v6 has NO header checksum, and since neither the
 * addresses nor ports change, the L4 checksum (which covers the v6 pseudo-header)
 * is unchanged too — nothing to recompute. Called from octeon_ft_rx with L3
 * already located; PIP guarantees no extension headers/frags here (those set
 * IP_exc / is_frag and are rejected upstream). Returns 1 if consumed.
 */
static int octeon_ft_rx6(struct cvmx_wqe *work, u8 *l2, u8 *l3, int ip_off,
			 int n_ig)
{
	struct ipv6hdr *ip6 = (struct ipv6hdr *)l3;
	struct octeon_ft_flow *flow;
	struct octeon_ft_key6 key;
	u8 *l4 = l3 + sizeof(struct ipv6hdr);	/* fixed 40-byte v6 header */
	struct net_device *od;
	u32 fb;

	/* A next-header that is not TCP/UDP means an extension header PIP did not
	 * flag — punt to the slow path rather than misread L4. */
	if (ip6->nexthdr != IPPROTO_TCP && ip6->nexthdr != IPPROTO_UDP)
		return 0;

	/* TCP control packets -> slow path (see octeon_ft_rx). */
	if (ip6->nexthdr == IPPROTO_TCP) {
		struct tcphdr *th = (struct tcphdr *)l4;

		if (th->syn || th->fin || th->rst)
			return 0;
	}

	memset(&key, 0, sizeof(key));
	key.saddr = ip6->saddr;
	key.daddr = ip6->daddr;
	key.l4proto = ip6->nexthdr;
	key.sport = *(__be16 *)(l4 + 0);
	key.dport = *(__be16 *)(l4 + 2);
	key.vlan_id = work->word2.s.vlan_valid ? work->word2.s.vlan_id : 0;
	if (n_ig == 2)
		key.vlan_id2 = (((u16)l2[ip_off - 4] << 8) |
				l2[ip_off - 3]) & 0x0fff;

	key.iif_port = cvmx_wqe_get_port(work);

	rcu_read_lock();
	flow = rhashtable_lookup(&octeon_ft_table6, &key, octeon_ft_rht6_params);
	if (!flow && n_ig && !READ_ONCE(octeon_ft_vlan_strict)) {
		/* VLAN-agnostic bridged-VLAN rules — see octeon_ft_rx. */
		key.vlan_id = 0;
		key.vlan_id2 = 0;
		flow = rhashtable_lookup(&octeon_ft_table6, &key,
					 octeon_ft_rht6_params);
	}
	if (!flow) {
		rcu_read_unlock();
		return 0;
	}
	if (ip6->hop_limit <= 1) {		/* let the slow path emit ICMPv6 */
		rcu_read_unlock();
		return 0;
	}
	/* Oversize for the egress MTU -> slow path (ICMPv6 packet-too-big). */
	if (work->word1.len - ip_off > flow->mtu) {
		rcu_read_unlock();
		return 0;
	}
	if (flow->n_eg > n_ig &&
	    !octeon_ft_has_headroom(work, (flow->n_eg - n_ig) * OCT_VLAN_HLEN)) {
		rcu_read_unlock();
		return 0;
	}

	if (flow->rw.set_dmac)
		ether_addr_copy(l2, flow->rw.dmac);
	if (flow->rw.set_smac)
		ether_addr_copy(l2 + ETH_ALEN, flow->rw.smac);
	ip6->hop_limit--;			/* no checksum to update */

	atomic_inc(&octeon_ft_hits);
	od = flow->out_dev;
	octeon_ft_vlan_xform(work, l2, n_ig, flow->n_eg, flow->eg_vid);
	fb = work->word1.len;

	/* Tail-drop AQM: bound the egress port's in-flight bytes. */
	if (octeon_ft_aqm_overlimit(flow->eport, fb)) {
		cvm_oct_free_work(work);
		rcu_read_unlock();
		return 1;
	}

	CVMX_SYNCWS;
	if (cvm_oct_transmit_qos(od, work, 1, 0)) {
		octeon_ft_aqm_unreserve(flow->eport, fb);
		atomic_inc(&octeon_ft_tx_fail);
		rcu_read_unlock();
		return 1;
	}
	atomic_inc(&octeon_ft_tx_ok);
	atomic64_inc(&flow->packets);
	atomic64_add(fb, &flow->bytes);
	if (octeon_ft_fau_stats)
		octeon_ft_fau_account(fb);
	flow->lastused = jiffies;
	rcu_read_unlock();
	return 1;
}

/* The registered RX hook. Return 1 if consumed, 0 to fall through. */
static int octeon_ft_rx(struct cvmx_wqe *work)
{
	struct octeon_ft_flow *flow;
	struct octeon_ft_key key;
	struct iphdr *iph;
	u8 *l2, *l3, *l4;
	int ihl, ip_off, n_ig;

	/* Cheap rejects from the PIP-parsed WQE (no memory touch). */
	if (work->word2.s.bufs != 1) {
		atomic_inc(&octeon_ft_r_bufs);
		return 0;
	}

	/* Diagnostic (verbose): dump PIP's parse for ANY 802.1Q-tagged frame
	 * (detected from the buffer, independent of PIP's vlan flags), to
	 * characterize how a stacked tag is reported (ip_offset, vlan_stacked,
	 * not_IP, tcp_or_udp) and where L3 actually starts in head[]. */
	if (octeon_ft_verbose) {
		u8 *p = octeon_ft_pkt_data(work);

		if (work->word1.len >= ETH_HLEN &&
		    p[12] == 0x81 && p[13] == 0x00) {
			static atomic_t dn = ATOMIC_INIT(0);

			if (atomic_inc_return(&dn) <= 16)
				pr_info(DRV_NAME ": WQE vvalid=%u vid=%u stk=%u ipoff=%u notIP=%u tu=%u len=%u head=%*ph\n",
					work->word2.s.vlan_valid,
					work->word2.s.vlan_id,
					work->word2.s.vlan_stacked,
					work->word2.s.ip_offset,
					work->word2.s.not_IP,
					work->word2.s.tcp_or_udp,
					work->word1.len, 32, p);
		}
	}

	/* L4_error: PIP flagged a malformed L4 (truncated TCP/UDP header, UDP
	 * length overrunning the IP payload, port 0, bad checksum, illegal flag
	 * combos). Reading ports/flags from such a header would build a key
	 * from garbage and could fast-forward a packet the stack would drop.
	 * With this check, PIP guarantees the L4 header is in bounds.
	 */
	if (work->word2.snoip.rcv_error || work->word2.s.not_IP ||
	    work->word2.s.IP_exc || work->word2.s.is_frag ||
	    work->word2.s.L4_error || !work->word2.s.tcp_or_udp) {
		atomic_inc(&octeon_ft_r_wqe);
		return 0;
	}

	l2 = octeon_ft_pkt_data(work);
	ip_off = work->word2.s.ip_offset;	/* PIP counts every 802.1Q tag */
	n_ig = octeon_ft_n_ingress_tags(work);
	/* L3 must sit exactly past the L2 header + n_ig tags. Anything else
	 * (e.g. 3+ stacked tags, which PIP still flags as stacked but whose
	 * ip_off is 26) is out of scope -> slow path. */
	if (ip_off != ETH_HLEN + n_ig * OCT_VLAN_HLEN) {
		atomic_inc(&octeon_ft_r_ipoff);
		return 0;
	}
	l3 = l2 + ip_off;
	iph = (struct iphdr *)l3;
	if (iph->version != 4) {
		if (iph->version == 6)
			return octeon_ft_rx6(work, l2, l3, ip_off, n_ig);
		return 0;
	}
	ihl = iph->ihl * 4;
	/* PIP's IP_exc covers a bad IHL; keep an explicit floor anyway so L4
	 * can never be computed inside the IP header (one compare, ~free). */
	if (ihl < (int)sizeof(struct iphdr)) {
		atomic_inc(&octeon_ft_r_wqe);
		return 0;
	}
	l4 = l3 + ihl;

	/* TCP control packets take the slow path: conntrack must see handshakes
	 * and closes (the sw flowtable hook tears the flow down on fin/rst), and
	 * a syn on a reused 5-tuple must never be eaten by a leftover entry —
	 * matches nf_flow_offload_ip_hook's fin/rst handling.
	 */
	if (iph->protocol == IPPROTO_TCP) {
		struct tcphdr *th = (struct tcphdr *)l4;

		if (th->syn || th->fin || th->rst) {
			atomic_inc(&octeon_ft_r_ctl);
			return 0;
		}
	}

	/* Build the lookup key: 5-tuple + the ingress VLAN stack (outermost VID,
	 * plus the inner VID for QinQ), so the same 5-tuple on different VLANs
	 * stays distinct.
	 */
	memset(&key, 0, sizeof(key));
	key.saddr = iph->saddr;
	key.daddr = iph->daddr;
	key.l4proto = iph->protocol;
	/* TCP and UDP both have src/dst port as the first two 16-bit words. */
	key.sport = *(__be16 *)(l4 + 0);
	key.dport = *(__be16 *)(l4 + 2);
	key.vlan_id = work->word2.s.vlan_valid ? work->word2.s.vlan_id : 0;
	if (n_ig == 2)
		/* inner tag TCI is the 2 bytes just before L3's ethertype,
		 * i.e. at l2 + ip_off - 4. */
		key.vlan_id2 = (((u16)l2[ip_off - 4] << 8) |
				l2[ip_off - 3]) & 0x0fff;

	key.iif_port = cvmx_wqe_get_port(work);

	rcu_read_lock();
	flow = rhashtable_lookup(&octeon_ft_table, &key, octeon_ft_rht_params);
	if (!flow && n_ig && !READ_ONCE(octeon_ft_vlan_strict)) {
		/* Bridged-VLAN (lower-device) rules are VLAN-agnostic: the
		 * kernel excludes in_vlan_ingress encaps from the match key
		 * (nf_flow_rule_match), so the entry is keyed untagged while
		 * the wire packet carries the tag. Retry with the VLAN fields
		 * cleared; the xform below rewrites the ACTUAL ingress stack
		 * to the entry's egress stack, so forwarding stays correct.
		 */
		key.vlan_id = 0;
		key.vlan_id2 = 0;
		flow = rhashtable_lookup(&octeon_ft_table, &key,
					 octeon_ft_rht_params);
	}
	if (!flow) {
		atomic_inc(&octeon_ft_r_miss);
		rcu_read_unlock();
		return 0;	/* miss -> slow path */
	}

	if (iph->ttl <= 1) {		/* let the slow path emit ICMP */
		rcu_read_unlock();
		return 0;
	}

	/* Oversize for the egress MTU -> slow path, which fragments or emits
	 * ICMP frag-needed (PMTUD). word1.len excludes the FCS; the IP packet
	 * length is len - ip_off (egress tag changes don't alter it). Matches
	 * nf_flow_offload_ip_hook's nf_flow_exceeds_mtu punt.
	 */
	if (work->word1.len - ip_off > flow->mtu) {
		rcu_read_unlock();
		return 0;
	}

	/* A net tag push grows the frame; ensure buffer headroom BEFORE mutating
	 * anything, so a no-room case falls back to the slow path pristine (no
	 * double-NAT).
	 */
	if (flow->n_eg > n_ig &&
	    !octeon_ft_has_headroom(work, (flow->n_eg - n_ig) * OCT_VLAN_HLEN)) {
		rcu_read_unlock();
		return 0;
	}

	/* Incremental checksum update (RFC 1624): capture the L3/L4 fields we
	 * are about to change, apply the rewrite + TTL decrement, then fold the
	 * old->new deltas into the IP and L4 checksums. Avoids re-summing the
	 * whole payload every packet (the dominant per-packet cost). The IP
	 * header checksum covers the addresses + TTL; the L4 checksum covers the
	 * pseudo-header addresses + the ports.
	 */
	{
		struct octeon_ft_rewrite *rw = &flow->rw;
		__be32 o_saddr = iph->saddr, o_daddr = iph->daddr;
		__be16 o_sport = *(__be16 *)(l4 + 0);
		__be16 o_dport = *(__be16 *)(l4 + 2);
		__sum16 *l4c = NULL;

		octeon_ft_apply_rewrite(rw, l2, iph, l4);
		ip_decrease_ttl(iph);	/* updates iph->check for the TTL change */

		if (rw->set_saddr)
			csum_replace4(&iph->check, o_saddr, iph->saddr);
		if (rw->set_daddr)
			csum_replace4(&iph->check, o_daddr, iph->daddr);

		if (iph->protocol == IPPROTO_TCP)
			l4c = &((struct tcphdr *)l4)->check;
		else if (iph->protocol == IPPROTO_UDP &&
			 ((struct udphdr *)l4)->check != 0)
			/* check==0 means "no UDP checksum"; leave it off. */
			l4c = &((struct udphdr *)l4)->check;

		if (l4c) {
			if (rw->set_saddr)
				csum_replace4(l4c, o_saddr, iph->saddr);
			if (rw->set_daddr)
				csum_replace4(l4c, o_daddr, iph->daddr);
			if (rw->set_sport)
				csum_replace2(l4c, o_sport, rw->sport);
			if (rw->set_dport)
				csum_replace2(l4c, o_dport, rw->dport);
			if (iph->protocol == IPPROTO_UDP && *l4c == 0)
				*l4c = CSUM_MANGLED_0;
		}
	}

	/* Make our writes visible to PKO, then transmit out the egress port.
	 * cvm_oct_transmit_qos with do_free=1 hands the packet buffer to PKO
	 * (HW recycles it to FPA) and frees the WQE descriptor.
	 */
	atomic_inc(&octeon_ft_hits);
	{
		struct net_device *od = flow->out_dev;
		u32 fb;

		/* Rewrite the ingress tag stack to the egress stack (retag / pop /
		 * push / QinQ, any combination). The tags are not covered by the
		 * IP/L4 checksums, so this never touches the checksums.
		 */
		octeon_ft_vlan_xform(work, l2, n_ig, flow->n_eg, flow->eg_vid);
		fb = work->word1.len;	/* egress length (post tag xform) */

		/* Tail-drop AQM: bound the egress port's in-flight bytes. */
		if (octeon_ft_aqm_overlimit(flow->eport, fb)) {
			cvm_oct_free_work(work);
			rcu_read_unlock();
			return 1;
		}

		CVMX_SYNCWS;
		if (cvm_oct_transmit_qos(od, work, 1, 0)) {
			octeon_ft_aqm_unreserve(flow->eport, fb);
			atomic_inc(&octeon_ft_tx_fail);
			rcu_read_unlock();
			return 1;
		}
		atomic_inc(&octeon_ft_tx_ok);
		/* account (a single flow maps to one core via its POW tag, so
		 * these are effectively uncontended; atomics keep it correct if
		 * the hash ever spreads a tuple).
		 */
		atomic64_inc(&flow->packets);
		atomic64_add(fb, &flow->bytes);
		if (octeon_ft_fau_stats)
			octeon_ft_fau_account(fb);
		flow->lastused = jiffies;
	}
	rcu_read_unlock();
	return 1;
}

/* ============================ control path ============================ */

static bool octeon_ft_dev_is_ours(struct net_device *dev)
{
	struct ethtool_drvinfo info = {};

	if (!dev || !dev->ethtool_ops || !dev->ethtool_ops->get_drvinfo)
		return false;
	dev->ethtool_ops->get_drvinfo(dev, &info);
	return strcmp(info.driver, OCTEON_ETH_DRV_NAME) == 0;
}

/*
 * Return the underlying octeon physical netdev for @dev, or NULL if @dev is
 * not octeon-backed. @dev may be the physical port itself or an 802.1Q
 * subinterface on top of it (router-on-a-stick). PKO transmit needs the
 * physical port; a VLAN subinterface is a software construct the staging
 * driver knows nothing about.
 */
static struct net_device *octeon_ft_phys_dev(struct net_device *dev)
{
	if (!dev)
		return NULL;
	if (is_vlan_dev(dev))
		dev = vlan_dev_real_dev(dev);	/* flattens a stack to the phys dev */
	return octeon_ft_dev_is_ours(dev) ? dev : NULL;
}

/*
 * Walk an 802.1Q subinterface stack (single or stacked QinQ, capped at 2) and
 * fill vids[] in wire order (vids[0] = outermost). The immediate parent is
 * reached via dev_get_iflink (vlan_dev_real_dev would flatten straight to the
 * physical port and skip the outer tag). Returns the tag count (0/1/2). A 3rd
 * tag is ignored here; the hook's ip_offset consistency check rejects 3-tag
 * ingress packets, and 3-tag egress is out of scope.
 */
static int octeon_ft_vlan_stack(struct net_device *dev, u16 vids[2])
{
	struct net_device *par;
	u16 inner, outer = 0;
	int n = 1;

	if (!is_vlan_dev(dev))
		return 0;
	inner = vlan_dev_vlan_id(dev);
	par = dev_get_by_index(dev_net(dev), dev_get_iflink(dev));
	if (par) {
		if (is_vlan_dev(par)) {
			outer = vlan_dev_vlan_id(par);
			n = 2;
		}
		dev_put(par);
	}
	if (n == 2) {
		vids[0] = outer;	/* outermost on the wire */
		vids[1] = inner;
	} else {
		vids[0] = inner;
	}
	return n;
}

/* Diagnostic: dump everything the flowtable presents for a flow — match
 * dissectors (incl. VLAN/META) and the full action list with the VLAN encoding
 * decoded. Gated by the verbose param. This is how the VLAN encoding was
 * characterized (egress VLAN is implicit in the REDIRECT device, not a
 * VLAN_PUSH action); kept for debugging future flow classes.
 */
static void octeon_ft_dump_rule(struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_dissector *d = rule->match.dissector;
	struct flow_action_entry *act;
	int i;

	pr_info(DRV_NAME ": DUMP cookie=%lx dissector used_keys=0x%llx\n",
		f->cookie, (unsigned long long)d->used_keys);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct flow_match_meta m;

		flow_rule_match_meta(rule, &m);
		pr_info(DRV_NAME ":   META ingress_ifindex key=%u mask=%u\n",
			m.key->ingress_ifindex, m.mask->ingress_ifindex);
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan m;

		flow_rule_match_vlan(rule, &m);
		pr_info(DRV_NAME ":   VLAN id key=%u mask=%u tpid=0x%04x prio=%u\n",
			m.key->vlan_id, m.mask->vlan_id,
			ntohs(m.key->vlan_tpid), m.key->vlan_priority);
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan m;

		flow_rule_match_cvlan(rule, &m);
		pr_info(DRV_NAME ":   CVLAN id key=%u mask=%u tpid=0x%04x\n",
			m.key->vlan_id, m.mask->vlan_id, ntohs(m.key->vlan_tpid));
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			pr_info(DRV_NAME ":   ACT[%d] MANGLE htype=%u off=%u mask=0x%08x val=0x%08x\n",
				i, act->mangle.htype, act->mangle.offset,
				act->mangle.mask, act->mangle.val);
			break;
		case FLOW_ACTION_VLAN_PUSH:
			pr_info(DRV_NAME ":   ACT[%d] VLAN_PUSH vid=%u proto=0x%04x prio=%u\n",
				i, act->vlan.vid, ntohs(act->vlan.proto),
				act->vlan.prio);
			break;
		case FLOW_ACTION_VLAN_POP:
			pr_info(DRV_NAME ":   ACT[%d] VLAN_POP\n", i);
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			pr_info(DRV_NAME ":   ACT[%d] VLAN_MANGLE vid=%u proto=0x%04x\n",
				i, act->vlan.vid, ntohs(act->vlan.proto));
			break;
		case FLOW_ACTION_REDIRECT: {
			struct net_device *od = act->dev;
			struct net_device *phys = octeon_ft_phys_dev(od);

			pr_info(DRV_NAME ":   ACT[%d] REDIRECT dev=%s vlan=%d phys=%s vid=%d\n",
				i, od ? od->name : "(null)",
				od ? is_vlan_dev(od) : -1,
				phys ? phys->name : "(none)",
				(od && is_vlan_dev(od)) ? vlan_dev_vlan_id(od) : -1);
			break;
		}
		case FLOW_ACTION_CSUM:
			pr_info(DRV_NAME ":   ACT[%d] CSUM flags=0x%x\n",
				i, act->csum_flags);
			break;
		default:
			pr_info(DRV_NAME ":   ACT[%d] id=%u\n", i, act->id);
			break;
		}
	}
}

/* MSB-first (network order) byte k of a host u32. Correct on big-endian
 * Octeon, which is what this driver targets.
 */
static inline u8 be_byte(u32 v, int k)
{
	return (v >> (8 * (3 - k))) & 0xff;
}

/*
 * Decode one flowtable MANGLE into the explicit rewrite. The flowtable encodes
 * these as host-endian u32 (offset/mask/val) per net/netfilter/
 * nf_flow_table_offload.c {flow_offload_eth_dst,eth_src,ipv4_snat/dnat,
 * port_snat/dnat}. We extract the semantic value rather than replaying the raw
 * word (which would misplace bytes on big-endian). Returns 0 on success,
 * -EOPNOTSUPP for a mangle we don't recognize.
 */
static int octeon_ft_decode_mangle(struct octeon_ft_rewrite *rw,
				   const struct flow_action_entry *e)
{
	u32 val = e->mangle.val, mask = e->mangle.mask;

	switch (e->mangle.htype) {
	case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
		switch (e->mangle.offset) {
		case 0:		/* dst MAC [0:4] (eth_dst entry0, mask 0) */
			rw->dmac[0] = be_byte(val, 0);
			rw->dmac[1] = be_byte(val, 1);
			rw->dmac[2] = be_byte(val, 2);
			rw->dmac[3] = be_byte(val, 3);
			rw->set_dmac = 1;
			break;
		case 4:
			if (mask == 0xffff0000) {	/* dst MAC [4:6] */
				rw->dmac[4] = be_byte(val, 2);
				rw->dmac[5] = be_byte(val, 3);
				rw->set_dmac = 1;
			} else {			/* src MAC [0:2] */
				rw->smac[0] = be_byte(val, 0);
				rw->smac[1] = be_byte(val, 1);
				rw->set_smac = 1;
			}
			break;
		case 8:		/* src MAC [2:6] (eth_src entry1, mask 0) */
			rw->smac[2] = be_byte(val, 0);
			rw->smac[3] = be_byte(val, 1);
			rw->smac[4] = be_byte(val, 2);
			rw->smac[5] = be_byte(val, 3);
			rw->set_smac = 1;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
		if (e->mangle.offset == offsetof(struct iphdr, saddr)) {
			rw->saddr = (__force __be32)val;
			rw->set_saddr = 1;
		} else if (e->mangle.offset == offsetof(struct iphdr, daddr)) {
			rw->daddr = (__force __be32)val;
			rw->set_daddr = 1;
		} else {
			return -EOPNOTSUPP;
		}
		break;
	case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
	case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
		/* L4 word at offset 0 = [src_port:16 | dst_port:16] in network
		 * (big-endian) order. mask = bits to KEEP, so:
		 *   mask 0x0000ffff -> keep dst, SET src port (high 16 of val)
		 *   mask 0xffff0000 -> keep src, SET dst port (low 16 of val)
		 * The new port value already sits at its big-endian byte
		 * position within val (verified on hardware), so take the
		 * matching half directly. (Octeon is big-endian only.)
		 */
		if (mask == 0x0000ffff) {		/* sets source port */
			rw->sport = (__force __be16)(val >> 16);
			rw->set_sport = 1;
		} else if (mask == 0xffff0000) {	/* sets dest port */
			rw->dport = (__force __be16)(val & 0xffff);
			rw->set_dport = 1;
		} else {
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static struct octeon_ft_flow *octeon_ft_parse(struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_match_ports ports;
	struct flow_match_basic basic;
	struct octeon_ft_flow *flow;
	struct flow_action_entry *act;
	struct net_device *redir;
	u16 *kvid, *kvid2, *kiif;	/* point at the active key's fields */
	bool is_v6, got_iif = false;
	u16 vids[2];
	u16 push_vid[2];
	int i, n, n_push = 0;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return ERR_PTR(-EOPNOTSUPP);

	flow_rule_match_basic(rule, &basic);
	if (basic.key->ip_proto != IPPROTO_TCP &&
	    basic.key->ip_proto != IPPROTO_UDP)
		return ERR_PTR(-EOPNOTSUPP);
	if (basic.key->n_proto == htons(ETH_P_IP))
		is_v6 = false;
	else if (basic.key->n_proto == htons(ETH_P_IPV6))
		is_v6 = true;
	else
		return ERR_PTR(-EOPNOTSUPP);
	if (is_v6 ? !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS)
		  : !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS))
		return ERR_PTR(-EOPNOTSUPP);

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return ERR_PTR(-ENOMEM);

	flow->is_v6 = is_v6;
	flow->cookie = f->cookie;
	flow_rule_match_ports(rule, &ports);
	if (is_v6) {
		struct flow_match_ipv6_addrs a6;

		flow_rule_match_ipv6_addrs(rule, &a6);
		flow->key6.saddr = a6.key->src;
		flow->key6.daddr = a6.key->dst;
		flow->key6.sport = ports.key->src;
		flow->key6.dport = ports.key->dst;
		flow->key6.l4proto = basic.key->ip_proto;
		kvid = &flow->key6.vlan_id;
		kvid2 = &flow->key6.vlan_id2;
		kiif = &flow->key6.iif_port;
	} else {
		struct flow_match_ipv4_addrs a4;

		flow_rule_match_ipv4_addrs(rule, &a4);
		flow->key.saddr = a4.key->src;
		flow->key.daddr = a4.key->dst;
		flow->key.sport = ports.key->src;
		flow->key.dport = ports.key->dst;
		flow->key.l4proto = basic.key->ip_proto;
		kvid = &flow->key.vlan_id;
		kvid2 = &flow->key.vlan_id2;
		kiif = &flow->key.iif_port;
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			/* v6 is routed only — reject any non-MAC mangle (we do
			 * not do NAT66/PAT; such flows fall to the slow path). */
			if (is_v6 &&
			    act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				goto unsupported;
			if (octeon_ft_decode_mangle(&flow->rw, act))
				goto unsupported;
			break;
		case FLOW_ACTION_REDIRECT:
			flow->out_dev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			flow->csum_flags = act->csum_flags;
			break;
		case FLOW_ACTION_VLAN_PUSH:
			/* Bridged-VLAN encoding (production gw): the flowtable
			 * spans the LOWER devices and dev_fill_forward_path
			 * emits the egress tag as an explicit push action (the
			 * REDIRECT dev is then the physical port, so the
			 * device-implicit stack below resolves to 0 tags).
			 */
			if (act->vlan.proto != htons(ETH_P_8021Q))
				goto unsupported;
			if (n_push >= 2)
				goto unsupported;
			push_vid[n_push++] = act->vlan.vid;
			break;
		case FLOW_ACTION_VLAN_POP:
			/* Ingress tag consumption; the ingress stack is keyed
			 * via FLOW_DISSECTOR_KEY_VLAN below, and the hook's
			 * xform rewrites the whole stack — nothing to record.
			 */
			break;
		default:
			/* NEVER install a rule we cannot execute faithfully:
			 * an ignored action means the hook would forward
			 * misbuilt packets and silently black-hole the flow
			 * (exactly the gw VLAN_PUSH production bug). Punt to
			 * the software flowtable instead.
			 */
			goto unsupported;
		}
	}

	if (!flow->out_dev)
		goto unsupported;

	/* Resolve the egress VLAN stack: the flowtable encodes it implicitly as
	 * the REDIRECT device being a (possibly stacked) 802.1Q subinterface, not
	 * as VLAN_PUSH actions. PKO needs the physical port, so unwrap to it and
	 * record the egress tag stack to apply in the hook.
	 */
	redir = flow->out_dev;
	flow->out_dev = octeon_ft_phys_dev(redir);	/* flattens to phys dev */
	if (!flow->out_dev)
		goto unsupported;
	/* Egress L3 MTU is the REDIRECT (possibly VLAN sub-) device's, not the
	 * physical port's. Cached at install like the sw flowtable's route MTU;
	 * an MTU change is picked up when the flow is re-offloaded. */
	flow->mtu = READ_ONCE(redir->mtu);
	flow->eport = cvm_oct_get_port(flow->out_dev);	/* AQM counter index */
	flow->n_eg = octeon_ft_vlan_stack(redir, vids);
	if (flow->n_eg >= 1)		/* eg_vid[] is zero (kzalloc) otherwise */
		flow->eg_vid[0] = vids[0];
	if (flow->n_eg == 2)
		flow->eg_vid[1] = vids[1];

	/* Explicit VLAN_PUSH actions (bridged-VLAN/lower-device encoding) and a
	 * subinterface REDIRECT are alternative encodings of the egress stack —
	 * both at once would need stacking logic no real config produces; refuse
	 * rather than guess. A single push: that IS the egress stack.
	 */
	if (n_push) {
		if (flow->n_eg)
			goto unsupported;
		if (n_push == 2)	/* QinQ-via-push: untested, punt */
			goto unsupported;
		flow->n_eg = 1;
		flow->eg_vid[0] = push_vid[0];
	}

	/* Ingress VLAN match keys (bridged-VLAN/lower-device encoding): the
	 * ingress device is the physical port and the tag(s) are explicit match
	 * keys instead of being consumed by a subinterface.
	 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan mv;

		flow_rule_match_vlan(rule, &mv);
		if (mv.mask->vlan_tpid &&
		    mv.key->vlan_tpid != htons(ETH_P_8021Q))
			goto unsupported;
		if (mv.mask->vlan_id)
			*kvid = mv.key->vlan_id;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan mv;

		flow_rule_match_cvlan(rule, &mv);
		if (mv.mask->vlan_tpid &&
		    mv.key->vlan_tpid != htons(ETH_P_8021Q))
			goto unsupported;
		if (mv.mask->vlan_id)
			*kvid2 = mv.key->vlan_id;
	}

	/* Resolve the ingress VLAN stack from the META ingress_ifindex (the
	 * subiface strips the tag(s), so they are not in the match): map ifindex
	 * -> netdev -> VID stack. Keys the flow so a (double-)tagged packet
	 * matches what the hook reads from the WQE + buffer.
	 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct flow_match_meta meta;

		flow_rule_match_meta(rule, &meta);
		if (meta.key->ingress_ifindex) {
			struct net_device *in;

			in = dev_get_by_index(dev_net(flow->out_dev),
					      meta.key->ingress_ifindex);
			if (in) {
				struct net_device *iphys = octeon_ft_phys_dev(in);

				/* key on the physical ingress port; the hook reads
				 * the same number from the WQE (cvmx_wqe_get_port). */
				if (iphys) {
					*kiif = (u16)cvm_oct_get_port(iphys);
					got_iif = true;
				}
				n = octeon_ft_vlan_stack(in, vids);
				if (n >= 1)
					*kvid = vids[0];
				if (n == 2)
					*kvid2 = vids[1];
				dev_put(in);
			}
		}
	}

	/* Without a resolved ingress port the key can't match the hook's key —
	 * don't install a dead flow; let it take the slow path. */
	if (!got_iif)
		goto unsupported;

	/* The RX hook dereferences out_dev from softirq with only RCU pinning
	 * the flow entry — pin the netdev itself for the flow's lifetime. The
	 * ref is dropped in octeon_ft_flow_free (RCU-deferred on the data-path
	 * disposal paths). */
	dev_hold(flow->out_dev);
	return flow;

unsupported:
	kfree(flow);
	return ERR_PTR(-EOPNOTSUPP);
}

/* Free a flow + drop its out_dev reference. Direct call only for entries that
 * were NEVER published in a table; anything the RX hook may have seen must go
 * through the RCU-deferred variant.
 */
static void octeon_ft_flow_free(struct octeon_ft_flow *flow)
{
	dev_put(flow->out_dev);
	kfree(flow);
}

static void octeon_ft_flow_free_rcu(struct rcu_head *head)
{
	octeon_ft_flow_free(container_of(head, struct octeon_ft_flow, rcu));
}

/* Remove a flow from all tables and free it (RCU-deferred: the RX hook may
 * still hold it under rcu_read_lock). The cookie-table removal doubles as the
 * ownership token: exactly one caller wins a DESTROY-vs-evict/flush race, the
 * loser sees -ENOENT and must not touch the flow again.
 */
static void octeon_ft_del(struct octeon_ft_flow *flow)
{
	if (rhashtable_remove_fast(&octeon_ft_cookie_table, &flow->cookie_node,
				   octeon_ft_cookie_params))
		return;		/* lost the race; another path is freeing it */
	if (flow->is_v6)
		rhashtable_remove_fast(&octeon_ft_table6, &flow->node,
				       octeon_ft_rht6_params);
	else
		rhashtable_remove_fast(&octeon_ft_table, &flow->node,
				       octeon_ft_rht_params);
	atomic_dec(&octeon_ft_count);
	call_rcu(&flow->rcu, octeon_ft_flow_free_rcu);
}

static int octeon_ft_replace(struct flow_cls_offload *f)
{
	struct octeon_ft_flow *flow, *old;
	int err;

	if (octeon_ft_verbose)
		octeon_ft_dump_rule(f);

	flow = octeon_ft_parse(f);
	if (IS_ERR(flow))
		return PTR_ERR(flow);

	flow->lastused = jiffies;
	flow->last_polled = jiffies;

	/* Key-unique insert. rhashtable_insert_fast() does NOT reject duplicate
	 * keys, and the same rule legitimately arrives more than once (one
	 * delivery per claimed device block_cb, plus hw-refresh re-adds), so a
	 * plain insert breeds duplicates: they leak, they starve FLOW_CLS_STATS
	 * (the poll can hit a copy the RX hook is not updating, so the gc tears
	 * the flow down every second and it is immediately re-offloaded), and a
	 * copy that outlives its flow keeps matching a reused 5-tuple with stale
	 * NAT/MAC data — silently black-holing every connection on that tuple
	 * (the production gw "TCP dies entering the fast path" bug).
	 * Same cookie  -> duplicate delivery of the same rule: drop ours, ok.
	 * Other cookie -> stale entry of a dead flow: evict it, install fresh.
	 */
	/* The whole lookup/evict/insert dance runs under rcu_read_lock: REPLACE,
	 * DESTROY and STATS arrive on separate workqueues, so an entry returned
	 * by a lookup here can be octeon_ft_del'ed (and, without an RCU read
	 * section, FREED) by a concurrent DESTROY before we dereference it.
	 * Holding the read lock pins the memory; octeon_ft_del's cookie-removal
	 * token makes the double-evict race itself harmless.
	 */
	rcu_read_lock();
retry:
	if (flow->is_v6)
		old = rhashtable_lookup_get_insert_fast(&octeon_ft_table6,
				&flow->node, octeon_ft_rht6_params);
	else
		old = rhashtable_lookup_get_insert_fast(&octeon_ft_table,
				&flow->node, octeon_ft_rht_params);
	if (IS_ERR(old)) {
		rcu_read_unlock();
		octeon_ft_flow_free(flow);	/* never published */
		return PTR_ERR(old);
	}
	if (old) {
		if (old->cookie == flow->cookie) {
			rcu_read_unlock();
			octeon_ft_flow_free(flow);	/* never published */
			return 0;	/* already installed */
		}
		pr_info_ratelimited(DRV_NAME ": evicting stale flow cookie=%lx for reinstall cookie=%lx\n",
				    old->cookie, f->cookie);
		octeon_ft_del(old);
		goto retry;
	}
	err = rhashtable_lookup_insert_fast(&octeon_ft_cookie_table,
					    &flow->cookie_node,
					    octeon_ft_cookie_params);
	if (err == -EEXIST) {
		/* A freed flow's cookie (a kernel address) can be recycled for a
		 * new flow with a different tuple while the old entry lingers;
		 * evict the holder and retake the cookie. */
		old = rhashtable_lookup_fast(&octeon_ft_cookie_table,
					     &flow->cookie,
					     octeon_ft_cookie_params);
		if (old) {
			pr_info_ratelimited(DRV_NAME ": evicting cookie-collision flow cookie=%lx\n",
					    old->cookie);
			octeon_ft_del(old);
		}
		err = rhashtable_lookup_insert_fast(&octeon_ft_cookie_table,
						    &flow->cookie_node,
						    octeon_ft_cookie_params);
	}
	rcu_read_unlock();
	if (err) {
		/* The flow WAS visible in the 5-tuple table between the insert
		 * above and this removal — an RX-hook reader on the other core
		 * may still hold it under rcu_read_lock. RCU-defer the free
		 * (a plain kfree here is a use-after-free).
		 */
		if (flow->is_v6)
			rhashtable_remove_fast(&octeon_ft_table6, &flow->node,
					       octeon_ft_rht6_params);
		else
			rhashtable_remove_fast(&octeon_ft_table, &flow->node,
					       octeon_ft_rht_params);
		call_rcu(&flow->rcu, octeon_ft_flow_free_rcu);
		return err;
	}
	atomic_inc(&octeon_ft_count);
	if (octeon_ft_verbose) {
		if (flow->is_v6)
			pr_info(DRV_NAME ": +flow6 cookie=%lx %pI6c.%u->%pI6c.%u proto=%u iif=%u ivid=%u/%u out=%s n_eg=%u evid=%u/%u dmac=%pM\n",
				f->cookie, &flow->key6.saddr,
				ntohs(flow->key6.sport), &flow->key6.daddr,
				ntohs(flow->key6.dport), flow->key6.l4proto,
				flow->key6.iif_port, flow->key6.vlan_id,
				flow->key6.vlan_id2, flow->out_dev->name,
				flow->n_eg, flow->eg_vid[0], flow->eg_vid[1],
				flow->rw.dmac);
		else
			pr_info(DRV_NAME ": +flow cookie=%lx %pI4:%u->%pI4:%u proto=%u iif=%u ivid=%u/%u out=%s n_eg=%u evid=%u/%u snat=%pI4 dmac=%pM\n",
				f->cookie, &flow->key.saddr,
				ntohs(flow->key.sport), &flow->key.daddr,
				ntohs(flow->key.dport), flow->key.l4proto,
				flow->key.iif_port, flow->key.vlan_id,
				flow->key.vlan_id2, flow->out_dev->name,
				flow->n_eg, flow->eg_vid[0], flow->eg_vid[1],
				&flow->rw.saddr, flow->rw.dmac);
	}
	return 0;
}

/* find by cookie (DESTROY/STATS carry only the cookie) via the secondary index. */
static struct octeon_ft_flow *octeon_ft_find_cookie(unsigned long cookie)
{
	return rhashtable_lookup_fast(&octeon_ft_cookie_table, &cookie,
				      octeon_ft_cookie_params);
}

static int octeon_ft_stats(struct flow_cls_offload *f)
{
	struct octeon_ft_flow *flow;
	u64 p, b, dp, db;
	unsigned long lastused;

	/* Pin the entry against a concurrent DESTROY (separate workqueue). */
	rcu_read_lock();
	flow = octeon_ft_find_cookie(f->cookie);
	if (!flow) {
		rcu_read_unlock();
		return -ENOENT;
	}

	/* Report the delta since the last poll; flow_stats_update accumulates
	 * and uses lastused to refresh the flow's timeout (keeping an active
	 * offloaded flow alive and visible in conntrack -L).
	 */
	p = atomic64_read(&flow->packets);
	b = atomic64_read(&flow->bytes);
	dp = p - flow->last_packets;
	db = b - flow->last_bytes;
	flow->last_packets = p;
	flow->last_bytes = b;
	flow->last_polled = jiffies;	/* liveness signal for the orphan GC */
	lastused = flow->lastused;
	rcu_read_unlock();

	flow_stats_update(&f->stats, db, dp, 0, lastused,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);
	return 0;
}

static int octeon_ft_destroy(struct flow_cls_offload *f)
{
	struct octeon_ft_flow *flow;

	rcu_read_lock();
	flow = octeon_ft_find_cookie(f->cookie);
	if (!flow) {
		rcu_read_unlock();
		return -ENOENT;
	}
	octeon_ft_del(flow);
	rcu_read_unlock();
	return 0;
}

/* Drop every installed flow (all three tables). Used when the last flow block
 * unbinds; octeon_ft_del() resolves races against concurrent DESTROYs.
 */
static void octeon_ft_flush_all(void)
{
	struct rhashtable_iter iter;
	struct octeon_ft_flow *flow;
	int n = 0;

	rhashtable_walk_enter(&octeon_ft_cookie_table, &iter);
	rhashtable_walk_start(&iter);
	while ((flow = rhashtable_walk_next(&iter))) {
		if (IS_ERR(flow))
			continue;	/* -EAGAIN: resize in progress, go on */
		octeon_ft_del(flow);
		n++;
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	if (n)
		pr_info(DRV_NAME ": flushed %d flows on last unbind\n", n);
}

/* Orphan GC. When nftables replaces a flowtable (every fw4 reload), the new
 * table is bound before the old one is destroyed, and the old table's per-flow
 * FLOW_CLS_DESTROY work is dispatched only after UNBIND removed our block_cb —
 * so those DESTROYs reach nobody and the entries would leak forever. A live
 * hardware-offloaded flow is STATS-polled by the nf flowtable gc every couple
 * of seconds (and/or hit by traffic); an entry that has seen neither for
 * OCT_FT_ORPHAN_AGE lost its owner — evict it.
 */
#define OCT_FT_GC_PERIOD	(30 * HZ)
#define OCT_FT_ORPHAN_AGE	(90 * HZ)

static void octeon_ft_gc_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(octeon_ft_gc_work, octeon_ft_gc_work_fn);

static void octeon_ft_gc_work_fn(struct work_struct *work)
{
	struct rhashtable_iter iter;
	struct octeon_ft_flow *flow;
	int n = 0;

	rhashtable_walk_enter(&octeon_ft_cookie_table, &iter);
	rhashtable_walk_start(&iter);
	while ((flow = rhashtable_walk_next(&iter))) {
		unsigned long seen;

		if (IS_ERR(flow))
			continue;	/* -EAGAIN: resize in progress */
		seen = time_after(flow->last_polled, flow->lastused) ?
			flow->last_polled : flow->lastused;
		if (time_after(jiffies, seen + OCT_FT_ORPHAN_AGE)) {
			octeon_ft_del(flow);
			n++;
		}
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	if (n)
		pr_info_ratelimited(DRV_NAME ": gc evicted %d orphan flows\n", n);
	schedule_delayed_work(&octeon_ft_gc_work, OCT_FT_GC_PERIOD);
}

static int octeon_ft_setup_cb(enum tc_setup_type type, void *type_data,
			      void *cb_priv)
{
	struct flow_cls_offload *f = type_data;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return octeon_ft_replace(f);
	case FLOW_CLS_DESTROY:
		return octeon_ft_destroy(f);
	case FLOW_CLS_STATS:
		return octeon_ft_stats(f);
	default:
		return -EOPNOTSUPP;
	}
}

static void octeon_ft_block_release(void *cb_priv)
{
}

static LIST_HEAD(octeon_ft_block_cb_list);

static int octeon_ft_indr_setup(struct net_device *dev, struct Qdisc *sch,
				void *cb_priv, enum tc_setup_type type,
				void *type_data, void *data,
				void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct flow_block_offload *bo = type_data;
	struct flow_block_cb *block_cb;

	/* Accept blocks for our physical ports and for 802.1Q subinterfaces on
	 * top of them (router-on-a-stick) — the latter is how inter-VLAN flows
	 * are delivered.
	 */
	if (type != TC_SETUP_FT || !octeon_ft_phys_dev(dev))
		return -EOPNOTSUPP;

	switch (bo->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_indr_block_cb_alloc(octeon_ft_setup_cb, dev,
						    dev, octeon_ft_block_release,
						    bo, dev, sch, data, cb_priv,
						    cleanup);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_add(block_cb, bo);
		list_add_tail(&block_cb->driver_list, &octeon_ft_block_cb_list);
		netdev_info(dev, "octeon_flowtable: claimed flow block\n");
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(bo->block, octeon_ft_setup_cb,
						dev);
		if (!block_cb)
			return -ENOENT;
		/* Must use the *indirect* removal: it also does
		 * list_del(&block_cb->indr.list), taking the block off the
		 * global flow_block_indr_list that flow_indr_block_cb_alloc()
		 * added it to. The plain flow_block_cb_remove() leaves a
		 * dangling entry there, so a later flow_indr_dev_unregister()
		 * (module unload) walks freed memory and crashes in
		 * nf_flow_table_gc_cleanup. Matches ice/mlx5/sfc.
		 */
		flow_indr_block_cb_remove(block_cb, bo);
		list_del(&block_cb->driver_list);
		/* Last bound device gone = no hardware flowtable left, so no
		 * entry may stay installed. Belt-and-braces against any DESTROY
		 * lost in the unbind/teardown ordering: a leftover entry would
		 * keep matching (and stale-rewriting) its 5-tuple forever.
		 */
		if (list_empty(&octeon_ft_block_cb_list))
			octeon_ft_flush_all();
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int __init octeon_ft_init(void)
{
	int ret;

	/* zero the global FAU counters (HW registers persist across reloads) */
	cvmx_fau_atomic_write64(OCT_FT_FAU_BYTES, 0);
	cvmx_fau_atomic_write64(OCT_FT_FAU_PKTS, 0);

	ret = rhashtable_init(&octeon_ft_table, &octeon_ft_rht_params);
	if (ret)
		return ret;
	ret = rhashtable_init(&octeon_ft_table6, &octeon_ft_rht6_params);
	if (ret) {
		rhashtable_destroy(&octeon_ft_table);
		return ret;
	}
	ret = rhashtable_init(&octeon_ft_cookie_table, &octeon_ft_cookie_params);
	if (ret) {
		rhashtable_destroy(&octeon_ft_table6);
		rhashtable_destroy(&octeon_ft_table);
		return ret;
	}

	ret = flow_indr_dev_register(octeon_ft_indr_setup, NULL);
	if (ret) {
		rhashtable_destroy(&octeon_ft_cookie_table);
		rhashtable_destroy(&octeon_ft_table6);
		rhashtable_destroy(&octeon_ft_table);
		return ret;
	}

	cvm_oct_register_rx_hook(octeon_ft_rx);
	schedule_delayed_work(&octeon_ft_gc_work, OCT_FT_GC_PERIOD);
	pr_info(DRV_NAME ": loaded\n");
	return 0;
}

static void octeon_ft_free_flow(void *ptr, void *arg)
{
	octeon_ft_flow_free(ptr);	/* drops the out_dev ref too */
}

static void __exit octeon_ft_exit(void)
{
	WRITE_ONCE(cvm_oct_flow_aqm_fau, 0);	/* stop PKO's FAU subtract */
	cancel_delayed_work_sync(&octeon_ft_gc_work);
	cvm_oct_unregister_rx_hook();	/* synchronize_net() inside */
	flow_indr_dev_unregister(octeon_ft_indr_setup, NULL,
				 octeon_ft_block_release);
	/* octeon_ft_del frees via call_rcu into MODULE text
	 * (octeon_ft_flow_free_rcu); all callbacks must run before the module
	 * is unmapped. */
	rcu_barrier();
	rhashtable_destroy(&octeon_ft_cookie_table);
	rhashtable_free_and_destroy(&octeon_ft_table6, octeon_ft_free_flow, NULL);
	rhashtable_free_and_destroy(&octeon_ft_table, octeon_ft_free_flow, NULL);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(octeon_ft_init);
module_exit(octeon_ft_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nftables flow offload for Cavium Octeon CN50XX");
MODULE_AUTHOR("cavium-offload-port");
