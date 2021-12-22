# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = cond
DIR = daemon/cond
SRC = ../..

#
# Define product to Wi-Fi platform mappings below. New Wi-Fi platform
# implementations may be added as needed.  The default Wi-Fi platform
# is "supplicant", which selects the wifi_platform.c designed to use
# the wpa_supplicant and hostapd to manage Wi-Fi hardware.
#

# Sample Wi-Fi platform mapping
#ifeq ($(PROD),my_product)
#WIFI_PLATFORM := my_product_wifi_platform
#endif

# Generic platform with stub implementations
ifeq ($(PROD),generic)
WIFI_PLATFORM := generic
endif
ifeq ($(PROD),mt7688)
WIFI_PLATFORM := mt7688
endif
# Default Wi-Fi platform
WIFI_PLATFORM ?= supplicant

ifeq ($(PROD),raspberry_pi)
GATT_SOURCES = ble/gatt_service.c \
	ble/dbus_client.c \
	ble/dbus_utils.c
endif

#
# List of source files to build
#
SOURCES = \
	$(GATT_SOURCES) \
	cond.c \
	platform/$(WIFI_PLATFORM)/wifi_platform.c \
	wifi_utils.c \
	wifi.c \
	wifi_conf.c \
	wifi_interface.c \
	$(NULL)

#
# List of libraries to link
#
LIBS = jansson

#
# List of dependencies on Ayla libraries 
#
LIBDEPS = $(LIB_PLATFORM) $(LIB_AYLA)

#
# Compiler and linker flags
#
ifeq ($(PROD),raspberry_pi)
TARGET_CFLAGS += -DSUPPORT_BLE_WIFI_SETUP -I/usr/include/dbus-1.0 -I/usr/lib/arm-linux-gnueabihf/dbus-1.0/include
TARGET_LDFLAGS += -ldbus-1
endif

#
# List of pre-processor definitions
#
#DEFINES =

#
# Optional: For vi users, list any directories to make tags for in addition to
# the ones in common_defs.mk
#
TAGS_DIRS = $(NULL)

#
# Include optional Wi-Fi platform-specific makefile
#
-include platform/$(WIFI_PLATFORM)/*.mk

#
# Include common variables
#
include $(SRC)/make/common_defs.mk

#
# Build rules for this target
#
.PHONY: default install all

default: all

all: $(EXEC)

install: all
	$(INSTALL) $(BUILD)/$(EXEC) $(INSTALL_ROOT)/bin/$(EXEC)
	mkdir -p $(INSTALL_ROOT)/etc/config
	$(INSTALL) config/$(WIFI_PLATFORM)/* $(INSTALL_ROOT)/etc/config
	$(INSTALL) platform/$(WIFI_PLATFORM)/*.sh $(INSTALL_ROOT)/bin/

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk 

