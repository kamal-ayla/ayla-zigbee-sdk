# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = appd
DIR = dl/bt_gatewayd
SRC = ../..

#
# List of source files to build
#
SOURCES = \
	bt_gatt.c \
	bt_interface.c \
	bt_utils.c \
	dbus_client.c \
	dbus_utils.c \
	gateway.c \
	main.c \
	node.c \
	$(NULL)

#
# List of libraries to link
#
LIBS = ssl crypto curl jansson dbus-1

#
# List of dependencies on Ayla libraries 
#
LIBDEPS = $(LIB_PLATFORM) $(LIB_AYLA) $(LIB_APP)

#
# Compiler and linker flags
#
ifeq ($(PROD),)
MACHINE := i386-linux-gnu
endif
MACHINE ?= $(shell $(CC) -dumpmachine)
TARGET_CFLAGS = -I/usr/include/dbus-1.0/ -I/usr/lib/$(MACHINE)/dbus-1.0/include/
#TARGET_LDFLAGS =

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

all: $(EXEC)

default: all

install: all
	$(INSTALL) $(BUILD)/$(EXEC) $(INSTALL_ROOT)/bin/$(EXEC)

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk

