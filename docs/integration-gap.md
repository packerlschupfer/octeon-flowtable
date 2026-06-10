# Integration gap: what `cavium-ip-offload.ko` consumes vs. what mainline provides

*2026-06-03. Method: `readelf -W --syms` undefined-symbol list of the EdgeOS module
(interface facts only — no disassembly, no code inspection), cross-referenced against
mainline Linux master (mainline Linux, commit `ba3e43a9e`). This is the "diff between
the 676-symbol kallsyms menu and mainline" deliverable named in the brief — with a
surprise ending.*

## Headline finding

The briefing's wall #2 assumed the offload module consumes a large slice of the ~676
`cvmx_*`/`octeon_*` symbols the EdgeOS kernel exports. **It doesn't.** The module has
only **61 undefined symbols total**, of which only **17 are Octeon/UBNT-specific**.
The remaining 44 are bog-standard kernel API (`kmalloc`, `printk`, seq_file, procfs,
spinlocks, `dev_queue_xmit`, …).

Why so few: the Cavium SDK implements most `cvmx_*` operations as `static inline`
MMIO accessors in headers, so they compile *into* the module and never appear as
imports. Mainline carries GPL versions of those same headers in
`arch/mips/include/asm/octeon/` — so that entire surface is already covered, clean.

## The 17 Octeon/UBNT-specific imports, classified

### A. Already in mainline AND exported — zero work (5)

| symbol | where | export |
|---|---|---|
| `cvmx_helper_get_ipd_port` | `arch/mips/cavium-octeon/executive/cvmx-helper-util.c` | `EXPORT_SYMBOL_GPL` |
| `cvmx_helper_get_number_of_interfaces` | `executive/cvmx-helper.c` | `EXPORT_SYMBOL_GPL` |
| `cvmx_helper_interface_get_mode` | `executive/cvmx-helper.c` | `EXPORT_SYMBOL_GPL` |
| `cvmx_helper_ports_on_interface` | `executive/cvmx-helper.c` | `EXPORT_SYMBOL_GPL` |
| `octeon_get_clock_rate` | `arch/mips/cavium-octeon/csrc-octeon.c` | `EXPORT_SYMBOL` |

### B. In mainline, not exported — one-line patches (2)

| symbol | where |
|---|---|
| `cvmx_pko_get_base_queue` | `arch/mips/cavium-octeon/executive/cvmx-pko.c` |
| `cvmx_pko_get_num_queues` | `executive/cvmx-pko.c` |

### C. The real gap — UBNT/SDK patches to the ethernet driver (5)

These are the genuine integration points the SDK/EdgeOS `octeon-ethernet` driver
provides and mainline staging does not:

| symbol | inferred role (from name + signature context only) |
|---|---|
| `cvm_oct_register_rx_callback` | register an RX intercept at the work-queue-entry (WQE) level, before the skb/netif path |
| `cvm_ipfwd_rx_hook` | the fastpath RX hook variable the driver calls into |
| `cvm_ipfwd_tx_hook` | TX-side counterpart |
| `cvm_oct_transmit_qos` | transmit a raw WQE on a chosen PKO queue. **Mainline staging still declares this in `drivers/staging/octeon/ethernet-tx.h:10`** — a leftover prototype from the old out-of-tree consumers; the definition is gone. Re-adding it is reconstruction of a known-good mainline interface, not SDK copying. |
| `cvm_oct_transmit_qos_not_free` | same, without freeing the WQE/buffer |

Related good news: mainline staging still `EXPORT_SYMBOL(cvm_oct_free_work)`
(`drivers/staging/octeon/ethernet.c:194`) — the driver was *designed* to have
external WQE consumers.

### D. Ignorable (5)

| symbol | why ignorable |
|---|---|
| `octeon_feature_map` | SDK feature bitmap; mainline equivalent is the `octeon_has_feature()` inline — trivial substitution |
| `cvmx_helper_get_pknd` | port-kind is an Octeon II (CN68xx) concept; CN5020 predates it — compat shim, dead on our silicon |
| `is_app_int`, `rx_dpi_1`, `rx_dpi_2`, `tx_dpi` | Trend Micro DPI (`tdts.ko`) integration hooks — out of scope for IP offload |

## What this means for the project

1. **Wall #2 is ~5 symbols tall, not 676.** The work is: patch the staging driver
   with (a) a WQE-level RX callback/hook and (b) a WQE-level QoS transmit function
   (whose prototype mainline already carries), plus two `EXPORT_SYMBOL` lines for the
   PKO queue getters. Everything else the module needs is either standard kernel API
   or inlined GPL headers.

2. **Architectural inference** (from the interface alone — to be *validated*, not
   assumed, by the Phase 2 black-box spec): the EdgeOS offload is a **CPU-driven
   software fastpath**, not autonomous hardware forwarding. The module intercepts
   WQEs at RX, forwards via its own flow state, and transmits directly via PKO —
   skipping the Linux network stack, but the CPU still touches every packet. Imports
   consistent with this: per-cpu data (`__per_cpu_offset`, `__cpu_online_mask`),
   spinlocks, procfs/seq_file control plane, `dev_queue_xmit` (slow-path punt),
   `pskb_expand_head` (skb interop).
   *Consequence for Phase 4 step 3: the "no CPU touches the packet" ambition likely
   exceeds what EdgeOS itself does. Parity does not require it.*

3. **This raises the odds that Phase 0's early exit fires.** If EdgeOS's offload is
   "skip the stack, keep the CPU", then nftables flowtable software offload is the
   same idea minus Octeon zero-copy WQE handling. The gap to parity may be smaller
   than the briefing's framing suggests. Measure before building.

## Caveats

- Symbol *names* and counts are interface facts; the role descriptions in §C are
  inferences from naming and the mainline leftover prototype. Phase 2 must
  characterize actual behavior black-box before any of this hardens into spec.
- The EdgeOS kernel exports ~750 `cvmx_`/`octeon_`/`cavium_` symbols (from its
  `kallsyms`). Either way, exported ≠ consumed.
