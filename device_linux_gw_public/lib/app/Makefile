#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

LIB = libapp.a
DIR = lib/app
SRC = ../..

#
# List of source files to build
#
SOURCES	:= \
	app.c		\
	conf_access.c	\
	data.c		\
	msg_client.c	\
	props.c		\
	ops.c		\
	sched.c		\
	schedeval.c	\
	$(NULL)

# Optional components
-include gateway.mk

#
# List of libraries to link
#
#LIBS = ssl crypto jansson

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

all: $(LIB)

install: all
	$(INSTALL) $(BUILD)/$(LIB) $(INSTALL_ROOT)/$(DIR)/$(LIB)

#
# Include common build rules
#
include $(SRC)/make/common_cc.mk 

