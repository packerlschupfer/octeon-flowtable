# Building octeon-flowtable from source

This builds three things: a **patched kernel** (the staging `octeon_ethernet`
driver gains the offload hooks), the **module** `octeon_flowtable.ko`, and the
**OpenWrt package** that wraps it. The driver targets the **octeon** OpenWrt
target (CN50xx — e.g. the EdgeRouter Lite 3).

> Don't need to build at all? Prebuilt images are on the [Releases](../../releases)
> page (CI builds them from this repo — see [`ci/`](../ci/)); flashing is in
> [INSTALLING.md](INSTALLING.md). Build from source only if you want to change the
> code or target a different base.

## 1. Prerequisites

- A Linux host with a working **OpenWrt buildroot** for the `octeon` target.
  ```sh
  git clone https://github.com/openwrt/openwrt
  cd openwrt
  ./scripts/feeds update -a && ./scripts/feeds install -a
  make menuconfig      # Target System = "Cavium Networks Octeon"
                       # Target Profile = "Ubiquiti EdgeRouter Lite" (or generic)
  make defconfig
  make -j$(nproc) tools/install toolchain/install   # first time only; slow
  ```
- This repo checked out somewhere (referred to below as `<repo>`).
- The build was developed against **OpenWrt's kernel 6.18.34**; other 6.x should
  work but the staging patch path (`patches-6.18`) and line offsets may need a
  refresh.

## 2. Add the staging hook patch

The offload needs a few exported hooks added to the in-tree (built-in)
`octeon_ethernet` driver. Copy the patch into the target's patch directory:

```sh
cp <repo>/src/staging-patches/120-octeon-flowtable-hooks.patch \
   <openwrt>/target/linux/octeon/patches-6.18/
```

It adds: `cvm_oct_register_rx_hook` / `cvm_oct_unregister_rx_hook` (the WQE-level
RX intercept in `cvm_oct_poll`), `cvm_oct_transmit_qos` (WQE→PKO egress, with the
AQM FAU op), `cvm_oct_get_port`, and `cvm_oct_flow_aqm_fau`. It applies clean
against pristine 6.18.34 — verify if you like:

```sh
cd <openwrt> && tar xf dl/linux-6.18.34.tar.xz && cd linux-6.18.34
patch -p1 --dry-run < ../target/linux/octeon/patches-6.18/120-octeon-flowtable-hooks.patch
```

## 3. Kernel config (the line-rate baseline)

In `make kernel_menuconfig` (or the target config), set:

- `CONFIG_CAVIUM_OCTEON_CVMSEG_SIZE=2`  — async IOBDMA scratch
- `# CONFIG_OCTEON_ILM is not set`
- `# CONFIG_MODVERSIONS is not set`  — OpenWrt default

And add `octeon_ethernet.receive_group_order=1` to the **boot cmdline** (PIP
GRPTAG hash-steering — the lever that gets multi-flow forwarding to line rate).
For a packaged image add it to `ERLITE_CMDLINE` in
`target/linux/octeon/image/Makefile`; on a USB/U-Boot setup put it in `bootcmd`.

## 4. Add the module package

```sh
cp -r <repo>/package/octeon-flowtable <openwrt>/package/
make menuconfig    # Kernel modules → Network Devices → <M> kmod-octeon-flowtable
```

The package compiles `octeon_flowtable.c`, autoloads the `.ko`, and installs the
procd init script + `/etc/config/octeon-flowtable`.

## 5. Build

```sh
cd <openwrt>
make -j$(nproc)                    # full image (kernel with the patch + package)
# or just the kernel + module bits:
make target/linux/compile
make package/octeon-flowtable/compile
```

The image lands in `bin/targets/octeon/generic/`. Flash it per
[INSTALLING.md](INSTALLING.md).

### Building the module standalone (without a full image)

```sh
cd <repo>/src/octeon_flowtable
make octeon OWRT=<openwrt>          # auto-finds KDIR + the mips64 toolchain
# or fully manual:
make KDIR=<openwrt>/build_dir/target-*/linux-octeon_generic/linux-6.18.34 \
     ARCH=mips CROSS_COMPILE=<openwrt>/staging_dir/toolchain-*/bin/mips64-openwrt-linux-musl-
```

Produces `octeon_flowtable.ko` (ELF64 MSB MIPS64). The `missing
MODULE_DESCRIPTION` modpost warning is OpenWrt's `CONFIG_MODULE_STRIPPED`
behaviour, not a defect. The module's vermagic must match the kernel you load it
on.

## 6. Iterating on the staging driver (incremental rebuild + reflash)

Editing the staging hooks means a kernel rebuild. The full recipe — incremental
`make vmlinux`, the `Module.symvers` trailing-tab fixup for new exports, building
the bootable `vmlinux.64` (strip + DTB graft), and the flash/recovery steps for a
USB-booted ERLite — is in **[prompts/build.md](../prompts/build.md)**.

## Troubleshooting

- **`.rej` when the patch applies**: your kernel version differs from 6.18.34;
  regenerate the patch against your tree (it's a plain diff of four staging files
  under `drivers/staging/octeon/`).
- **`insmod`: "Unknown symbol cvm_oct_…"**: the running kernel doesn't have the
  staging patch (or you flashed the wrong vmlinux). Rebuild + reflash the kernel.
- **vermagic mismatch on load**: build the module against the *same* kernel tree
  you flashed.
