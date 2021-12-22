GATEWAY_SUPPORT ?= 1

ifeq ($(GATEWAY_SUPPORT),1)
DEFINES += GATEWAY_SUPPORT
SOURCES += gateway_interface.c \
	   $(NULL)
endif
