# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.

# Supplicant-specific sources
SOURCES += \
	$(SRC)/ext/hostap/src/utils/common.c \
	$(SRC)/ext/hostap/src/utils/os_unix.c \
	$(SRC)/ext/hostap/src/utils/wpa_debug.c \
	$(SRC)/ext/hostap/src/utils/trace.c \
	$(SRC)/ext/hostap/src/utils/wpabuf.c \
	$(SRC)/ext/hostap/src/common/wpa_ctrl.c \
	$(NULL)

# Supplicant-specific CFLAGS
TARGET_CFLAGS += \
	-I$(SRC)/ext/hostap/src/common \
	-I$(SRC)/ext/hostap/src/utils \
	-I$(SRC)/ext/hostap/src
	