# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

EXEC = appd
DIR = app/appd
SRC = ../..

#
# List of source files to build
#
SOURCES = \
	appd.c \
	main.c \
	$(NULL)

#
# List of libraries to link
#
LIBS = curl ssl crypto jansson 

#
# List of dependencies on Ayla libraries 
#
LIBDEPS = $(LIB_PLATFORM) $(LIB_AYLA) $(LIB_APP)

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
.PHONY: all default install

all: $(EXEC)

default: all

install: all
	$(INSTALL) $(BUILD)/$(EXEC) $(INSTALL_ROOT)/bin/$(EXEC)
	mkdir -p $(INSTALL_ROOT)/etc/files
	$(INSTALL) files/ayla_solution.png $(INSTALL_ROOT)/etc/files/ayla_solution.png

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk

