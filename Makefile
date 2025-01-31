include $(TOPDIR)/rules.mk

PKG_NAME:=ayla-zigbee-sdk
PKG_RELEASE:=1.11

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)
PKG_INSTALL_DIR := $(PKG_BUILD_DIR)/device_linux_gw_public/build/$(TARGET_CROSS)/obj

# Deps
GST_INCLUDE=$(STAGING_DIR)/usr/include/gstreamer-1.0
GLIB_INCLUDE=$(STAGING_DIR)/usr/include/glib-2.0

# CMake
CMAKE_INCLUDES="$(GST_INCLUDE);$(GLIB_INCLUDE);$(TOOLCHAIN_DIR)/usr/include"
CMAKE_LIBRARY_PATH="$(STAGING_DIR)/usr/lib;$(STAGING_DIR)/root-brcmbca/lib"
CMAKE_C_FLAGS="-Wl,-verbose -I$(GST_INCLUDE) -I$(GLIB_INCLUDE) -L$(STAGING_DIR)/usr/lib -L$(STAGING_DIR)/root-brcmbca/lib"
CMAKE_TOOLCHAIN_FLAGS=-DCMAKE_C_COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc -DCMAKE_CXX_COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)g++ -DCMAKE_LINKER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc -DCMAKE_AR=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)ar
CMAKE_DEFS=$(CMAKE_TOOLCHAIN_FLAGS) -DCMAKE_C_FLAGS=$(CMAKE_C_FLAGS) -DCMAKE_CXX_FLAGS=$(CMAKE_C_FLAGS)

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
	cd $(PKG_BUILD_DIR)/kvsd_apps && \
	echo $(CMAKE_C_FLAGS) > ./cmake_c_flags.cmake && \
	echo $(CMAKE_TOOLCHAIN_FLAGS) > ./cmake_toolchain_flags.cmake && \
	echo $(CMAKE_INCLUDES) > ./cmake_includes.cmake && \
	echo $(CMAKE_LIBRARY_PATH) > ./cmake_library_path.cmake

	$(MAKE)  COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc COMPILER_INCLUDES=-I$(TOOLCHAIN_DIR)/usr/include/ LINKER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc ARCHIVE=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)ar  -C $(PKG_BUILD_DIR)/v3.2/app/builder/ember \
	GENERATE_LIBRARY=1 \
	NO_READLINE=1 \
	STAGING_DIR="$(STAGING_DIR)" \
	TOOLCHAIN_DIR="$(TOOLCHAIN_DIR)" \
	TARGET_CROSS="$(TARGET_CROSS)"

	$(MAKE) COMPILER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc COMPILER_INCLUDES=-I$(TOOLCHAIN_DIR)/usr/include/ LINKER=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)gcc ARCHIVE=$(TOOLCHAIN_DIR)/bin/$(TARGET_CROSS)ar -C $(PKG_BUILD_DIR)/device_linux_gw_public \
	PROD="generic" \
	APP="kvsd" \
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
	$(INSTALL_DIR) $(1)/usr/bin
	# $(INSTALL_BIN) $(PKG_BUILD_DIR)/kvsd_apps/kvsd_stream_hls/build/kvsd_stream_hls $(1)/usr/bin
	# $(INSTALL_BIN) $(PKG_BUILD_DIR)/kvsd_apps/kvsd_stream_master/build/kvsd_stream_master $(1)/usr/bin

	# $(INSTALL_DIR) $(1)/usr/bin/test
	# $(CP) $(PKG_BUILD_DIR)/kvsd_apps/amazon-kinesis-video-streams-producer-sdk-cpp/build/* $(1)/usr/bin/test

	$(INSTALL_DIR) $(1)/bin
	$(INSTALL_BIN) ./files/apply_ota.sh $(1)/bin/apply_ota.sh
	$(INSTALL_BIN) ./files/get_sysinfo.sh $(1)/bin/get_sysinfo.sh
	$(INSTALL_BIN) ./files/get_stainfo.sh $(1)/bin/get_stainfo.sh
	$(INSTALL_BIN) ./files/devd_config.sh $(1)/bin/devd_config.sh
	$(INSTALL_BIN) ./files/decision_tree.sh $(1)/bin/decision_tree.sh
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_BIN) ./files/radio_fw_version.conf $(1)/etc/config/radio_fw_version.conf
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/daemon/devd/devd $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/app/kvsd/kvsd $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/acgi/acgi $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/acli/acli $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/ota/ota_update $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/util/gw_setup_agent/gw_setup_agent $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/host_util/config_gen/config_gen $(1)/usr/bin
	#$(INSTALL_BIN) ./files/kvs_streaming_rtsp.sh $(1)/usr/bin/kvs_streaming_rtsp.sh
	$(INSTALL_DIR) $(1)/lib/app
	$(CP) $(PKG_INSTALL_DIR)/lib/app/libapp.a $(1)/lib/app
	$(INSTALL_DIR) $(1)/lib/ayla
	$(CP) $(PKG_INSTALL_DIR)/lib/ayla/libayla.a $(1)/lib/ayla
	$(INSTALL_DIR) $(1)/lib/platform
	$(CP) $(PKG_INSTALL_DIR)/lib/platform/libplatform.a $(1)/lib/platform
	$(INSTALL_DIR) $(1)/etc/ssl/certs
	$(CP) $(PKG_BUILD_DIR)/device_linux_gw_public/daemon/devd/certs/* $(1)/etc/ssl/certs
	$(INSTALL_DIR) $(1)/etc/ayla/ssl/certs
	$(CP) ./files/cert.pem $(1)/etc/ayla/ssl/certs/
	$(INSTALL_DIR) $(1)/etc/init.d/
	$(INSTALL_BIN) ./files/ayla.init $(1)/etc/init.d/ayla
	$(INSTALL_DIR) $(1)/www/docroot
	$(CP) ./files/docroot/* $(1)/www/docroot
	$(INSTALL_DIR) $(1)/etc/ayla
	$(INSTALL_BIN) ./files/cdc-acm.ko $(1)/etc/ayla



endef


$(eval $(call BuildPackage,ayla-zigbee-sdk))

