# Installing octeon-flowtable on an OpenWrt ERLite-3 (CN50xx)

Getting the offload onto the box and turning it on.

## 1. Get the image

Grab a prebuilt image from the [**Releases**](../../releases) page (built by CI —
the kernel already carries the staging hook patch and `kmod-octeon-flowtable` is
baked in). Two variants:

- **`lean-…`** — offload + `conntrack`/`tcpdump`; a clean base to add packages to.
- **`router-…`** — lean + LuCI web UI + WireGuard, for a turnkey home router.

Each variant ships two files (plus a `.manifest` and `sha256sums-*.txt` — verify
those first):

| file | use |
|---|---|
| `…-ubnt_edgerouter-lite-squashfs-sysupgrade.tar` | the install image (methods A & B below) |
| `…-ubnt_edgerouter-lite-initramfs-kernel.bin` | RAM-boot kernel for U-Boot/TFTP (method C / recovery) |

> The kernel **must** be one of these patched builds — a stock OpenWrt kernel
> lacks the exported hooks and the module won't load. Building your own instead:
> [BUILDING.md](BUILDING.md).

## 2. Flash it

The ERLite-3 boots from its internal USB stick: **`sda1`** = a FAT partition with
`vmlinux.64` (+ a `vmlinux.64.md5` that UBNT's U-Boot **enforces**), **`sda2`** =
the squashfs rootfs. Pick the method that matches where you're starting from.

### A. Already running OpenWrt → `sysupgrade`

```sh
scp <variant>-…-squashfs-sysupgrade.tar root@<box>:/tmp/
ssh root@<box> 'sysupgrade -n /tmp/<variant>-…-squashfs-sysupgrade.tar'   # -n: don't keep settings
```

### B. Fresh install / coming from EdgeOS → write the USB stick on a PC

Pull the stick, plug it into a Linux PC, and write it directly. **This erases the
stick — double-check `lsblk` so `DEV` is the stick, not your disk.**

```sh
DEV=/dev/sdX                                   # the ERLite stick (CHECK with lsblk!)
TAR=<variant>-openwrt-octeon-generic-ubnt_edgerouter-lite-squashfs-sysupgrade.tar
BOARD=sysupgrade-ubnt_edgerouter-lite          # the tar's internal dir

sudo parted -s "$DEV" mklabel msdos
sudo parted -s "$DEV" mkpart primary fat32 1MiB 257MiB
sudo parted -s "$DEV" set 1 boot on
sudo parted -s "$DEV" mkpart primary ext2 257MiB 100%
sudo partprobe "$DEV"; sleep 1

# sda1: FAT boot partition with the kernel + its mandatory md5
sudo mkfs.vfat -F32 -n BOOT "${DEV}1"
m=$(mktemp -d); sudo mount "${DEV}1" "$m"
tar xOf "$TAR" "$BOARD/kernel" | sudo tee "$m/vmlinux.64" >/dev/null
md5sum "$m/vmlinux.64" | cut -d' ' -f1 | sudo tee "$m/vmlinux.64.md5" >/dev/null  # bare lowercase hex + newline
sudo sync; sudo umount "$m"; rmdir "$m"

# sda2: squashfs rootfs (overlay auto-grows on first boot)
tar xOf "$TAR" "$BOARD/root" | sudo dd of="${DEV}2" bs=4M conv=fsync
sync
```

(Partition suffix is `sdX1`/`sdX2` for a USB stick; on NVMe/mmc it's `p1`/`p2`.)
Reinsert the stick and power on. UBNT's U-Boot loads `vmlinux.64` from `sda1`; the
kernel's baked-in cmdline (`root=/dev/sda2 … receive_group_order=1`) takes over.

> ⚠ If you ever edit `vmlinux.64` by hand, regenerate `vmlinux.64.md5` — U-Boot
> aborts to its prompt with "md5 checksum error" on a mismatch, and it re-checks at
> `bootoctlinux` too (you can't boot the in-RAM image around it). The file must be
> the bare lowercase md5 + a newline (33 bytes), **not** `md5  filename`.

### C. Try it without flashing → U-Boot + TFTP (also the recovery path)

With a serial console (**115200 8N1**) and a TFTP server on your PC, interrupt
U-Boot at power-on and RAM-boot the initramfs kernel:

```
setenv serverip <your-PC-ip>
tftpboot $loadaddr <variant>-…-initramfs-kernel.bin
bootoctlinux $loadaddr numcores=2
```

Nothing is written to the stick, so this both lets you try the image and recovers a
box with a bad on-disk image. (`initramfs-kernel.bin` is self-contained; no md5
file involved.) If U-Boot networking is uncooperative, re-imaging the stick on a PC
(method B) is the reliable fallback — the ERLite can't be permanently bricked this
way. For the canonical per-board U-Boot procedure see the
[OpenWrt EdgeRouter Lite device page](https://openwrt.org/toh/ubiquiti/edgerouter.lite).

## 3. Confirm the module is present

```sh
ssh root@<box> '
  lsmod | grep -q octeon_flowtable || modprobe octeon_flowtable
  ls /sys/module/octeon_flowtable/parameters/'
```

If the parameters directory is missing, the running kernel doesn't have the
staging hook patch (rebuild + reflash — see BUILDING.md).

## 4. Turn on hardware flow offload (fw4 — no custom nftables)

The flowtable comes from OpenWrt's firewall. In `/etc/config/firewall`:

```
config defaults
	option flow_offloading      '1'
	option flow_offloading_hw   '1'
```

```sh
ssh root@<box> 'uci commit firewall; fw4 reload; dmesg | grep "claimed flow block"'
```

fw4 installs an **offloaded** flowtable on the LAN/WAN zone devices (`flags
offload`), which this module claims via `flow_indr_dev_register`. Forwarded
TCP/UDP flows then bypass the Linux stack; misses fall through to normal routing
(OpenWrt's `701-netfilter-…ignore-EOPNOTSUPP-on-flowtable` patch makes unsupported
flows degrade gracefully). Expect one "claimed flow block" per octeon port.

## 5. Configure — `/etc/config/octeon-flowtable`

```
config octeon-flowtable 'settings'
	option enabled    '1'
	option irq_spread '1'      # pin the two Ethernet group IRQs one-per-core
	option aqm_limit  '0'      # tail-drop AQM byte cap per egress port; 0 = off
	option fau_stats  '0'      # global HW byte/packet counters
	option verbose    '0'
```

```sh
ssh root@<box> 'service octeon-flowtable restart'
```

- **`irq_spread`** pins the two Ethernet group IRQs one-per-core — the tuning that
  gets multi-flow forwarding to line rate. Leave it on.
- **`aqm_limit`** bounds worst-case queue latency under bufferbloat. Pick
  `link_bps × target_seconds ÷ 8` (e.g. 1 Gbps × 5 ms ≈ `625000`). `0` disables it.
- **`fau_stats`** enables the global hardware byte/packet counters (small per-packet
  cost; off by default).

## 6. Verify

```sh
# drive a forwarded TCP/UDP flow LAN→WAN (e.g. iperf3 from a LAN host to a WAN host)
ssh root@<box> '
  conntrack -L 2>/dev/null | grep -c HW_OFFLOAD                # offloaded flows
  grep Ethernet /proc/interrupts                                # one group IRQ per CPU
  cat /sys/module/octeon_flowtable/parameters/hw_tx_bytes 2>/dev/null   # if fau_stats=1
  cat /sys/module/octeon_flowtable/parameters/aqm_drops 2>/dev/null     # if aqm_limit>0
'
```

Offloaded traffic is **invisible to `tcpdump`** on the egress netdev (full stack
bypass) but stays visible in `conntrack -L` with live byte counts (per-flow
software stats fed back through the flowtable stats callback). That tcpdump-blind /
conntrack-visible combination is the signal that the fast path is actually firing.

## What's accelerated

IPv4 + IPv6, NAT + routing, untagged + single-VLAN (retag/pop/push) + QinQ
(double-tag) forwarding. Per the hardware, PIP parses ≤ 2 VLAN tags and no IPv6
extension headers / fragments — anything else falls to the slow path (correct,
just not accelerated). Full capability matrix and measured results in
[FUTURE-WORK.md](FUTURE-WORK.md) and [PROJECT-OUTCOME.md](PROJECT-OUTCOME.md).

## Changing the MAC address

The interface MACs originate in the board **EEPROM** (the `64k(eeprom)` flash
region), read by both U-Boot and the OS — which is why they survive an OS reinstall.
The OpenWrt wiki documents no MAC procedure for this board; here are three levels,
easiest/most reversible first.

**1. Per-interface in OpenWrt (recommended, reversible).** Overrides what the
running system uses, without touching the EEPROM:

```sh
# persistent (UCI):
ssh root@<box> "uci set network.wan.macaddr='02:11:22:33:44:55'; uci commit network; /etc/init.d/network restart"
# or one-shot:
ssh root@<box> 'ip link set dev eth1 address 02:11:22:33:44:55'
```

If you're inventing one, use a locally-administered address (second hex digit
`2`, `6`, `A`, or `E`).

**2. From U-Boot (serial console).** Octeon U-Boot keeps the MACs in its environment
as the standard `ethaddr` / `eth1addr` / `eth2addr` vars; set and persist them:

```
setenv ethaddr  02:11:22:33:44:55
setenv eth1addr 02:11:22:33:44:56
setenv eth2addr 02:11:22:33:44:57
saveenv
```

You can do the same from a running OpenWrt with `fw_setenv` once the env partition
is exposed (`uboot-envtools` + a `wholeflash` mtd in the kernel cmdline). Caveat:
this is the **bootloader's own** MAC (used for its TFTP networking); on this board
the **EEPROM is authoritative** for the MAC the OS brings up, so for a durable change
to the actual NIC MAC use level 3.

**3. Permanent, in the board EEPROM (stock UBNT firmware).** The factory MAC lives
in the EEPROM and is written with Ubiquiti's board tool, which only ships in EdgeOS
— so boot EdgeOS (or its rescue image) to use it:

```
ubnt-hal-e getBoardIdE                 # read board id (e100 = ERLite-3)
ubntw mac dc9fdb803198 <n>             # write base MAC (12 hex digits) + NIC count
```

This rewrites the EEPROM, so the new MAC then appears under both U-Boot and OpenWrt.
(`<n>` is the interface count the tool provisions; the exact arg is EdgeOS-specific
and undocumented by UBNT — verify against your firmware version.)

## Rollback

```sh
ssh root@<box> '
  uci set firewall.@defaults[0].flow_offloading_hw=0; uci commit firewall; fw4 reload
  rmmod octeon_flowtable'
```

`rmmod` is clean; the box keeps forwarding in software.
