# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = ota_update
DIR = util/ota
SRC = ../..

#
# List of source files to build
#
SOURCES = \
	ota_download.c \
	ota_update.c \
	$(NULL)

#
# List of libraries to link
#
ifeq ($(PROD),raspberry_pi)
LIBS = ayla curl ssl crypto jansson pthread
else
LIBS = curl ssl crypto jansson pthread
endif

#
# Compiler and linker flags
#
#TARGET_CFLAGS =
#TARGET_LDFLAGS =

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

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk 

