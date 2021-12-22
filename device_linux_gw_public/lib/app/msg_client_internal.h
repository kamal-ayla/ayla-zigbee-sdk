/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __LIB_APP_MSG_CLIENT_INTERNAL_H__
#define __LIB_APP_MSG_CLIENT_INTERNAL_H__

struct file_event_table;
struct timer_head;

/*
 * Initialize the cloud client message interface.
 */
int msg_client_init(struct file_event_table *file_events,
	struct timer_head *timers);

/*
 * Cleanup all resources associated with the cloud client message interface.
 */
void msg_client_cleanup(void);

/*
 * Connect to the cloud client at the specified path.
 */
int msg_client_connect(const char *path);

/*
 * Close the cloud client connect.
 */
void msg_client_connect_close(void);

/*
 * Enable the cloud client to receive commands and properties from the cloud.
 */
int msg_client_listen_enable(void);

/*
 * Register a callback for when the connection to the cloud client
 * goes up or down.
 */
void msg_client_set_connection_status_callback(void (*callback)(bool));

/*
 * Register a callback for cloud up/down events.
 */
void msg_client_set_cloud_event_callback(void (*callback)(bool));

/*
 * Register a callback for LAN client up/down events.  This is only called
 * when the first client connects or the last client disconnects.
 */
void msg_client_set_lan_event_callback(void (*callback)(bool));

/*
 * Register a callback for user registration events.
 */
void msg_client_set_registration_callback(void (*callback)(bool));

/*
 * Register a callback for when a new registration token is received.
 */
void msg_client_set_regtoken_callback(void (*callback)(const char *));

/*
 * Register a callback for when the cloud client has changed the system time.
 */
void msg_client_set_time_change_callback(void (*callback)(void));

/*
 * Register a callback for when the cloud client has requested a factory reset.
 */
void msg_client_set_factory_reset_callback(void (*callback)(void));


#endif /* __LIB_APP_MSG_CLIENT_INTERNAL_H__ */
