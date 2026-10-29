#!/bin/bash
# Fake PPPoE ISP for testing octeon_flowtable's PPPoE offload, run on a Linux
# bench host cabled to a DUT LAN port. Needs: pppoe (rp-pppoe), ppp, iperf3.
#
#   bench NIC  ----cable----  DUT LAN port (e.g. eth2)
#     VLAN $VID subif in netns 'isp': pppoe-server (kernel mode) + iperf3 on 10.64.0.1
#   DUT: interface 'pppwan' = PPPoE over <port>.$VID, peer 10.64.0.1, no default
#        route, in the wan zone (masq). LAN clients reach 10.64.0.1 via PPPoE.
#
# One cable carries both the LAN (untagged) and the "ISP" (VLAN $VID), so the
# test exercises PPPoE-over-VLAN encap on egress and decap on ingress.
#
# Usage: NIC=<bench nic> [DUT=root@192.168.1.1] [DUT_PORT=eth2] [VID=40] \
#        pppoe-isp.sh start|stop|dut-up|dut-down
#   then: ip route add 10.64.0.1/32 via <DUT LAN IP>; iperf3 -c 10.64.0.1 [-R]
#   watch: /sys/module/octeon_flowtable/parameters/{hits,r_ppp,pppoe_sessions}
set -u
NIC=${NIC:?set NIC= to the bench interface cabled to the DUT}
DUT=${DUT:-root@192.168.1.1}
DUT_PORT=${DUT_PORT:-eth2}
VID=${VID:-40}
NS=isp
DIR=/tmp/pppoe-isp
S="ssh -o BatchMode=yes -o ConnectTimeout=5 $DUT"

case "${1:-}" in
start)
	mkdir -p $DIR
	cat > $DIR/options <<EOF
noauth
lcp-echo-interval 10
lcp-echo-failure 3
mtu 1492
mru 1492
nodefaultroute
noipdefault
EOF
	# pppoe-server -k hard-codes /etc/ppp/plugins/rp-pppoe.so; Debian ships
	# it under /usr/lib/pppd/<ver>/. Temporary symlink (removed on stop).
	if [ ! -e /etc/ppp/plugins/rp-pppoe.so ]; then
		sudo mkdir -p /etc/ppp/plugins
		sudo ln -s "$(ls /usr/lib/pppd/*/rp-pppoe.so | head -1)" /etc/ppp/plugins/rp-pppoe.so
		touch $DIR/plugin-link-ours
	fi
	sudo ip netns add $NS 2>/dev/null
	if ! sudo ip -n $NS link show isp$VID >/dev/null 2>&1; then
		sudo ip link add link $NIC name isp$VID type vlan id $VID
		sudo ip link set isp$VID netns $NS
	fi
	sudo ip -n $NS link set lo up
	sudo ip -n $NS link set isp$VID up
	sudo ip netns exec $NS /usr/sbin/pppoe-server -k -I isp$VID -L 10.64.0.1 \
		-R 10.64.0.10 -N 4 -O $DIR/options
	echo "isp up: netns $NS, pppoe-server on isp$VID (start iperf3 after dut-up)"
	;;
stop)
	for p in $(sudo ip netns pids $NS 2>/dev/null); do sudo kill "$p"; done
	sleep 1
	sudo ip netns del $NS 2>/dev/null
	if [ -e $DIR/plugin-link-ours ]; then
		sudo rm -f /etc/ppp/plugins/rp-pppoe.so
		sudo rmdir /etc/ppp/plugins 2>/dev/null
		rm -f $DIR/plugin-link-ours
	fi
	echo "isp down"
	;;
dut-up)
	$S "uci -q batch <<EOF
set network.pppwan=interface
set network.pppwan.proto='pppoe'
set network.pppwan.device='$DUT_PORT.$VID'
set network.pppwan.username='bench'
set network.pppwan.password='bench'
set network.pppwan.defaultroute='0'
set network.pppwan.peerdns='0'
set network.pppwan.ipv6='0'
add_list firewall.@zone[1].network='pppwan'
commit network
commit firewall
EOF
reload_config"
	for i in $(seq 1 15); do
		sudo ip netns exec $NS ip -4 addr show 2>/dev/null | grep -q 10.64.0.1 && break
		sleep 2
	done
	sudo ip netns exec $NS iperf3 -s -D -B 10.64.0.1
	$S "ip -4 addr show pppoe-pppwan | grep inet"
	;;
dut-down)
	$S "uci -q delete network.pppwan; uci -q del_list firewall.@zone[1].network='pppwan'; uci commit network; uci commit firewall; reload_config"
	echo "pppwan removed from DUT config"
	;;
*)
	echo "usage: NIC=<if> $0 start|stop|dut-up|dut-down"; exit 2 ;;
esac
