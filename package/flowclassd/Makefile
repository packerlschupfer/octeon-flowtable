# SPDX-License-Identifier: GPL-2.0-or-later
#
# flowclassd — nDPI classification of forwarded flows before hardware offload.
#
# Classifies the first packets of each forwarded TCP/UDP flow via NFQUEUE and
# writes the result into the conntrack mark; fw4 (with the
# 100-flow-offloading-gate patch, src/fw4-patches/) then offloads the flow.
# The per-flow byte counters already reach conntrack from octeon_flowtable.

include $(TOPDIR)/rules.mk

PKG_NAME:=flowclassd
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=octeon-flowtable

# Single source of truth: the daemon lives with the rest of the project.
FC_SRC:=$(CURDIR)/../../src/flowclassd

include $(INCLUDE_DIR)/package.mk

define Package/flowclassd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=nDPI flow classifier gating hardware flow offload
  DEPENDS:=+libndpi +libnetfilter-queue +libnfnetlink +libmnl \
	+kmod-nft-queue +kmod-nf-conntrack-netlink +firewall4
endef

define Package/flowclassd/description
  Queues the first packets of each forwarded TCP/UDP flow (after fw4's forward
  filtering) to an nDPI daemon, which stores the application id in the
  conntrack mark (bit 31 = classified, bits 16-30 = nDPI protocol id). With
  firewall option flow_offloading_gate, fw4 only offloads a flow once it is
  classified, or after flow_offloading_gate_packets packets as a fallback.
endef

define Package/flowclassd/conffiles
/etc/config/flowclassd
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(FC_SRC)/flowclassd.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall \
		-o $(PKG_BUILD_DIR)/flowclassd $(PKG_BUILD_DIR)/flowclassd.c \
		$(TARGET_LDFLAGS) -lndpi -lnetfilter_queue -lnfnetlink -lmnl -lm
endef

define Package/flowclassd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config \
		$(1)/etc/uci-defaults $(1)/etc/sysctl.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/flowclassd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/flowclassd.init $(1)/etc/init.d/flowclassd
	$(INSTALL_CONF) ./files/flowclassd.config $(1)/etc/config/flowclassd
	$(INSTALL_DATA) ./files/flowclassd.uci-defaults $(1)/etc/uci-defaults/90-flowclassd
	$(INSTALL_DATA) ./files/flowclassd.sysctl $(1)/etc/sysctl.d/90-flowclassd.conf
endef

$(eval $(call BuildPackage,flowclassd))
