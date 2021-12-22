/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_CONF_ID_H__
#define __AYLA_CONF_ID_H__

extern bool conf_loaded;
extern bool conf_reset;
extern char *conf_ads_region;
extern char *conf_ads_host_override;

void devd_conf_init(void);

#endif /* __AYLA_CONF_ID_H__ */
