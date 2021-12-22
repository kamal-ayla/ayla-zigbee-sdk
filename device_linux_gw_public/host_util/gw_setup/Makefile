#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

VERSION ?= 0.6
RELEASE ?= -eng

OEM_MODEL ?= ayla

DIR = gw_setup
SRC = ../..

BUILD ?= $(SRC)/build
PROGRAM ?= gw_setup-$(OEM_MODEL)-$(VERSION)
UNIX2DOS ?= unix2dos
PKG_PN := AY004TGW0

#
# source control revision from git.
#
SCM_REV := $(shell git rev-parse --verify --short HEAD)
SCM_PLUS := $(shell git diff-index --quiet HEAD || echo +)
SCM_REV := $(SCM_REV)$(SCM_PLUS)
ifneq ($(BUILD_NAME)$(SCM_PLUS),)
ifneq ($(WORKSPACE_LABEL),)
SCM_REV := $(WORKSPACE_LABEL)/$(SCM_REV)
endif
SCM_REV := $(USER)/$(SCM_REV)
endif
TIMESTAMP := $(shell date '+%Y-%m-%d %H:%M:%S')

default: $(BUILD)/$(PROGRAM).zip

TEXT_FILES = \
	ayla_lib.tcl \
	gw_setup.tcl \
	product/$(OEM_MODEL)/model_info.tcl \
	product/$(OEM_MODEL)/README.txt \
	$(NULL)

$(BUILD)/$(PROGRAM).zip: $(BUILD)/$(PROGRAM)
	(cd $(BUILD)/$(PROGRAM) && zip -q -r ../$(PROGRAM).zip .)

.PHONY: FORCE

$(BUILD)/$(PROGRAM): FORCE
	rm -rf $@
	mkdir -p $@
	cp $(TEXT_FILES) $@
	echo set build_ver \
	    "{$(PKG_PN) $(VERSION)$(PKG_RELEASE) $(TIMESTAMP) $(SCM_REV)}" \
	    > $@/build.tcl	
	(cd $@; $(UNIX2DOS) $(notdir $(TEXT_FILES)))
