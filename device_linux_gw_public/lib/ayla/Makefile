#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

LIB := libayla.a
DIR := lib/ayla
SRC := ../..
EXT_DIR := $(SRC)/ext

EXT_SOURCES := \
	hashmap/src/hashmap.c

#
# List of source files to build
#
SOURCES	:= \
	$(EXT_DIR)/$(EXT_SOURCES)	\
	amsg.c		\
	amsg_protocol.c	\
	assert.c	\
	async.c		\
	base64.c	\
	buffer.c	\
	clock_utils.c   \
	cmd_handle.c	\
	conf_io.c	\
	conf_rom.c	\
	crc8.c		\
	crc16.c		\
	crc32.c		\
	crypto.c	\
	endian.c	\
	file_io.c	\
	file_event.c	\
	filesystem_monitor.c	\
	hex.c		\
	http.c		\
	http_client.c	\
	json_interface.c	\
	json_parser.c	\
	lan_ota.c	\
	log.c		\
	lookup_by_name.c \
	lookup_by_val.c	\
	msg_cli.c	\
	msg_conf.c	\
	msg_utils.c	\
	network_utils.c	\
	parse_argv.c	\
	parse_date.c	\
	serv_proxy.c	\
	server.c	\
	socket.c	\
	str_utils.c	\
	time_utils.c	\
	timer.c		\
	uri_encoding_maps.c	\
	uri_decode.c	\
	uri_encode.c	\
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

