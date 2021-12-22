/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __GATT_SERVICE_H__
#define __GATT_SERVICE_H__

/*
 * Initialize the GATT service.
 */
int gatt_init(struct file_event_table *file_events, struct timer_head *timers);

/*
 * Free resources.
 */
int gatt_cleanup(void);

#endif /* __GATT_SERVICE_H__ */
