# Contributing

Issues and PRs are welcome — bug reports, hardware quirks on other CN50xx boards,
new flow classes, or just questions. There's also a
[Discord](https://discord.gg/vbNaQRQ4cs) if you'd rather chat.

## The one hard rule: stay clean-room

This is a **clean-room** reimplementation. Do **not** contribute anything derived
from Ubiquiti's binary `cavium_ip_offload.ko` or from the license-restricted
Cavium SDK — no code, struct layouts, register encodings, or constants taken from
either. Hardware facts must come from:

- the **GPL kernel headers** in `arch/mips/include/asm/octeon/`, or
- the **public OCTEON hardware reference manual** (cite it),

and **black-box behavioural observation** of the vendor (interface/behaviour only,
no disassembly) — see `docs/offload-behavior-spec.md` and `docs/integration-gap.md`
for the kind of observation that's acceptable. If in doubt, ask first.

## Build & test

- Build: `src/octeon_flowtable/README.md` (needs the staging hook patch in the
  kernel — `src/staging-patches/`).
- The kernel build / reflash and the on-hardware test rigs are in `prompts/` and
  `src/octeon_flowtable/tests/`.
- Please note which hardware you tested on (board + OpenWrt/kernel version) in the
  PR — this is EOL silicon and behaviour can vary by board.

## Style

Match the surrounding kernel style (the module is written to kernel norms:
`checkpatch`-clean-ish, comments explaining the *why* and the hardware reasoning).
Keep clean-room provenance notes in comments where a non-obvious hardware fact
comes from a specific GPL header or the HRM.
