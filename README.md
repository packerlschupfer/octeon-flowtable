# octeon-flowtable

A clean-room **nftables flow-offload backend** for the Cavium **Octeon+ (CN50xx)**
packet complex — the SoC in the Ubiquiti EdgeRouter Lite 3 (ERLite-3). It gives an
ERLite-3 running mainline OpenWrt hardware-accelerated NAT/routing that **matches
the proprietary EdgeOS offload**, using only GPL kernel sources and the public
OCTEON hardware reference manual — **no Ubiquiti binary, no Cavium SDK**.

A WQE-level RX hook intercepts forwarded packets *before* any skb is built,
rewrites L2/L3/L4 in the FPA packet buffer (NAT, next-hop MAC, TTL/hop-limit, VLAN
tags) with incremental checksums, and transmits via PKO — no skb, no Linux stack.
Misses fall through to normal forwarding.

## Results (on a real ERLite-3, CN5020, 2×500 MHz)

| metric | OpenWrt software | **this driver** | EdgeOS vendor |
|---|---|---|---|
| single-flow TCP NAT | 764 Mbps | **932 Mbps** | 940 Mbps |
| multi-flow TCP NAT | 941 Mbps | **935 Mbps** | 563 Mbps |
| UDP-64 NAT (pps) | 73k | **431k** | 247–431k |
| RTT under load | 6.1 ms | **2.1 ms** | 2.6 ms |

5.9× the software baseline on small-packet pps, 3× better latency — matching the
vendor it reverse-engineered.

## What it accelerates

IPv4 + IPv6, NAT + routing, untagged + **802.1Q VLAN** (retag / pop / push) +
**QinQ** (two tags) — all hardware-verified. Plus a global hardware FAU counter
and a **PKO tail-drop AQM** that cuts bufferbloat 54 ms → 4 ms at equal
throughput. See [`docs/PROJECT-OUTCOME.md`](docs/PROJECT-OUTCOME.md) and
[`docs/FUTURE-WORK.md`](docs/FUTURE-WORK.md) for the full matrix.

## Layout

```
docs/      design RFC, hardware model, behavioural spec, build + install guides, outcome
src/octeon_flowtable/     the kernel module
src/octeon_flowtable/tests/  regression suite + nft/topology rigs (see Testing)
src/staging-patches/      the octeon_ethernet hook patch (adds the exported hooks)
package/octeon-flowtable/ OpenWrt kmod package (init script + UCI config)
prompts/   reproducible runbooks (build / deploy / EdgeOS→OpenWrt config migration)
```

## Quick start

1. Apply `src/staging-patches/120-octeon-flowtable-hooks.patch` to your OpenWrt
   octeon kernel (`target/linux/octeon/patches-6.18/`) and rebuild.
2. Build the module: `cd src/octeon_flowtable && make octeon OWRT=/path/to/openwrt`
   (or select **kmod-octeon-flowtable** from `package/` in menuconfig).
3. Enable hardware offload in `/etc/config/firewall`:
   `option flow_offloading '1'` + `option flow_offloading_hw '1'`, then `fw4 reload`.
4. Verify: `conntrack -L | grep HW_OFFLOAD`.

Building from source: [`docs/BUILDING.md`](docs/BUILDING.md). Installing,
enabling, and tuning: [`docs/INSTALLING.md`](docs/INSTALLING.md). Reproducible
runbooks: [`prompts/`](prompts/).

## Prebuilt image

Don't want to build the toolchain yourself? Tagged releases ship ready-to-flash
EdgeRouter Lite images on the [Releases](../../releases) page, built by CI straight
from this repo (GitHub Actions → [`build-image.yml`](.github/workflows/build-image.yml)).
The kernel already carries the staging hook patch and the offload tuning
(CVMSEG=2, `receive_group_order=1`); `kmod-octeon-flowtable` is preinstalled.

Two variants are published (asset names prefixed `lean-` / `router-`):

- **lean** — offload + `conntrack`/`tcpdump`; a clean base to add your own packages to.
- **router** — lean + LuCI web UI + WireGuard, for a more turnkey home router.

The build is fully pinned (OpenWrt main @ `84f4f77`, kernel 6.18.34, pinned
`packages`/`luci` feeds) so a given tag reproduces byte-for-byte — see [`ci/`](ci/).

> ⚠ **Unofficial community image for EOL hardware — flash at your own risk.** The
> ERLite installs via USB stick / U-Boot, not web sysupgrade; recovery over
> U-Boot/TFTP is in [`docs/INSTALLING.md`](docs/INSTALLING.md). Verify
> `sha256sums.txt` before flashing.

To cut a release: push a `v*` tag (`git tag v0.1.0 && git push origin v0.1.0`).
`workflow_dispatch` builds the image without releasing (artifacts only).

## Testing

[`src/octeon_flowtable/tests/regression.sh`](src/octeon_flowtable/tests/regression.sh)
is a 14-check regression suite run from a bench host against a DUT router
(~8 min, exit code = failure count). It covers both flowtable VLAN encodings
(subinterface-implicit and bridged-LAN lower-device with explicit
`VLAN_PUSH`/VLAN-agnostic match), TCP **and** UDP in both directions at line
rate, kill-`-9` tuple reuse with stale-entry eviction, fw4 reload churn
(duplicate-entry leak check), orphan GC, link-flap recovery, and idle settle.
One check is worth stealing for any offload driver: it **proves fast-path
engagement from the driver's `tx_ok` packet counter delta** rather than
inferring it from throughput — on an idle router the software path also hits
line rate, which can silently turn an offload test into a no-op. Run it after
any module or staging-patch change before deploying.

The suite's preflight self-heals its topology (a `wan` netns endpoint and a
tagged client subif). The remaining `tests/*.nft` files and `*-rig.sh` scripts
are the standalone topology rigs used during VLAN/QinQ/IPv6 bring-up.

## Status & caveats

Tested on the [ERLite-3](https://openwrt.org/toh/ubiquiti/edgerouter_lite) / CN5020
against OpenWrt's kernel 6.18.34 (see that page for the device's hardware specs,
serial-console pinout, and the stock OpenWrt install/recovery procedure). The
CN50xx is a ~2008-era Octeon Plus part, long since discontinued; this exists
because it's a tractable hardware-fast-path target on cheap silicon, not because
the world needs another router stack. Hardware ceilings (PIP
parses ≤ 2 VLAN tags; the flowtable offloads TCP/UDP/GRE only; v6 extension
headers go slow-path) are documented in `docs/FUTURE-WORK.md`. VPN: transit
WireGuard/IPsec-NAT-T is already offloaded (it's UDP); VPN *termination* is crypto,
a separate problem — see `docs/COP2-CRYPTO-SCOPING.md`.

## Community

Questions, hardware quirks, or just want to compare notes on Octeon NPU
programming? If you'd rather not open a GitHub issue for every little thing,
you're welcome to drop into the Discord:

**[discord.gg/vbNaQRQ4cs](https://discord.gg/vbNaQRQ4cs)**

No pressure either way — issues and PRs are equally welcome. There's also a
[project wiki](../../wiki) for longer-form notes.

## License

GPL-2.0 (it's a Linux kernel module). See [`LICENSE`](LICENSE).
