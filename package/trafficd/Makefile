# SPDX-License-Identifier: GPL-2.0
#
# trafficd — per-client / per-application traffic accounting + a static
# web view. Reads conntrack over ctnetlink;
# apps come from flowclassd's conntrack mark, offloaded bytes from
# octeon_flowtable's stats feedback. The page is served by uhttpd at /traffic/.

include $(TOPDIR)/rules.mk

PKG_NAME:=trafficd
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=octeon-flowtable

TD_SRC:=$(CURDIR)/../../src/trafficd

include $(INCLUDE_DIR)/package.mk

define Package/trafficd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Per-client / per-application traffic analysis
  DEPENDS:=+libnetfilter-conntrack +kmod-nf-conntrack-netlink +uhttpd
endef

define Package/trafficd/description
  Accounts forwarded traffic per LAN client (by MAC, named from dnsmasq
  or ISC dhcpd leases) and per application (nDPI id from flowclassd's conntrack mark),
  keeps 24 h at 5-minute and 31 days at hourly resolution, and serves a
  self-contained page at http://<router>/traffic/.
endef

define Package/trafficd/conffiles
/etc/config/trafficd
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(TD_SRC)/trafficd.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall \
		-o $(PKG_BUILD_DIR)/trafficd $(PKG_BUILD_DIR)/trafficd.c \
		$(TARGET_LDFLAGS) -lnetfilter_conntrack -lnfnetlink -lmnl
endef

define Package/trafficd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config \
		$(1)/www/traffic $(1)/lib/upgrade/keep.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/trafficd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/trafficd.init $(1)/etc/init.d/trafficd
	$(INSTALL_CONF) ./files/trafficd.config $(1)/etc/config/trafficd
	$(INSTALL_DATA) ./files/www/index.html $(1)/www/traffic/index.html
	$(LN) /tmp/trafficd $(1)/www/traffic/data
	echo /etc/trafficd/ > $(1)/lib/upgrade/keep.d/trafficd
endef

$(eval $(call BuildPackage,trafficd))
