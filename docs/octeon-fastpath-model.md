# The Octeon+ CN5020 fastpath model

*Phase 1 deliverable, 2026-06-04. Sources: GPL headers/executive in the mainline kernel tree
(arch/mips/include/asm/octeon/, arch/mips/cavium-octeon/executive/) and the GPL
staging driver `drivers/staging/octeon/`. No SDK, no UBNT binaries, no HRM.
Detailed per-block catalogs with full register citations: `docs/blocks/{pip-ipd,
pow,pko,fpa-fau}.md`. Integration-surface analysis: `docs/integration-gap.md`.*

## 1. The life of a forwarded packet (hardware view)

```
wire → GMX (MAC) → PIP parse/classify ──► FPA pool 0 (packet data, 2048 B bufs)
                        │                 FPA pool 1 (WQE, 128 B)
                        ▼
                  IPD writes packet + WQE, submits to POW
                        ▼
            POW input queue (qos 0-7) ── group g (0-15)
                        ▼  (only cores whose PP_GRP_MSK has bit g)
            core: get_work → WQE  [tag held: ORDERED/ATOMIC per PIP config]
                        ▼
            core processes (this is where Linux/our code runs)
                        ▼
            PKO command (2 words) → queue doorbell → wire
                        └─ dontfree=0 ⇒ PKO frees the RX buffer to pool 0
```

1. **PIP parses** L2/L3/L4, validates IP and TCP/UDP checksums, and computes a
   32-bit **tag** whose low 24 bits are a CRC over per-port-selected tuple fields
   (classically the 5-tuple) — `PIP_PRT_TAGX` (blocks/pip-ipd.md §1.3; pow.h:2098-2106).
   The tag's **tag_type** (ORDERED/ATOMIC, selectable per traffic class, e.g.
   `tcp4_tag_type`) defines the ordering domain. PIP also assigns a 3-bit **qos**
   (port default / VLAN-prio / DSCP maps / 4 watchers) and a 4-bit **group**,
   optionally folding low tag bits into the group via GRPTAG → hash-spread of
   flows across groups.
2. **IPD** pulls a 2048-byte buffer from FPA pool 0 and a 128-byte WQE from pool
   1 — autonomously, no CPU — writes the frame and the parse results
   (`cvmx_wqe`: word1 = len/port/qos/grp/tag, word2 = parse flags, packet_ptr,
   96-byte inline prefix), and submits the WQE to POW (blocks/pip-ipd.md §2-3).
3. **POW schedules** the WQE to any core subscribed to its group
   (`CVMX_POW_PP_GRP_MSKX`). While a core holds an **ATOMIC** tag, no other core
   can hold the same tag value ⇒ per-flow serialization without locks; **ORDERED**
   preserves per-tag FIFO order across cores (blocks/pow.md §2-3).
4. **The core** does whatever software does. For plain Linux: build/claim an skb
   and `netif_receive_skb()`. For a fastpath: rewrite headers in the buffer and
   hand the same buffer to PKO.
5. **PKO transmits** from a 2-word command (word0 = lengths/flags/FAU-ops,
   word1 = buf_ptr). With `dontfree=0` the hardware **frees the RX buffer back to
   its FPA pool after TX**; with `ipoffp1` set it **computes and inserts the
   TCP/UDP checksum**; with `reg0` set it **decrements a FAU counter on
   completion**. In-order multi-core TX uses `CVMX_PKO_LOCK_ATOMIC_TAG`, which
   piggybacks on the POW tag the core already holds (blocks/pko.md §1,3,5).

**The steady-state forwarding loop needs zero CPU buffer management and zero
software locks**: IPD allocates, PKO frees, POW tags serialize, FAU counts.
The CPU's only mandatory role on this silicon is the per-packet decision.

## 2. The one hard truth: no hardware flow table

CN50XX has **no TCAM and no exact-match classifier** (blocks/pip-ipd.md §4).
Steering granularity is: per-port config + tuple-hash tag + GRPTAG group
spreading + exactly **4 watchers** that each match ONE 16-bit field. Therefore:

> **Any per-flow offload on this chip is software running on a core.** The
> hardware cannot forward a packet end-to-end without CPU. What the hardware
> *does* provide is everything that makes a software fastpath cheap:
> pre-parsed headers, pre-validated checksums, a pre-computed flow hash (the
> tag), per-flow atomicity (ATOMIC tag), flow→core affinity (GRPTAG), zero-copy
> buffers (FPA recycling), TX checksum insertion, and completion accounting (FAU).

This matches the black-box evidence from EdgeOS: its module imports per-CPU
primitives and procfs (integration-gap.md §C), maintains `/proc/cavium/ipv4/cache`
(a software flow cache), and its offload runs with measurable CPU load
(phase0-results.md cfg D: 48-85% busy). EdgeOS's "hardware offload" is a
WQE-level software fastpath — and ours will be too. The 940 Mbps / 278+ kpps
numbers it posts define what this architecture achieves when done well.

## 3. What the mainline staging driver does today (and leaves on the table)

RX (`ethernet-rx.c`): one NAPI instance per POW group; IRQ `OCTEON_IRq_WORKQ0+g`
fires on group-nonempty (threshold regs, rx:488-513). Poll loop: restrict the
core's group mask to g (rx:200-211), `get_work` (sync, or async-IOBDMA when
CVMSEG > 0), zero-copy skb claim when `bufs==1` (the FPA packet buffer IS an skb
data area; skb pointer stashed at fpa_head−8, rx:248-262, 285-291), checksum
verdict from word2 → `CHECKSUM_UNNECESSARY` (rx:339-345), `netif_receive_skb`
(rx:352), WQE freed to pool 1 + FAU counter bump (rx:380-383).

TX (`ethernet-tx.c`): 2-word PKO command; `REUSE_SKBUFFS_WITHOUT_FREE` donates
eligible skb buffers to pool 0 with `dontfree=0` (+25% claimed, tx:296-357 —
**but it's compiled out when CONFIG_NETFILTER is set**, ethernet-defines.h:24-27,
so router builds never get it); TCP/UDP checksum via `ipoffp1` (tx:362-371);
per-qos FAU in-flight counters that PKO decrements in hardware (tx:398);
`CVMX_PKO_LOCK_NONE` + per-queue spinlock-protected free lists.

Known idle capacity in the staging driver as shipped by OpenWrt:

| # | Lever | Where | Effect |
|---|---|---|---|
| 1 | `receive_group_order=1..4` | module/boot param, ethernet.c:47-54, 696-735 | PIP GRPTAG spreads flows across 2^n groups ⇒ per-group IRQs ⇒ **hardware RSS** onto both cores. OpenWrt default 0 = single group = the single-core ceiling we measured (50% busy wall, phase0-results.md). |
| 2 | `CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE` | kernel config; **OpenWrt ships 0** (`target/linux/octeon/config-6.18`) | =0 forces `USE_ASYNC_IOBDMA=0`: every get_work/FAU op is a synchronous pipeline-stalling IO load. >0 enables the async scratch pipeline the driver was designed around. |
| 3 | `REUSE_SKBUFFS_WITHOUT_FREE` | killed by CONFIG_NETFILTER | zero-copy TX donation disabled on every firewall build. A fastpath that bypasses netfilter for established flows can use the equivalent technique safely. |
| 4 | PKO queues/port = 1 | `cvmx-config.h:10-20` (build constant) | silicon supports 32 queues; >1/port enables qos TX queues (what EdgeOS's `cvm_oct_transmit_qos` uses). |

Items 1+2 are pure-config Phase 0 addendum experiments worth running before any
new code: they may move the software baseline (B3: 663 Mbps / 61 kpps)
substantially, which re-baselines the success metric's "no regression" floor.

## 4. The shape of the offload (Phase 3 preview, hardware-constrained)

Given §2, a `flow_block_offload` backend for this chip can only mean:

1. **Hook at the WQE** — process work *before* skb construction (the
   single most expensive part of the slow path). The driver needs an RX
   callback at the top of `cvm_oct_poll` — exactly the seam EdgeOS's
   `cvm_oct_register_rx_callback`/`cvm_ipfwd_rx_hook` patch adds
   (integration-gap.md §C).
2. **Classify by tag-assisted exact match** — the WQE arrives carrying
   `word1.tag` (the 5-tuple CRC). Use it as the flow-table hash key; compare
   full tuple from the inline `packet_data[96]` prefix (headers are right
   there — no buffer touch needed for the lookup).
3. **Serialize per flow with the ATOMIC tag** the core already holds (PIP
   `tcp4_tag_type=ATOMIC`) — no locks in the flow hot path.
4. **Rewrite in place** (MAC addresses, TTL, NAT fields + incremental
   checksum) in the FPA buffer.
5. **Transmit via PKO with `dontfree=0`** — hardware frees the buffer;
   `ipoffp1` covers L4 checksum after NAT rewrite. Use
   `CVMX_PKO_LOCK_ATOMIC_TAG` for ordering, or per-queue exclusivity.
6. **Count via FAU**, report to nftables flowtable stats on the slow tick.
7. **Miss path**: fall through to the existing skb path (the packet was
   going there anyway — strictly additive).

Multi-core comes from GRPTAG flow spreading (lever #1), not from anything we
must invent. Eviction/aging mirror the flowtable's own conntrack timeouts; the
EdgeOS knobs (`flow_new_lifetime`/`flow_old_lifetime`, proc-cavium-snapshot.txt)
hint at a two-stage age-out worth black-box-characterizing in Phase 2.

**Per-packet budget sanity check**: 940 Mbps @1500 B ≈ 78 kpps; EdgeOS cfg D did
85 kpps at ~45% of 2×500 MHz ⇒ ≈ 5.3 µs of CPU per packet — thousands of cycles,
comfortable for hash+rewrite+PKO-store. At 278 kpps/85% it spent ≈ 3 µs/pkt.
The architecture above fits the budget; the slow path's skb+netfilter walk
(≈ 65 µs/pkt at cfg A2's 15 kpps/core) is what's being deleted.

## 5. Verification queue (feeding Phases 2-3)

- [ ] Run levers #1/#2 (group spreading, CVMSEG) on the bench unit → new software baseline.
- [ ] CN5020 stepping: pass-1 quirks (PIP drop counters, PKO static priority) — check `cvmx_octeon_is_pass1()` on live silicon.
- [ ] Confirm PIP tag CRC covers the tuple as configured by mainline defaults (read `PIP_PRT_TAGX` via /dev/mem on the bench unit; compare both OSes).
- [ ] EdgeOS Phase 2 black-box: does `/proc/cavium/stats` populate under load? What do `ipv4/cache`, `flows` expose? 8-flow TCP regression root cause.
- [ ] Phase 3 design doc → netdev@ before code.
