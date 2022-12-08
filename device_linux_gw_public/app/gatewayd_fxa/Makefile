# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = appd
DIR = app/gatewayd_fxa
SRC = ../..


#
# Compiler and linker flags
#



TARGET_LDFLAGS += -L$(STAGING_DIR)/usr/lib/
TARGET_LDLIBS += -pthread -lubox -ltransformer


#
# AT&T virtual node paths
#
ATT_DIR = nodes

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
	$(ATT_SOURCES) \
	gateway.c \
	main.c \
	node.c \
	$(NULL)

#
# List of libraries to link
#
LIBS = ssl crypto curl jansson transformer pthread ubox opkg bb

#
# Compiler and linker flags
#
TARGET_CFLAGS +=   $(ATT_INCLUDES)
TARGET_LDFLAGS +=   $(ATT_LDFLAGS)

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

