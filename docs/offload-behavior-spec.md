# Black-box behavioral spec: `cavium_ip_offload` (EdgeOS)

*Phase 2 deliverable. Method: drive EdgeOS v3.0.1 (4.9.79-UBNT, offload module v6.1)
as a black box from outside — controlled traffic + observation of `/proc/cavium`,
`tcpdump`, netdev counters. NO disassembly. Bench unit: ERLite-3, eth1=LAN
192.168.1.1, eth2=WAN 198.51.100.1, masquerade NAT. Started 2026-06-04.*

> Status: foundational observations done (flow-cache structure, stack-bypass,
> lifecycle). Remaining matrix items tracked in §6.

## 1. The offload is a NAT-aware software flow cache

`/proc/cavium/ipv4/cache` exposes the live flow table. Each forwarded
**connection produces TWO unidirectional entries** (one per direction):

```
| In   | Out  | Src        |SrcP | Dst        |DstP |Proto|PPPoE|VLAN| <translated tuple> |...counters|
| eth1 | eth2 |192.168.1.2 |5201 |198.51.100.2|36682|  6  |  0  |  0 |198.51.100.1|36682|198.51.100.2|5201| 6 |0...
| eth2 | eth1 |198.51.100.2|36682|198.51.100.1|5201 |  6  |  0  |  0 |198.51.100.2|5201 |192.168.1.2 |36682|...
```

Per entry: ingress iface, egress iface, the **match 5-tuple** (pre-translation),
proto (6=TCP, 17=UDP), PPPoE/VLAN tags, and the **post-NAT rewritten tuple**
("Flow Info" columns). So a single masqueraded TCP connection = 2 entries:
- `eth1→eth2`: 192.168.1.2:5201 → 198.51.100.2:36682, rewrite src to 198.51.100.1
- `eth2→eth1`: reply, rewrite dst back to 192.168.1.2

**This confirms the Phase 1 inference** (docs/octeon-fastpath-model.md §2): the
chip has no exact-match hardware classifier, so the offload is a software flow
table keyed on the 5-tuple, carrying the NAT translation inline. `cache_size`
= 8192 entries default (`/proc/cavium/ipv4/cache_size`).

## 2. The fastpath completely bypasses the Linux network stack

Two independent proofs, under a saturating offloaded TCP flow:
- **`tcpdump -ni eth2 'tcp port 5201'` captures 0 packets.** The offloaded
  packets never reach AF_PACKET / the netdev receive path.
- **`/sys/class/net/eth2/statistics/tx_packets` does not increment** (delta = 0
  over 2 s at line rate). The offload transmits via PKO directly
  (`cvm_oct_transmit_qos`, the import seen in integration-gap.md §C), NOT via
  the netdev `ndo_start_xmit`, so `dev->stats` is never touched.

**Design consequence for our octeon_flowtable driver**: offloaded-flow byte/packet
counters CANNOT be read from `/proc/net/dev` or standard netdev stats — they must
be reconstructed (from PKO hardware counters, FAU, or our own per-flow state) and
fed back to the nftables flowtable stats. This is exactly the counter-reporting
problem Phase 3 must design for, now empirically confirmed as mandatory.

(Corollary: any userspace monitoring on the EdgeOS box — iftop, vnstat,
conntrack byte counts — undercounts offloaded traffic. The vendor reconciles
this somewhere; finding where is a §6 item.)

## 3. Flow lifecycle: timeout-aged, not teardown-driven

- Entries are created when a flow is first forwarded (both directions appear
  together once bidirectional traffic establishes the masquerade mapping).
- **Entries persist after the flow ends** (8 entries still present immediately
  after iperf3 exits) — eviction is **timer-based, not FIN/RST-driven**.
- Lifetime knobs (`/proc/cavium/`): `flow_new_lifetime=1200`,
  `flow_old_lifetime=400`. **Measured: an idle flow's cache entry survived >420 s
  with no eviction** → the values are NOT 400 s; units are **seconds, active-flow
  hold ≈ `flow_new_lifetime`=1200 s (20 min)**. (Exact eviction point not pinned —
  a longer poll was contaminated by concurrent test traffic whose ports
  substring-matched the watched flow; methodology note: lifetime tests need an
  otherwise-idle DUT + exact-tuple matching.) Design note: this is a generous
  idle hold — far longer than Linux conntrack UDP timeouts (30/180 s) —
  so the offload keeps NAT mappings warm well past conntrack; our flowtable
  driver should tie eviction to the conntrack flowtable's own gc, not invent a
  parallel 20-min timer.

## 4. The display counters are not the hot-path counters

The trailing numeric columns in `/proc/cavium/ipv4/cache` stay **0 even at line
rate** — the fastpath does not update them per packet (a per-packet cache write
would cost what the offload exists to avoid). `/proc/cavium/stats` reads **empty
even under full offloaded load**. So packet accounting in the fast path is either
absent by design or kept in hardware (PKO/FAU) and not surfaced here. (Whether
EdgeOS's `show ubnt offload statistics` populates from elsewhere = §6.)

## 4a. Offloaded traffic is UNACCOUNTED — EdgeOS doesn't solve it either

> CAVEAT (noted 2026-06-04): the zeros below were measured on the v3.0.1 bench
> unit's **minimal** config. EdgeOS may have an accounting/statistics option that
> was simply not enabled there — so "the vendor never accounts offloaded traffic"
> is NOT established; it may be a config toggle. Treat the zeros as "this config
> showed nothing", not "the vendor can't". Our driver provides the counters
> regardless (§ below / src/octeon_flowtable stats), which is the point.

Where do offloaded byte/packet counts surface on the bench config? **Nowhere:**
- `show ubnt offload statistics` reads **all zeros** under active line-rate
  offloaded load (RX/TX/Bypass/per-proto all 0). It does report structure:
  flow table = 8192 buckets / 1 MB / `flows_max` ≈ 4 MB.
- `conntrack -L` shows the offloaded TCP connection's **state** advancing
  (SYN→…→LAST_ACK→CLOSE) but carries **no byte counts** for it — so the
  control packets (SYN/FIN) traverse the slow path (conntrack tracks teardown)
  while **data packets are offloaded and uncounted**.
- `/sys/class/net/*/statistics` frozen (§2), `/proc/cavium/stats` empty (§4).

So the standard flowtable model is confirmed: hand a connection to the fastpath
once established, return to slow path for teardown; the data plane is invisible
to all the usual accounting.

**Opportunity for our driver (beat EdgeOS, don't just match)**: the mainline
nftables flowtable framework has a per-flow stats callback
(`flow_offload_tuple` stats / `nf_flow_offload_stats`). If our octeon_flowtable
periodically reads PKO/FAU per-flow counters and feeds them back, offloaded
traffic stays visible in `conntrack -L` byte counts and nft flowtable stats —
something EdgeOS never did. Cheap to wire if we keep a FAU counter per flow
(the staging TX path already drives PKO `reg0` FAU decrements, docs/blocks/pko.md
§1).

## 5. Control-plane surface (from observation)

`/proc/cavium/` tree:
- `ipv4/`, `ipv6/`: per-family `fwd`, `vlan`, `pppoe`, `gre`, `bonding` toggles
  + `cache` (flow dump), `cache_size`, `flows` (root-only, empty at rest),
  `ignore_cache_flush`.
- `flow_new_lifetime` (1200), `flow_old_lifetime` (400), `stats` (empty).
- Mirrors `show ubnt offload` (forwarding/vlan/pppoe/gre/bonding enable flags).

## 6a. Acceleration scope (which flows take the fastpath)

| traffic | offloaded? | evidence |
|---|---|---|
| TCP (NAT) | **yes** | 2 cache entries, proto 6, tx bypass (§1-2) |
| UDP (NAT) | **yes** | 2 cache entries, proto 17 |
| ICMP | **no** | 0 cache entries (proto 1) even during active 10pps ping — ICMP rides the normal stack |
| TTL=1 (would expire) | **no** | router emits ICMP "time to live exceeded" via slow path, 0 cache entry — offload validates TTL before caching so the slow path can generate the error |
| fragmented UDP | **partial** | the flow gets a proto-17 cache entry (the FIRST fragment carries L4 ports → matches/caches). Non-first fragments lack L4 ports so can't match a 5-tuple — they almost certainly punt to the slow path for reassembly. Treat fragments as a slow-path edge case in our driver. |

Detail: the **reverse-direction entry is pre-installed with `In=(null)`** — the
offload creates the return-path NAT mapping when the forward flow is learned, and
the ingress iface binds only when reply traffic actually arrives. So the offload
front-runs the reply: a forward packet installs *both* halves of the bidirectional
NAT translation atomically. (Design note for our driver: install fwd+reverse flow
state together at learn time, don't wait for the reply.)

Remaining scope cases (§6): plain-forward-no-NAT, VLAN-tagged, fragments, TTL=1,
IP-options, conntrack-helper flows.

## 6b. Multi-flow scaling — the Phase 0 "regression" does NOT reproduce

Flow-count sweep (re-measured 2026-06-04, EdgeOS offload):

| -P | Mbps | cache entries |
|---|---|---|
| 1  | 939 | 16 |
| 2  | 939 | 22 |
| 4  | 918 | 32 |
| 8  | 938 | 50 |
| 16 | 936 | 84 |

**EdgeOS holds 918–939 Mbps from 1 to 16 flows** — flat, no regression. The
Phase 0 cfg D figure (563 Mbps @ 8 flows, retrans 4150) was a **transient rig
artifact** (workstation/dock state during that one measurement), NOT an offload
property. Cache occupancy scales ~linearly (~5 entries per iperf -P stream:
data+control conns × 2 directions + lingering), far below the 8192 cap — no cache
pressure at these counts. docs/PROJECT-OUTCOME.md cfg D should be read with this
correction. (Removes one of the Phase 4 "open questions"; the driver need not
solve a regression that isn't real.)

## 6c. pps capacity envelope (pktgen, 64-byte UDP through NAT)

iperf3 capped at the *client* (~480 kpps offered) so Phase 0 only proved
EdgeOS ≥278 kpps. pktgen (kernel generator, ~540–590 kpps offered) finds the
real ceilings:

| scenario | offered | **delivered** | router CPU | note |
|---|---|---|---|---|
| 1 flow (fixed 5-tuple) | 571k | **247k pps** | 1 core | single-core forward ceiling |
| 8 established flows | 541k | **431k pps** | both cores 88–95% | multi-core forward ceiling |
| random src port (all-miss) | 593k | **23k pps** | both cores 89–96% | **new-flow SETUP ceiling** |

**Three load-bearing numbers:**
1. **Single-core 64B forwarding ≈ 247 kpps**, **two-core ≈ 431 kpps** — these are
   the real EdgeOS small-packet ceilings (the Phase 0 iperf3 278k was
   client-limited and roughly the single-core figure).
2. **New-flow setup ≈ 23k flows/sec.** A flood of *new* flows (random src ports →
   every packet a cache miss) collapses delivery 19× (431k→23k) with both cores
   pegged in the slow-path learn. This is the offload's worst case: it accelerates
   *established* flows brilliantly but the learn path is ~20× more expensive per
   packet. Relevant to: port scans, SYN floods, many-short-connection workloads.
3. vs our software baseline (B5: 73k single / ~98k multi), EdgeOS offload is
   **~3.4× (1 core) to ~4.4× (2 core)** faster on small packets — the gap the
   WQE fastpath must close, and the honest Phase 4 target.

**Design notes for the driver:**
- Optimize the *learn* path, not just the steady state — 23k/s setup is the real
  fragility. Consider a lightweight learn (defer full flow install, or rate-limit
  learning under miss-storms) so a flow flood degrades to slow-path forwarding,
  not slow-path *learning* of throwaway flows.
- The 8-flow→431k result needed flows spread across both POW groups (the
  group-spreading from Phase 0 levers); single-tuple is inherently 1-core 247k.

## 7. Remaining Phase 2 matrix (to complete the spec)

- [ ] **Lifetime units**: idle a flow, time the eviction → derive `flow_*_lifetime`
  units + new-vs-old semantics.
- [ ] **Acceleration scope**: which flows offload? NAT vs plain-forward, VLAN
  (toggle `ipv4 vlan`), fragments, TTL=1, IP options, ICMP, conntrack-helper
  flows. Method: per case, check for a `/proc/cavium/ipv4/cache` entry + tx
  bypass.
- [ ] **8-flow TCP regression** (Phase 0 cfg D: 563 vs B5 941): vary -P 2/4/8/16,
  watch per-core CPU + cache occupancy → distinguish PKO contention vs cache
  thrash vs ordering.
- [ ] **Capacity envelope**: pps generator (pktgen/trafgen) since iperf3 caps
  ~480 kpps; find the real UDP-64 ceiling (Phase 0 only proved ≥278 kpps).
- [ ] **Flow-scaling**: 1k/10k/65k concurrent → cache_size interaction, eviction
  storms, new-flow setup-rate saturation (SYN flood).
- [ ] **Counter reconciliation**: where (if anywhere) does EdgeOS surface
  offloaded byte counts (`show ubnt offload statistics`, SNMP, conntrack)?
- [x] **hwnat knob — RESOLVED**: inert on the e100. No `/proc/cavium` entry, no
  runtime config option, absent from `show ubnt offload` on the bench unit. It's
  a CLI knob the gw's older firmware exposed but with no machinery on CN5020 — a
  no-op inherited from other EdgeOS platforms (confirms Phase 1 hypothesis).

## 8. Phase 2 status

Substantially complete. Characterized: flow-cache architecture (§1), total
stack-bypass (§2), timeout-aged lifecycle (§3), counter-invisibility +
reconstruction opportunity (§4/4a), control surface (§5), acceleration scope
TCP/UDP/ICMP/TTL/fragments (§6a), no multi-flow regression (§6b), full pps
envelope incl. new-flow-setup ceiling (§6c). The two design-changing findings:
**(1) counters must be reconstructed** — and we can beat EdgeOS by feeding
flowtable stats; **(2) the learn path is the fragility** (23k/s, 19× cliff) —
optimize it. Minor open: VLAN-tagged scope (gw uses VLANs heavily; needs a
tagged rig — defer to when we build the VLAN test setup), plain-forward-no-NAT
scope. Enough to write the Phase 3 design doc.
