#!/bin/bash
# octeon_flowtable regression suite — run from the workstation.
#
# Covers every bug class found 2026-06-05:
#   T1/T2  untagged NAT fast path, TCP+UDP, both directions (zombie/dup bugs)
#   T3/T4  bridged-VLAN (gw-style lower-device encoding: VLAN_PUSH actions +
#          wildcard-vid match) TCP+UDP both directions
#   T5     fast-path engagement proof (tx_ok delta vs transferred packets)
#   T6     SYN punt + stale-entry eviction (kill -9, reconnect same src port)
#   T7     fw4 reload churn under traffic — no duplicate-entry leak
#   T8     orphan GC — entries with lost DESTROYs reaped (flows -> 0)
#   T9     link flap mid-flow — recovery, no crash
#   T10    flows settle to 0 when idle (no leak in steady state)
#
# Topology (bench dev unit, erlite-test stick):
#   ws dock  (enxf4a80d2031c7, 192.168.1.2)  -> DUT eth2 untagged LAN
#   ws dock36 (VLAN 36 subif,  192.168.36.2) -> DUT br-v36 (bridge over eth2.36)
#   ws eno1 macvlan in 'wan' netns (198.51.100.2) -> DUT eth1 (WAN, masq)
# The 198.51.100.0/24 route on the ws picks the client path per test leg.
set -u
DUT=${1:-192.168.1.1}
SRV=198.51.100.2
DOCK=enxf4a80d2031c7
S="ssh -o BatchMode=yes -o ConnectTimeout=5 root@$DUT"
P=/sys/module/octeon_flowtable/parameters
PASS=0; FAIL=0; SKIP=0
say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); say "PASS: $*"; }
bad()  { FAIL=$((FAIL+1)); say "FAIL: $*"; }
skip() { SKIP=$((SKIP+1)); say "SKIP: $*"; }

par()      { $S "cat $P/$1" 2>/dev/null; }
flows()    { par flows; }
cthw()     { $S "grep -c HW_OFFLOAD /proc/net/nf_conntrack" 2>/dev/null; }
route_untagged() { sudo ip route replace 198.51.100.0/24 via 192.168.1.1  dev $DOCK; }
route_vlan()     { sudo ip route replace 198.51.100.0/24 via 192.168.36.1 dev dock36; }

# Mbps from iperf3 receiver line; empty on error
tcp_mbps() { # args: bind extra...
  local b=$1; shift
  timeout 25 iperf3 -c $SRV -B "$b" -t 8 -i 0 "$@" 2>/dev/null |
    awk '/receiver/{print int($7)}' | tail -1
}
udp_loss() { # args: bind extra... -> "loss%/mbps"
  local b=$1; shift
  timeout 25 iperf3 -c $SRV -B "$b" -u -b 100M --length 1200 -t 8 -i 0 "$@" 2>/dev/null |
    awk '/receiver/{gsub(/[()%]/,"",$12); print $12"/"int($7)}' | tail -1
}

say "=== octeon_flowtable regression vs $DUT $(date -u +%H:%M:%SZ) ==="

# ---- preflight (self-healing rig) ------------------------------------------
$S true || { say "ABORT: DUT unreachable"; exit 1; }
[ -n "$(par flows)" ] || { say "ABORT: module not loaded / no debug params"; exit 1; }
# wan netns + macvlan endpoint on eno1 (198.51.100.2 <-> DUT eth1)
if ! sudo ip netns exec wan true 2>/dev/null; then
  say "preflight: recreating wan netns"
  sudo ip netns add wan
  sudo ip link add wan0 link eno1 type macvlan mode bridge
  sudo ip link set wan0 netns wan
  sudo ip netns exec wan ip addr add 198.51.100.2/24 dev wan0
  sudo ip netns exec wan ip link set wan0 up
  sudo ip netns exec wan ip link set lo up
fi
# tagged client subif on the dock (VLAN 36 <-> DUT br-v36)
if ! ip link show dock36 >/dev/null 2>&1; then
  say "preflight: recreating dock36"
  sudo ip link add link $DOCK name dock36 type vlan id 36
  sudo ip addr add 192.168.36.2/24 dev dock36
  sudo ip link set dock36 up
fi
sudo ip netns pids wan | xargs -r ps -o comm= -p 2>/dev/null | grep -q iperf3 ||
  sudo ip netns exec wan iperf3 -s -D
ping -c1 -W2 -I 192.168.36.2 192.168.36.1 >/dev/null 2>&1 || say "WARN: vlan36 path unreachable"
$S "uci set firewall.@defaults[0].flow_offloading_hw='1'; uci commit firewall; fw4 reload" >/dev/null 2>&1
sleep 2
say "preflight ok: flows=$(flows) hw-conntrack=$(cthw)"

# ---- T1/T2 untagged --------------------------------------------------------
route_untagged
m=$(tcp_mbps 192.168.1.2);      [ "${m:-0}" -ge 800 ] && ok "T1a untagged TCP up    ${m}Mbps" || bad "T1a untagged TCP up    ${m:-ERR}Mbps (<800)"
m=$(tcp_mbps 192.168.1.2 -R);   [ "${m:-0}" -ge 800 ] && ok "T1b untagged TCP down  ${m}Mbps" || bad "T1b untagged TCP down  ${m:-ERR}Mbps (<800)"
r=$(udp_loss 192.168.1.2);    l=${r%%/*}; [ "${l%.*}" = "0" ] && ok "T2a untagged UDP up    loss=$r" || bad "T2a untagged UDP up    loss=${r:-ERR}"
r=$(udp_loss 192.168.1.2 -R); l=${r%%/*}; [ "${l%.*}" = "0" ] && ok "T2b untagged UDP down  loss=$r" || bad "T2b untagged UDP down  loss=${r:-ERR}"

# ---- T3/T4 bridged VLAN (gw encoding) --------------------------------------
route_vlan
m=$(tcp_mbps 192.168.36.2);     [ "${m:-0}" -ge 600 ] && ok "T3a vlan TCP up        ${m}Mbps" || bad "T3a vlan TCP up        ${m:-ERR}Mbps (<600 rig-cap aware)"
m=$(tcp_mbps 192.168.36.2 -R);  [ "${m:-0}" -ge 800 ] && ok "T3b vlan TCP down      ${m}Mbps" || bad "T3b vlan TCP down      ${m:-ERR}Mbps (<800)"
r=$(udp_loss 192.168.36.2);   l=${r%%/*}; [ "${l%.*}" = "0" ] && ok "T4a vlan UDP up        loss=$r" || bad "T4a vlan UDP up        loss=${r:-ERR}"
pre=$(par tx_ok)
r=$(udp_loss 192.168.36.2 -R); l=${r%%/*}; [ "${l%.*}" = "0" ] && ok "T4b vlan UDP down      loss=$r" || bad "T4b vlan UDP down      loss=${r:-ERR}"
post=$(par tx_ok)

# ---- T5 fast-path engagement (T4b moved ~83k pkts at 100M/1200B/8s) --------
d=$((post - pre))
[ "$d" -ge 50000 ] && ok "T5  fast-path engaged  tx_ok+=$d" || bad "T5  fast-path engaged  tx_ok+=$d (<50000: traffic rode slow path)"

# ---- T6 SYN punt + eviction (kill -9, reuse source port) -------------------
$S "dmesg -c >/dev/null"
timeout 20 iperf3 -c $SRV -B 192.168.36.2 --cport 47711 -t 30 -i 0 >/dev/null 2>&1 &
ip=$!; sleep 4; kill -9 $ip 2>/dev/null; wait $ip 2>/dev/null
# the abandoned server side still thinks a test is running; restart it so the
# reconnect is judged on the DUT fast path, not on iperf3 server state
sudo ip netns pids wan | xargs -r sudo kill 2>/dev/null; sleep 1
sudo ip netns exec wan iperf3 -s -D; sleep 1
m=$(tcp_mbps 192.168.36.2 --cport 47711 -R)
ev=$($S "dmesg | grep -c 'evicting stale flow'")
if [ "${m:-0}" -ge 600 ]; then ok "T6  tuple-reuse after kill-9  ${m}Mbps (evictions=$ev)"
else bad "T6  tuple-reuse after kill-9  ${m:-ERR}Mbps"; fi

# ---- T7 reload churn under traffic: no duplicate leak ----------------------
timeout 30 iperf3 -c $SRV -B 192.168.36.2 -R -t 20 -i 0 >/dev/null 2>&1 &
ip=$!; sleep 3
$S "for i in 1 2 3 4 5 6 7 8; do fw4 reload >/dev/null 2>&1; done"
mid=$(flows)
wait $ip 2>/dev/null
# expect a handful of live entries (control+data x2 dirs + stragglers), not dozens
[ "${mid:-99}" -le 12 ] && ok "T7  reload churn       flows=$mid (<=12)" || bad "T7  reload churn       flows=$mid (>12: duplicate leak)"

# ---- T8 orphan GC ----------------------------------------------------------
say "T8  waiting up to 150s for orphan GC + flow aging..."
t8=fail
for i in $(seq 1 15); do
  sleep 10
  f=$(flows); c=$(cthw)
  [ "${f:-1}" -eq 0 ] && { t8=ok; break; }
  # entries are fine if conntrack still owns them (2 entries per conn)
  [ "${c:-0}" -gt 0 ] && [ "$f" -le $((c * 2)) ] && { t8=ok; break; }
done
[ $t8 = ok ] && ok "T8  orphan GC/settle   flows=$f ct_hw=${c:-0}" || bad "T8  orphan GC/settle   flows=$f ct_hw=${c:-0} (orphans not reaped)"

# ---- T9 link flap mid-flow -------------------------------------------------
timeout 45 iperf3 -c $SRV -B 192.168.36.2 -R -t 30 -i 1 >/tmp/t9.txt 2>&1 &
ip=$!; sleep 4
$S "ip link set eth1 down; sleep 2; ip link set eth1 up"
wait $ip 2>/dev/null
# judge by the best of the last 5 one-second intervals (exclude summary lines)
tail5=$(grep -E 'sec.*Mbits/sec *$|sec.*Mbits/sec  *$' /tmp/t9.txt |
        grep -v 'receiver\|sender' | tail -5 | awk '{v=int($7); if(v>m)m=v} END{print m+0}')
alive=$($S "echo yes" 2>/dev/null)
if [ "$alive" = yes ] && [ "${tail5:-0}" -ge 700 ]; then ok "T9  flap recovery      best-tail=${tail5}Mbps, DUT alive"
else bad "T9  flap recovery      best-tail=${tail5:-ERR}Mbps alive=$alive"; fi

# ---- T10 idle settle -------------------------------------------------------
say "T10 waiting 60s idle..."
sleep 60
f=$(flows); c=$(cthw)
if [ "${f:-1}" -eq 0 ] || { [ "${c:-0}" -gt 0 ] && [ "$f" -le $((c * 2)) ]; }; then
  ok "T10 idle settle        flows=$f ct_hw=${c:-0}"
else bad "T10 idle settle        flows=$f ct_hw=${c:-0}"; fi

route_untagged   # leave routing in the boring state
say "=== done: PASS=$PASS FAIL=$FAIL SKIP=$SKIP ==="
exit $((FAIL > 0))
