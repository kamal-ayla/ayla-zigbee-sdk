/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_DNSS_H__
#define __AYLA_DNSS_H__

#define AYLA_MDNS_PORT 10276

void dnss_up(void);
void dnss_down(void);
void dnss_mdns_up(u32 ifaddr);
void dnss_mdns_down(void);

#endif /* __AYLA_DNSS_H__ */
