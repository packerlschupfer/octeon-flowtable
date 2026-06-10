#!/bin/sh
# Re-establish the inter-VLAN router-on-a-stick rig on the bench (run after reboot).
modprobe 8021q
ip link add link eth2 name eth2.10 type vlan id 10 2>/dev/null
ip link add link eth2 name eth2.20 type vlan id 20 2>/dev/null
ip addr add 192.168.10.1/24 dev eth2.10 2>/dev/null
ip addr add 192.168.20.1/24 dev eth2.20 2>/dev/null
ip link set eth2.10 up; ip link set eth2.20 up
# allow inter-VLAN forwarding (fw4 default-drops unassigned ifaces)
nft list chain inet fw4 forward 2>/dev/null | grep -q 'eth2.10' || \
  nft insert rule inet fw4 forward iifname { eth2.10, eth2.20 } oifname { eth2.10, eth2.20 } accept
echo "rig: $(ip -o addr show eth2.10 | grep -o 'inet [0-9.]*') $(ip -o addr show eth2.20 | grep -o 'inet [0-9.]*')"
