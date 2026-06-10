# RFC: `octeon_flowtable` — nftables flow offload for the mainline Octeon staging driver

*Phase 3 design doc, 2026-06-04. Audience: netdev reviewers + future maintainers
with no prior context. Status: DRAFT for internal review before netdev@ submission.
Backing data: `docs/octeon-fastpath-model.md` (HW model), `docs/blocks/*.md`
(register catalogs, all cited from in-tree GPL headers), `docs/offload-behavior-spec.md`
(black-box spec of the EOL vendor offload we benchmark against), and
`docs/PROJECT-OUTCOME.md` (measurements + targets).*

---

## 1. Motivation

The Cavium Octeon+ CN50XX (e.g. CN5020, the SoC in the Ubiquiti EdgeRouter Lite
ERLite-3) is a dual-core 500 MHz MIPS64 with a hardware packet-processing
complex (PIP/IPD parse+input, POW work scheduler, PKO output, FPA buffer pool,
FAU atomic counters). Mainline runs this board via the staging
`octeon_ethernet` driver, which uses that complex only for plain RX/TX.

Measured on an ERLite-3 (OpenWrt, kernel 6.18; full method in
`docs/PROJECT-OUTCOME.md`):

| config | TCP single-flow | UDP-64 NAT (pps) | RTT under load |
|---|---|---|---|
| plain forwarding | 137 Mbps | 13k | 58 ms |
| nft software flowtable + RPS + group-spread (best no-driver) | ~764 Mbps | 73k | 6.1 ms |
| **vendor offload (EOL EdgeOS, for reference)** | **940 Mbps** | **247k (1 core) / 431k (2 core)** | **2.6 ms** |

The free levers (software flowtable, hardware RX group-spreading, CVMSEG async
IOBDMA) already bring bulk TCP near line rate. The remaining gap is **small-packet
forwarding (≈3–4×), forwarding latency (≈2.3×), and single-flow determinism**.
This RFC proposes a hardware-assisted nftables flow-offload backend to close it,
built as an extension of the staging driver — not a rewrite.

This is a hobbyist/research effort on EOL silicon; the value is a clean,
upstreamable example of `flow_block_offload` on an unusual NPU, not a product.

## 2. Hardware reality: no exact-match classifier

The single most important constraint (`docs/blocks/pip-ipd.md` §4):

> **CN50XX PIP has no TCAM and no exact-match flow classifier.** Hardware
> steering is limited to per-port config, a tuple-CRC *hash* tag, and 4 global
> single-16-bit-field "watchers".

Therefore a per-flow offload on this chip **cannot** be "install a 5-tuple rule
in hardware and let the NPU forward autonomously." It must be a **software
fast-path running on a core**, where the hardware's contribution is everything
that makes that path cheap:

- **PIP** delivers packets pre-parsed (L2/L3/L4 offsets, IP/TCP-UDP checksum
  already verified) with a **flow-affine 32-bit tag** (CRC over the 5-tuple) in
  the work-queue entry (WQE).
- **POW** schedules the WQE to a core, uses the tag for **per-flow ORDERED
  delivery** (in-order, no reordering within a flow) and **flow→core affinity**
  (group spreading), with no CPU in the scheduling loop. (The staging driver
  defaults to `CVMX_POW_TAG_TYPE_ORDERED`, `cvmx-config.h` /
  `cvmx-helper.c:400-404` — see §6 for why ORDERED is sufficient and we need no
  per-flow lock.)
- **FPA** recycles packet buffers in hardware (IPD allocates, PKO frees) —
  **zero-copy, no per-packet kmalloc/free**.
- **PKO** transmits from a 2-word command, **inserts the L4 checksum** after our
  NAT rewrite, and frees the buffer back to FPA on completion.
- **FAU** gives lock-free per-flow counters that PKO can update from hardware.

Independent confirmation that this is the *only* viable shape: the EOL vendor
offload, black-box-characterized in `docs/offload-behavior-spec.md`, is exactly
this — a NAT-aware **software flow cache** (`/proc/cavium/ipv4/cache`, two
unidirectional entries per connection) whose data plane completely bypasses the
Linux stack (tcpdump-blind, netdev counters frozen). We are not inventing an
architecture; we are building the clean-room mainline equivalent.

## 3. Architecture overview

```
              ┌─────────────── control plane (process / workqueue ctx) ───────────────┐
nft flowtable │  flow_block_offload(FLOW_BLOCK_BIND)                                    │
  ──offload──▶│   → octeon_ft_add(flow): build octeon_flow{} from flow_offload_tuple    │
   request    │   → insert into rhashtable keyed by (hashed) 5-tuple, BOTH directions   │
              │  flow_offload stats callback ← read FAU per-flow counters (periodic)    │
              │  flow teardown / gc ← tie to nf_flow_table gc (NOT a parallel timer)    │
              └────────────────────────────────────────────────────────────────────────┘
                                            │ publishes/retires entries
                                            ▼  (RCU-protected rhashtable)
   wire→PIP→IPD→POW──get_work──▶ core (NAPI/SoftIRQ, ATOMIC tag held)
                                   │  octeon_ft_rx_hook(wqe):              ← FAST PATH
                                   │    1. lookup 5-tuple from WQE inline   (no skb)
                                   │       packet_data[] prefix, keyed by   (no buffer touch)
                                   │       wqe.tag as the hash
                                   │    2. miss → return 0 (fall through to skb slow path)
                                   │    3. hit  → rewrite L2/L3/L4 in the FPA buffer
                                   │       (MAC, dec TTL, NAT addr/port, incr-checksum)
                                   │    4. cvm_oct_transmit_qos(wqe, out_port)  → PKO
                                   │       dontfree=0 (HW frees), ipoffp1 (HW L4 cksum)
                                   │    5. bump FAU counter; done — CPU never built an skb
                                   ▼
                                  PKO → wire   (+ HW buffer recycle to FPA)
```

The fast path is a single hook at the top of the driver's POW work loop, before
skb construction — the most expensive part of the slow path. On a miss it returns
and the packet proceeds through the normal `netif_receive_skb` path unchanged
(strictly additive; correctness is never worse than today).

## 4. The `flow_block_offload` → Octeon mapping

nftables/`nf_flow_table` already drives this model upstream (it's how the generic
software flowtable and the mlx5/etc. hardware offloads work). We implement the
`flow_block_cb` / `flow_offload` ndo path:

| nft flowtable concept | Octeon realization |
|---|---|
| `flow_offload` (a 2-tuple-direction conntrack flow) | one `octeon_flow` object, two `octeon_flow_dir` halves |
| `flow_offload_tuple` (5-tuple + dir + xmit info) | rhashtable key + cached rewrite (out ifidx, next-hop MAC, NAT addr/port) |
| FLOW_OFFLOAD_ADD / DEL | `octeon_ft_add()` / `octeon_ft_del()` insert/retire RCU entry |
| flow timeout / gc | driven by `nf_flow_table` gc — we do NOT re-implement aging |
| flow stats request | read FAU per-flow counter, return to nft |
| xmit: neigh + dev | precomputed at ADD: out PKO queue + L2 header bytes |

Crucially, we lean on the **existing flowtable framework for flow lifecycle,
conntrack integration, NAT tuple computation, and teardown.** Our novel code is
only: (a) the rhashtable + WQE-level RX hook, (b) the in-buffer rewrite, (c) the
PKO transmit, (d) FAU stat plumbing. This is far less than the vendor's
standalone `/proc/cavium` flow cache, and it inherits the framework's correctness
(conntrack states, NAT helpers, IPS_OFFLOAD teardown on FIN/RST).

### Flow classes accelerated (v1)
Established **IPv4 TCP and UDP** 5-tuple flows, with SNAT/DNAT/masquerade
(the ERLite's whole purpose). Explicitly **slow-path** (return 0 from the hook):
non-first IP fragments (no L4 ports to match — matches vendor behavior,
`offload-behavior-spec.md` §6a), TTL≤1 (must generate ICMP), IP-options packets,
ICMP, any new/unestablished flow, anything not in the table. IPv6 and VLAN: v2
(see §9). This mirrors the vendor's actual scope, measured.

## 5. Per-flow state

```c
struct octeon_flow_dir {
    /* lookup key — compared after a wqe.tag hash hit */
    __be32  saddr, daddr;
    __be16  sport, dport;
    u8      proto;            /* IPPROTO_TCP | IPPROTO_UDP */
    u8      in_ifidx;         /* ingress octeon port */
    /* rewrite (precomputed at ADD) */
    __be32  nat_saddr, nat_daddr;
    __be16  nat_sport, nat_dport;
    u8      out_port;         /* egress octeon port → PKO base queue */
    u8      l2_hdr[14];       /* dst+src MAC, ethertype for the egress */
    u16     fau_reg;          /* FAU register holding this dir's packet count */
    struct rhash_head node;
};
```

- **Keyed/hashed by the 5-tuple; the WQE's `tag` (PIP's CRC over the same tuple)
  is the hash seed** — so a hit is usually one cache-line compare. We do NOT trust
  the tag as an exact key (CRC collisions); it only buckets the rhashtable.
- **Where it lives**: main-memory rhashtable, RCU-protected (readers in SoftIRQ,
  writers in process/wq ctx). Rationale: FPA pools are for packet buffers, not
  control structs; an rhashtable gives O(1) lookup, RCU-safe teardown, and reuses
  the kernel's proven implementation (the vendor used a custom bucket table — we
  don't need to). Sizing: target 8k–64k flows (vendor default 8192,
  `offload-behavior-spec.md` §4a); memory ≈ 64 B/dir × 2 × 64k ≈ 8 MB worst case.
- **Both directions installed atomically at ADD** — the vendor pre-installs the
  reverse mapping before reply traffic (the `In=(null)` observation,
  `offload-behavior-spec.md` §6a); the nft flowtable already hands us both
  directions, so this is free.

## 6. Data path detail

In `octeon_ft_rx_hook(struct cvmx_wqe *wqe)`, called from the POW work loop
(the staging driver's `cvm_oct_poll`, before `netif_receive_skb`):

0. **Single-buffer only**: handle `wqe.word2.bufs == 1` (packet in one FPA
   buffer — the common case; the staging RX zero-copy path uses the same gate,
   `ethernet-rx.c` `skb_in_hw = bufs==1`). Multi-buffer/jumbo (`bufs>1`) →
   return 0 (slow path). Keeps the in-place rewrite and PKO single-segment
   submit simple in v1.
1. **Cheap rejects from WQE word2** (already parsed by PIP, no memory touch):
   `not_IP`, `IP_exc`, `is_frag`, `rcv_error`, `!tcp_or_udp` → return 0.
2. **Lookup**: hash = `cvmx_wqe.tag`; read the 5-tuple from the inline
   `wqe.packet_data[]` prefix (PIP copies the headers into the WQE — no packet
   buffer access needed for the lookup). rhashtable_lookup; miss → return 0.
3. **TTL/HL check** from the inline header; ≤1 → return 0 (slow path emits ICMP).
4. **Rewrite in place** in the FPA packet buffer (`wqe.packet_ptr`): dst/src MAC
   (from `l2_hdr`), decrement TTL, NAT addr/port, **incremental checksum update**
   for L3; L4 checksum left to PKO (`ipoffp1`).
5. **Transmit**: `cvm_oct_transmit_qos(wqe, out_port, queue)` — submit the
   packet buffer to PKO with `dontfree=0` (HW recycles the RX buffer to FPA) and
   `ipoffp1` set (HW computes the post-NAT L4 checksum). Then **free the WQE
   buffer itself** back to the WQE FPA pool (`cvmx_fpa_free(wqe, WQE_POOL, …)` /
   the existing `cvm_oct_free_work` path frees both — the packet buffer goes to
   PKO, only the WQE descriptor needs freeing here).
6. **Ordering**: no per-flow lock is needed. The fast path is **read-mostly on
   flow state** — the rewrite fields (`l2_hdr`, NAT addr/port, out_port) are
   immutable after ADD, and the only per-flow write is the FAU counter, which is
   hardware-atomic. The default **ORDERED** tag (§2) gives in-order PKO submission
   within a flow (the ordered-FIFO→in-order-PKO idiom, `docs/blocks/pow.md` §7)
   without forcing single-core-per-flow exclusion. So ORDERED is not just
   sufficient, it's preferable to ATOMIC here — **no PIP reconfiguration
   required.** Use `CVMX_PKO_LOCK_NONE` per (port,queue) with the queue chosen so
   a given flow always uses the same PKO queue (per-flow-affine, like the staging
   `qos` selection), or `CVMX_PKO_LOCK_ATOMIC_TAG` if cross-core same-queue
   contention is observed.
7. **Account**: the PKO command's `reg0`/`size0` fields atomically bump the
   per-flow FAU counter on transmit completion — hardware-side, no CPU add
   (`docs/blocks/pko.md` §1).
8. Return 1 (consumed). The CPU never allocated an skb, never called into the
   network stack, never freed a packet buffer (HW did).

**Why this is fast**: steps 1–2 touch only the 128-byte WQE (in L2 cache, POW
delivered it); step 4 touches one packet buffer once; PKO + FPA do TX and buffer
recycling in hardware. Per-packet budget at the vendor's 247k pps/core ≈ 2000
cycles — ample for a hash lookup + header rewrite (`octeon-fastpath-model.md` §4).

**Latency caveat (honest)**: bypassing the skb path removes the qdisc/stack
queueing, but the measured vendor latency advantage (2.6 ms vs our 6.1 ms) is not
*automatic* from stack bypass — the staging TX path has no BQL and a deep
(1000-entry) PKO free list (`offload-behavior-spec.md` notes; Phase 0 found no
`byte_queue_limits` on the octeon netdev). Hitting ≤6 ms / approaching 2.6 ms
likely requires **PKO output-queue depth discipline** in the fast path (bound
in-flight bytes per queue), not just deletion of the stack walk. This is a v1
milestone-3 risk to measure, not a freebie.

## 7. Control path

- **ADD** (`FLOW_OFFLOAD_ADD`): build both `octeon_flow_dir`, resolve egress port
  + next-hop MAC from the flowtable's `dst`/neigh, allocate two FAU registers,
  rhashtable_insert (RCU). Reject (leave to slow path) anything we don't handle.
- **Counters**: a periodic worker (or the flowtable stats callback) reads each
  flow's FAU counters, feeds packet/byte deltas back via the flowtable stats API
  so **`conntrack -L` and nft show real offloaded counts** — something the vendor
  never did (`offload-behavior-spec.md` §4a). This keeps conntrack's own timeout
  refreshed too, so teardown stays correct.
- **DEL / gc**: tied to `nf_flow_table` gc and conntrack teardown (IPS_OFFLOAD
  cleared on FIN/RST). We do NOT run a parallel 20-min timer like the vendor; the
  framework already does aging correctly.
- **Learn-storm safety** (the vendor's measured 19× cliff under new-flow floods,
  `offload-behavior-spec.md` §6c): ADD happens in the framework's context, not
  the data path — a miss in the hook is *cheap* (it just returns to the normal
  skb path). So a flood of new flows degrades to ordinary software forwarding,
  NOT to slow-path learning of throwaway flows. This is a structural improvement
  over the vendor design and should be validated (a learn-rate test is in the
  plan). Optionally cap ADD rate / table size.

## 8. Integration with the staging driver

Per `docs/integration-gap.md`, the binary vendor module imports only ~5
Octeon-specific symbols beyond standard kernel API; the real work is small,
GPL-clean driver patches:

1. Add a WQE-level RX callback hook in `cvm_oct_poll` (the vendor's
   `cvm_oct_register_rx_callback` / `cvm_ipfwd_rx_hook` seam — we add our own,
   clean).
2. Reinstate a WQE-level QoS transmit `cvm_oct_transmit_qos()` — **mainline still
   carries its prototype at `drivers/staging/octeon/ethernet-tx.h:10`** (a
   dangling decl from old out-of-tree consumers); reconstructing a known-good
   mainline interface, not copying SDK code.
3. Two `EXPORT_SYMBOL` lines for `cvmx_pko_get_base_queue` /
   `cvmx_pko_get_num_queues` (in-tree, currently unexported).
4. `octeon_flowtable.ko`: the rhashtable + hook + transmit + flow_block_offload
   registration on the octeon netdevs. New file, all our code.

Everything else (PIP/POW/PKO/FPA/FAU accessors) is already inlined from the GPL
`arch/mips/include/asm/octeon/cvmx-*.h` headers.

## 9. Open questions / alternatives

1. **`flow_offload_hw` vs a private hook?** The clean upstream path is to present
   as `flow_block_offload` hardware offload (`flow_offloading_hw`). But the
   "hardware" here is a software fast-path on the NPU cores — is that an
   acceptable use of the HW-offload API, or should it be a distinct mechanism?
   (We think HW-offload is right: it's offload *from the Linux stack* onto the
   NPU complex, even if a core still runs the fast routine.)
2. **Staging driver as the base.** `drivers/staging/octeon/` has been
   deletion-proposed before. Is extending it acceptable, or must this wait on /
   drive de-staging? Multi-queue: the driver is single-netdev-queue but the PKO
   layer is multi-queue; we use PKO queues directly, sidestepping `ndo_select_queue`.
3. **IPv6 and VLAN** (v2): the vendor offloads both; the gw config is VLAN-heavy
   (router-on-a-stick). PIP tags IPv6 and parses VLAN; the hook generalizes. Defer
   to keep v1 reviewable.
4. **CVMSEG / async IOBDMA dependency**: the fast path benefits from
   `CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE>0` (Phase 0 lever #2). Should the driver
   depend on / select it?
5. **Tag-type assumption**: we rely on PIP being configured so TCP/UDP flows
   carry an ATOMIC tag for lock-free per-flow ordering. The staging driver's
   default PIP config (and `receive_group_order`) must be compatible; if not, a
   small PIP setup change is needed.

## 10. Test / bring-up plan (Phase 4 milestones)

1. **Plumbing**: `octeon_flowtable.ko` registers `flow_block_offload`, accepts
   ADD/DEL, returns success, programs nothing. Verify nft offload state + no
   crashes. (1–2 wk)
2. **One flow, one direction**: hook matches a single hardcoded 5-tuple, rewrites,
   PKO-transmits; verify the packet egresses correctly and bypasses the skb path
   (tcpdump-blind like the vendor). Single-flow benchmark vs B5. (2 wk)
3. **Full table + NAT + both directions + FAU stats**: rhashtable, real
   flow_block_offload, conntrack counters visible. Re-run the Phase 0 matrix. (2 wk)
4. **Scale + learn-storm**: 10k/65k flows, pktgen new-flow flood; verify no 19×
   cliff (degrade-to-forwarding). Targets (`docs/PROJECT-OUTCOME.md`): single-flow
   TCP ≥750 Mbps, UDP-64 ≥200 kpps, RTT ≤6 ms, no regression vs B5, learn ≥23k/s.
5. Iterate or declare done. Each milestone time-boxed; 3× overrun ⇒ design rework.

**Go/no-go**: if the fast path cannot beat the B5 free-lever baseline
(73k pps / 6.1 ms) meaningfully, the driver isn't worth its complexity and B5
ships as the answer. The bar is EdgeOS-class small-packet pps + determinism.

---

## On review (decided 2026-06-04)

This is a self-hosted patch for the maintainer's own EOL-silicon router, **not an
upstreaming effort** — so it is NOT gated on netdev@ review. The briefing
suggested an early netdev RFC; on reflection that's overhead with little payoff
here (slow, the octeon staging driver is itself deletion-proposed, and the goal
per the project brief is "ERLite forwards NAT at acceptable speed", not "ship an
upstream driver"). The real risk-reduction netdev review would have provided —
catching a fundamental design flaw before months of Phase 4 — we get instead from
**adversarial self-review of this doc against the in-tree code**, which already
paid off (it caught an ATOMIC-vs-ORDERED tag overclaim, §6, that would have cost
real bring-up time). Proceed to Phase 4 on the go/no-go above; revisit
upstreaming only if v1 works and there's appetite. The §9 open questions are
notes-to-self, not questions-for-a-list.
