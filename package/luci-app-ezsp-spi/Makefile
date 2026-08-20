include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for the EZSP-SPI Zigbee router
LUCI_DESCRIPTION:=Status, join/leave and configuration for a Silabs EM35x \
	Zigbee NCP running as a mesh router.
LUCI_DEPENDS:=+ezsp-spi +rpcd
LUCI_PKGARCH:=all

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=nico

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
