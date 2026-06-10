# SPDX-License-Identifier: GPL-2.0
#
# OpenWrt package for octeon_flowtable — the clean-room nftables flow-offload
# backend for the Cavium Octeon+ (CN50xx) packet complex.
#
# Drop this directory into an OpenWrt tree under package/ (or a feed) and select
# kmod-octeon-flowtable. It requires the staging-driver hook patch
# (target/linux/octeon/patches-6.18/120-octeon-flowtable-hooks.patch) to be
# present in the kernel — that patch adds cvm_oct_register_rx_hook,
# cvm_oct_transmit_qos, cvm_oct_get_port and cvm_oct_flow_aqm_fau.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=octeon-flowtable
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=octeon-flowtable

# Single source of truth: the module .c lives with the rest of the project.
FT_SRC:=$(CURDIR)/../../src/octeon_flowtable

include $(INCLUDE_DIR)/package.mk

define KernelPackage/octeon-flowtable
  SUBMENU:=Network Devices
  TITLE:=Octeon+ (CN50xx) nftables flow offload
  DEPENDS:=@TARGET_octeon +kmod-nft-offload +kmod-nf-flow
  FILES:=$(PKG_BUILD_DIR)/octeon_flowtable.ko
  AUTOLOAD:=$(call AutoProbe,octeon_flowtable)
endef

define KernelPackage/octeon-flowtable/description
  WQE-level RX hook + in-buffer NAT/VLAN(retag/push/pop/QinQ) rewrite + PKO
  transmit, IPv4/IPv6, per-flow software stats fed to conntrack, a global FAU
  counter, and a tail-drop AQM. Bypasses the Linux stack for forwarded flows;
  misses fall through to normal routing.
endef

define KernelPackage/octeon-flowtable/config
  # nothing tunable at build time
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(FT_SRC)/octeon_flowtable.c $(PKG_BUILD_DIR)/
	echo 'obj-m := octeon_flowtable.o' > $(PKG_BUILD_DIR)/Makefile
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)" modules
endef

# install the procd init script + default UCI config
define KernelPackage/octeon-flowtable/install
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/octeon-flowtable.init $(1)/etc/init.d/octeon-flowtable
	$(INSTALL_CONF) ./files/octeon-flowtable.config $(1)/etc/config/octeon-flowtable
endef

$(eval $(call KernelPackage,octeon-flowtable))
