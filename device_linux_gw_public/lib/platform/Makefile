#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

LIB = libplatform.a
DIR = lib/platform
SRC = ../..

#
# Default product
#
DEFAULT_PROD := generic

#
# If a PROD variable was not passed in to the make job, use the default
#
PROD ?= $(DEFAULT_PROD)

#
# List of required source files to build
#
LIB_SOURCE_FILES := \
	conf.c \
	crypto.c \
	ota.c \
	system.c \
	$(NULL)

#
# Build all source files in the platform-specific directory
# (specified by the PROD variable).  Use the default implementation for
# any required source files not included in the platform-specific directory.
#
ifneq ($(PROD),$(DEFAULT_PROD))
PLATFORM_SOURCES := $(notdir $(wildcard $(PROD)/*.c))
SOURCES += $(PLATFORM_SOURCES:%.c=$(PROD)/%.c)
endif
DEFAULT_SOURCES := $(filter-out $(PLATFORM_SOURCES), $(LIB_SOURCE_FILES))
SOURCES += $(DEFAULT_SOURCES:%.c=$(DEFAULT_PROD)/%.c)

#
# List of libraries to link
#
#LIBS =

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
# Include optional platform-specific makefile
#
-include $(PROD)/*.mk

#
# Include common variables
#
include $(SRC)/make/common_defs.mk

#
# Build rules for this target
#
.PHONY: default install all

default: all

all: $(LIB)

install: all
	$(INSTALL) $(BUILD)/$(LIB) $(INSTALL_ROOT)/$(DIR)/$(LIB)

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk 

