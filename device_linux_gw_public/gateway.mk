GATEWAY_SUPPORT ?= 1

ifeq ($(GATEWAY_SUPPORT),1)
APP ?= gatewayd
endif
