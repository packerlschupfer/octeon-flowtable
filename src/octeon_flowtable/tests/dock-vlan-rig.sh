#!/bin/sh
# Workstation/dock side of the inter-VLAN test rig: two netns (v10, v20), each
# holding one 802.1Q subinterface of the WAN dock NIC, with a default route via
# the bench. Forces a 192.168.10.2 <-> 192.168.20.2 flow to hairpin through the
# bench (eth2.10 in, eth2.20 out) so the retag fast path is exercised.
DOCK=${1:-enxWANDONGLE}   # WAN dock NIC, lives in the `wan` netns
for ns in v10 v20; do sudo ip netns del $ns 2>/dev/null; sudo ip netns add $ns; done
sudo ip netns exec wan sh -c "
  for v in dvl10 dvl20; do ip link del \$v 2>/dev/null; done
  ip link add link $DOCK name dvl10 type vlan id 10
  ip link add link $DOCK name dvl20 type vlan id 20
"
sudo ip netns exec wan ip link set dvl10 netns v10
sudo ip netns exec wan ip link set dvl20 netns v20
sudo ip netns exec v10 sh -c "ip link set lo up; ip addr add 192.168.10.2/24 dev dvl10; ip link set dvl10 up; ip route add default via 192.168.10.1"
sudo ip netns exec v20 sh -c "ip link set lo up; ip addr add 192.168.20.2/24 dev dvl20; ip link set dvl20 up; ip route add default via 192.168.20.1"
echo "v10=$(sudo ip netns exec v10 ip -o addr show dvl10 | grep -o 'inet [0-9.]*')  v20=$(sudo ip netns exec v20 ip -o addr show dvl20 | grep -o 'inet [0-9.]*')"
