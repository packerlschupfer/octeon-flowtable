# flowclassd — classify flows before they are hardware-offloaded

Forwarded flows normally reach the fast path after the TCP handshake (or the
2nd UDP packet), so nothing in the stack ever sees their payload. flowclassd
holds each new flow in the slow path just long enough for nDPI to name it.

```
fw4 forward (prio 0)            table inet flowclassd forward (prio +10)
  ct mark & 0x80000000  -> flow offload        ct mark bit31 == 0, ct packets <= 24
  ct packets > 24       -> flow offload  ...      -> queue 0-1 (bypass)
  zone rules (accept/drop)                            |
                                         flowclassd -q0 / -q1 (nDPI 5.0)
                                         verdict + NFQA_CT{CTA_MARK, mask 0xffff0000}
```

Conntrack mark: bit 31 = classified, bits 16–30 = nDPI protocol id (app if
known, else master), bits 0–15 untouched. Id → name/category:
`/var/run/flowclassd.protos`. Byte counts: the normal conntrack counters
(octeon_flowtable feeds offloaded bytes into them).

Pieces:
- `src/flowclassd/flowclassd.c`, package `package/flowclassd`
- fw4 patch `src/fw4-patches/100-flow-offloading-gate.patch` → firewall
  options `flow_offloading_gate` (set by the package) and
  `flow_offloading_gate_packets` (24)
- kernel: `kmod-nf-conntrack-netlink` (glue for setting the mark from the
  verdict; also gives conntrack events + `conntrack -L`), `kmod-nft-queue`

Design points (each one learned the hard way on the bench):
- The queue chain runs **after** fw4: an NFQUEUE accept resumes at the next
  hook, so queueing inside fw4's chain would skip the zone rules.
- Flows are keyed by **5-tuple, not conntrack id**: a packet arriving while an
  earlier one of the same flow is queued gets a temporary unconfirmed conntrack
  (merged later by clash resolution). QUIC ClientHellos split over two Initials
  hit this every time.
- Done = nDPI final state, or **both** master and app known (`QUIC/YouTube`).
  A lone `QUIC`/`TLS`/`HTTP` is nDPI's pre-SNI guess; stopping there loses the
  service. Give up after `max_packets` (16) with nDPI's best guess.
- nDPI direction hints are 1 = client→server, 2 = server→client (0 = unknown).
- Fail-open: queue `bypass` + `NFQA_CFG_F_FAIL_OPEN`; with no daemon, flows are
  unmarked and offloaded by fw4's packet-count rule (bench: 927 Mbit/s).

Bench result: 152/152 sweep flows finished, 76 % to a named service (rest =
sites nDPI has no signature for), half within 2 packets; classified iperf flow
offloaded in hardware at 930 Mbit/s with conntrack byte counts intact.

Cost on a CN5020 (500 MHz, measured): nDPI ~0.7 ms of CPU per classified flow, the
NFQUEUE trip ~100-270 µs per queued packet. Bulk traffic is unaffected once offloaded, but
every new flow costs roughly 1.3 ms, so the ceiling is about 1,500 new flows/s. A home
network typically sits far below that.

Not in the release images: build it yourself (docs/BUILDING.md). flowclassd is
GPL-2.0-or-later because it links libndpi (LGPL-3.0-or-later).
