# SPDX-License-Identifier: GPL-2.0
#
# luci-app-trafficd: a LuCI menu entry (Status > Traffic) showing trafficd's page.
# The page stays reachable at /traffic/ without a login: this adds a link, not access control.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-trafficd
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=cavium-offload-port

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-trafficd
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI menu entry for trafficd
  DEPENDS:=+luci-base +trafficd
  PKGARCH:=all
endef

define Package/luci-app-trafficd/description
  Adds Status > Traffic to LuCI, showing the trafficd page.
endef

define Build/Compile
endef

define Package/luci-app-trafficd/install
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d \
		$(1)/www/luci-static/resources/view/trafficd
	$(INSTALL_DATA) ./files/menu.json $(1)/usr/share/luci/menu.d/luci-app-trafficd.json
	$(INSTALL_DATA) ./files/acl.json $(1)/usr/share/rpcd/acl.d/luci-app-trafficd.json
	$(INSTALL_DATA) ./files/index.js $(1)/www/luci-static/resources/view/trafficd/index.js
endef

define Package/luci-app-trafficd/postinst
#!/bin/sh
[ -n "$$IPKG_INSTROOT" ] || rm -f /tmp/luci-indexcache* 2>/dev/null
exit 0
endef

$(eval $(call BuildPackage,luci-app-trafficd))
