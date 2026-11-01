# trafficd — per-client / per-application traffic analysis

A small collector plus a self-contained web page at `http://<router>/traffic/`:

- **Clients & applications:** bytes per LAN client and per application over 1 h, 24 h,
  7 d and 30 d, with a throughput graph per client.
- **Internet uplink** (`#uplink`): download and upload rate on the internet link, with peaks.

It reads conntrack over ctnetlink: a dump every 30 s, plus DESTROY events for connections
that end in between. Netdev counters can't be used, because offloaded packets bypass them;
`octeon_flowtable` feeds its byte counts back into conntrack instead.

- **Client:** the LAN endpoint, by MAC. Named from dnsmasq or ISC dhcpd leases, else from
  reverse DNS if the name resolves back to the same address. The router's own internet
  traffic appears as `<hostname> (router)`.
- **Application:** the nDPI id [flowclassd](../flowclassd/README.md) stores in the conntrack
  mark. Without flowclassd everything shows as "Unclassified"; the rest still works.
- **Uplink** counts traffic whose far end is outside the LAN, plus the router's own. It
  excludes traffic between your own subnets and the inside of WireGuard tunnels (`wg*`).
- **Storage:** 24 h in RAM (empties on restart); hourly data for 31 days in
  `/etc/trafficd/history.bin`, saved hourly and on stop, kept across sysupgrade.
  JSON for the page: `/tmp/trafficd/`.

## Privacy: restrict who can see the page

The page has **no login** and shows per-device traffic. It depends on uhttpd, whose default
config listens on all addresses, so anyone on any LAN zone can read it (fw4's default blocks
WAN). If you have guest or IoT networks, bind it to one address:

```sh
uci -q delete uhttpd.main.listen_http; uci -q delete uhttpd.main.listen_https
uci add_list uhttpd.main.listen_http='192.168.1.1:80'
uci commit uhttpd; /etc/init.d/uhttpd restart
```

Also consider whether the people on your network know their traffic is being recorded.

## Limits

- A connection's first bytes can land in "Unclassified" if a dump catches it before
  classification.
- IPv6 clients are grouped by MAC only while their address is in the neighbour table;
  otherwise they appear by address.
- Reverse-DNS lookups are synchronous (capped at 8 per minute), so a dead local resolver
  stalls the collector for a few seconds per lookup.
