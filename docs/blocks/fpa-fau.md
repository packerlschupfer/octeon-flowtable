# FPA & FAU Catalog — Cavium Octeon+ CN5020 (CN50XX)

*Extracted 2026-06-04. Files under `arch/mips/include/asm/octeon/`: cvmx-fpa.h, cvmx-fpa-defs.h, cvmx-fau.h, cvmx-asm.h, cvmx.h, cvmx-config.h; staging files under `drivers/staging/octeon/`.*

> **Build-config note**: on real octeon kernel builds the pool/FAU constants come from
> `cvmx-config.h` (included at octeon-ethernet.h:22). `octeon-stubs.h` is the x86
> COMPILE_TEST fallback only — its values (e.g. pool size "16") are simulator
> placeholders. Authoritative sizes: **packet pool 0 = 16 cache lines = 2048 B,
> WQE pool 1 = 1 line = 128 B, PKO command pool 2 = 8 lines = 1024 B**
> (cvmx-config.h:24-26,33-42).

## 1. FPA — Free Pool Allocator

### 1.1 Pool model
- **8 pools** (`CVMX_FPA_NUM_POOLS 8`, cvmx-fpa.h:44); per-pool register arrays confirm (fpa-defs.h:34-63, int bits 262-285).
- A pool = hardware LIFO free-list of fixed-size blocks in DRAM. SW bookkeeping: `cvmx_fpa_pool_info_t {name,size,base,starting_element_count}` (cvmx-fpa.h:81-90) in `cvmx_fpa_pool_info[]` (96). Accessors: get_name(106)/get_base(117)/is_member(131)/get_block_size(286).
- Per-pool HW block size in `FPFx_SIZE` registers (11-bit pools 1-8, fpa-defs.h:170-181; 12-bit pool 0, 198-209), unit = cache lines.

### 1.2 Operations
- **Sync alloc** `cvmx_fpa_alloc(pool)` (cvmx-fpa.h:185-193): CSR read of FPA DID address pops a block; NULL on exhaustion.
- **Async alloc** `cvmx_fpa_async_alloc(scr_addr, pool)` (202-215): IOBDMA, result later in CVMSEG scratch (8-byte aligned scr_addr).
- **Free** `cvmx_fpa_free(ptr, pool, num_cache_lines)` (248-264): `CVMX_SYNCWS` then IO write; the written value = cache lines NOT to write back (235,262). `cvmx_fpa_free_nosync` (226-237): compiler barrier only — use only for unmodified buffers.
- **Enable** `cvmx_fpa_enable()` (144-177) after CSR config, before any op. `cvmx_fpa_shutdown_pool(pool)` (277) drains/validates.

### 1.3 Constraints
`CVMX_FPA_MIN_BLOCK_SIZE 128`, `CVMX_FPA_ALIGNMENT 128` (cvmx-fpa.h:45-46): all buffers 128-byte aligned, multiple of 128. Staging driver over-allocates +256 and aligns, stashing the kmalloc ptr below the block (ethernet-mem.c:83-102); skbuff pool reserves `256 - (data & 0x7f)` (ethernet-mem.c:33). Sizes always expressed as size/128 cache lines (ethernet-mem.c:35, ethernet.c:187).

### 1.4 Pool roles & filling (staging driver)
| Pool | # | Size | Role | Filled by |
|---|---|---|---|---|
| PACKET_POOL | 0 | 2048 B | RX packet data — **skbuff-backed**: the FPA buffer IS an skb's data area, skb ptr stashed at fpa_head−8 | `cvm_oct_fill_hw_skbuff` (ethernet-mem.c:24-39) |
| WQE_POOL | 1 | 128 B | work queue entries | `cvm_oct_fill_hw_memory` (77-106) |
| OUTPUT_BUFFER_POOL | 2 | 1024 B | PKO command rings | same |

Init `cvm_oct_configure_common_hw()` (ethernet.c:141-165): packet pool & WQE pool filled with `num_packet_buffers` (default 1024, ethernet.c:33); output pool 1024 buffers. RED setup `cvmx_helper_setup_red(num/4, num/8)` (ethernet.c:164). WQE recycling: `cvm_oct_free_work()` frees segments to their `segment_ptr.s.pool` + WQE to pool 1 (ethernet.c:185-190).

### 1.5 Exhaustion / monitoring
- Occupancy: `CVMX_FPA_QUEX_AVAILABLE(pool)` (fpa-defs.h:62).
- Errors: `CVMX_FPA_INT_SUM/INT_ENB` — per-queue `qN_und` (alloc on empty), `qN_coff`, `qN_perr`, ECC (fpa-defs.h:246-305). Sync alloc returns NULL on empty (cvmx-fpa.h:189-192).
- `FPFx_MARKS` rd/wr watermarks are the FPA's internal on-chip cache spill/refill levels, not RED (fpa-defs.h:155-196).
- RED pass/drop thresholds vs free-buffer count throttle IPD when the packet pool runs low; exact register programming lives in cvmx-helper-util (not fully present in sparse tree) — flag for verification.

## 2. Hardware-managed recycling (the CPU-free loop)

- **IPD allocates** packet buffers (pool 0) + WQEs (pool 1) autonomously on packet arrival.
- **PKO frees** packet buffers to their pool on transmit when `dontfree=0`.
- Steady-state forwarding therefore needs **zero CPU buffer management**; the CPU only maintains FAU counters. The staging driver's `REUSE_SKBUFFS_WITHOUT_FREE` TX path exploits this by donating skb data buffers into pool 0 (ethernet-tx.c:296-357), falling back to core-free when `buffers_to_free < -100` (ethernet-tx.c:393-394).

## 3. FAU — Fetch-and-Add Unit

### 3.1 Model
Atomic-counter unit on the IOB, one IO address `CVMX_FAU_LOAD_IO_ADDRESS` (cvmx-fau.h:39). **2048-byte register file** (`CVMX_FAU_BITS_REGISTER = 10,0`, cvmx-fau.h:46; "0 <= reg < 2048" throughout, e.g. 123,143,164). Byte-addressed; step 2/4/8 by width.

### 3.2 Operations
- Sizes 8/16/32/64 (`cvmx_fau_op_size_t`, cvmx-fau.h:48-53).
- `cvmx_fau_fetch_and_add{8..64}` — returns pre-update value (170-218).
- `cvmx_fau_atomic_add{8..64}` — add without fetch (526-567).
- `cvmx_fau_atomic_write{8..64}` — overwrite via `noadd=1` bit (44, 121-134, 576-617). **No compare-and-swap exists.**
- Tag-wait variants `cvmx_fau_tagwait_fetch_and_add*` — apply only after the core's pending tag switch completes; return `{error:1, value}` (55-93, 232-313).
- Add value limited to **22 bits** (`CVMX_FAU_BITS_INEVAL = 35,14`; cvmx-fau.h:42,148-149).
- Endian swizzle for sub-64-bit indices (108-116).
- Async (IOBDMA) variants deliver the pre-update value to CVMSEG scratch (337-517).
- No latency figures in headers.

### 3.3 FAU allocation map in the staging driver
FAU space carved **top-down** from 2048:
- `FAU_TOTAL_TX_TO_CLEAN` = 2044 (ethernet-defines.h:35) — TX cleanup trigger counter (ethernet-tx.c:402-404, 474-480).
- `FAU_NUM_PACKET_BUFFERS_TO_FREE` = 2040 (ethernet-defines.h:36) — buffers PKO owes back (ethernet-tx.c:177-179, 383, 393, 447; init ethernet.c:761).
- **Per-port per-qos blocks**: `priv->fau = fau - num_queues*4` descending (ethernet.c:677, 832, 905-907); counter `priv->fau + qos*4` = skbs in flight on that queue. TX fetch-adds `MAX_SKB_TO_FREE` to learn how many to free (ethernet-tx.c:85-88, 380-387). Passed to PKO as `reg0`/`size0=32` so **PKO decrements the counter on TX completion in hardware** (ethernet-tx.c:254,398).

## 4. CVMSEG / IOBDMA

IOBDMA = non-blocking IO load: `cvmx_send_single()` store to magic address `0xffffffffffffa200` (cvmx.h:302-306); the unit (FPA/FAU/POW) deposits the response into the core's **CVMSEG** scratchpad at `scraddr`; read after `CVMX_SYNCIOBDMA` (cvmx-asm.h:44-47,75). CVMSEG size set by `CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE` (cache lines); all async paths gated: `USE_ASYNC_IOBDMA = (CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE > 0)` (ethernet-defines.h:30). `CVMX_SYNCWS` = double `syncw` (CN3xxx errata Core-401 workaround, cvmx-asm.h:56-64).

⚠ Check OpenWrt octeon kernel config for `CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE` — if 0, the driver runs all-synchronous (slower). Worth verifying on the bench unit.

## Flags
1. RED register programming not fully visible in sparse tree (§1.5).
2. cvmx-fpa.c executive body absent in sparse checkout — shutdown_pool/get_block_size bodies unseen.
3. No FAU latency claims in headers — HRM-only fact, do without.
4. Threshold registers (`fpa_packet_threshold` etc., fpa-defs.h:1074+) — CN50XX applicability not asserted by header.
