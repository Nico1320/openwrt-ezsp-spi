include $(TOPDIR)/rules.mk

PKG_NAME:=ezsp-spi
PKG_VERSION:=0.1
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=nico

include $(INCLUDE_DIR)/package.mk

define Package/ezsp-spi
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=Zigbee
  TITLE:=EZSP-SPI host for Silabs EM35x Zigbee NCPs
  DEPENDS:=+kmod-spi-dev
endef

define Package/ezsp-spi/description
  Talks EZSP to a Silicon Labs EM35x Zigbee NCP (EM3581 and relatives) over
  SPI, and joins an existing Zigbee network as a router so the device acts as
  a repeater. The NCP's own stack does the routing; this only brings it up.

  Needed because the open source ecosystem speaks EZSP over ASH/UART only,
  and nothing speaks EZSP over SPI.

  Requires a device tree that exposes the NCP as a spidev and leaves its
  reset / wake / host-interrupt GPIOs free. See the README.
endef

define Package/ezsp-spi/conffiles
/etc/config/ezsp-spi
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) $(TARGET_LDFLAGS) \
		-Wall -Wextra -std=gnu99 -flto \
		-o $(PKG_BUILD_DIR)/ezsp-spi $(PKG_BUILD_DIR)/*.c
endef

define Package/ezsp-spi/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ezsp-spi $(1)/usr/sbin/ezsp-spi
	$(INSTALL_BIN) ./files/ezsp-spi-flash $(1)/usr/sbin/ezsp-spi-flash
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ezsp-spi.init $(1)/etc/init.d/ezsp-spi
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ezsp-spi.config $(1)/etc/config/ezsp-spi
	$(call Package/ezsp-spi/install/vendor,$(1))
endef

# Optional vendor support tarball for ezsp-spi-flash. Built locally with
# ./mkvendor.sh from a firmware image you own, and picked up automatically if
# present -- so a personal build ships it and flashing needs no manual steps.
#
# It is never committed and never distributed: TestNCP_SPI is a Linksys
# application over Silicon Labs' EmberZNet stack, proprietary and outside the
# GPL obligations covering the kernel and busybox in the same firmware.
ifneq ($(wildcard ./files/zbroot.tar.gz),)
define Package/ezsp-spi/install/vendor
	$(INSTALL_DIR) $(1)/root
	$(INSTALL_DATA) ./files/zbroot.tar.gz $(1)/root/zbroot.tar.gz
endef
else
define Package/ezsp-spi/install/vendor
	@echo "  note: files/zbroot.tar.gz absent -- ezsp-spi-flash will need it"
	@echo "        at /root/zbroot.tar.gz (see ./mkvendor.sh)"
endef
endif

$(eval $(call BuildPackage,ezsp-spi))
