include $(TOPDIR)/rules.mk

PKG_NAME:=ayla-zigbee-sdk
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)
PKG_INSTALL_DIR := $(PKG_BUILD_DIR)/device_linux_gw_public/build/$(TARGET_CROSS)/obj

include $(INCLUDE_DIR)/package.mk

define Package/ayla-zigbee-sdk
	SECTION:=utils
	CATEGORY:=Utilities
	DEPENDS:= +libcurl +jansson +nginx +fcgi +spawn-fcgi +transformer-tch
	TITLE:=AYLA
endef


define Package/ayla-zigbee-sdk/description
	Ayla Module
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE)  COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc COMPILER_INCLUDES=-I$(TOOLCHAIN_DIR)/usr/include/ LINKER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc ARCHIVE=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)ar  -C $(PKG_BUILD_DIR)/v2.7/app/builder/ember \
	GENERATE_LIBRARY=1 \
	NO_READLINE=1 \
	STAGING_DIR="$(STAGING_DIR)" \
	TOOLCHAIN_DIR="$(TOOLCHAIN_DIR)" \
	TARGET_CROSS="$(TARGET_CROSS)"
	
	$(MAKE) COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc COMPILER_INCLUDES=-I$(TOOLCHAIN_DIR)/usr/include/ LINKER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc ARCHIVE=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)ar -C $(PKG_BUILD_DIR)/device_linux_gw_public \
	PROD="generic" \
	APP="zb_gatewayd" \
	NO_WIFI=1 \
	STAGING_DIR="$(STAGING_DIR)" \
	TOOLCHAIN_DIR="$(TOOLCHAIN_DIR)" \
	TARGET_CROSS="$(TARGET_CROSS)" \
	PKG_BUILD_DIR="$(PKG_BUILD_DIR)"
	
	$(MAKE) COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc COMPILER_INCLUDES=-I$(TOOLCHAIN_DIR)/usr/include/ LINKER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc ARCHIVE=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)ar -C $(PKG_BUILD_DIR)/device_linux_gw_public/host_util/config_gen \
	STAGING_DIR="$(STAGING_DIR)" \
	TOOLCHAIN_DIR="$(TOOLCHAIN_DIR)" \
	TARGET_CROSS="$(TARGET_CROSS)"
endef
define Package/ayla-zigbee-sdk/install
	$(INSTALL_DIR) $(1)/bin
	$(INSTALL_BIN) ./files/apply_ota.sh $(1)/bin/apply_ota.sh
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/daemon/devd/devd $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/app/zb_gatewayd/appd $(1)/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/acgi/acgi $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/acli/acli $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/ota/ota_update $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/gw_setup_agent/gw_setup_agent $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/host_util/config_gen/config_gen $(1)/usr/bin
	$(INSTALL_DIR) $(1)/lib/app
	$(CP) $(PKG_INSTALL_DIR)/lib/app/libapp.a $(1)/lib/app
	$(INSTALL_DIR) $(1)/lib/ayla
	$(CP) $(PKG_INSTALL_DIR)/lib/ayla/libayla.a $(1)/lib/ayla
	$(INSTALL_DIR) $(1)/lib/platform
	$(CP) $(PKG_INSTALL_DIR)/lib/platform/libplatform.a $(1)/lib/platform
	$(INSTALL_DIR) $(1)/etc/ssl/certs
	$(CP) $(PKG_BUILD_DIR)/device_linux_gw_public/daemon/devd/certs/* $(1)/etc/ssl/certs
	$(INSTALL_DIR) $(1)/etc/init.d/
	$(INSTALL_BIN) ./files/ayla.init $(1)/etc/init.d/ayla
	$(INSTALL_DIR) $(1)/www/docroot
	$(CP) ./files/docroot/* $(1)/www/docroot
	$(INSTALL_DIR) $(1)/etc/ayla
	$(INSTALL_BIN) ./files/cdc-acm.ko $(1)/etc/ayla



endef


$(eval $(call BuildPackage,ayla-zigbee-sdk))

