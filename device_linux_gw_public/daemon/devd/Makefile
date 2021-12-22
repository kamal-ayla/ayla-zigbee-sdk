# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = devd
DIR = daemon/devd
SRC = ../..



#
# List of source files to build
#
SOURCES = \
	app_if.c \
	client_lan.c \
	dapi.c \
	devd_conf.c \
	dnss.c \
	ds_client.c \
	ds_main.c \
	msg_server.c \
	notify.c \
	ops_devd.c \
	props_client.c \
	props_if.c \
	serv.c \
	$(NULL)

# Optional components
-include gateway.mk

#
# List of libraries to link
#
LIBS = curl ssl crypto jansson pthread transformer

#
# List of dependencies on Ayla libraries 
#
LIBDEPS = $(LIB_PLATFORM) $(LIB_AYLA) $(STAGING_DIR)/usr/lib/libtransformer.so

#
# Compiler and linker flags
#
#TARGET_CFLAGS =
#TARGET_LDFLAGS =

#
# List of pre-processor definitions
#
ifeq ($(NOTIFY_DEBUG),1)
DEFINES += NP_DEBUG
endif

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
.PHONY: default install all

default: all

all: $(EXEC)

install: all
	$(INSTALL) $(BUILD)/$(EXEC) $(INSTALL_ROOT)/bin/$(EXEC)
	mkdir -p $(INSTALL_ROOT)/etc/config
	$(INSTALL) config/* $(INSTALL_ROOT)/etc/config
	mkdir -p $(INSTALL_ROOT)/etc/certs
	$(INSTALL) certs/* $(INSTALL_ROOT)/etc/certs

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk 

