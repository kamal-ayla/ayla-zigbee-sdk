/*
 * Copyright 2015-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef AMSG_ENDPOINT_H_
#define AMSG_ENDPOINT_H_

#include "amsg_protocol.h"

/* Define to enable verbose debugging messages */
/* #define AMSG_DEBUG */

/* Connection-related events passed to event handler callbacks */
enum amsg_endpoint_event {
	AMSG_ENDPOINT_DISCONNECT		= 0,
	AMSG_ENDPOINT_CONNECT
};

/*
 * Forward declarations
 */
struct amsg_pending_resp;
struct amsg_resp_info;
struct amsg_user_data_entry;

/*
 * Access to application's thread management data structures.
 */
struct amsg_thread_control {
	struct file_event_table *fd_events;
	struct timer_head *timers;
	/* TODO: optional multi-thread locking scheme */
};

/*
 * Generic message socket endpoint.  This is used for basic tracking of the
 * connection and state, as well as connection event handling.
 */
struct amsg_endpoint {
	int sock;
	struct amsg_thread_control *control;
	int (*event_handler)(struct amsg_endpoint *, enum amsg_endpoint_event);
	void *event_data;
	uint16_t sequence_num;
	LIST_HEAD(, amsg_user_data_entry) user_data;
	TAILQ_HEAD(, amsg_pending_resp) pending_resp_queue;
	TAILQ_HEAD(, amsg_resp_info) resp_info_queue;
};

/*
 * Message client state.
 */
struct amsg_client {
	struct amsg_endpoint endpoint;
	struct amsg_thread_control control;
	int (*event_callback)(struct amsg_endpoint *,
	    enum amsg_endpoint_event);
};

/*
 * Message server session (one per connected client).
 */
struct amsg_server_session {
	struct amsg_endpoint endpoint;
	LIST_ENTRY(amsg_server_session) list_entry;
};

/*
 * Message server state.
 */
struct amsg_server {
	struct amsg_endpoint server;
	struct amsg_thread_control control;
	void (*session_event_callback)(struct amsg_endpoint *,
	    enum amsg_endpoint_event);
	size_t max_sessions;
	size_t num_sessions;
	LIST_HEAD(, amsg_server_session) session_list;
};

/*
 * Helper macro to iterate through a server's connected sessions.  It is not
 * safe to disconnect a session during iteration.
 */
#define AMSG_SERVER_SESSION_FOREACH(endpoint, state)			\
	for (struct amsg_server_session *__s = (state)->session_list.lh_first; \
	(((endpoint) = __s ? &__s->endpoint : NULL) != NULL);	\
	__s = __s->list_entry.le_next)

/*
 * Initialize a message client.  Must be called prior to invoking other
 * functions.  fd_events and timers are optional arguments for clients.
 * The fd_events parameter may be set to NULL if asynchronous message handling
 * is not required.  The timers parameter may be set to null if asynchronous
 * send timeouts are not required.
 */
int amsg_client_init(struct amsg_client *state,
	struct file_event_table *fd_events, struct timer_head *timers);

/*
 * Cleanup resources associated with a message client.
 */
void amsg_client_cleanup(struct amsg_client *state);

/*
 * Connect to a message server with the specified socket path.
 */
int amsg_client_connect(struct amsg_client *state, const char *path);

/*
 * Disconnect from a server.
 */
int amsg_client_disconnect(struct amsg_client *state);

/*
 * Register a connection event callback to be called after the connection
 * is established or disconnected.
 */
void amsg_client_set_event_callback(struct amsg_client *state,
	int (*callback)(struct amsg_endpoint *, enum amsg_endpoint_event));

/*
 * Initialize a message server.  Must be called prior to invoking other
 * functions.  The timers parameter may be set to null if asynchronous send
 * timeouts are not required.
 */
int amsg_server_init(struct amsg_server *state,
	struct file_event_table *fd_events, struct timer_head *timers);
/*
 * Cleanup resources associated with a message server.
 */
void amsg_server_cleanup(struct amsg_server *state);

/*
 * Create a server socket with the file mode specified, bind it to path, and
 * begin accepting connections from message clients.
 */
int amsg_server_start(struct amsg_server *state, const char *path, int mode);

/*
 * Disconnect all clients and shutdown the message server.
 */
int amsg_server_stop(struct amsg_server *state);

/*
 * Disconnect all connected clients.
 */
void amsg_server_session_disconnect_all(struct amsg_server *state);

/*
 * Iterate through all connected clients and call func for each one.  Set arg
 * to pass user data to func.  If func returns non-zero, iteration will stop
 * immediately.  A negative value returned by func is considered an error and
 * is returned by this function.  It is safe to disconnect a client during
 * iteration.
 */
int amsg_server_session_foreach(struct amsg_server *state,
	int (*func)(struct amsg_endpoint *, void *), void *arg);

/*
 * Limit the number of clients a server will accept.  0 means unlimited.
 */
void amsg_server_set_max_sessions(struct amsg_server *state,
	size_t max_sessions);

/*
 * Register a client connection event callback to be called after a client is
 * added or disconnected.
 */
void amsg_server_set_session_event_callback(struct amsg_server *state,
	void (*callback)(struct amsg_endpoint *, enum amsg_endpoint_event));

/*
 * Register a handler to support a message interface.  All incoming messages
 * with the specified interface ID will be handled by this function.
 * Interface 0 (AMSG_INTERFACE_INTERNAL) is reserved and cannot be used.
 * Interface handler functions should look up and handle each message type
 * supported by this interface.  If an unexpected message type is received,
 * return AMSG_ERR_TYPE_UNSUPPORTED.  Interface handlers are stored globally,
 * so they only need to be registered once per process.
 */
void amsg_set_interface_handler(uint8_t interface,
	enum amsg_err (*handler)(struct amsg_endpoint *,
	const struct amsg_msg_info *, struct amsg_resp_info *));

/*
 * Disconnect a server or client endpoint.
 */
void amsg_disconnect(struct amsg_endpoint *endpoint);

/*
 * Return true if the endpoint is connected.
 */
bool amsg_connected(const struct amsg_endpoint *endpoint);

/*
 * Set user data for an endpoint.  Multiple data slots may be managed by using
 * different IDs.  If free_data is set, it is called when the endpoint is
 * destroyed.
 * Returns data, or NULL if memory allocation failed.
 */
void *amsg_set_user_data(struct amsg_endpoint *endpoint, int id,
	void *data, void (*free_data)(void *));

/*
 * Remove an endpoint's user data with the specified ID. The free_data function
 * is NOT called, so any resources in use will need to be freed by the
 * application.
 * Returns the data, or NULL if the ID is invalid.
 */
void *amsg_clear_user_data(struct amsg_endpoint *endpoint, int id);

/*
 * Return an endpoint's user data pointer, or NULL, if the ID is invalid.
 */
void *amsg_get_user_data(const struct amsg_endpoint *endpoint, int id);

/*
 * Returns a pointer to a resp_info structure for use when asynchronously
 * responding to a message (out of the message handler).  This async resp_info
 * is passed to amsg_send_resp() when the application is ready to send the
 * reply. Once amsg_send_resp() has been called, the pointer returned by this
 * function is NO LONGER VALID and should not be used again.  Use
 * amsg_free_async_resp_info() to free the resp_info returned by this function
 * if a response will not be sent.
 */
struct amsg_resp_info *amsg_alloc_async_resp_info(
	struct amsg_resp_info *resp_info);

/*
 * Use this function to free a resp_info structure allocated with
 * amsg_alloc_async_resp_info(), but never passed to amsg_send_resp().  This
 * is useful if an endpoint disconnected or an error occurred before the
 * response was sent.
 */
void amsg_free_async_resp_info(struct amsg_resp_info **resp_info_ptr);

/*
 * Send a message.  If a custom resp_handler is specified, the reply
 * handling information is queued so the reply can be handled appropriately
 * when it is received.  resp_arg is passed to the resp_handler.
 * If timeout_ms is 0, timeouts are disabled.
 */
enum amsg_err amsg_send(struct amsg_endpoint *endpoint, uint8_t interface,
	uint8_t type, const void *payload, size_t size,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms);

/*
 * Send a message and block until a response is received.  If a custom
 * resp_handler is specified, it will be invoked before this function
 * returns.  resp_arg is passed to the resp_handler.  A dedicated response
 * socket is created for this call, so other messaging traffic in either
 * direction will not interrupt this message transaction.  If timeout_ms is 0,
 * timeouts are disabled.
 */
enum amsg_err amsg_send_sync(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, const void *payload, size_t size,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms);

/*
 * Send a custom message response.  This may be invoked either inside or
 * outside an incoming message handler.  See the comment on the
 * amsg_resp_info structure for details on sending asynchronous responses.
 * resp_info is passed in as a pointer to a pointer, so the function
 * can set the resp_info pointer to NULL once it has been used.
 */
enum amsg_err amsg_send_resp(struct amsg_resp_info **resp_info_ptr,
	uint8_t interface, uint8_t type, const void *payload, size_t size);

/*
 * Send a default response message.  This should be used to return an amsg_err
 * code when a response is expected, but there is no dedicated response
 * message to send.  A default response message will automatically be returned
 * when the message handler function returns, so this function should only
 * be used when when response timing is critical, and the response is
 * required either before or after the message handler returns.
 */
enum amsg_err amsg_send_default_resp(struct amsg_resp_info **resp_info_ptr,
	enum amsg_err err);

/*
 * Ping a messaging endpoint.  If an async_callback is used, the function
 * will return immediately and the callback will be invoked when the ping
 * response arrives.  If async_callback is NULL, the function will block
 * until the response is received.  An optional timeout may be specified.
 */
enum amsg_err amsg_ping(struct amsg_endpoint *endpoint, uint32_t timeout_ms,
	void (*async_callback)(enum amsg_err, uint32_t));

/*
 * Debug print helpers
 */
#define AMSG_DEBUG_PRINT_MSG_INFO(label, info)	\
	log_debug("%s: [%hu] flags=0x%02X type=[%hhu:%hhu] data_size=%zu", \
	    label, (info).sequence_num, \
	    (info).flags, (info).interface, (info).type, (info).payload_size)

#endif /* AMSG_ENDPOINT_H_ */
