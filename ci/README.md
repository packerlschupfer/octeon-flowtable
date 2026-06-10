# ci/ — reproducible image build inputs

These files drive [`.github/workflows/build-image.yml`](../.github/workflows/build-image.yml),
which builds a flashable OpenWrt image for the EdgeRouter Lite (Octeon+ CN5020)
with the hardware flow offload baked into the kernel.

## Why a full image and not just the `.ipk`/`.apk`

The staging `octeon_ethernet` driver is **built into `vmlinux`**, and
`octeon_flowtable.ko` links against symbols only the *patched* kernel exports
(`cvm_oct_register_rx_hook`, `cvm_oct_transmit_qos`, `cvm_oct_get_port`,
`cvm_oct_flow_aqm_fau`) — plus the module's vermagic must match the running kernel.
So a stock-kernel OpenWrt image + a sideloaded package **cannot** work: the patch
lives in the kernel. The only viable deliverable is a from-source image build, and
that is what CI produces.

## Files

| file | role |
|---|---|
| `feeds.conf` | pinned `packages` + `luci` feeds (the workflow appends a `src-link` to this repo) |
| `config.seed` | **lean** variant — ERLite target + the offload + conntrack/tcpdump |
| `config-router.seed` | **router** variant — lean + LuCI web UI + WireGuard (more turnkey) |
| `apply-target-tweaks.sh` | re-applies CVMSEG=2, drops `OCTEON_ILM`, adds the `receive_group_order=1` cmdline |

The workflow builds both variants in parallel (a matrix) and publishes them in one
release; image filenames are prefixed `lean-` / `router-` since both build the same
device. Add a third `config-*.seed` + a matrix entry to ship another variant.

The staging kernel patch itself lives at
[`../src/staging-patches/120-octeon-flowtable-hooks.patch`](../src/staging-patches/);
the workflow copies it into `target/linux/octeon/patches-6.18/` before building.

## Pinning / reproducibility

Everything is pinned so a given tag reproduces:

- **OpenWrt**: `OPENWRT_REF` in the workflow (`84f4f77`, main @ 2026-06-03, kernel 6.18.34).
- **packages feed**: the `^<rev>` in `feeds.conf`.
- The driver `.c` and the staging patch are in this repo.

The one remaining non-determinism is the host toolchain (Ubuntu runner image); for
the offload itself that's immaterial. To move to a newer base, bump `OPENWRT_REF`
**and** the feed rev together, then re-validate that `120-…patch` still applies (CI
fails loudly with a `.rej` if it doesn't).

## Triggers & cost

- Push a tag `v*` → build **and** publish a GitHub Release with the image + `sha256sums.txt`.
- `workflow_dispatch` → build only, upload artifacts (no release).

A cold build (toolchain from source) is ~45–90 min on a GitHub-hosted runner; the
two variants build on parallel runners. The `dl/` and toolchain caches make reruns
much faster. Free for public repos.

## Reproduce locally

```sh
git clone https://git.openwrt.org/openwrt/openwrt.git && cd openwrt
git checkout 84f4f779204a3de631670fee69141188b56e4976
cp /path/to/octeon-flowtable/ci/feeds.conf feeds.conf
echo "src-link flowoffload /path/to/octeon-flowtable/package" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a -p flowoffload
./scripts/feeds install kmod-octeon-flowtable conntrack tcpdump
cp /path/to/octeon-flowtable/src/staging-patches/120-octeon-flowtable-hooks.patch \
   target/linux/octeon/patches-6.18/
sh /path/to/octeon-flowtable/ci/apply-target-tweaks.sh "$PWD"
cp /path/to/octeon-flowtable/ci/config.seed .config && make defconfig   # or config-router.seed
make download -j$(nproc) && make -j$(nproc)
# image: bin/targets/octeon/generic/*edgerouter-lite*
```
