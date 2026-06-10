# Scoping: a clean-room Octeon CN50xx COP2 crypto driver (for IPsec)

*2026-06-04. SCOPING ONLY — not started. A possible **separate** clean-room
project, distinct from `octeon_flowtable` (different hardware block, different
kernel subsystem). Written so a future effort can start from facts, not vibes.*

## Why this is a different project from the flow offload

The flow offload (`octeon_flowtable`) accelerates **plaintext L3/L4 forwarding** —
PIP/POW/PKO/FPA, NAT/VLAN rewrite. VPN *termination* is **crypto**: the box
en/decrypts. That bottleneck is the cipher, not the forwarding, and it lives in a
completely different block — the **per-core COP2 crypto coprocessor** — reached
through the Linux **crypto API + XFRM (IPsec)**, not the flowtable. None of the
flow-offload code, hooks, or staging patch is reused. (Transit VPN — forwarding
encrypted WireGuard/IPsec-NAT-T UDP — is already offloaded *as UDP* by
`octeon_flowtable`; native ESP isn't, because the Linux flowtable is TCP/UDP/GRE
only. See `docs/FUTURE-WORK.md`.)

## The hardware (CN5020)

Each of the two cn50xx cores has a **COP2 crypto unit** (MIPS coprocessor 2),
driven by synchronous `dmtc2`/`dmfc2` instructions to crypto registers:

- **Hashes**: MD5, SHA-1, SHA-256, SHA-512 (the IPsec *auth* / HMAC half).
- **Ciphers**: AES-128/192/256, DES/3DES, and a GF(2^128) multiply for
  AES-GCM/GMAC (the IPsec *encryption* half — the bulk cost).
- **CRC** (not crypto, but same unit).

It is **synchronous and per-core** — not a queued engine like PKO or the newer
Nitrox. The core *is* the crypto: an `aes` instruction runs in the pipeline
(~1 cycle/round-ish), so there is no async submit/complete model. Software AES on
these cores is ~10–20 cycles/byte; COP2 AES is a small multiple of memory
bandwidth → a realistic **3–5× IPsec throughput** win, but still CPU-bound
(2 cores, ~500 MHz) — order ~300–600 Mbps AES-GCM, not line rate.

## What already exists upstream (and what's missing)

- **Hashes — DONE upstream.** `arch/mips/cavium-octeon/crypto/octeon-{md5,sha1,
  sha256,sha512}.c` register `ahash` transforms backed by the COP2 hash
  instructions, using `asm/octeon/crypto.h` (`octeon_crypto_enable/disable` +
  the `write_octeon_64bit_hw*` register macros). **Not enabled** in this OpenWrt
  build (`CONFIG_CRYPTO_*_OCTEON` all unset) — step 0 is simply turning them on;
  that already accelerates HMAC-SHA auth.
- **Ciphers — MISSING.** There is **no** in-tree COP2 AES/3DES/GCM driver, and
  `crypto.h` only defines the *hash* register macros — **not** the cipher ones.
  This is the real work, and the clean-room crux below.

## Clean-room feasibility (the crux)

This project's rule: GPL sources only, no Cavium SDK, no UBNT binary.

- **Hashes**: trivially clean — the GPL kernel already has them. The hash COP2
  path is raw register access (`crypto.h`: `dmtc2 rt, 0x0048+index` etc.), no
  named cipher macros involved.
- **Ciphers**: the COP2 AES/3DES/GMUL register indices and `dmtc2` function codes
  are **not** in any GPL source on hand. **Confirmed** by a system-wide search
  (the GPL `cvmx-asm.h`/`crypto.h`, the reference kernel tree, the OpenWrt build):
  zero AES/3DES COP2 macros — only the hash registers are exposed. The SDK's
  `cvmx-asm.h` had `CVMX_MT_AES_*` / `CVMX_MF_AES_*` / `CVMX_MT_3DES_*`, but that's
  SDK. **This is the gate for the whole project**: without the cipher encodings,
  only the auth (HMAC-SHA) half of IPsec can be accelerated, and the cipher (the
  bulk cost) stays software. The encodings **are** in the public
  **OCTEON Programmer's Reference / HW Reference Manual** (COP2 instruction
  encodings) — the same class of public doc this project already used for the
  PIP/POW/PKO blocks. So: feasible, but the cipher instruction encodings must be
  re-derived from the public HRM, not copied from the SDK. Budget real time for
  getting the register indices/byte-order exactly right (debugging a wrong AES
  round is painful — same flavour as the big-endian mangle bug here).

## Integration shape

- Register Linux crypto transforms backed by COP2:
  `skcipher` (cbc(aes), ecb(aes)), `aead` (gcm(aes), the rfc4106 wrapper IPsec
  uses), and lean on the existing `ahash` (hmac via the upstream hash drivers).
- IPsec (XFRM) then uses these via the crypto API automatically — no XFRM device
  offload (`xdo_dev_*`) needed; this is *crypto* offload, not *packet* offload.
  (The octeon_ethernet driver has no `xdo_dev_*` and doesn't need them.)
- Per-op: `octeon_crypto_enable()` (saves the caller's COP2 state, claims the
  unit, **disables preemption/bh**), run the cipher instructions over the buffer,
  `octeon_crypto_disable()` (restores). Because the unit is per-core and the ops
  hold the core, batch per request and keep the critical section tight.

## Challenges / risks

1. **Cipher instruction encoding from the HRM** (the clean-room cost above).
2. **COP2 context**: state is per-core and saved/restored on the CU2 exception;
   the enable/disable bracket + no-preempt is mandatory, and getting it wrong
   corrupts another task's crypto or oopses. The upstream hash drivers are the
   reference pattern.
3. **Sync model = no overlap**: the core blocks during crypto, so max throughput
   is `cores × cipher-rate`, and it competes with the forwarding cores. Pin/affine
   thoughtfully; measure against software to prove the win is real.
4. **Scope creep**: do AES-GCM (modern IPsec) first; 3DES/DES is legacy.

## Milestones

0. Enable the upstream COP2 **hash** drivers; measure HMAC-SHA speedup. (Hours.)
1. Clean-room **AES-CBC** `skcipher` from the HRM; KAT-verify vs the generic
   driver (`tcrypt`); wire one cn50xx core. (The hard, encoding-sensitive step.)
2. **AES-GCM** `aead` (incl. the GF multiply) + the rfc4106 IPsec wrapper.
3. IPsec end-to-end: a tunnel up, throughput vs software, both cores.
4. Package as a kmod (mirrors `package/octeon-flowtable`).

## Honest assessment — and why not now

- **Value is real but narrow**: it accelerates **IPsec termination only**.
  **WireGuard cannot be accelerated on this silicon at all** — CN50xx (≈2008) has
  no ChaCha20/Poly1305 hardware; that crypto is software-only forever here.
- **No current consumer**: the gw doesn't terminate (or, now, even forward) any
  VPN. Like QinQ, this would be speculative.
- **EOL silicon, modest ceiling**: even done well, ~300–600 Mbps AES-GCM on two
  500 MHz cores.
- It's a **genuinely interesting clean-room exercise** (COP2 coprocessor
  programming from the public HRM, crypto-API integration) and a natural sibling
  to the flow-offload work — but it's a *new* project, not a continuation, and
  there's no use case driving it today. Start it only if a real IPsec-termination
  need appears on the box.
