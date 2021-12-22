/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef _DEVD_MSG_SERVER_H_
#define _DEVD_MSG_SERVER_H_

#include <ayla/msg_defs.h>

struct msg_client_state {
	char *name;
	pid_t pid;
	struct amsg_endpoint *endpoint;
	u64 connect_time_ms;

	/* Registrations */
	bool reg_dest_changes;
	bool reg_clock_events;
	bool reg_registration_changes;
};

/*
 * Setup and start devd's amsg server for local communications.
 */
int msg_server_create(struct device_state *dev, const char *path, int mode);

/*
 * Stop and free resources associated with devd's amsg server.
 */
void msg_server_cleanup(void);

/*
 * Lookup a msg server client by application name.
 * Returns NULL if not found.
 */
struct msg_client_state *msg_server_lookup_client(const char *app_name);

/*
 * Send a message to connected clients.  A filter function may be used to
 * send to a subset of clients.  If a complete_callback is provided,
 * it will be invoked after all message responses have been received.
 * Returns 0 on success or -1 if no messages were sent.
 */
int msg_server_broadcast(uint8_t interface, uint8_t type,
	const void *payload, size_t size,
	void (*complete_callback)(bool, void *), void *complete_arg,
	bool (*filter)(const struct amsg_endpoint *),
	uint32_t timeout_ms);

/*
 * Send dest mask to clients registered for updates.
 */
int msg_server_dests_changed_event(void (*complete_callback)(bool, void *),
	void *complete_arg);

/*
 * Send clock info to clients registered for updates.
 */
int msg_server_clock_event(void (*complete_callback)(bool, void *),
	void *complete_arg);

/*
 * Send user registration status and regtoken to clients registered for updates.
 */
int msg_server_registration_event(bool status_changed,
	void (*complete_callback)(bool, void *), void *complete_arg);

/*
 * Send factory reset to connected clients.
 */
int msg_server_factory_reset_event(void (*complete_callback)(bool, void *),
	void *complete_arg);

#endif /* _DEVD_MSG_SERVER_H_ */
