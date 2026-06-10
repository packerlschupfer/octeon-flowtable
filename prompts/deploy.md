# Runbook / prompt: deploy + verify octeon_flowtable on an ERLite-3

Action-oriented companion to `docs/INSTALLING.md`. Usable as a Claude prompt — e.g.
"deploy octeon-flowtable on the ERLite at 192.168.1.1 and confirm flows offload."

## Assumes

- An OpenWrt image built per `prompts/build.md` (kernel has the staging hook
  patch; `kmod-octeon-flowtable` installed) is flashed and the box is reachable
  over SSH.

## 1. Module loaded?

```sh
ssh root@<box> '
  lsmod | grep -q octeon_flowtable || modprobe octeon_flowtable
  ls /sys/module/octeon_flowtable/parameters/'
```
If the params dir is missing, the staging hook patch isn't in the running kernel
(rebuild + reflash — `prompts/build.md`).

## 2. Turn on hardware flow offload (fw4 — no custom nftables)

```sh
ssh root@<box> '
  uci set firewall.@defaults[0].flow_offloading=1
  uci set firewall.@defaults[0].flow_offloading_hw=1
  uci commit firewall
  fw4 reload
  dmesg | grep "claimed flow block" | tail'
```
Expect "claimed flow block" for each LAN/WAN octeon port — the module now backs
fw4's flowtable.

## 3. Tune (optional) — /etc/config/octeon-flowtable + `service octeon-flowtable restart`

```
config octeon-flowtable 'settings'
	option irq_spread '1'      # pin the two Ethernet group IRQs one-per-core
	option aqm_limit  '625000' # ~5 ms bound on a 1 Gbps egress; 0 = off
	option fau_stats  '0'
```

## 4. Verify

```sh
# drive a forwarded TCP/UDP flow LAN->WAN (e.g. iperf3 from a LAN host to a WAN host)
ssh root@<box> '
  conntrack -L 2>/dev/null | grep -c HW_OFFLOAD          # offloaded flows
  grep Ethernet /proc/interrupts                          # one group IRQ per CPU
  cat /sys/module/octeon_flowtable/parameters/aqm_drops   # if aqm_limit>0
'
```

Offloaded data is invisible to `tcpdump` on the egress netdev (full stack bypass)
but shows live byte counts in `conntrack -L`. If a flow class isn't offloaded it
silently falls to normal routing (correct, just not accelerated) — check the
capability matrix in `docs/FUTURE-WORK.md`.

## Rollback

```sh
ssh root@<box> '
  uci set firewall.@defaults[0].flow_offloading_hw=0; uci commit firewall; fw4 reload
  rmmod octeon_flowtable'
```
`rmmod` is clean (the unbind bug is fixed); the box keeps forwarding in software.
