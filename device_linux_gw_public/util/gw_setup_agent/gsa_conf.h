/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_GSA_CONF_H__
#define __AYLA_GSA_CONF_H__

#include <jansson.h>

#define CONF_FILE "/config/devd.conf"

/*
 * System config items.
 */
#define CONF_MODEL_MAX		24	/* max string length for device ID */
#define CONF_DEV_ID_MAX		20	/* max string length for device ID */
#define CONF_MFG_SN_MAX		32	/* max string length for mfg serial */
#define CONF_DEV_SN_MAX		20	/* max string length for DSN */
#define CONF_OEM_MAX		20	/* max string length for OEM strings */
#define CONF_OEM_KEY_MAX	256	/* max length of encrypted OEM key */

#endif /* __AYLA_GSA_CONF_H__ */
