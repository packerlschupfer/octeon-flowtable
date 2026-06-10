# Out-of-tree build for octeon_flowtable.ko
#
# The kernel must already carry the staging-driver hook patch
# (../staging-patches/120-octeon-flowtable-hooks.patch) — it exports the symbols
# this module links against (cvm_oct_register_rx_hook, cvm_oct_transmit_qos,
# cvm_oct_get_port, cvm_oct_flow_aqm_fau).
#
# Generic cross build against any prepared kernel tree:
#   make KDIR=/path/to/linux ARCH=mips CROSS_COMPILE=mips64-...-
#
# Convenience build against an OpenWrt octeon build tree (auto-detects the
# kernel build dir + the mips64 toolchain under it):
#   make octeon OWRT=/path/to/openwrt

obj-m += octeon_flowtable.o

KDIR ?= /lib/modules/$(shell uname -r)/build
OWRT ?= /path/to/openwrt

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

octeon:
	@set -e; \
	KB=$$(ls -d $(OWRT)/build_dir/target-mips64_octeon*/linux-octeon_generic/linux-6.* 2>/dev/null | head -1); \
	TC=$$(ls -d $(OWRT)/staging_dir/toolchain-mips64_octeon*_gcc-*/bin 2>/dev/null | head -1); \
	if [ -z "$$KB" ]; then echo "kernel build dir not found under $(OWRT) — set OWRT=/path/to/openwrt"; exit 1; fi; \
	if [ -z "$$TC" ]; then echo "toolchain not found under $(OWRT) — build the OpenWrt target first"; exit 1; fi; \
	CROSS=$$(ls $$TC/mips64-openwrt-linux-*-gcc 2>/dev/null | head -1 | sed 's/gcc$$//'); \
	echo "KDIR=$$KB"; echo "CROSS_COMPILE=$$CROSS"; \
	$(MAKE) -C $$KB M=$(CURDIR) ARCH=mips CROSS_COMPILE=$$CROSS modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null || \
		rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions
