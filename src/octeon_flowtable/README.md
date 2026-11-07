# octeon_flowtable.ko

Clean-room nftables flow-offload backend for the Cavium Octeon+ (CN50xx) packet
complex, built on the mainline staging `octeon_ethernet` driver. Design:
`../../docs/design-rfc-octeon-flowtable.md`; how-it-works and results:
`../../docs/PROJECT-OUTCOME.md`.

A WQE-level RX hook intercepts forwarded packets before any skb is built, rewrites
L2/L3/L4 in the FPA buffer (NAT, next-hop MAC, TTL/hop-limit, VLAN retag/pop/push,
QinQ) with incremental checksums, and transmits via PKO — no skb, no stack.
Misses fall through to normal Linux forwarding. Handles IPv4 + IPv6, NAT +
routing, untagged + 802.1Q + QinQ.

## Prerequisite: the staging hook patch

The kernel must carry `../staging-patches/120-octeon-flowtable-hooks.patch`, which
adds the exported hook/transmit/port/AQM symbols to the in-tree (built-in)
`octeon_ethernet` driver. Drop it into `target/linux/octeon/patches-6.18/` and
rebuild the kernel. (It applies clean against pristine 6.18.34.)

## Build

```sh
# generic, against any prepared kernel tree:
make KDIR=/path/to/linux ARCH=mips CROSS_COMPILE=mips64-...-

# convenience, against an OpenWrt octeon build tree:
make octeon OWRT=/path/to/openwrt
```

Produces `octeon_flowtable.ko` (ELF64 MSB MIPS64). The `missing
MODULE_DESCRIPTION` modpost warning is OpenWrt's `CONFIG_MODULE_STRIPPED`
behaviour (it strips modinfo from every module) — not a defect.

## Use

The flowtable comes from fw4 — no custom nftables needed. Set `flow_offloading
'1'` + `flow_offloading_hw '1'` in `/etc/config/firewall`; fw4 then installs an
offloaded flowtable that this module claims. See `../../docs/INSTALLING.md` (or the
`octeon-flowtable` OpenWrt package, which wraps the load + tuning).

## Module parameters (`/sys/module/octeon_flowtable/parameters/`)

| param | r/w | meaning |
|---|---|---|
| `verbose` | rw | log flow install/teardown + the flowtable rule dump (debug) |
| `aqm_limit` | rw | tail-drop AQM: per-egress-port in-flight byte cap; 0 = off |
| `aqm_drops` | ro | AQM drop count |
| `aqm_inflight` | ro | current in-flight bytes per port |
| `fau_stats` | rw | accumulate global fast-path totals in hardware FAU |
| `hw_tx_bytes` / `hw_tx_packets` | ro | the FAU totals (with `fau_stats=1`) |
| `vlan_strict` | rw | disable the wildcard-VID fallback lookup (see Security model) |

## Security model

The RX hook parses **untrusted wire packets** in softirq context, so the rules
are: never trust a header PIP didn't validate, never mutate the packet before
every punt-decision is made, and never diverge from what the software slow path
would forward (a divergence is a firewall bypass).

What the fast path enforces:

- **PIP validation is mandatory.** Rejected up front: `rcv_error`, `not_IP`,
  `IP_exc`, `is_frag`, **`L4_error`** (malformed/truncated L4, UDP length overrun,
  port 0, bad L4 csum, illegal TCP flag combos), non-TCP/UDP, multi-buffer WQEs,
  and any `ip_offset` that doesn't exactly match the L2 header + reported tag
  stack. An explicit `ihl >= 20` floor backs up PIP's bounds guarantee; all header
  reads stay within the single 2 KB FPA buffer regardless.
- **Egress MTU is enforced** (cached from the REDIRECT device at install).
  Oversize packets punt to the slow path so it can fragment / emit ICMP
  frag-needed / packet-too-big — PMTUD works through the fast path.
- **TCP syn/fin/rst and TTL/hop-limit ≤ 1 punt** to the slow path (conntrack must
  see handshakes/teardown; the stack must emit ICMP TTL-exceeded).
- **Flows match 5-tuple + ingress PIP port + ingress VLAN stack**, so a tuple
  spoofed from a different physical port never matches. The one deliberate
  wildcard: bridged-VLAN rules are keyed untagged by the kernel, so a tagged miss
  retries with VID 0 — which lets a tagged packet with an *unconfigured* VID match
  an untagged-keyed flow (low impact: needs the same port + an established tuple).
  `vlan_strict=1` closes this on configs with no bridged VLANs.
- **Unknown flowtable actions refuse the offload** (`-EOPNOTSUPP` → kernel falls
  back to software offload) rather than installing a rule the hook can't execute
  faithfully.
- **Lifetime safety:** flow entries are RCU-freed on every disposal path ever
  visible to the RX hook; `out_dev` holds a netdev reference for the flow's
  lifetime (dropped RCU-deferred); module exit does `rcu_barrier()` before the
  callbacks' text is unloaded.
- **Inherited limits (same as the kernel's software flowtable):** no TCP
  window/sequence tracking on offloaded flows; offloaded packets bypass netfilter
  rules added *after* the flow was established (flows age out within seconds once
  STATS polling stops refreshing them).

## tests/

Example bench rigs (nftables rulesets + netns setup scripts) used to verify each
flow class on hardware — `ft.nft` (NAT), `ftvlan.nft` (inter-VLAN retag),
`ftpp.nft` (VLAN↔untagged push/pop), `ftqq.nft` (QinQ), `ftv6.nft` (IPv6), and
`bench-vlan-rig.sh` / `dock-vlan-rig.sh`. They hardcode example lab addresses;
adapt to your setup.
