/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/queue.h>

#include <dbus/dbus.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/file_event.h>
#include <ayla/timer.h>

#include "dbus_utils.h"
#include "dbus_client.h"

/* #define DBUS_CLIENT_DEBUG */
#ifdef DBUS_CLIENT_DEBUG
#define log_dbus_debug(...)	\
	log_debug(__VA_ARGS__)
#else
#define log_dbus_debug(...)
#endif


struct dbus_client_send_callback_state {
	void (*handler)(DBusMessage *, void *, const char *);
	void *handler_arg;
};

struct dbus_client_msg_handler {
	void (*msg_handler)(DBusMessage *, void *);
	void *msg_handler_arg;
	struct dbus_client_msg_filter filter;
	bool filter_enabled;
};

struct dbus_client_obj_handler {
	struct dbus_client_msg_handler handler;
	DBusObjectPathVTable vtable;
};

struct dbus_client_timer {
	DBusTimeout *timeout;
	struct timer_head *timers;
	struct timer timer;
};

struct dbus_client_state {
	struct file_event_table *file_events;
	struct timer_head *timers;
	DBusConnection *conn;
	struct timer dispatch_timer;
};

static struct dbus_client_state state;


static void dbus_client_dispatch_timeout(struct timer *timer)
{
	struct dbus_client_state *state = CONTAINER_OF(struct dbus_client_state,
	    dispatch_timer, timer);

	while (dbus_connection_get_dispatch_status(state->conn) ==
	    DBUS_DISPATCH_DATA_REMAINS) {
		dbus_connection_dispatch(state->conn);
	}
}

static void dbus_client_dispatch_status_update(DBusConnection *connection,
	DBusDispatchStatus new_status, void *data)
{
	struct dbus_client_state *state = (struct dbus_client_state *)data;

	switch (new_status) {
	case DBUS_DISPATCH_DATA_REMAINS:
		if (!timer_active(&state->dispatch_timer)) {
			timer_set(state->timers, &state->dispatch_timer, 0);
		}
		break;
	case DBUS_DISPATCH_NEED_MEMORY:
		/* Try dispatch after delay in case of low memory error */
		timer_set(state->timers, &state->dispatch_timer, 1000);
		break;
	default:
		break;
	}
}

static void dbus_client_dispatch(void)
{
	dbus_client_dispatch_status_update(state.conn,
	    DBUS_DISPATCH_DATA_REMAINS, &state);
}

static void dbus_client_fd_event_handler(void *arg, int fd, int events)
{
	DBusWatch *watch = (DBusWatch *)arg;
	unsigned watch_flags = 0;

	if (events & POLLIN) {
		watch_flags |= DBUS_WATCH_READABLE;
	}
	if (events & POLLOUT) {
		watch_flags |= DBUS_WATCH_WRITABLE;
	}

	log_dbus_debug("fd=%d in=%u out=%u", fd, events & POLLIN,
	    events & POLLOUT);

	if (!dbus_watch_handle(watch, watch_flags)) {
		log_err("dbus_watch_handle failed");
	}
}

static dbus_bool_t dbus_client_fd_event_reg(DBusWatch *watch, void *data)
{
	struct dbus_client_state *state = (struct dbus_client_state *)data;
	int fd;
	unsigned watch_flags;
	unsigned events = 0;

	if (!dbus_watch_get_enabled(watch)) {
		/* No need to pre-allocate an event listener */
		return TRUE;
	}
	fd = dbus_watch_get_unix_fd(watch);
	watch_flags = dbus_watch_get_flags(watch);
	if (watch_flags & DBUS_WATCH_READABLE) {
		events |= POLLIN;
	}
	if (watch_flags & DBUS_WATCH_WRITABLE) {
		events |= POLLOUT;
	}
	if (file_event_reg_pollf(state->file_events, fd,
	    dbus_client_fd_event_handler, events, watch) < 0) {
		log_err("failed to register listener: fd=%d", fd);
		return FALSE;
	}

	log_dbus_debug("fd=%d in=%u out=%u", fd, events & POLLIN,
	    events & POLLOUT);

	return TRUE;
}

static void dbus_client_fd_event_unreg(DBusWatch *watch, void *data)
{
	struct dbus_client_state *state = (struct dbus_client_state *)data;
	int fd;

	if (!dbus_watch_get_enabled(watch)) {
		/* Event listener already removed */
		return;
	}
	fd = dbus_watch_get_unix_fd(watch);

	log_dbus_debug("fd=%d", fd);

	if (file_event_unreg(state->file_events, fd, NULL, NULL, watch) < 0) {
		log_err("failed to unregister listener: fd=%d", fd);
	}
}

static void dbus_client_fd_event_toggle(DBusWatch *watch, void *data)
{
	if (dbus_watch_get_enabled(watch)) {
		dbus_client_fd_event_reg(watch, data);
	} else {
		dbus_client_fd_event_unreg(watch, data);
	}
}

static void dbus_client_timer_free(void *data)
{
	struct dbus_client_timer *timer_state =
	    (struct dbus_client_timer *)data;

	log_dbus_debug("timer free: timer_state=%p", timer_state);

	if (!timer_state) {
		return;
	}
	timer_cancel(timer_state->timers, &timer_state->timer);
	free(timer_state);
}

static void dbus_client_timer_timeout(struct timer *timer)
{
	struct dbus_client_timer *timer_state =
	    CONTAINER_OF(struct dbus_client_timer, timer, timer);
	u64 interval_ms = dbus_timeout_get_interval(timer_state->timeout);

	log_dbus_debug("interval_ms=%llu timer_state=%p",
	    (long long unsigned)interval_ms, timer_state);

	/* Reset timer */
	timer_set(timer_state->timers, &timer_state->timer, interval_ms);
	/* Handle timeout event */
	dbus_timeout_handle(timer_state->timeout);
}

static dbus_bool_t dbus_client_timeout_add(DBusTimeout *timeout, void *data)
{
	struct dbus_client_state *state = (struct dbus_client_state *)data;
	struct dbus_client_timer *timer_state;
	u64 interval_ms;

	timer_state = (struct dbus_client_timer *)malloc(sizeof(*timer_state));
	if (!timer_state) {
		log_err("malloc failed");
		return FALSE;
	}
	timer_init(&timer_state->timer, dbus_client_timer_timeout);
	timer_state->timeout = timeout;
	timer_state->timers = state->timers;
	/* Associate the timer state with the timeout structure */
	dbus_timeout_set_data(timeout, timer_state, dbus_client_timer_free);

	log_dbus_debug("timer added: timer_state=%p", timer_state);

	if (!dbus_timeout_get_enabled(timeout)) {
		/* Timer was allocated for later */
		return TRUE;
	}
	interval_ms = dbus_timeout_get_interval(timeout);
	timer_set(state->timers, &timer_state->timer, interval_ms);

	log_dbus_debug("started: interval=%llums",
	    (long long unsigned)interval_ms);

	return TRUE;
}

static void dbus_client_timeout_remove(DBusTimeout *timeout, void *data)
{
	log_dbus_debug("removing timer");
	/* Clear the timer state (invokes dbus_client_timer_free()) */
	dbus_timeout_set_data(timeout, NULL, NULL);
}

static void dbus_client_timeout_toggle(DBusTimeout *timeout, void *data)
{
	struct dbus_client_timer *timer_state =
	    (struct dbus_client_timer *)dbus_timeout_get_data(timeout);
	u64 interval_ms;

	if (dbus_timeout_get_enabled(timeout)) {
		interval_ms = dbus_timeout_get_interval(timeout);
		timer_set(timer_state->timers, &timer_state->timer,
		    interval_ms);
		log_dbus_debug("started: interval=%llums",
		    (long long unsigned)interval_ms);
	} else {
		timer_cancel(timer_state->timers, &timer_state->timer);
		log_dbus_debug("stopped");
	}
}

static void dbus_client_msg_debug_handler(DBusMessage *msg, void *arg)
{
	dbus_utils_msg_print(__func__, msg);
}

static void dbus_client_send_callback(DBusPendingCall *pending, void *user_data)
{
	struct dbus_client_send_callback_state *callback =
	    (struct dbus_client_send_callback_state *)user_data;
	DBusMessage *msg = dbus_pending_call_steal_reply(pending);
	DBusError err;

	dbus_error_init(&err);

	if (dbus_message_get_type(msg) == DBUS_MSG_TYPE_METHOD_RETURN) {
		callback->handler(msg, callback->handler_arg, NULL);
	} else if (dbus_set_error_from_message(&err, msg)) {
		log_err("error: %s - %s", err.name, err.message);
		callback->handler(NULL, callback->handler_arg, err.name);
		dbus_error_free(&err);
	} else {
		callback->handler(NULL, callback->handler_arg, NULL);
	}
	dbus_message_unref(msg);
	dbus_pending_call_unref(pending);
}

static void dbus_client_msg_handler_init(
	struct dbus_client_msg_handler *handler,
	void (*msg_handler)(DBusMessage *, void *), void *arg,
	const struct dbus_client_msg_filter *filter)
{
	handler->msg_handler = msg_handler;
	handler->msg_handler_arg = arg;
	if (filter) {
		handler->filter.type = filter->type;
		if (filter->sender) {
			handler->filter.sender = strdup(filter->sender);
		}
		if (filter->interface) {
			handler->filter.interface = strdup(filter->interface);
		}
		if (filter->member) {
			handler->filter.member = strdup(filter->member);
		}
		if (filter->path) {
			handler->filter.path = strdup(filter->path);
		}
		if (filter->destination) {
			handler->filter.destination =
			    strdup(filter->destination);
		}
		handler->filter_enabled = true;
	}
}

static void dbus_client_msg_handler_cleanup(void *user_data)
{
	struct dbus_client_msg_handler *handler =
	    (struct dbus_client_msg_handler *)user_data;

	if (!handler) {
		return;
	}
	free((void *)handler->filter.sender);
	free((void *)handler->filter.interface);
	free((void *)handler->filter.member);
	free((void *)handler->filter.path);
	free((void *)handler->filter.destination);
}

static int dbus_client_msg_subscribe(
	const struct dbus_client_msg_filter *filter)
{
	char rule[DBUS_MAXIMUM_MATCH_RULE_LENGTH + 1];
	DBusError err;

	if (dbus_utils_match_rule_create(rule, sizeof(rule), filter,
	    state.conn) < 0) {
		return -1;
	}
	log_dbus_debug("match rule: %s", rule);
	dbus_error_init(&err);
	dbus_bus_add_match(state.conn, rule, &err);
	if (dbus_error_is_set(&err)) {
		log_err("failed to add match [%s]: %s", rule, err.message);
		dbus_error_free(&err);
		return -1;
	}
	return 0;
}

static int dbus_client_msg_unsubscribe(
	const struct dbus_client_msg_filter *filter)
{
	char rule[DBUS_MAXIMUM_MATCH_RULE_LENGTH + 1];
	DBusError err;

	if (dbus_utils_match_rule_create(rule, sizeof(rule), filter,
	    state.conn) < 0) {
		return -1;
	}
	log_dbus_debug("match rule: %s", rule);
	dbus_error_init(&err);
	dbus_bus_remove_match(state.conn, rule, &err);
	if (dbus_error_is_set(&err)) {
		log_err("failed to remove match [%s]: %s", rule, err.message);
		dbus_error_free(&err);
		return -1;
	}
	return 0;
}

static DBusHandlerResult dbus_client_msg_handler(DBusConnection *connection,
	DBusMessage *msg, void *user_data)
{
	struct dbus_client_msg_handler *handler =
	    (struct dbus_client_msg_handler *)user_data;

	if (!handler->filter_enabled ||
	    dbus_utils_msg_filter_eval(&handler->filter, msg)) {
		handler->msg_handler(msg, handler->msg_handler_arg);
	}
	/* Allow other message handlers to evaluate the message */
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void dbus_client_obj_handler_free(DBusConnection *connection,
	void *user_data)
{
	struct dbus_client_obj_handler *handler =
	    (struct dbus_client_obj_handler *)user_data;

	/* Reusing standard message handler for registered objects */
	dbus_client_msg_handler_cleanup(&handler->handler);
	free(user_data);
}

static DBusHandlerResult dbus_client_obj_msg_handler(
	DBusConnection *connection, DBusMessage *msg, void *user_data)
{
	struct dbus_client_obj_handler *handler =
	    (struct dbus_client_obj_handler *)user_data;

	/* Reuse standard message handler for registered objects */
	dbus_client_msg_handler(connection, msg, &handler->handler);
	/* Allow other message handlers to evaluate the message */
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int dbus_client_init(struct file_event_table *file_events,
	struct timer_head *timers)
{
	DBusError err;

	if (state.conn) {
		log_warn("already connected");
		return 0;
	}
	state.file_events = file_events;
	state.timers = timers;

	dbus_error_init(&err);

	/* Connect to the system bus */
	state.conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		log_err("failed to connect to DBUS: %s", err.message);
		dbus_error_free(&err);
		goto error;
	}
	/* Do not exit process if disconnected */
	dbus_connection_set_exit_on_disconnect(state.conn, FALSE);

	/* Set a dispatch handler to service queued data */
	timer_init(&state.dispatch_timer, dbus_client_dispatch_timeout);
	dbus_connection_set_dispatch_status_function(state.conn,
	    dbus_client_dispatch_status_update, &state, NULL);

	/* Set file descriptor event listener controls */
	if (!dbus_connection_set_watch_functions(state.conn,
	    dbus_client_fd_event_reg, dbus_client_fd_event_unreg,
	    dbus_client_fd_event_toggle, &state, NULL)) {
		log_err("failed to setup DBUS watch functions");
		goto error;
	}
	/* Set timer controls */
	if (!dbus_connection_set_timeout_functions(state.conn,
	    dbus_client_timeout_add, dbus_client_timeout_remove,
	    dbus_client_timeout_toggle, &state, NULL)) {
		log_err("failed to setup DBUS timeout functions");
		goto error;
	}
	log_debug("connected to D-Bus: client name \"%s\"",
	    dbus_bus_get_unique_name(state.conn));

	/* Immediately dispatch to flush pending messages and complete init */
	dbus_client_dispatch();
	return 0;
error:
	dbus_client_cleanup();
	return -1;
}

void dbus_client_cleanup(void)
{
	if (!state.conn) {
		return;
	}
	dbus_connection_flush(state.conn);
	dbus_connection_close(state.conn);
	state.conn = NULL;
}

int dbus_client_send(DBusMessage *msg)
{
	if (!dbus_connection_send(state.conn, msg, NULL)) {
		return -1;
	}
	return 0;
}

int dbus_client_send_with_reply(DBusMessage *msg,
	void (*reply_handler)(DBusMessage *, void *, const char *),
	void *arg, int timeout_ms)
{
	struct dbus_client_send_callback_state *callback = NULL;
	DBusPendingCall *pending = NULL;

	ASSERT(msg != NULL);
	ASSERT(reply_handler != NULL);
	ASSERT(dbus_message_get_type(msg) == DBUS_MSG_TYPE_METHOD_CALL);

	callback = (struct dbus_client_send_callback_state *)malloc(
	    sizeof(*callback));
	if (!callback) {
		log_err("malloc failed");
		goto error;
	}
	callback->handler = reply_handler;
	callback->handler_arg = arg;
	if (!dbus_connection_send_with_reply(state.conn, msg,
	    &pending, timeout_ms)) {
		log_err("insufficient memory");
		goto error;
	}
	if (!pending) {
		/* Either disconnected or does not support Unix FDs */
		log_err("disconnected");
		goto error;
	}
	if (!dbus_pending_call_set_notify(pending,
	    dbus_client_send_callback, callback, free)) {
		log_err("insufficient memory");
		dbus_pending_call_cancel(pending);
		goto error;
	}
	return 0;
error:
	if (pending) {
		/* Automatically free()s callback */
		dbus_pending_call_unref(pending);
	}
	return -1;
}

int dbus_client_send_sync(DBusMessage *msg,
	DBusMessage **reply, int timeout_ms)
{
	DBusMessage *reply_msg;
	DBusError err;
	int rc = 0;

	dbus_error_init(&err);
	reply_msg = dbus_connection_send_with_reply_and_block(state.conn,
	    msg, timeout_ms, &err);
	if (dbus_error_is_set(&err)) {
		log_err("error: %s - %s", err.name, err.message);
		dbus_error_free(&err);
		rc = -1;
	}
	if (reply) {
		*reply = reply_msg;
	} else if (reply_msg) {
		dbus_message_unref(reply_msg);
	}
	return rc;
}

struct dbus_client_msg_handler *dbus_client_msg_handler_add(
	const struct dbus_client_msg_filter *filter,
	void (*msg_handler)(DBusMessage *, void *), void *arg)
{
	struct dbus_client_msg_handler *handler;

	ASSERT(msg_handler != NULL);

	handler = (struct dbus_client_msg_handler *)calloc(1, sizeof(*handler));
	if (!handler) {
		log_err("malloc failed");
		return NULL;
	}
	dbus_client_msg_handler_init(handler, msg_handler, arg, filter);
	/* Add a callback to handle incoming messages */
	if (!dbus_connection_add_filter(state.conn, dbus_client_msg_handler,
	    handler, dbus_client_msg_handler_cleanup)) {
		log_err("failed to add connection filter");
		dbus_client_msg_handler_cleanup(handler);
		return NULL;
	}
	/* Automatically add a match rule for signal handlers */
	if (filter && filter->type == DBUS_MSG_TYPE_SIGNAL) {
		dbus_client_msg_subscribe(filter);
	}
	return handler;
}

void dbus_client_msg_handler_remove(struct dbus_client_msg_handler *handler)
{
	ASSERT(handler != NULL);

	/* Remove automatic match rule added for signals */
	if (handler->filter_enabled &&
	    handler->filter.type == DBUS_MSG_TYPE_SIGNAL) {
		dbus_client_msg_unsubscribe(&handler->filter);
	}
	/* dbus_client_msg_handler_free() will be called on removal */
	dbus_connection_remove_filter(state.conn, dbus_client_msg_handler,
	    handler);
}

struct dbus_client_msg_handler *dbus_client_signal_handler_add(
	const char *sender, const char *interface, const char *member,
	const char *path, void (*msg_handler)(DBusMessage *, void *), void *arg)
{
	struct dbus_client_msg_filter filter = {
		.type = DBUS_MSG_TYPE_SIGNAL,
		.sender = sender,
		.interface = interface,
		.member = member,
		.path = path
	};

	ASSERT(interface != NULL);

	return dbus_client_msg_handler_add(&filter, msg_handler, arg);
}

int dbus_client_obj_path_register(const char *path, enum dbus_msg_type type,
	const char *interface, const char *member,
	void (*msg_handler)(DBusMessage *, void *), void *arg)
{
	struct dbus_client_obj_handler *handler;
	struct dbus_client_msg_filter filter = {
		.type = type,
		.interface = interface,
		.member = member
	};
	DBusError err;
	bool success;

	ASSERT(path != NULL);
	ASSERT(msg_handler != NULL);

	handler = (struct dbus_client_obj_handler *)calloc(1,
	    sizeof(*handler));
	if (!handler) {
		log_err("malloc failed");
		return -1;
	}
	/*
	 * Use standard message handler for messages received for a registered
	 * object.  The filter parameters are optional, but can be used to
	 * setup the incoming message handler to ignore unwanted messages.
	 */
	dbus_client_msg_handler_init(&handler->handler, msg_handler, arg,
	    &filter);
	handler->vtable.unregister_function = dbus_client_obj_handler_free;
	handler->vtable.message_function = dbus_client_obj_msg_handler;
	/* Register the new path on the bus */
	dbus_error_init(&err);
	success = dbus_connection_try_register_object_path(state.conn, path,
	    &handler->vtable, handler, &err);
	if (dbus_error_is_set(&err)) {
		log_err("failed to register object path [%s]: %s", path,
		    err.message);
		dbus_error_free(&err);
		success = false;
	}
	if (!success) {
		free(handler);
		return -1;
	}
	return 0;
}

void dbus_client_obj_path_unregister(const char *path)
{
	dbus_connection_unregister_object_path(state.conn, path);
}

void dbus_client_msg_debug_enable(bool enable)
{
	static struct dbus_client_msg_handler *handler;

	if (enable == (handler != NULL)) {
		return;
	}
	if (enable) {
		handler = dbus_client_msg_handler_add(NULL,
		    dbus_client_msg_debug_handler, NULL);
	} else {
		dbus_client_msg_handler_remove(handler);
		handler = NULL;
	}
}
