GATEWAY_SUPPORT ?= 1

ifeq ($(GATEWAY_SUPPORT),1)
DEFINES += GATEWAY_SUPPORT
SOURCES += gateway_client.c \
	   gateway_if.c \
	   vt_node_list.c \
	   $(NULL)
endif
