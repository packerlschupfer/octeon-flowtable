# POW Work Scheduler Catalog — Cavium Octeon+ CN5020 (CN50XX)

*Extracted 2026-06-04 from `cvmx-pow.h` (pow.h) / `cvmx-pow-defs.h` (defs.h) + `cvmx-address.h`, `cvmx-wqe.h`. This generation names the unit POW ("Packet Order / Work unit", pow.h:29); SSO is the later-generation name — `cvmx-sso-defs.h` does not apply to CN5020.*

## 0. Addressing and sub-DIDs

POW ops are loads/stores to IO-segment addresses; the sub-DID encodes the op AND its effect on the per-core "pending switch" bit (pow.h:1173-1185):
- did 0 `CVMX_OCT_DID_TAG_SWTAG` — **sets** pending-switch (cvmx-address.h:324)
- did 1 `TAG1` — no effect (325)
- did 2 `TAG2` — HW-internal, **never use** (pow.h:1183-1185)
- did 3 `TAG3` — **clears** pending-switch (327)
- did 4 `NULL_RD` (328)

## 1. Work model: get_work / add_work, sync and async

Work = a DRAM WQE handed to POW by physical pointer (`addr:40`, pow.h:488-489) + tag/type/qos/group metadata.

**get_work**: a load with `swork` layout, did SWTAG (pow.h:1360-1366). Response `s_work`: WQE pointer or `no_work:1` (pow.h:485-495; `no_work` can also mean "work too deep in input queue", pow.h:472-485). `wait:1` in the request address (pow.h:256-260): `CVMX_POW_WAIT` stalls until work or timeout; `CVMX_POW_NO_WAIT` returns immediately (pow.h:87-90, 1345-1349).
- `cvmx_pow_work_request_sync_nocheck(wait)` — caller guarantees no pending switch (pow.h:1351-1372)
- `cvmx_pow_work_request_sync(wait)` — waits for prior switch first (pow.h:1385-1394)

**Async via IOBDMA** (`cvmx_pow_iobdma_store_t`, pow.h:1218-1246): `scraddr` (8-byte aligned scratch), `len=1`, did SWTAG, `wait` (pow.h:1447-1450).
- `cvmx_pow_work_request_async[_nocheck](scr_addr, wait)` (pow.h:1438-1476)
- `cvmx_pow_work_response_async(scr_addr)` — `CVMX_SYNCIOBDMA` then scratch read; NULL on no_work (pow.h:1488-1499)

## 2. Tag types and tag switching

### Semantics (pow.h:64-82)
- **ORDERED (0)** — order maintained per tag value (FIFO); multiple cores may hold same tag; NO mutual exclusion (pow.h:65-66).
- **ATOMIC (1)** — order maintained AND **at most one PP holds the tag** (pow.h:67-68). The hardware lock: per-tag critical section.
- **NULL (2)** — no ordering/sync held (pow.h:1731-1734). NEVER switch NULL→NULL (pow.h:69-72).
- **NULL_NULL (3)** — NULL with no POW entry reserved; power-on and post-deschedule state; exited by work request or NULL_RD (pow.h:74-81).

### Switch ops (`cvmx_pow_tag_op_t`, pow.h:95-173)
SWTAG(0), SWTAG_FULL(1) (use when coming from NULL), SWTAG_DESCH(2), DESCH(3), ADDWQ(4), UPDATE_WQP_GRP(5), SET/CLR_NSCHED(6/7), NOP(15).

### API family
- `cvmx_pow_tag_sw[_nocheck](tag, type)` — SWTAG; illegal from NULL (use _full) or to NULL (pow.h:1534-1619)
- `cvmx_pow_tag_sw_full[_nocheck](wqp, tag, type, group)` — SWTAG_FULL; WQE phys ptr in store address offset; checks wqp matches the attached WQE (pow.h:1641-1729)
- `cvmx_pow_tag_sw_null[_nocheck]()` — to NULL via did TAG1 (doesn't set pending bit); **completes immediately, never wait on it** (pow.h:1755-1791)

### One-switch-at-a-time rule
"A tag switch cannot be started if a previous switch is still pending" (pow.h:1612-1617, 1722-1727, 2044-2049). Completion polled via CHORD hw register: `cvmx_pow_tag_sw_wait()` spins on `CVMX_MF_CHORD` (RDHWR 30) until `switch_complete`; warns after 2^31 cycles = deadlock hint (pow.h:1302-1338). A SWTAG **to NULL** would set the pending bit and never clear it → permanent hang; that's why NULL switches use TAG1 (pow.h:1521-1523). Runtime checks `CVMX_ENABLE_POW_CHECKS` default-on (pow.h:59-62, checks listed 38-48).

## 3. Groups — core steering

**16 groups** (pow.h:1842-1846); group fields are 4 bits throughout (pow.h:206,224).

`CVMX_POW_PP_GRP_MSKX(core)` (defs.h:43): per-core `grp_msk:16` + eight 4-bit per-input-queue priorities `qos0_pri..qos7_pri` (defs.h:526-549).
- `cvmx_pow_set_group_mask(core_num, mask)` (pow.h:1851-1858): bit g set ⇒ core eligible for group-g work. **This is the entire core-steering mechanism**: POW schedules a WQE of group g only to a core whose mask includes g.
- `cvmx_pow_set_priority(core, priority[8])` (pow.h:1860-1908): per-QoS-input-queue static priority, 0 highest, 0xF skip; must be contiguous. Valid on CN50XX (gated only against CN3XXX, pow.h:1876-1877).

## 4. Deschedule / reschedule

16 deschedule lists, one per group (pow.h:386-388).
- `cvmx_pow_desched(no_sched)` — DESCH via TAG3; `CVMX_SYNCWS` first; no_sched=1 ⇒ won't be rescheduled (pow.h:2053-2092). Illegal from NULL/NULL_NULL.
- `cvmx_pow_tag_sw_desched[_nocheck](tag, type, group, no_sched)` — atomic switch+deschedule via TAG3; completes immediately (pow.h:1952-2051).
- **ORDERED reschedule caveat** (pow.h:1919-1943): with no pending switch at deschedule, HW reschedules only the FIFO head (ORDERED behaves like ATOMIC). Recommendation: **deschedule with next-state ATOMIC**, switch to ORDERED after reschedule.
- SET/CLR_NSCHED strict ordering rules (pow.h:142-168; `opsdone`/`swdone` poll bits).

## 5. Submitting new work — fastpath re-injection

`cvmx_pow_work_submit(wqp, tag, tag_type, qos, grp)` (pow.h:1806-1838) — self-contained ADDWQ. **A fastpath module CAN construct/modify a WQE and inject it into the scheduler**; the submitted tag is unrelated to the core's current tag (pow.h:1794-1797).

Sets: in DRAM WQE — `word1.tag`, `word1.tag_type`, qos, grp via wqe helpers (pow.h:1813-1817); in the store — op=ADDWQ + type/tag/qos/grp (1819-1824); store address did **TAG1**, offset = WQE phys ptr (1826-1830). **`CVMX_SYNCWS` issued before the store** — "POW may read values from DRAM at this time" (pow.h:1832-1837). `qos` selects one of 8 input queues (pow.h:381-400).

## 6. Synchronization idioms

- ATOMIC tag = lock; ORDERED tag = order-preserving FIFO; NULL releases all (pow.h:1731-1734).
- `CVMX_SYNCWS` before submit/desched — flush WQE writes before POW reads DRAM (pow.h:1836, 2043, 2077). POW does NOT re-read DRAM for in-flight WQEs: tag switches don't update the DRAM WQE; SW must track tag/type itself (pow.h:1557-1563, 1668-1674).
- `CVMX_SYNCIOBDMA` before reading async scratch results (pow.h:1492).
- Pending-switch interlock: MF_CHORD / `cvmx_pow_tag_sw_wait()` (pow.h:1302-1338).

## 7. Tag structure; PIP and PKO relationships

**Tag = 8 SW bits (top) + 24 HW bits**: "By default, the top 8 bits of the tag are reserved for software, and the low 24 are set by the IPD unit" (pow.h:2098-2106; `CVMX_TAG_SW_BITS=8`, `SW_SHIFT=24`). PIP/IPD computes the low 24 bits at parse time per PIP_PRT_TAGX config — work arrives pre-classified into an ordering domain with zero core involvement. Compose/split: `cvmx_pow_tag_compose/get_sw_bits/get_hw_bits` (pow.h:2150-2183).

**PKO ordering idiom**: reserved SW tag subgroup `CVMX_TAG_SUBGROUP_PKO = 0x1` within `CVMX_TAG_SW_BITS_INTERNAL = 0x1` (`SUBGROUP_MASK=0xFFFF`, `SUBGROUP_SHIFT=16`, pow.h:2108-2130). Holding an ATOMIC/ORDERED tag in that subgroup (tagged by PKO queue) serializes/orders PKO submission across cores. (Flagged: the in-order-transmit idiom is implied by these constants + ORDERED semantics, not stated verbatim.)

## CN50XX applicability
- 16 groups, 8 QoS input queues; `PP_GRP_MSKX` is a 16-entry array but CN5020 has cores 0-1 only (defs.h:43).
- Static priorities valid (pow.h:1876-1877).
