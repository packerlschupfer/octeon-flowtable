# PKO Packet Output Catalog — Cavium Octeon+ CN5020 (CN50XX)

*Extracted 2026-06-04. Files: **pko.h** = `arch/mips/include/asm/octeon/cvmx-pko.h`; **pko.c** = `arch/mips/cavium-octeon/executive/cvmx-pko.c`; **pko-defs.h**, **packet.h** (= cvmx-packet.h), **config.h** (= cvmx-config.h), **cmd-queue.h** (= cvmx-cmd-queue.h), **helper.c** (= executive/cvmx-helper.c).*

## 1. Command model

### Command word 0 — `union cvmx_pko_command_word0` (pko.h:171-263)

| Field | Bits | Meaning | Cite |
|---|---|---|---|
| `total_bytes` | 16 | packet length incl. L2, excl. trailing CRC | pko.h:242-245 |
| `segs` | 6 | segment count; gather-list length if `gather` | 238-246 |
| `dontfree` | 1 | clear ⇒ HW frees buffers to FPA after TX; set ⇒ leaves them | 232-247 |
| `ignore_i` | 1 | force per-buffer `i` bit to 0 | 227-248 |
| `ipoffp1` | 7 | non-zero ⇒ (ipoffp1−1)=byte offset to IP header; HW computes+inserts TCP/UDP checksum | 221-249 |
| `gather` | 1 | word1 points to a LIST of buf_ptrs | 216-250 |
| `rsp` / `wqp` | 1/1 | completion response / word3 is a WQE pointer | 209-252 |
| `n2` | 1 | don't pollute L2 with outgoing data | 204-253 |
| `le` | 1 | little-endian segment interpretation | 199-254 |
| `reg0`,`subone0`,`size0` | 11/1/2 | FAU register auto-op on completion: subtract 1 (subone) or packet size; 8/16/32/64-bit | 175-260 |
| `reg1`,`subone1`,`size1` | | second FAU op | 185-258 |

The reg0/reg1 FAU ops are how **PKO itself signals TX completion into FAU counters with no CPU**.

### Word 1 — `union cvmx_buf_ptr` (packet.h:38-67)
`addr:40` (first data byte), `size:16`, `pool:3` (FPA pool to free into), `back:4` (cache lines from addr back to buffer start), `i:1` (invert free decision; **HW sets i=0 on all RX buffers**, packet.h:43-58).

### Modes
Linked (default, segs chained); gather (`gather=1`, word1 → array of buf_ptrs); 3-word variant `cvmx_pko_send_packet_finish3` adds word3 (WQE ptr / addr to zero) (pko.h:460-483).

## 2. Queues and ports

- `cvmx_pko_get_base_queue(port)` (pko.h:541-547) → per-range mapping (pko.h:495-532): iface0 ports 0-15, iface1 16-31, PCI 32-35, LOOP 36-39; else `CVMX_PKO_ILLEGAL_QUEUE` 0xFFFF (pko.h:80).
- `cvmx_pko_get_num_queues(port)` (pko.h:555-567).
- **Kernel build config: 1 queue/port on every interface** (`CVMX_PKO_QUEUES_PER_PORT_INTERFACE0/1 = 1`, config.h:10-12; PCI/LOOP also 1). Multi-queue per port is a **build-config knob**, not a hardware limit.
- **CN50XX hardware max: 32 output queues** (`CVMX_PKO_MAX_OUTPUT_QUEUES`, pko.h:71-75); 40 output ports (pko.h:76).
- Per-queue command rings come from **FPA pool 2** (`CVMX_FPA_OUTPUT_BUFFER_POOL`, config.h:41), 1024 B (8 lines, config.h:26). Ring size registered in `PKO_REG_CMD_BUF` with odd word count so a 2-word command never spans buffers (pko.c:185-198; pko.h:65-68). Queue rings created via `cvmx_cmd_queue_initialize(CVMX_CMD_QUEUE_PKO(q), depth=0(unbounded), pool2, ...)` (pko.c:489-497); first buffer phys → `PKO_MEM_QUEUE_PTRS.buf_ptr` (pko.c:518-530).

### QoS arbitration — `cvmx_pko_config_port(port, base_queue, num_queues, priority[])` (pko.c:326-534)
- Priorities 0-8 → 8-bit round-robin `qos_mask` participation patterns: 0→0x00, 1→0x01, 2→0x11, 3→0x49, 4→0x55, 5→0x57, 6→0x77, 7→0x7f, 8→0xff (pko.c:445-486).
- `CVMX_PKO_QUEUE_STATIC_PRIORITY = 9` (pko.h:79): static-priority queues must be contiguous from base_queue, lower number = higher priority (pko.c:317-401); not on pass1 (pko.c:435).
- Per-queue descriptor → `CVMX_PKO_MEM_QUEUE_PTRS` (`tail/index/port/queue/qos_mask/buf_ptr`, pko-defs.h:1164-1189).

## 3. Doorbell and locking

**Send = write command words into the queue ring + ring doorbell** — `cvmx_pko_send_packet_finish` (pko.h:417-439): `cvmx_cmd_queue_write2(...)` then `cvmx_pko_doorbell(port, queue, 2)` (pko.h:427-431). Doorbell (pko.h:324-341): IO address did `CVMX_OCT_DID_PKT_SEND` + port + queue; **`CVMX_SYNCWS` before the doorbell write** (command words in DRAM before HW reads them; pko.h:339-340).

**Locking modes** (`cvmx_pko_lock_t`, pko.h:96-115):
- `CVMX_PKO_LOCK_NONE` — caller guarantees queue exclusivity (pko.h:97-102).
- `CVMX_PKO_LOCK_ATOMIC_TAG` — `cvmx_pko_send_packet_prepare` does `cvmx_pow_tag_sw_full` to an ATOMIC tag composed of (SW_BITS_INTERNAL, SUBGROUP_PKO, queue) (pko.h:376-398); `finish` waits the switch before writing (pko.h:425-426). **Requires work context** (core holding a POW entry); passes fake WQE ptr 0x80 — therefore the entry CANNOT be descheduled meanwhile (pko.h:367-368). This is the in-order-TX-from-work-context mechanism: the ATOMIC tag serializes sends per queue in arrival order.
- `CVMX_PKO_LOCK_CMD_QUEUE` — ll/sc lock inside the command queue (cmd-queue.h:236+); no tag interaction, no ordering guarantee (pko.h:109-114). ⚠ pko.h:36-41 comment has an apparent typo (says CMD_QUEUE twice; second should be ATOMIC_TAG).

## 4. TX checksum offload
`ipoffp1` ⇒ HW computes and inserts **TCP/UDP checksum** given the IP header offset (≤126 bytes, 7-bit field) (pko.h:221-226). No separate IP-header-checksum request bit documented in word0. Staging driver sets it for IPv4/ihl=5/non-fragmented TCP|UDP (ethernet-tx.c:362-371).

## 5. dontfree and buffer ownership — zero-copy core
- `dontfree=0` ⇒ PKO frees each segment to the pool named in its buf_ptr `pool` field after transmit; `back` recovers the buffer start (packet.h:48-63).
- Per-buffer `i` bit inverts the decision; `ignore_i` overrides (pko.h:227-236).
- **Zero-copy forwarding**: RX buffers arrive with i=0; hand the WQE's packet_ptr chain to PKO with dontfree=0 ⇒ hardware transmits AND frees the RX buffer back to the packet pool. No CPU copy, no CPU free.

## 6. Init/teardown order
Global init `cvmx_pko_initialize_global` (pko.c:185-230): program PKO_REG_CMD_BUF; reset all queues to ILLEGAL_PID=63 (pko.c:158-177, pko.h:78). Enable `cvmx_pko_enable` (pko.c:236-253): `PKO_REG_FLAGS.{ena_dwb,ena_pko,store_be}=1`.
**Runtime order** (helper.c:974-994): `cvmx_ipd_enable()` first → per-interface enable → **`cvmx_pko_enable()` LAST** ("at this point IPD/PIP must be fully functional and PKO must be disabled").
Teardown `cvmx_pko_shutdown` (pko.c:281-307): disable; per-queue PKO_MEM_QUEUE_PTRS ← illegal/empty + `cvmx_cmd_queue_shutdown`; `PKO_REG_FLAGS.reset`.

## 7. TX stats
`cvmx_pko_get_port_status(port, clear, *status)` (pko.h:576-614) → `{u32 packets; u64 octets; u64 doorbell}` (pko.h:117-121). Index via `PKO_REG_READ_IDX`; packets from `PKO_MEM_COUNT0` (32-bit, pko-defs.h:89-100), octets from `PKO_MEM_COUNT1` (48-bit, 102-113); outstanding doorbell per base queue from `PKO_MEM_DEBUG8.cn50xx.doorbell` / DEBUG9 on CN3XXX (pko.h:601-613 — verify which model mask CN50XX hits if this counter matters).

## Flags
1. pko.h:36-41 locking comment typo (§3).
2. Only L4 checksum insertion documented (§4).
3. 1 queue/port is build config, not silicon limit — silicon supports 32 queues total (§2). Raising `CVMX_PKO_QUEUES_PER_PORT_*` is the path to per-port QoS queues (what EdgeOS's `cvm_oct_transmit_qos` exploits).
4. DEBUG8 vs DEBUG9 model-mask question (§7).
