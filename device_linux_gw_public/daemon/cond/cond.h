/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_COND_H__
#define __AYLA_COND_H__

#include <ayla/timer.h>
#include <ayla/file_event.h>

/* Default config file */
#define COND_CONF_FILE		"/config/cond.conf"

struct cond_state {
	struct timer_head timers;
	struct file_event_table file_events;
};

extern bool debug;
extern bool foreground;
extern bool use_net_events;
extern struct cond_state cond_state;

extern char msg_sock_path[];
extern char devd_msg_sock_path[];

#endif /* __AYLA_COND_H__ */
