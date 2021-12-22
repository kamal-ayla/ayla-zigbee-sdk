/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef _AYLA_HTTP_CLIENT_H_
#define _AYLA_HTTP_CLIENT_H_

#include <ayla/token_table.h>

/*
 * Error codes returned by send callback.
 */
#define HTTP_CLIENT_ERRORS(def)					\
	def(no error,		HTTP_CLIENT_ERR_NONE)		\
	def(failed,		HTTP_CLIENT_ERR_FAILED)		\
	def(timed out,		HTTP_CLIENT_ERR_TIMEOUT)	\
	def(was canceled,	HTTP_CLIENT_ERR_CANCELED)

DEF_ENUM(http_client_err, HTTP_CLIENT_ERRORS);

/*
 * Debug types.
 */
enum http_client_debug {
	HTTP_CLIENT_DEBUG_NONE		= 0,
	HTTP_CLIENT_DEBUG_INFO		= BIT(0),
	HTTP_CLIENT_DEBUG_HEADERS	= BIT(1),
	HTTP_CLIENT_DEBUG_DATA		= BIT(2)
};

/*
 * Structure passed as a parameter of the request complete callback.
 */
struct http_client_req_info {
	u16 curl_error;			/* CURLcode reported by Curl library */
	u16 http_status;		/* Status indicated in the response */
	size_t sent_bytes;		/* Outgoing data size */
	size_t received_bytes;		/* Incoming data size */
	u32 time_ms;			/* Total connection time w/ server */
	u32 upload_speed_bps;		/* Average upload speed in bits/sec */
	u32 download_speed_bps;		/* Average download speed in bits/sec */
	const char *content_type;	/* Response Content-type header */
	const char *local_ip;		/* Local IP for this connection */
	const char *remote_ip;		/* Destination IP for this connection */
};

struct file_event_table;
struct timer_head;
struct http_client;
struct http_client_context;
struct http_tag;
enum http_method;


/*
 * Allocate a new HTTP client state.  Returns a pointer to a http_client
 * on success, and NULL on failure.
 */
struct http_client *http_client_init(struct file_event_table *fd_events,
	struct timer_head *timers);

/*
 * Free resources associated with an HTTP client.  Cleans up
 * all contexts associated with the client.
 */
void http_client_cleanup(struct http_client *client);

/*
 * Add a new context to the HTTP client.  Each client has independent config
 * and may initiate HTTP requests independently.  A single context may only
 * conduct one request at a time.
 * Returns a pointer to a client_context on success, and NULL on error.
 */
struct http_client_context *http_client_context_add(struct http_client *client);

/*
 * Free resources associated with a context and remove it from the client.
 * If a request is in progress, it will be canceled.
 */
void http_client_context_remove(struct http_client_context *context);

/*
 * Set functions to handle incoming and outgoing data in the body of HTTP
 * requests.  If read_func is NULL, no data will be sent.  If write_func is
 * NULL, all received data will be ignored.  A user-defined argument may be
 * included, and will be passed to the read and write functions.  Each callback
 * should return how much data it successfully handled, or -1 on error.
 */
void http_client_context_set_data_funcs(struct http_client_context *context,
	ssize_t (*read_func)(void *, size_t, size_t, void *),
	ssize_t (*write_func)(const void *, size_t, size_t, void *), void *arg);

/*
 * Set an overall maximum request time in seconds.  A stalled transfer time
 * limit can be set as a parameter of client_send() to customize individual
 * requests.
 */
void http_client_context_set_timeout(struct http_client_context *context,
	u32 timeout_secs);

/*
 * Set a mask of the desired debug to log for the context.  Debug types are
 * defined by the http_client_debug enumeration.  A label string may be
 * provided to differentiate debug by context.
 */
void http_client_context_set_debug(struct http_client_context *context,
	unsigned debug_mask, const char *debug_label);

/*
 * Use a custom header field in the next request.  This must be specified
 * for each client_send() call.  Value may contain printf style format
 * specifiers.
 */
int http_client_add_header(struct http_client_context *context,
	const char *name, const char *value, ...)
	__attribute__((format(printf, 3, 4)));

/*
 * Use custom header parsers on the response to the next request.  These
 * must be set for each client_send() call.  Parse_list must end with a NULL
 * entry.
 */
int http_client_set_header_parsers(struct http_client_context *context,
	const struct http_tag *parse_list);

/*
 * Send an HTTP request from this context.  Context and request specific setup
 * will affect the behavior of this call.  Specify the size of any outgoing
 * data with size, or set it to -1 to attempt to send without a known size.
 * Callback is invoked at the end of the request, and specifies success or the
 * error that occurred.  Arg is the user-defined context argument to pass to
 * the callback and data handler functions.  The stall_timeout is the number of
 * seconds to wait before timing out, when the average throughput speed is
 * under 100 B/s.
 * Returns 0 on success, and -1 on error.
 */
int http_client_send(struct http_client_context *context,
	enum http_method method, const char *url, ssize_t content_size,
	void (*callback)(enum http_client_err,
	const struct http_client_req_info *, void *), void *arg,
	u32 stall_timeout);

/*
 * Immediately halt the current request.  If there is no request in progress,
 * just cleans up request-specific configuration.
 * Returns 0 on success, and -1 on error.
 */
int http_client_cancel(struct http_client_context *context);

/*
 * Return true if a request is in progress, otherwise false.
 */
bool http_client_busy(const struct http_client_context *context);

/*
 * Return a pointer to a statically allocated string representation of the
 * error code.
 */
const char *http_client_err_string(enum http_client_err err);


#endif /* _AYLA_HTTP_CLIENT_H_ */
