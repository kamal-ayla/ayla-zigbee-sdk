/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_DATA_H__
#define __LIB_APP_DATA_H__

#include <ayla/file_event.h>

struct file_event_table;
struct timer_head;

/*
 * XXX DO NOT USE.  This is only here to avoid breaking some legacy appds
 * that omitted this in their own headers.
 */
extern char app_sock_path[];

#define DATA_POLL_INTERVAL 3000		/* milliseconds between app_polls */

/*
 * Legacy data client initialization function.  This also connects to the
 * msg_client interface in order to allow older applications to access newer
 * features.
 *
 * THIS FUNCTION IS DEPRECATED.  It is preferred to use the app library,
 * which is newer and initializes the data interface internally.
 */
#define data_client_init(file_events)	\
	data_client_init_legacy(file_events, &timers, app_sock_path)
int data_client_init_legacy(struct file_event_table *file_events,
	struct timer_head *timers, const char *socket_path);

#endif /* __LIB_APP_DATA_H__ */
