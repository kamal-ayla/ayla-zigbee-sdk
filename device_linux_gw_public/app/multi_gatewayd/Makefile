# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = appd
DIR = app/multi_gatewayd
SRC = ../..


#
# ZigBee Stack SDK paths
#
EMBER_STACK_DIR = /home/pi/EmberZNet/v5.7.4.0
EMBER_PROJECT_NAME = ember
EMBER_LIB_DIR = $(EMBER_PROJECT_NAME)/lib
EMBER_LIB_NAME = em3588stack5.7.4
ZIGBEE_DIR = zigbee

#
# ZigBee protocol agent source files
#
ZIGBEE_SOURCES = \
	$(ZIGBEE_DIR)/$(EMBER_PROJECT_NAME)/$(EMBER_PROJECT_NAME)_callbacks.c \
	$(ZIGBEE_DIR)/$(EMBER_PROJECT_NAME)/call-command-handler.c \
	$(ZIGBEE_DIR)/$(EMBER_PROJECT_NAME)/callback-stub.c \
	$(ZIGBEE_DIR)/$(EMBER_PROJECT_NAME)/stack-handler-stub.c \
	$(ZIGBEE_DIR)/$(EMBER_PROJECT_NAME)/cli.c \
	$(ZIGBEE_DIR)/appd_interface.c \
	$(ZIGBEE_DIR)/zb_callback.c \
	$(ZIGBEE_DIR)/zb_interface.c


ZIGBEE_INCLUDES = \
  -I$(EMBER_STACK_DIR)/protocol/zigbee_5.7 \
  -I$(EMBER_STACK_DIR)/protocol/zigbee_5.7/stack \
  -I$(EMBER_STACK_DIR)/platform/base \
  -I$(EMBER_STACK_DIR)/platform/base/hal \
  -I$(ZIGBEE_DIR)/$(EMBER_PROJECT_NAME) \
  -I$(ZIGBEE_DIR) \
  -I./ \


APP_BUILDER_OUTPUT_DIRECTORY=.
APP_BUILDER_CONFIG_HEADER=$(APP_BUILDER_OUTPUT_DIRECTORY)/$(EMBER_PROJECT_NAME).h
APP_BUILDER_STORAGE_FILE=$(APP_BUILDER_OUTPUT_DIRECTORY)/$(EMBER_PROJECT_NAME)_endpoint_config.h

PLATFORM_HEADER_FILE ?= \"../../../platform/base/hal/micro/unix/compiler/gcc.h\"
BOARD_HEADER_FILE    ?= \"../../../platform/base/hal/micro/unix/host/board/host.h\"

ZIGBEE_DEFINES = \
  UNIX \
  UNIX_HOST \
  PHY_NULL \
  CONFIGURATION_HEADER=\"../../../protocol/zigbee_5.7/app/framework/util/config.h\" \
  EZSP_HOST \
  GATEWAY_APP \
  ZA_GENERATED_HEADER=\"$(APP_BUILDER_CONFIG_HEADER)\" \
  ATTRIBUTE_STORAGE_CONFIGURATION=\"$(APP_BUILDER_STORAGE_FILE)\" \
  PLATFORM_HEADER=$(PLATFORM_HEADER_FILE) \
  BOARD_HEADER=$(BOARD_HEADER_FILE) \
  EMBER_AF_API_EMBER_TYPES=\"stack/include/ember-types.h\" \
  EZSP_ASH=1 \


ZIGBEE_LDFLAGS = -Lzigbee/$(EMBER_LIB_DIR) -l$(EMBER_LIB_NAME) -lreadline -lncurses


#
# Bluetooth Stack SDK paths
#
BLUETOOTH_DIR = bluetooth

#
# Bluetooth protocol agent source files
#
BLUETOOTH_SOURCES = \
	$(BLUETOOTH_DIR)/bt_gatt.c \
	$(BLUETOOTH_DIR)/bt_interface.c \
	$(BLUETOOTH_DIR)/bt_utils.c \
	$(BLUETOOTH_DIR)/dbus_client.c \
	$(BLUETOOTH_DIR)/dbus_utils.c


ifeq ($(PROD),)
MACHINE := i386-linux-gnu
endif
MACHINE ?= $(shell $(CC) -dumpmachine)
BLUETOOTH_INCLUDES = -I/usr/include/dbus-1.0/ -I/usr/lib/$(MACHINE)/dbus-1.0/include/
BLUETOOTH_LDFLAGS = -ldbus-1


#
# List of source files to build
#
SOURCES = \
	$(ZIGBEE_SOURCES) \
	$(BLUETOOTH_SOURCES) \
	gateway.c \
	main.c \
	node.c \
	$(NULL)

#
# List of libraries to link
#
LIBS = ssl crypto curl jansson

#
# Compiler and linker flags
#
TARGET_CFLAGS += $(ZIGBEE_INCLUDES) $(BLUETOOTH_INCLUDES)
TARGET_LDFLAGS += $(ZIGBEE_LDFLAGS) $(BLUETOOTH_LDFLAGS)

#
# List of pre-processor definitions
#
DEFINES += $(ZIGBEE_DEFINES)
DEFINES += GATEWAY_SUPPORT

#
# Optional: For vi users, list any directories to make tags for in addition to
# the ones in common_defs.mk
#
TAGS_DIRS = $(NULL)

#
# Include common variables
#
include $(SRC)/make/common_defs.mk

#
# Build rules for this target
#
.PHONY: all default install

all: $(EXEC)

default: all

install: all
	$(INSTALL) $(BUILD)/$(EXEC) $(INSTALL_ROOT)/bin/$(EXEC)

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk

