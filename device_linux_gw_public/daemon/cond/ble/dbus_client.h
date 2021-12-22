/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __DBUS_CLIENT_H__
#define __DBUS_CLIENT_H__

struct dbus_client_msg_handler;

int dbus_client_init(struct file_event_table *file_events,
	struct timer_head *timers);

void dbus_client_cleanup(void);

int dbus_client_send(DBusMessage *msg);

int dbus_client_send_with_reply(DBusMessage *msg,
	void (*reply_handler)(DBusMessage *, void *, const char *),
	void *arg, int timeout_ms);

int dbus_client_send_sync(DBusMessage *msg,
	DBusMessage **reply, int timeout_ms);

struct dbus_client_msg_handler *dbus_client_msg_handler_add(
	const struct dbus_client_msg_filter *filter,
	void (*msg_handler)(DBusMessage *, void *), void *arg);

void dbus_client_msg_handler_remove(struct dbus_client_msg_handler *handler);

struct dbus_client_msg_handler *dbus_client_signal_handler_add(
	const char *sender, const char *interface, const char *member,
	const char *path, void (*msg_handler)(DBusMessage *, void *),
	void *arg);

int dbus_client_obj_path_register(const char *path, enum dbus_msg_type type,
	const char *interface, const char *member,
	void (*msg_handler)(DBusMessage *, void *), void *arg);

void dbus_client_obj_path_unregister(const char *path);

void dbus_client_msg_debug_enable(bool enable);

int dbus_client_new_path_register(const char *path, const char *interface,
	void (*msg_handler)(DBusMessage *, void *), void *arg);

void dbus_client_new_path_unregister(const char *path);

#endif /* __DBUS_CLIENT_H__ */
