#!/bin/sh
# Re-apply the octeon-target tweaks the driver was validated with. These are NOT
# upstream defaults, so a from-source build must set them explicitly:
#
#   1. CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=2  — 2 cache lines of CVMSEG scratch for the
#      async IOBDMA the fast path relies on (default is 1).
#   2. CONFIG_OCTEON_ILM off                — unused interrupt-latency-monitor driver.
#   3. octeon_ethernet.receive_group_order=1 on the ERLite kernel cmdline — spreads RX
#      across one POW group per core (PIP GRPTAG hash-steering), the multi-flow
#      line-rate lever.
#
# Done with sed (not a committed .patch) on purpose: it stays correct across small
# upstream changes to these files, and fails loudly here if the files move.
set -eu

OW=${1:?usage: apply-target-tweaks.sh <openwrt-dir>}
cfg="$OW/target/linux/octeon/config-6.18"
img="$OW/target/linux/octeon/image/Makefile"

[ -f "$cfg" ] || { echo "ERROR: $cfg not found (kernel version changed?)" >&2; exit 1; }
[ -f "$img" ] || { echo "ERROR: $img not found" >&2; exit 1; }

# 1. CVMSEG scratch = 2
if grep -q '^CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=' "$cfg"; then
	sed -i 's/^CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=.*/CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=2/' "$cfg"
else
	echo 'CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=2' >> "$cfg"
fi

# 2. Drop OCTEON_ILM if it is enabled
if grep -q '^CONFIG_OCTEON_ILM=' "$cfg"; then
	sed -i 's/^CONFIG_OCTEON_ILM=.*/# CONFIG_OCTEON_ILM is not set/' "$cfg"
fi

# 3. Append the cmdline param to ERLITE_CMDLINE (idempotent)
if ! grep -q '^ERLITE_CMDLINE:=' "$img"; then
	echo "ERROR: ERLITE_CMDLINE not found in $img (image Makefile changed?)" >&2
	exit 1
fi
if ! grep -q 'octeon_ethernet.receive_group_order' "$img"; then
	sed -i '/^ERLITE_CMDLINE:=/ s/$/ octeon_ethernet.receive_group_order=1/' "$img"
fi

echo "== target tweaks applied =="
grep -E 'CAVIUM_OCTEON_CVMSEG_SIZE|OCTEON_ILM' "$cfg" || true
grep -E '^ERLITE_CMDLINE' "$img" || true
