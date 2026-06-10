# Prompt: migrate an EdgeOS (Vyatta) config to OpenWrt on the ERLite-3

A Claude prompt for porting a working **EdgeOS / EdgeMAX `config.boot`** to an
equivalent **OpenWrt** config on the same ERLite-3, so you can swap EdgeOS for
OpenWrt + this flow-offload driver. Paste this, attach the EdgeOS `config.boot`,
and have Claude produce `/etc/config/*`.

## Ground rules

- **Secrets stay local.** `config.boot` and any `/etc/bind` material contain live
  PSKs / TSIG / rndc keys / passwords. Sanitise before sharing; never commit them.
- **Don't disturb the production router** while building the replacement config
  offline. Cut over in a planned window (EdgeOS boots slowly with many VLANs;
  budget rollback time).
- Target is the **octeon** OpenWrt image from this repo (kernel hook patch +
  `kmod-octeon-flowtable`).

## Mapping (EdgeOS → OpenWrt)

| EdgeOS | OpenWrt |
|---|---|
| `interfaces ethernet ethN` | `config interface` + `config device` (ethN) |
| `ethN vif V` (802.1Q) | `config device` `type '8021q'` `ifname 'ethN'` `vid 'V'` → an interface on `ethN.V` |
| `address a.b.c.d/p` | `config interface` `proto 'static'` `ipaddr/netmask` (or `proto 'dhcp'`) |
| `firewall name` / `zone-policy zone` | fw4 `config zone` + `config rule` / `config forwarding` |
| `service nat rule … masquerade` | fw4 `option masq '1'` on the WAN zone (or an nftables snat rule) |
| `service dhcp-server shared-network` | **dnsmasq** (`/etc/config/dhcp`) for simple setups, or **isc-dhcp-server** for complex pools/reservations |
| `service dns forwarding` | dnsmasq, or **bind** (`named`) if you need authoritative/zones |
| `protocols static route` | `config route` |
| `system name-server` / `gateway-address` | per-interface DNS / default route |
| many `vif`/zones (router-on-a-stick) | one trunk port with `ethN.V` devices + fw4 zones per VLAN |

## Steps for Claude

1. Parse `config.boot`. List every interface/vif, address, zone, NAT rule, DHCP
   pool, DNS setting, and static route. Flag anything with no clean OpenWrt
   equivalent (e.g. EdgeOS-specific QoS, suspend-on-idle).
2. Emit `/etc/config/network` (interfaces + `8021q` devices for each vif),
   `/etc/config/firewall` (zones, forwardings, rules, masq), and either
   `/etc/config/dhcp` (dnsmasq) or isc-dhcp-server + bind configs, matching the
   EdgeOS intent exactly. Preserve VLAN IDs and subnet numbering.
3. **Enable the offload**: in `/etc/config/firewall`, set
   `option flow_offloading '1'` + `option flow_offloading_hw '1'` so fw4 installs
   the hardware flowtable this driver backs (see `prompts/deploy.md`).
4. Produce a **diff-style migration report**: what mapped 1:1, what changed shape,
   and anything dropped — so a human can review before cutover.

## Gotchas to check

- VLAN naming: keep `ethN.V` consistent; a typo'd VID or interface name silently
  breaks one VLAN only — verify each against the source.
- A "trap"/management VLAN or a cable-modem management subnet (e.g. an extra VLAN
  just to reach `192.168.100.1`) is easy to miss — carry it over explicitly.
- isc-dhcp-server vs dnsmasq: pick per complexity; don't half-convert (orphaned
  reservations).
- After cutover, confirm forwarded flows offload (`conntrack -L | grep
  HW_OFFLOAD`) and that the router-on-a-stick inter-VLAN traffic is accelerated
  (VLAN retag is supported; see `docs/FUTURE-WORK.md`).
