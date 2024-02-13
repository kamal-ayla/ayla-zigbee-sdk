#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

#
# Toolchain and dependency setup
#
# For cross compilation, define:
#   TOOLCHAIN_DIR - where to find standard libraries for the target plaform
#   ARCH - the compiler prefix
#   EXTLIB_DIR - where to find required external libraries
#
# For native compilation, these variables can be left undefined
#

#export ARCH = $(TARGET_CROSS)
#ARCH = arm-openwrt-linux
ARCH = $(TARGET_CROSS)
export STAGING_DIR = $(STAGING_DIR)


ifneq ($(TOOLCHAIN_DIR),)
	TOOLCHAIN_DIR := $(abspath $(TOOLCHAIN_DIR))
	TOOL_DIR = $(TOOLCHAIN_DIR)/bin/
	TARGET_CFLAGS += -I$(TOOLCHAIN_DIR)/include
	TARGET_LDFLAGS += -L$(TOOLCHAIN_DIR)/lib 
endif
ifneq ($(ARCH),native)
	export ARCH
	export TOOL_ROOT = $(TOOL_DIR)$(ARCH)
	export CC = $(TOOL_ROOT)gcc
	export AR = $(TOOL_ROOT)ar
endif

#
# Install directory
#
# INSTALL_ROOT can be defined externally to
# customize the make install behavior
#
INSTALL_ROOT ?= $(SRC)/build/$(ARCH)
INSTALL_ROOT := $(abspath $(INSTALL_ROOT))

INSTALL := install -D

#
# Build directories
#
BUILD_ROOT = $(SRC)/build/$(ARCH)/obj
BUILD = $(BUILD_ROOT)/$(DIR)

#
# Library paths
#
LIBAYLA = $(SRC)/lib/ayla
LIBPLATFORM = $(SRC)/lib/platform
LIBAPP = $(SRC)/lib/app

#
# Find 'checkpatch_ayla' code' style checking script
#
CSTYLE := $(wildcard $(SRC)/../util/code_style/checkpatch_ayla)
ifeq ($(CSTYLE),)
CSTYLE := $(shell which checkpatch_ayla || echo :)
endif

#
# List of directories to make tags (for vi users)
#
TAGS_DIRS += \
	. \
	$(LIBAYLA) \
	$(LIBPLATFORM) \
	$(NULL)

#
# Compiler setup
#
ifeq ($(TYPE),RELEASE)
DEFINES += NDEBUG
TARGET_CFLAGS += -O2 -Wuninitialized
else
TARGET_CFLAGS += -O0 -g -ggdb
endif
#TARGET_CFLAGS += -Wall -Wunused -Werror -std=gnu99		# @TODO: MAN: Disabled for demo. Revert this.
TARGET_CFLAGS += -Wall -Wno-unused -Werror -std=gnu99
TARGET_CFLAGS += $(addprefix -D,$(sort $(DEFINES)))

#
# Build files
#
OBJS = $(SOURCES:%.c=$(BUILD)/%.o)
DEPS = $(SOURCES:%.c=$(BUILD)/%.d)

#
# Style checking
#
STYLE_OK := $(wildcard .style_ok)
ifneq ($(STYLE_OK),)
FILE_IGNORE := $(sort $(shell cat .style_ok))
DIR_IGNORE := $(filter %/*,$(FILE_IGNORE))
ifneq ($(DIR_IGNORE),)
FILE_IGNORE := $(filter-out $(DIR_IGNORE), $(FILE_IGNORE))
FILE_IGNORE += $(shell find $(DIR_IGNORE) -type f)
endif
endif
STYLE_CHECK_C := $(filter-out $(FILE_IGNORE), $(SOURCES))
STYLE_CHECK_H := $(filter-out $(FILE_IGNORE), $(shell find * -type f -name '*.h'))
CSTYLES = $(STYLE_CHECK_C:%.c=$(BUILD)/%.cs)
CSTYLES += $(STYLE_CHECK_H:%.h=$(BUILD)/%.hcs)

#
# Get GIT SHA1 revision.  Append a '+' if there are local changes.
#
BUILD_SCM_REV := $(shell git rev-parse --verify --short HEAD 2>/dev/null)
ifneq ($(BUILD_SCM_REV),)
BUILD_SCM_REV := $(BUILD_SCM_REV)$(shell git diff-index --quiet HEAD 2>/dev/null || echo +)
endif

#
# SW version tracking: define pre-processor variables
#
include $(SRC)/package_version.mk
ifeq ($(MAJOR_VERSION),)
$(error No MAJOR_VERSION variable defined in: $(SRC)/package_version.mk ***)
endif
ifeq ($(MINOR_VERSION),)
$(error No MINOR_VERSION variable defined in: $(SRC)/package_version.mk ***)
endif
BUILD_DATE := $(shell date '+%Y-%m-%d')
BUILD_TIME := $(shell date '+%H:%M:%S')
BUILD_USER := $(USER)
ifeq ($(TYPE),RELEASE)
DEFINES += BUILD_RELEASE
else
BUILD_TAG := eng-$(BUILD_SCM_REV)
endif
BUILD_VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)
ifneq ($(MAINTENANCE_VERSION),)
BUILD_VERSION := $(BUILD_VERSION).$(MAINTENANCE_VERSION)
endif
ifneq ($(BUILD_TAG),)
BUILD_VERSION := $(BUILD_VERSION)-$(BUILD_TAG)
endif
DEFINES += BUILD_VERSION=\"$(BUILD_VERSION)\"
DEFINES += BUILD_SCM_REV=\"$(BUILD_SCM_REV)\"
DEFINES += BUILD_DATE=\"$(BUILD_DATE)\"
DEFINES += BUILD_TIME=\"$(BUILD_TIME)\"
DEFINES += BUILD_USER=\"$(BUILD_USER)\"

#
# Point to required libraries.
# Some circular dependencies exist between lib/ayla and lib/platform.
#
TARGET_CFLAGS += -I$(LIBAYLA)/include -I$(LIBPLATFORM)/include -I$(LIBAPP)/include -I$(TOOLCHAIN_DIR)/usr/include/
TARGET_LDFLAGS += -L$(BUILD_ROOT)/lib/platform -L$(BUILD_ROOT)/lib/ayla -L$(BUILD_ROOT)/lib/app -L$(TOOLCHAIN_DIR)/usr/lib/

TARGET_LDLIBS += -lapp -layla -lplatform
LIB_PLATFORM = $(BUILD_ROOT)/lib/platform/libplatform.a
LIB_AYLA = $(BUILD_ROOT)/lib/ayla/libayla.a
LIB_APP = $(BUILD_ROOT)/lib/app/libapp.a
# Libraries listed by target makefiles in LIBS variable
TARGET_LDLIBS += $(foreach lib,$(LIBS),-l$(lib))
# Common standard libs
TARGET_LDLIBS += -lrt -lm
# External library directories listed in EXTLIB_DIR variable
ifdef EXTLIB_DIR
TARGET_CFLAGS += $(foreach path,$(EXTLIB_DIR),-I$(path)/include)
TARGET_LDFLAGS += $(foreach path,$(EXTLIB_DIR),-L$(path)/lib)
endif

#
# Override common implicit variables to preserve defined values
#
override CFLAGS += $(TARGET_CFLAGS)
override LDFLAGS += $(TARGET_LDFLAGS)
override LDLIBS += $(TARGET_LDLIBS)
