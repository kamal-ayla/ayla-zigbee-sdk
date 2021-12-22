# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

#
# Ayla Device Client for Linux
#
# Targets:
#
# all: build all core components and applications
# world: build all core components, applications, tests, and utilities
# release: force a clean world build
# install: build all and invoke "install" target
# clean: delete build output files
# help: print build README
# libs: just build common libraries
# daemons: build devd and core services
# app: just build the application daemon
# tests: build and install test apps
# utils: build device command line utilities and tools
# host_utils: build and install developer and manufacturing tools
#

# Optional components
-include gateway.mk

# Option to select custom application directory
APP ?= appd

#
# Directories to find build targets (by category)
#
LIB_DIRS := \
	lib/ayla \
	lib/platform \
	lib/app \
	$(NULL)

DAEMON_DIRS := \
	daemon/devd \
	$(NULL)

WIFI_DIRS := \
	daemon/cond \
	$(NULL)

LOGD_DIRS := \
	daemon/logd \
	$(NULL)

APP_DIRS := \
	app/$(APP) \
	$(NULL)

UTIL_DIRS := \
	util/acgi \
	util/acli \
	util/devdwatch \
	util/gw_setup_agent \
	util/ota \
	$(NULL)

TEST_DIRS := \
	$(NULL)

HOST_UTIL_DIRS := \
	host_util/config_gen \
	$(NULL)

# Include source for optional subsystems
ifneq ($(NO_WIFI),1)
DAEMON_DIRS += $(WIFI_DIRS)
endif
ifneq ($(NO_LOGD),1)
DAEMON_DIRS += $(LOGD_DIRS)
endif

# Default target is "all" but can be overridden if desired
TARGET ?= all
default: $(TARGET)

# Make recipes
all: libs daemons utils app

world: all host_utils tests

release:
	$(MAKE) -s TYPE=RELEASE clean
	$(MAKE) -s TYPE=RELEASE world

install:
	$(MAKE) -s TARGET=$@ $(filter-out $@, $(TARGET))

clean:
	rm -rf build
# No need to clean individual targets
#	$(MAKE) -s TARGET=$@ $(filter-out $@, $(TARGET))

help:
	@echo "Displaying make README..."
	@less make/README

libs: $(LIB_DIRS)
$(LIB_DIRS):
	$(MAKE) -s -C $@ $(TARGET)

daemons: $(DAEMON_DIRS)
$(DAEMON_DIRS): libs
	$(MAKE) -s -C $@ $(TARGET)

utils: $(UTIL_DIRS)
$(UTIL_DIRS): libs
	$(MAKE) -s -C $@ $(TARGET)

app: $(APP_DIRS)
$(APP_DIRS): libs
	$(MAKE) -s -C $@ $(TARGET)

tests: $(TEST_DIRS)
$(TEST_DIRS): libs
	$(MAKE) -s -C $@ $(TARGET)
	$(MAKE) -s -C $@ install

host_utils: $(HOST_UTIL_DIRS)
# Force native build for utils target
$(HOST_UTIL_DIRS):
	$(MAKE) -s ARCH=$(TARGET_CROSS) TOOLCHAIN_DIR= libs
	$(MAKE) -s ARCH=$(TARGET_CROSS) TOOLCHAIN_DIR= -C $@ $(TARGET)
	$(MAKE) -s ARCH=$(TARGET_CROSS) TOOLCHAIN_DIR= -C $@ install


# All directory lists used as targets must be PHONY
.PHONY: $(LIB_DIRS)
.PHONY: $(DAEMON_DIRS)
.PHONY: $(UTIL_DIRS)
.PHONY: $(APP_DIRS)
.PHONY: $(TEST_DIRS)
.PHONY: $(HOST_UTIL_DIRS)

# All non-file recipe names must be PHONY
.PHONY: all world release install clean help libs daemons utils app tests host_utils
