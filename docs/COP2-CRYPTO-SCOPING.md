# Scoping: a clean-room Octeon CN50xx COP2 crypto driver (for IPsec)

*2026-06-04. SCOPING ONLY — not started. A possible **separate** clean-room
project, distinct from `octeon_flowtable` (different hardware block, different
kernel subsystem). Written so a future effort can start from facts, not vibes.*

*Updated 2026-11-21: the COP2 cipher encodings are published in a BSD-licensed
header (Oracle UEK7), and a user-space OpenSSL provider is a second route; see
"Clean-room feasibility" and "User-space route".*

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

This project's rule: openly published kernel sources only (GPL kernel trees, including
the BSD-licensed Cavium `cvmx-*.h` headers they carry), no Cavium SDK, no UBNT binary.

- **Hashes**: trivially clean — the GPL kernel already has them. The hash COP2
  path is raw register access (`crypto.h`: `dmtc2 rt, 0x0048+index` etc.), no
  named cipher macros involved.
- **Ciphers**: the COP2 AES/3DES/GMUL register indices and `dmtc2`/`dmfc2`
  function codes are **not** in mainline's `cvmx-asm.h` (it carries only the hash
  registers; `crypto.h` the same). **Correction (2026-11-21):** they *are*
  published outside the SDK. Oracle's UEK7 kernel tree
  (`arch/mips/include/asm/octeon/cvmx-asm.h`, branch `uek7/u3`) ships the full
  header with `CVMX_MT_AES_*` / `CVMX_MF_AES_*`, `CVMX_MT_3DES_*` and the GF(2^128)
  `CVMX_MT_GFM_*` / `CVMX_MF_GFM_*` macros. The header is Cavium's BSD-style licence, the same licence and origin
  as the `cvmx-*.h` headers in mainline that `octeon_flowtable` already builds
  against, and compatible with a GPLv2 kernel module. So the encodings come from
  an openly published, permissively licensed header, not from the SDK, and need no
  re-derivation from the hardware manual. What stays hard is using them correctly
  (key/IV register layout, byte order, the CBC/GCM chaining) and verifying against
  known-answer tests. (Pointer from a community member on the project's Discord.)
- **Not usable as code**: wolfSSL's `wolfcrypt/src/port/cavium/cavium_octeon_sync.c`
  implements COP2 3DES/AES-CBC/AES-GCM, but it builds on the SDK headers and is
  GPLv3, so it can't go into a GPLv2 kernel driver. Useful only as confirmation of
  which operations the unit supports.

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

1. **Correct use of the cipher registers**: the encodings are available (see
   above), but key/IV layout, byte order and CBC/GCM chaining must be right;
   verify against known-answer tests before anything else.
2. **COP2 context**: state is per-core and saved/restored on the CU2 exception;
   the enable/disable bracket + no-preempt is mandatory, and getting it wrong
   corrupts another task's crypto or oopses. The upstream hash drivers are the
   reference pattern.
3. **Sync model = no overlap**: the core blocks during crypto, so max throughput
   is `cores × cipher-rate`, and it competes with the forwarding cores. Pin/affine
   thoughtfully; measure against software to prove the win is real.
4. **Scope creep**: do AES-GCM (modern IPsec) first; 3DES/DES is legacy.

## User-space route (OpenSSL provider)

Mainline Linux already lets ordinary processes use COP2 on Octeon: on a task's
first COP2 instruction the CU2 exception handler (`cnmips_cu2_call` in
`arch/mips/cavium-octeon/cpu.c`) enables the unit for that task and restores its
saved state, and the context switch saves and restores COP2 state from then on.
So an **OpenSSL 3 provider** using the COP2 AES/GCM (and hash) instructions needs
no kernel changes. It would speed up everything that does crypto in user space
through OpenSSL: OpenVPN, TLS servers and clients, strongSwan's IKE. It does
**not** help kernel IPsec (ESP runs in the kernel crypto API) or WireGuard.
AF_ALG and cryptodev, the older ways to reach kernel crypto from user space, add
a syscall per operation and aren't worth it here.

Of the two routes, the provider is the smaller first step (no kernel code, easy
to test against OpenSSL's own known-answer tests); the kernel driver below is
the one that serves IPsec.

## Milestones

0. Enable the upstream COP2 **hash** drivers; measure HMAC-SHA speedup. (Hours.)
1. **AES-CBC** `skcipher` using the UEK7 `cvmx-asm.h` encodings; KAT-verify vs the generic
   driver (`tcrypt`); wire one cn50xx core. (The hard, encoding-sensitive step.)
2. **AES-GCM** `aead` (incl. the GF multiply) + the rfc4106 IPsec wrapper.
3. IPsec end-to-end: a tunnel up, throughput vs software, both cores.
4. Package as a kmod (mirrors `package/octeon-flowtable`).

## Honest assessment — and why not now

- **Value is real but narrow**: the kernel driver accelerates **IPsec termination**;
  the user-space provider accelerates OpenSSL users (OpenVPN, TLS).
  **WireGuard cannot be accelerated on this silicon at all** — CN50xx (≈2008) has
  no ChaCha20/Poly1305 hardware; that crypto is software-only forever here.
- **No current consumer**: the gw terminates only WireGuard, which COP2 can't
  help, and runs no IPsec or OpenVPN. Like QinQ, this would be speculative.
- **EOL silicon, modest ceiling**: even done well, ~300–600 Mbps AES-GCM on two
  500 MHz cores.
- It's a **genuinely interesting clean-room exercise** (COP2 coprocessor
  programming from the published register encodings, crypto-API integration) and a natural sibling
  to the flow-offload work — but it's a *new* project, not a continuation, and
  there's no use case driving it today. Start it only if a real IPsec-termination
  need, or an OpenVPN/TLS throughput need, appears on the box.
