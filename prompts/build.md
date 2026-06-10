# Runbook / prompt: build the kernel + octeon_flowtable module

A reproducible recipe (also usable as a Claude prompt — paste it and point at your
OpenWrt tree). The octeon staging driver is **built into vmlinux**, so the hook
patch means a kernel build; the module is built out-of-tree against it.

## Prerequisites

- An OpenWrt source tree with the **octeon** target selected (`make menuconfig` →
  Target System = *Cavium Networks Octeon*).
- The mips64 toolchain built (`make toolchain/install` or a full `make` once).
- This repo checked out next to it.

## 1. Kernel config + cmdline (the B5 baseline)

In the kernel config (`make kernel_menuconfig` or the target's config):

- `CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=2`  (async IOBDMA scratch)
- `# CONFIG_OCTEON_ILM is not set`
- `# CONFIG_MODVERSIONS is not set`  (OpenWrt default; relevant below)

Boot cmdline must include `octeon_ethernet.receive_group_order=1` (PIP GRPTAG
hash-steering — the multi-flow line-rate lever). On a packaged image add it to
`ERLITE_CMDLINE` in `target/linux/octeon/image/Makefile`; on a USB/U-Boot setup
put it in the `bootcmd`.

## 2. Apply the staging hook patch

```sh
cp src/staging-patches/120-octeon-flowtable-hooks.patch \
   <openwrt>/target/linux/octeon/patches-6.18/
# verify it applies clean (optional):
#   tar xf dl/linux-6.18.34.tar.xz && cd linux-6.18.34 && \
#   patch -p1 --dry-run < .../120-octeon-flowtable-hooks.patch
```

## 3. Build

```sh
cd <openwrt>
make target/linux/{clean,compile}        # rebuilds vmlinux with the patch
# module:
cd <this-repo>/src/octeon_flowtable
make octeon OWRT=<openwrt>                # auto-finds KDIR + toolchain
```

Or just select **kmod-octeon-flowtable** (drop `package/octeon-flowtable` into
`<openwrt>/package/`) and `make` the whole image.

## 4. Iterating on the *staging* driver → incremental rebuild + reflash

When you edit the staging hook (ethernet-rx/tx), a full `make` is slow. Incremental:

```sh
cd <openwrt>/build_dir/target-*/linux-octeon_generic/linux-6.18.34
make ARCH=mips CROSS_COMPILE=<tc>/mips64-openwrt-linux-musl- vmlinux -j$(nproc)
```

If you added an `EXPORT_SYMBOL` and `CONFIG_MODVERSIONS` is off, modpost won't have
it — append it to `Module.symvers` by hand (note the **trailing tab** = empty
namespace field, else "parse error in symbol dump file"):

```sh
printf '0x00000000\tYOUR_SYMBOL\tvmlinux\tEXPORT_SYMBOL\t\n' >> Module.symvers
```

Make the bootable `vmlinux.64`. The kernel reserves a 1 MB `.appended_dtb`
section; graft the *trimmed* DTB out of the currently-booting image so the size
matches exactly:

```sh
TC=<tc>/mips64-openwrt-linux-musl-
# one-time: pull the working DTB off the box
ssh root@<box> 'mount /dev/sda1 /tmp/b' 2>/dev/null
scp root@<box>:/tmp/b/vmlinux.64 /tmp/cur.64
${TC}objcopy -O binary --only-section=.appended_dtb /tmp/cur.64 /tmp/erlite.dtb
# build the image:
${TC}strip -R .notes vmlinux -o /tmp/v64.s
${TC}objcopy --update-section .appended_dtb=/tmp/erlite.dtb -R .comment /tmp/v64.s /tmp/v64.final
# /tmp/v64.final should be byte-for-byte the same SIZE as the box's vmlinux.64
```

Flash (the box boots `vmlinux.64` + a bare-md5 `vmlinux.64.md5` from `/dev/sda1`):

```sh
scp /tmp/v64.final root@<box>:/tmp/new.64
ssh root@<box> '
  mount /dev/sda1 /tmp/b
  cp /tmp/b/vmlinux.64 /tmp/b/vmlinux.64.bak      # keep a fallback
  cp /tmp/new.64 /tmp/b/vmlinux.64
  md5sum /tmp/b/vmlinux.64 | awk "{print \$1}" > /tmp/b/vmlinux.64.md5
  sync; umount /tmp/b; reboot'
```

The ERLite is effectively un-brickable: it keeps a backup image and U-Boot can
TFTP/`fatload` a recovery kernel over serial. Build the module *after* the reflash
(its vermagic must match the running kernel).

## Debugging tip (no serial)

`echo 0 > /proc/sys/kernel/panic_on_oops` before a risky test — a *survivable*
oops then leaves the box up with the full trace in `dmesg` instead of rebooting
(`CONFIG_PANIC_ON_OOPS=y` otherwise turns every oops into a reboot). Restore to 1
after. NB an oops inside `module_exit` leaves the module zombie (refcount -1) →
reboot to clear.
