# Future work — octeon_flowtable beyond the met metric

*2026-06-04. The driver meets all success metrics and matches the vendor (see
docs/PROJECT-OUTCOME.md). These are the remaining items, each with enough design
to implement. None blocks the metric; ordered by value for the gw use case.*

## Done since M2
- ✅ Per-flow stats → `conntrack -L` visibility + keep-alive (`octeon_ft_stats`).
- ✅ Incremental checksum (RFC 1624) — fixed pps (335k→431k) AND latency (7.6→2.1ms).
- ✅ Cookie secondary index — O(1) STATS/DESTROY instead of O(N) walk.
- ✅ Reproducible build — `target/linux/octeon/patches-6.18/120-octeon-flowtable-hooks.patch`
  (verified: clean `make target/linux/clean` re-applies it; no `.rej`).

## 1. VLAN flows — retag + push/pop + QinQ  ✅ DONE (2026-06-04)
The gw routes *between* VLANs on one physical port (eth2.16 ↔ eth2.18 …) and
between tagged VLANs and the untagged WAN. **Every** 0/1/2-tag L2 transform —
**retag**, **pop**, **push**, and **QinQ** pop-both/push-both (and mixed
2↔1/1↔2) — is **implemented and verified on hardware**, with or without NAT.
Module-only, no staging/kernel change needed.

**What the flowtable actually presents (empirically, not as the design below
guessed)** — for an inter-VLAN routed flow over 802.1Q *subinterfaces*
(eth2.10 ↔ eth2.20):
- The ingress VLAN is **consumed by the subinterface** and is **NOT** in the
  match — no `FLOW_DISSECTOR_KEY_VLAN`. The ingress device is identified only by
  `META ingress_ifindex` (the subiface's ifindex).
- There is **NO `FLOW_ACTION_VLAN_PUSH`** and no encap/`in_vlan_ingress` data.
  The egress VLAN is **implicit in the REDIRECT device**: `act->dev` is the
  egress subinterface (`eth2.20`), so `vlan_dev_vlan_id()` = egress VID and
  `vlan_dev_real_dev()` = the physical octeon port for PKO.
- Actions are just the eth dst/src MAC mangles + REDIRECT (no IP/port NAT for
  pure inter-VLAN routing).
(The earlier guess about `encap[]`/`VLAN_PUSH` was wrong for subinterface-based
routing; see the dump via `octeon_ft_dump_rule`, verbose param.)

**Hardware facts (measured, `octeon_ft_dump_rule` + the WQE diagnostic):**
- PIP parses **both** stacked tags: for a double-tagged frame `vlan_stacked=1`
  and **`ip_offset=22`** (counts both tags), so the L3/L4/NAT rewrite math is
  unchanged. `word2.vlan_id` is the **outer** VID; the **inner** VID is only in
  the buffer (inner TCI at `l2 + ip_offset - 4`).
- The flowtable encodes egress VLANs **device-implicitly** in the REDIRECT
  device (no `VLAN_PUSH`). For a stack, `vlan_dev_real_dev()` **flattens** to the
  physical port; the immediate parent (for the outer VID) is reached via
  `dev_get_iflink` → `dev_get_by_index`.

**Implementation** (`octeon_flowtable.c`) — one unified mechanism, not per-case:
1. Flow key = 5-tuple + ingress VLAN stack: `vlan_id` (outermost) + `vlan_id2`
   (inner, QinQ). The hook reads the outer from `word2.vlan_id` and the inner
   from the buffer; install derives both from the `META ingress_ifindex`
   subiface via `octeon_ft_vlan_stack()`.
2. Install records the egress stack as `n_eg` (0/1/2) + `eg_vid[2]` (wire order),
   walked from the REDIRECT subiface; `out_dev` = the flattened physical port.
3. Hook applies `octeon_ft_vlan_xform()`: **`shift = (n_ig − n_eg) × 4`** bytes
   the 12-B dst+src MAC moves (forward = net pop, backward = net push); L3 + the
   ethertype never move (`ip_offset` points at L3). Then it writes the `n_eg`
   egress tags in the opened slot. This single formula covers retag (shift 0),
   pop (+4), push (−4), pop-both (+8), push-both (−8), and mixed. Tags are not in
   the IP/L4 checksums, so nothing else changes.
4. **`back`** (FPA free offset, in 128-B cache lines) is bumped/dropped only when
   the ±shift crosses a cache line — wrong `back` corrupts the FPA pool, so it is
   handled explicitly (|shift| ≤ 8 ⇒ at most one crossing). A net push checks
   headroom first (`octeon_ft_has_headroom`) so a no-room case falls back
   pristine (IPD first-mbuff skip = 184 B ⇒ addr ~192 B in, `back`=1 ⇒ always
   room; the check guards the general case).
5. `ip_offset` consistency guard (`ip_off == 14 + n_ig×4`) rejects 3+-tag ingress
   to the slow path.

**Verified** on hardware, both directions on the wire (tcpdump) + `[HW_OFFLOAD]`,
stable under `-P4` load (the ±4/±8 buffer surgery under multi-core concurrency
did not corrupt the FPA pool), and unloads cleanly:
- RETAG: eth2.10↔eth2.20 router-on-a-stick, egress VID rewritten 10↔20.
- POP/PUSH (single): tagged LAN (eth2.30) ↔ untagged WAN (eth1), ± masquerade.
- **QinQ** pop-both/push-both: double-tagged (eth2.50.60, outer 50 / inner 60) ↔
  untagged (eth1) — forward egress all plain IPv4 (both tags popped), reverse
  egress `802.1Q 0x8100 vlan 50, 0x8100 vlan 60` on the wire (both pushed), 864
  Mbps. Needed tx-checksum off on the test endpoint's subiface (the USB NIC
  mis-offloads the TCP checksum past two tags — a rig quirk, not the driver).
- Untagged NAT still line-rate `[HW_OFFLOAD]` (936 Mbps, no regression).
- Rigs: `tests/{ftvlan.nft,ftpp.nft,ftqq.nft,bench-vlan-rig.sh,dock-vlan-rig.sh}`.
NOTE: multi-flow throughput is **rig-limited** (test endpoints share one USB NIC;
the gw's switch would distribute VLANs across many client ports) — not a driver
limit.

### Still TODO for VLANs
- **3+ stacked tags — NOT offloadable (hardware limit, verified).** The CN50xx
  **PIP parses at most 2 VLAN tags**: a 3-tagged frame comes back `not_IP=1`,
  `ip_offset=0` (PIP gives up at the 3rd tag), so the hook's existing `not_IP`
  reject already sends it to the slow path. Offloading it would mean parsing past
  PIP by hand in the WQE hook — defeating the point of the HW-classified fast
  path. The gw has no QinQ at all, let alone triple tags. So this is closed:
  correct behavior, not a missing feature.
- **Multi-flow VLAN throughput — resolved as a bench-NIC limit, not a driver
  limit.** Measured: two-NIC push/pop (tagged client on the dock, untagged server
  on eno1, no hairpin) = **742 Mbps -P4**; router-on-a-stick retag (both endpoints
  on the one dock NIC) = 233 Mbps (single-NIC hairpin). For reference the *same*
  dock NIC carried **935–941 Mbps** as the RX/server side in the original non-VLAN
  benchmark, and the VLAN xform adds only a cheap in-buffer byte-shift (no
  checksum) — so the driver does line rate; the bench's RTL8153 USB NIC caps VLAN
  multi-flow (~740–800 Mbps as the tagged TX client). True line-rate VLAN
  multi-flow needs the gw's real topology (a switch trunk feeding many client
  ports). The only tag-transparent bench port is eth2 (direct cable to the dock);
  eth1↔eno1 runs through the VLAN-aware D-Link switch (tagged frames are filtered
  unless that switch is configured to trunk the VLAN — not done, it carries the
  bench management network). Closed: the driver is proven; the number is rig-bound.

## 2. IPv6 flows  ✅ DONE (2026-06-04)
IPv6 routing is **implemented and verified on hardware** — and it turned out
*simpler* than v4, because v6 is **routed, never NAT'd**.

**Hardware facts (measured, V6 diagnostic + `octeon_ft_dump_rule`):**
- PIP fully parses v6: `not_IP=0`, `tcp_or_udp=1`, `ip_offset` points at the v6
  header (=14 untagged). v6 extension headers / fragments become `IP_exc` /
  `is_frag` and are already rejected by the existing hook guards → slow path.
- The flowtable presents a v6 flow as `FLOW_DISSECTOR_KEY_IPV6_ADDRS` + **only
  eth MAC mangles + REDIRECT** — *no* IP6 address mangle, *no* port mangle. So
  there is no NAT66 in practice (OpenWrt routes v6).

**Implementation** (`octeon_flowtable.c`):
- Separate v6 5-tuple key (`octeon_ft_key6`, 128-bit src/dst + ports + proto +
  the VLAN fields) and a second data rhashtable (`octeon_ft_table6`); the cookie
  index + stats/destroy/accounting are shared (flow gains `is_v6`). The verified
  v4 path is untouched.
- `octeon_ft_rx6`: build the v6 key, look up table6, then **MAC rewrite +
  `hop_limit--`** and the shared `octeon_ft_vlan_xform`. **No checksum work at
  all** — v6 has no header checksum, and since neither the addresses nor ports
  change, the L4 checksum (v6 pseudo-header) is unchanged; the hop limit is not
  in it.
- A non-TCP/UDP next-header (an extension header PIP didn't flag) → slow path.
  NAT66/PAT (any non-MAC mangle) → rejected at install → slow path.

**Verified**: v6 TCP flow `fd00:1::2 ↔ fd00:2::2` routed eth1↔eth2 — `[HW_OFFLOAD]`,
**867 Mbps** (vs 402 slow-path), on the wire the forwarded packet has the correct
next-hop dst MAC and **`hlim 63`** (decremented exactly once), no v4/VLAN
regression, clean `rmmod`. Reuses the VLAN xform, so v6-over-VLAN works too. Rig:
`tests/ftv6.nft` (uses `meta l4proto`, not `ip protocol`). NB: the workstation NIC
had `disable_ipv6=1` — set it to 0 first.

## 3. ingress-port in the flow key  ✅ DONE (2026-06-04)
The key now includes the physical ingress octeon port, so the *same* 5-tuple+VLAN
arriving on different physical ports (multi-homing / policy routing) stays
distinct. This is the **only non-module-only feature** — it needed a small
staging export + a kernel rebuild & reflash (the staging driver is built-in,
`CONFIG_OCTEON_ETHERNET=y`).

- **Staging export**: `int cvm_oct_get_port(const struct net_device *dev)` returns
  `priv->port` (in `ethernet-tx.c`; captured in the reproducible patch
  `…/patches-6.18/120-octeon-flowtable-hooks.patch`, verified `patch -p1
  --dry-run` clean against pristine 6.18.34).
- **Module**: both keys (v4 `octeon_ft_key`, v6 `octeon_ft_key6`) gained
  `u16 iif_port`. The hook sets it from `cvmx_wqe_get_port(work)` (an inline — no
  export needed on the data path); install resolves it from the
  `META ingress_ifindex` subiface → physical dev → `cvm_oct_get_port`. A flow
  whose ingress port can't be resolved is refused (slow path), so no dead flows.
- **Verified on the reflashed kernel** (`6.18.34 #1`, `cvm_oct_get_port` in
  kallsyms): a NAT flow's two directions resolve to **different** ports
  (forward ingress eth1 → `iif=1`, reverse ingress eth2 → `iif=2`), and flows
  still `[HW_OFFLOAD]` — which proves the install-side `cvm_oct_get_port` matches
  the hook-side `cvmx_wqe_get_port` (a mismatch would mean zero offload). No
  regression, clean `rmmod`.

Reflash note (for next time): the bench boots `vmlinux.64` (= `vmlinux` `strip -R
.notes`, DTB in `.appended_dtb`, cmdline from U-Boot since `CONFIG_CMDLINE_BOOL`
is off) + a bare-md5 `vmlinux.64.md5` on `/dev/sda1`. The kernel's
`.appended_dtb` is a 1 MB reserved region — graft the trimmed 4892-byte DTB
extracted from the working image to match the exact on-disk size. Backup kept as
`vmlinux.64.working`.

## 4. FAU hardware counters  ✅ DONE — but NOT per-flow (that doesn't fit the HW)
**Key finding: per-flow FAU counters (RFC §5/§6) are hardware-infeasible.** The
Octeon FAU is only **2 KB** (`0 ≤ reg < 2047`; `CVMX_FAU_REG_END`), and the
staging driver already uses the top (growing down from ~2040). The RFC targeted
**8k–64k flows × 2 FAU registers** = 16k–128k registers — off by ~16–500×. You
cannot give thousands of flows their own FAU registers. So **per-flow stats
correctly stay in software** (`atomic64`, measured free); the RFC over-reached
here. (Also: the PKO command's FAU field `subone0` only ever *subtracts* — it is
built for the in-flight/AQM counter of §5, not a monotonic stats accumulator.)

**What was built instead — a global HW-atomic aggregate** (module-only, no
reflash): two fixed FAU 64-bit registers (offsets 8, 16) hold the total
fast-path bytes/packets. The hook issues a `cvmx_fau_atomic_add64` (a single
fire-and-forget I/O store the FAU serializes in hardware) — a *global* software
`atomic64` would instead bounce its cache line between the two NPU cores, so the
FAU counter is the *right* way to do a contention-free global total. Gated by the
`fau_stats` param (default off → zero hot-path cost); read back via the read-only
`hw_tx_bytes` / `hw_tx_packets` params (`/sys/module/octeon_flowtable/parameters/`).

**Verified**: with `fau_stats=1`, the counters track real traffic (e.g. 339 MB
iperf → `hw_tx_bytes≈370 M`, both directions incl. ACKs); gating off freezes them
(no hot-path cost); under `-P4` multi-core load the count is correct (HW
serializes both cores' adds, no lost increments); clean `rmmod`. NB the CPU-issued
FAU add was chosen over the PKO-side `reg0` update specifically to avoid a second
kernel reflash, and is the better design for a contention-free global counter.

## 5. PKO output-queue depth discipline (tail-drop AQM)  ✅ DONE (2026-06-04)
Bounds per-egress-port in-flight bytes so a bulk flow can't bloat the PKO output
queue and add latency. Implemented + verified on hardware.

**Mechanism**: a per-port 64-bit FAU in-flight-bytes counter (`OCT_FT_AQM_BASE +
port*8`). The hook adds a packet's egress bytes when it queues to PKO; the patched
`cvm_oct_transmit_qos` arms PKO to *subtract* the same on transmit (the
`reg0`/`subone0=0`/`size0=64` command fields — the one genuine use of the
subtract-style PKO→FAU op, which is why this, not the stats counter, needed the
staging tweak). So the counter = bytes given to PKO but not yet on the wire. When
it exceeds `aqm_limit`, the hook tail-drops (`cvm_oct_free_work`); a failed
transmit un-reserves. Reserve == subtract (both the post-xform `word1.len`) so the
counter doesn't drift. Needed a staging export (`cvm_oct_flow_aqm_fau`) + reflash.

**Params**: `aqm_limit` (per-port byte cap, 0=off, default off → zero hot-path
cost), `aqm_drops` (ro), `aqm_inflight` (ro, current per-port bytes).

**Verified** (egress bottlenecked to 100 Mbps, 1 Gbps offered):
| `aqm_limit` | peak in-flight | queue latency | throughput | drops |
|---|---|---|---|---|
| 8 MB (track) | 682 KB | **54.5 ms** (bloat) | 94.1 Mbps | 0 |
| 60 KB (bound)| 53 KB | **4.3 ms** | 93.9 Mbps | 105 |

→ **13× lower queue latency at the same throughput** (TCP backs off but keeps the
bottleneck full) — a textbook bufferbloat fix. AQM off = line rate, no drops, no
regression; clean `rmmod`.

## 6. Productisation  ✅ DONE (2026-06-04)
(Was out of scope in the original brief; done on request.) Full deployment guide in
`docs/INSTALLING.md`. Delivered:
- **OpenWrt package** `package/octeon-flowtable/` — a standard kmod package
  (compiles `src/octeon_flowtable/octeon_flowtable.c`, autoloads the `.ko`,
  installs the init script + UCI config). `DEPENDS:=@TARGET_octeon
  +kmod-nft-offload +kmod-nf-flow`.
- **procd init script** + `/etc/config/octeon-flowtable` — loads the module and
  applies the tunables (`aqm_limit`, `fau_stats`, `verbose`) and pins the two
  Ethernet group IRQs one-per-core (the B5 line-rate tuning). Init logic verified
  on hardware (params set, IRQ 24→CPU0 / 121→CPU1).
- **No custom nftables needed** — the flowtable comes from fw4: set
  `flow_offloading '1'` + `flow_offloading_hw '1'` in `/etc/config/firewall` and
  fw4 installs an `flags offload` flowtable on the zone devices that this module
  claims. **Verified end-to-end**: `fw4 reload` → module claims eth0/eth1/eth2
  blocks → a LAN→WAN flow shows `[HW_OFFLOAD]`.
- `receive_group_order=1` stays a boot-cmdline arg (documented; goes in
  `ERLITE_CMDLINE` or the U-Boot `bootcmd`, not settable at runtime).

## FIXED: `rmmod octeon_flowtable` crashed the kernel (was a reboot)
Unloading the module after a flowtable had ever been bound crashed the kernel
(`CONFIG_PANIC_ON_OOPS=y` turned the oops into a reboot, which is why it looked
like a clean reboot). **Root cause + fix (2026-06-04):** the FLOW_BLOCK_UNBIND
path used `flow_block_cb_remove()` instead of the indirect-variant
`flow_indr_block_cb_remove()`. `flow_indr_block_cb_alloc()` (BIND) adds the
block to the global `flow_block_indr_list` via `list_add(&block_cb->indr.list,
…)`; the plain remove frees the block_cb but leaves that list entry dangling, so
a later `flow_indr_dev_unregister()` (module unload) walks freed memory in
`__flow_block_indr_cleanup` → garbage `release`/`cb_priv` → crash in
`nf_flow_table_gc_cleanup` (unaligned/address error, ExcCode 04). The indirect
remove also does `list_del(&block_cb->indr.list)`. Matches ice/mlx5/sfc. One
line in `octeon_ft_indr_setup`.

How it was found (for next time): the bench has no serial/pstore/netconsole, so
set `panic_on_oops=0` — a *survivable* oops then leaves the box up with the full
trace in `dmesg` instead of rebooting. Bisected with markers: `insmod`→`rmmod`
with no bind is clean; the crash needs a bind. NB: an oops inside `module_exit`
leaves the module half-unloaded (refcount -1, un-removable) → reboot to clear,
then re-run `tests/bench-vlan-rig.sh`. Verified fixed across: bind/unbind/rmmod,
rmmod-while-bound, ×3 cycling, and a full real VLAN-offload teardown.
