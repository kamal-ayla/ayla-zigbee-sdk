/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __EMBER_INCLUDE_H__
#define __EMBER_INCLUDE_H__

/*
 * Include required Ember stack headers.
 */

#include "app/framework/include/af.h"

#include "app/ezsp-host/ash/ash-host.h"

#include "app/framework/util/util.h"
#include "app/framework/util/af-main.h"
#include "app/framework/util/af-event.h"
#include "app/framework/util/attribute-storage.h"
#include "app/framework/util/service-discovery.h"

#include "app/framework/security/crypto-state.h"
#include "app/framework/security/af-security.h"

#include \
    "app/framework/plugin/partner-link-key-exchange/partner-link-key-exchange.h"

#ifdef EMBER_AF_PLUGIN_FRAGMENTATION
#include "app/framework/plugin/fragmentation/fragmentation.h"
#endif

#include "app/util/common/library.h"
#include "app/util/common/form-and-join.h"
#include "app/util/security/security.h"
#include "app/util/serial/command-interpreter2.h"
#include "app/util/zigbee-framework/zigbee-device-common.h"
#include "app/util/zigbee-framework/zigbee-device-host.h"
#include "app/util/source-route-host.h"

#endif /* __EMBER_INCLUDE_H__ */

