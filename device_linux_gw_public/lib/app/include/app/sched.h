/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_SCHED_H__
#define __LIB_APP_SCHED_H__

#include <ayla/timer.h>

/*
 * Initialize the sched subsystem
 */
int sched_init(struct timer_head *appd_timer_head);

/*
 * Start processing schedules.
 */
int sched_start(void);

/*
 * Stop processing schedules and free any dynamically allocated memory
 * in the sched subsystem.
 */
int sched_destroy(void);

#endif /* __LIB_APP_SCHED_H__ */
