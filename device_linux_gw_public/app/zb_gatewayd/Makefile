# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = appd
DIR = app/zb_gatewayd
SRC = ../..

EMBER_STACK_DIR = ../../../v3.2
EMBER_PROJECT_NAME = ember
EMBER_LIB_DIR = $(EMBER_STACK_DIR)/app/builder/$(EMBER_PROJECT_NAME)/build/exe
#EMBER_LIB_DIR = /home/rajandi/ayla_owa/v2.7/app/builder/ember/build/exe
#EMBER_LIB_NAME = em3588stack6.7.6

EMBER_LIB_NAME = Z3GatewayHost_3_2
CALLBACK_STUB_UPDATED= $(OBJ_DIR)/callback-stub-updated


#
# AT&T virtual node paths
#
ATT_DIR = att

#
# AT&T source files
#
ATT_SOURCES = \
        $(ATT_DIR)/vt_interface.c \
        $(ATT_DIR)/att_interface.c

ATT_INCLUDES = \
        -I$(ATT_DIR) \
        -I./ \

ATT_LDFLAGS =



#
# List of source files to build
#
SOURCES = \
	$(EMBER_PROJECT_NAME)/$(EMBER_PROJECT_NAME)_callbacks.c \
	$(EMBER_PROJECT_NAME)/call-command-handler.c \
	$(EMBER_PROJECT_NAME)/callback-stub.c \
	$(EMBER_PROJECT_NAME)/stack-handler-stub.c \
	$(EMBER_PROJECT_NAME)/znet-cli.c \
	$(ATT_SOURCES) \
	appd_interface.c \
	zb_callback.c \
	zb_interface.c \
	gateway.c \
	main.c \
	node.c \
  speedtest.c \
	$(NULL)

#
# List of libraries to link
#
LIBS = ssl crypto curl jansson transformer pthread ubox opkg bb
#LIBDEPS = $(STAGING_DIR)/usr/lib/libfcgi.a

INCLUDES= \
  -I$(EMBER_STACK_DIR)/protocol/zigbee \
  -I$(EMBER_STACK_DIR)/protocol/zigbee/stack \
  -I$(EMBER_STACK_DIR)/platform/base \
  -I$(EMBER_STACK_DIR)/platform/base/hal \
  -I$(EMBER_STACK_DIR)/platform/radio/mac/ \
  -I$(EMBER_STACK_DIR)/platform/common/inc \
  -I$(EMBER_STACK_DIR)/util/silicon_labs/silabs_core \
  -I$(EMBER_STACK_DIR)/protocol/zigbee/app/framework/include \
  -I$(EMBER_PROJECT_NAME) \
  -I$(PKG_BUILD_DIR)/libopkg \
#
# Compiler and linker flags
#
TARGET_CFLAGS = $(INCLUDES) $(ATT_INCLUDES)

#TARGET_LDFLAGS = -l$(EMBER_LIB_NAME) -L$(EMBER_LIB_DIR) -lreadline -lncurses
TARGET_LDFLAGS += -L$(PKG_BUILD_DIR)/libopkg/
TARGET_LDFLAGS += -L$(STAGING_DIR)/usr/lib/
TARGET_LDLIBS += -pthread -lubox -ltransformer -lopkg -lbb
TARGET_LDFLAGS += -l$(EMBER_LIB_NAME) -L$(EMBER_LIB_DIR)



APP_BUILDER_OUTPUT_DIRECTORY=.
APP_BUILDER_CONFIG_HEADER=$(APP_BUILDER_OUTPUT_DIRECTORY)/ember.h
APP_BUILDER_STORAGE_FILE=$(APP_BUILDER_OUTPUT_DIRECTORY)/ember_endpoint_config.h

PLATFORM_HEADER_FILE ?= \"../../../platform/base/hal/micro/unix/compiler/gcc.h\"
BOARD_HEADER_FILE    ?= \"../../../platform/base/hal/micro/unix/host/board/host.h\"

DEFINES = \
  UNIX \
  UNIX_HOST \
  PHY_NULL \
  CONFIGURATION_HEADER=\"../../../protocol/zigbee/app/framework/util/config.h\" \
  EZSP_HOST \
  GATEWAY_APP \
  ZA_GENERATED_HEADER=\"$(APP_BUILDER_CONFIG_HEADER)\" \
  ATTRIBUTE_STORAGE_CONFIGURATION=\"$(APP_BUILDER_STORAGE_FILE)\" \
  PLATFORM_HEADER=$(PLATFORM_HEADER_FILE) \
  BOARD_HEADER=$(BOARD_HEADER_FILE) \
  EMBER_AF_API_EMBER_TYPES=\"stack/include/ember-types.h\" \
  EZSP_ASH=1 \
  

#
# List of pre-processor definitions
#
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


UPDATED = $(BUILD)/callback-stub-updated
$(UPDATED): $(EMBER_PROJECT_NAME)/callback-stub.c
	sed -i 's/emberAfPreMessageReceivedCallback(/emberAfPreMessageReceivedCallback_RedefinedByAppd(/g' $(EMBER_PROJECT_NAME)/callback-stub.c
	sed -i 's/emberAfStackStatusCallback(/emberAfStackStatusCallback_RedefinedByAppd(/g' $(EMBER_PROJECT_NAME)/callback-stub.c
	sed -i 's/emberAfTrustCenterJoinCallback(/emberAfTrustCenterJoinCallback_RedefinedByAppd(/g' $(EMBER_PROJECT_NAME)/callback-stub.c
	sed -i 's/emberAfMessageSentCallback(/emberAfMessageSentCallback_RedefinedByAppd(/g' $(EMBER_PROJECT_NAME)/callback-stub.c
	sed -i 's/emberAfNcpInitCallback(/emberAfNcpInitCallback_RedefinedByAppd(/g' $(EMBER_PROJECT_NAME)/callback-stub.c
	touch $(UPDATED)


all: $(UPDATED) $(EXEC)

default: all

install: all
	$(INSTALL) $(BUILD)/$(EXEC) $(INSTALL_ROOT)/bin/$(EXEC)

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk

