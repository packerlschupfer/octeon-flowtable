# Project outcome

*Summary for someone picking this up cold.*

## Goal

Clean-room reimplement Ubiquiti's `cavium_ip_offload` behaviour against a mainline
OpenWrt kernel, so an EdgeRouter Lite 3 (Octeon+ CN5020, EOL) approaches EdgeOS
NAT throughput under OpenWrt — without lifting code from the UBNT binary module or
the Cavium SDK. Hardware facts come only from the GPL kernel headers
(`arch/mips/include/asm/octeon/`) and the public OCTEON hardware reference manual.

## Outcome: ACHIEVED — and extended well past the original metric

A stable, clean-room kernel flow-offload driver (`octeon_flowtable.ko` + a small
staging-driver patch) that **matches or beats the EdgeOS vendor offload on every
success metric**, on real hardware:

| metric | OpenWrt software (best) | **this driver** | EdgeOS vendor |
|---|---|---|---|
| single-flow TCP NAT | 764 Mbps | **932 Mbps** | 940 Mbps |
| multi-flow TCP NAT | 941 Mbps | **935 Mbps** | 563 Mbps |
| UDP-64 NAT (pps) | 73k | **431k** | 247–431k |
| RTT under load | 6.1 ms | **2.1 ms** | 2.6 ms |

5.9× the software baseline on small-packet pps; 3× better latency; matches the
vendor it reverse-engineered — entirely from GPL sources.

## How it works (one paragraph)

The CN5020 has no exact-match hardware classifier, so the offload is a software
fast path running on the NPU cores, with the hardware making it cheap. nftables'
`flow_block_offload` installs flows; the driver decodes each into a key (5-tuple +
ingress VLAN + ingress port) and a rewrite, kept in an rhashtable. A WQE-level RX
hook in the staging driver's `cvm_oct_poll` (added by patch) intercepts forwarded
packets *before* any skb is built, looks them up by the PIP-parsed headers,
rewrites L2/L3/L4 in the FPA buffer (NAT addr/port, next-hop MAC, TTL/hop-limit,
VLAN tags) with incremental checksums, and transmits via PKO
(`cvm_oct_transmit_qos`) — HW recycles the buffer, no skb, no stack. Misses fall
through to normal Linux forwarding. Per-flow counters are fed back to the
flowtable so offloaded traffic stays visible in `conntrack -L`.

## Capabilities (all verified on hardware)

| class | status |
|---|---|
| IPv4 NAT + routing | ✓ (the headline metric) |
| IPv6 routing | ✓ (no NAT — v6 is routed; MAC + hop-limit, no checksum) |
| 802.1Q single VLAN — retag / pop / push | ✓ (router-on-a-stick, VLAN↔untagged, ± NAT) |
| QinQ (two stacked tags) | ✓ (pop-both / push-both, on the wire) |
| ingress-port keying | ✓ (same 5-tuple+VLAN on different physical ports) |
| global FAU hardware byte/packet counter | ✓ (opt-in, contention-free) |
| PKO tail-drop AQM | ✓ (bounds bufferbloat: 54 ms → 4 ms at equal throughput) |
| OpenWrt packaging + fw4 `flow_offloading_hw` path | ✓ |

Hardware ceilings found (not bugs — silicon limits): PIP parses **≤ 2 VLAN tags**
(a 3rd → not-IP → slow path); the Linux flowtable offloads **TCP/UDP/GRE only**
(no ESP); IPv6 extension headers / fragments → slow path. The full matrix and the
reasoning are in `FUTURE-WORK.md`.

## Where everything is

- **Design**: `docs/design-rfc-octeon-flowtable.md`
- **Hardware model**: `docs/octeon-fastpath-model.md`, `docs/blocks/*.md` (register-cited)
- **Vendor behavioural spec**: `docs/offload-behavior-spec.md`
- **Capability matrix + remaining work**: `docs/FUTURE-WORK.md`
- **Deploy**: `docs/INSTALLING.md`; **build/migrate runbooks**: `prompts/`
- **The driver**: `src/octeon_flowtable/`
- **Kernel patch**: `src/staging-patches/120-octeon-flowtable-hooks.patch`
- **OpenWrt package**: `package/octeon-flowtable/`

## Clean-room bugs/lessons that mattered

1. **Mangle endianness** — nftables flow-offload MANGLE actions are host-u32
   encoded for little-endian HW; on big-endian Octeon you must decode the
   *semantic* value, not byte-replay. (No upstream big-endian flow-offload driver
   exists to copy from.)
2. **Source port in the HIGH 16 bits** of the mangle val on big-endian — using
   `val & 0xffff` for both ports zeroed the source port (TCP stalled at IW10).
3. **PKO `LOCK_NONE` → `LOCK_CMD_QUEUE`** — two cores transmitting to one PKO queue
   corrupted it and panicked the kernel under load.
4. **Latency was CPU, not queueing** — full per-packet checksum recompute saturated
   the core; incremental checksum fixed pps and latency together.
5. **`rmmod` crash** — the FLOW_BLOCK_UNBIND path used `flow_block_cb_remove()`
   instead of the indirect-variant `flow_indr_block_cb_remove()`, leaving a
   dangling entry on the global indr list → use-after-free on unload. Matches
   ice/mlx5/sfc. (Found by setting `panic_on_oops=0` to survive the oops and read
   the trace, since the bench has no serial.)
6. **Per-flow FAU counters don't fit the hardware** — the FAU is 2 KB total; the
   software `atomic64` per-flow stats are correct, FAU is used only for a global
   aggregate + the AQM in-flight counter.

## VLAN buffer surgery (the reusable trick)

VLAN retag/pop/push/QinQ is one mechanism: shift the 12-byte dst+src MAC by
`(n_ingress − n_egress) × 4` bytes (L3 and the ethertype never move because
`ip_offset` already counts the tags), write the egress tags in the opened slot,
and fix `buf_ptr.back` (the FPA free offset, in 128-byte cache lines) only when
the ±shift crosses a line — getting `back` wrong corrupts the FPA pool. Tags are
not in the IP/L4 checksums, so nothing else changes.
