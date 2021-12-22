/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sys/queue.h>

#include <curl/curl.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/http.h>
#include <ayla/file_event.h>
#include <ayla/timer.h>
#include <ayla/time_utils.h>
#include <ayla/log.h>

#include <ayla/http_client.h>

/* Set CLIENT subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_CLIENT, __VA_ARGS__)

#define HTTP_CLIENT_DEBUG_BUF_SIZE	400	/* Curl debug buffer size */
#define CLIENT_HEADER_FIELD_BUF_SIZE	400	/* Arbitrary hdr line size */
#define CLIENT_LOW_SPEED_THRESHOLD	100	/* Bytes/sec */

static DEF_NAME_TABLE(http_client_err_names, HTTP_CLIENT_ERRORS);

struct http_client_context {
	struct http_client *client;
	u16 debug_mask;
	char *debug_label;
	CURL *curl;
	struct http_state header_parser;

	/* Functions to handle incoming and outgoing data */
	ssize_t (*data_read)(void *, size_t, size_t, void *);
	ssize_t (*data_write)(const void *, size_t, size_t, void *);
	void *data_arg;

	/* Request-specific state */
	struct {
		struct curl_slist *header;
		const struct http_tag *parse_list;
		void (*callback)(enum http_client_err,
		    const struct http_client_req_info *, void *);
		void *arg;
		bool started;
		size_t read_offset;
		size_t write_offset;
	} req;

	LIST_ENTRY(http_client_context) entry;
};

struct http_client {
	struct file_event_table *fd_events;
	struct timer_head *timers;
	CURLM *curlm;
	size_t running_handles;
	struct timer curl_timer;
	struct timer step_timer;
	LIST_HEAD(, http_client_context) contexts;
};


/*
 * Clear request-specific configuration.
 */
static void http_client_context_reset(struct http_client_context *context)
{
	if (context->req.header) {
		curl_slist_free_all(context->req.header);
	}
	memset(&context->req, 0, sizeof(context->req));

	/* Restore default method-specific Curl options */
	curl_easy_setopt(context->curl, CURLOPT_CUSTOMREQUEST, NULL);
	curl_easy_setopt(context->curl, CURLOPT_UPLOAD, 0L);
	curl_easy_setopt(context->curl, CURLOPT_POST, 0L);
	curl_easy_setopt(context->curl, CURLOPT_INFILESIZE, -1L);
	curl_easy_setopt(context->curl, CURLOPT_POSTFIELDSIZE, -1L);
}

static void http_client_request_done(struct http_client_context *context,
	enum http_client_err err, CURLcode ccode)
{
	void (*callback)(enum http_client_err,
	    const struct http_client_req_info *, void *);
	void *arg;
	long long_val;
	double float_val;
	struct http_client_req_info info = {
		.curl_error = ccode,
		.sent_bytes = context->req.read_offset,
		.received_bytes = context->req.write_offset
	};

	callback = context->req.callback;
	arg = context->req.arg;

	/* Reset request-specific state */
	http_client_context_reset(context);

	/* Invoke response complete callback */
	if (callback) {
		if (err == HTTP_CLIENT_ERR_NONE) {
			if (curl_easy_getinfo(context->curl,
			    CURLINFO_RESPONSE_CODE, &long_val) == CURLE_OK) {
				info.http_status = long_val;
			}
		}
		if (curl_easy_getinfo(context->curl, CURLINFO_TOTAL_TIME,
		    &float_val) == CURLE_OK) {
			info.time_ms = (u32)(float_val * 1000);
		}
		if (curl_easy_getinfo(context->curl, CURLINFO_SPEED_UPLOAD,
		    &float_val) == CURLE_OK) {
			info.upload_speed_bps = (u32)(float_val * 8);
		}
		if (curl_easy_getinfo(context->curl, CURLINFO_SPEED_DOWNLOAD,
		    &float_val) == CURLE_OK) {
			info.download_speed_bps = (u32)(float_val * 8);
		}
		curl_easy_getinfo(context->curl, CURLINFO_CONTENT_TYPE,
		    &info.content_type);
		curl_easy_getinfo(context->curl, CURLINFO_LOCAL_IP,
		    &info.local_ip);
		curl_easy_getinfo(context->curl, CURLINFO_PRIMARY_IP,
		    &info.remote_ip);

		callback(err, &info, arg);
	}
}

static void http_client_curl_handle_finished(struct http_client *client)
{
	struct http_client_context *context;
	enum http_client_err err;
	CURLMsg *msg;
	int msgs;
	CURL *curl;
	CURLcode ccode;
	CURLMcode mcode;

	do {
		msg = curl_multi_info_read(client->curlm, &msgs);
		if (!msg) {
			break;
		}
		switch (msg->msg) {
		case CURLMSG_DONE:
			break;
		default:
			log_warn("unexpected Curl msg code: %d", msg->msg);
			break;
		}
		--client->running_handles;
		/* msg is destroyed by curl_multi_remove_handle() */
		curl = msg->easy_handle;
		ccode = msg->data.result;
		mcode = curl_multi_remove_handle(client->curlm, curl);
		if (mcode != CURLM_OK) {
			log_warn("error finishing request: %s",
			    curl_multi_strerror(mcode));
		}
		/* Lookup context that initiated the request */
		LIST_FOREACH(context, &client->contexts, entry) {
			if (context->curl == curl) {
				break;
			}
		}
		if (!context) {
			/* Should never happen */
			log_err("request context missing");
			continue;
		}
		switch (ccode) {
		case CURLE_OK:
			err = HTTP_CLIENT_ERR_NONE;
			break;
		case CURLE_OPERATION_TIMEDOUT:
			err = HTTP_CLIENT_ERR_TIMEOUT;
			break;
		default:
			err = HTTP_CLIENT_ERR_FAILED;
		}
		/* Finalize the request */
		http_client_request_done(context, err, ccode);
	} while (msgs);
}

static void http_client_curl_sock_event(void *arg, int sock, int event)
{
	struct http_client *client = (struct http_client *)arg;
	CURLMcode mcode;
	int handles;
	int action;

	action = 0;
	if (event & (POLLIN | POLLPRI)) {
		action |= CURL_CSELECT_IN;
	}
	if (event & POLLOUT) {
		action |= CURL_CSELECT_OUT;
	}
	if (event & POLLERR) {
		action |= CURL_CSELECT_ERR;
	}
	do {
		mcode = curl_multi_socket_action(client->curlm, sock,
			action, &handles);
		if (handles < client->running_handles) {
			http_client_curl_handle_finished(client);
		}
	} while (mcode == CURLM_CALL_MULTI_PERFORM);
}

static int http_client_curl_sock_set(CURL *easy, curl_socket_t sock,
	int action, void *user_arg, void *socket_arg)
{
	struct http_client *client = (struct http_client *)user_arg;
	struct file_event_table *fet = client->fd_events;

	switch (action) {
	case CURL_POLL_IN:
		file_event_reg_pollf(fet, sock, http_client_curl_sock_event,
		    POLLIN | POLLPRI, client);
		break;
	case CURL_POLL_OUT:
		file_event_reg_pollf(fet, sock, http_client_curl_sock_event,
		    POLLOUT, client);
		break;
	case CURL_POLL_INOUT:
		file_event_reg_pollf(fet, sock, http_client_curl_sock_event,
		    POLLIN | POLLPRI | POLLOUT, client);
		break;
	case CURL_POLL_REMOVE:
		file_event_unreg(fet, sock, NULL, NULL, client);
		break;
	default:
		break;
	}
	return 0;	/* Curl docs say MUST return 0 */
}

static void http_client_curl_timeout(struct timer *timer)
{
	struct http_client *client =
	    CONTAINER_OF(struct http_client, curl_timer, timer);

	http_client_curl_sock_event(client, CURL_SOCKET_TIMEOUT, 0);
}

static void http_client_step_timeout(struct timer *timer)
{
	struct http_client *client =
	    CONTAINER_OF(struct http_client, step_timer, timer);

	http_client_curl_sock_event(client, CURL_SOCKET_TIMEOUT, 0);
}

static void http_client_curl_step(struct http_client *client)
{
	if (!timer_active(&client->step_timer)) {
		timer_set(client->timers, &client->step_timer, 0);
	}
}

static void http_client_curl_timer_set(CURLM *curlm, long delay_ms, void *arg)
{
	struct http_client *client = (struct http_client *)arg;

	if (delay_ms < 0) {
		timer_cancel(client->timers, &client->curl_timer);
		return;
	}
	timer_set(client->timers, &client->curl_timer, (u64)delay_ms);
}

static size_t http_client_curl_write(char *buf, size_t size, size_t num,
	void *arg)
{
	struct http_client_context *context = (struct http_client_context *)arg;
	ssize_t rc;

	if (!context->data_write) {
		return size * num;	/* Ignore incoming data */
	}
	rc = context->data_write(buf, size * num, context->req.write_offset,
	    context->data_arg);
	if (rc < 0) {
		/* Write error */
		return 0;
	}
	context->req.write_offset += rc;
	return rc;
}

static size_t http_client_curl_read(char *buf, size_t size, size_t num,
	void *arg)
{
	struct http_client_context *context = (struct http_client_context *)arg;
	ssize_t rc;

	if (!context->data_read) {
		return 0;	/* No data to read */
	}
	rc = context->data_read(buf, size * num, context->req.read_offset,
	    context->data_arg);
	if (rc < 0) {
		/* Read error */
		return CURL_READFUNC_ABORT;
	}
	context->req.read_offset += rc;
	return rc;
}

static size_t http_client_curl_header(char *buf, size_t size, size_t num,
	void *arg)
{
	struct http_client_context *context = (struct http_client_context *)arg;
	ssize_t len;
	size_t buf_len = size * num;

	if (context->header_parser.state == HS_DONE) {
		/*
		 * Some responses may include multiple headers.  Parse each
		 * header for fields we are interested in.
		 */
		http_parse_init(&context->header_parser,
		    context->req.parse_list, context->req.arg);
	}
	len = http_parse(&context->header_parser, buf, buf_len);
	if (len < 0) {
		log_err("failed to parse HTTP header");
		return 0;
	}
	return buf_len;
}

static int http_client_curl_debug(CURL *curl, curl_infotype info,
	char *buf, size_t len, void *arg)
{
	struct http_client_context *context = (struct http_client_context *)arg;
	const char *label;
	char str[HTTP_CLIENT_DEBUG_BUF_SIZE];

	/* All output is sent to debug log */
	if (!log_debug_enabled()) {
		return CURLE_OK;
	}
	/* Verbose debug enabled per context */
	if (!(context->debug_mask & BIT(info))) {
		return CURLE_OK;
	}
	/* Add label to extended debug */
	switch (info) {
	case CURLINFO_HEADER_IN:
		label = "HDR-IN  ";
		break;
	case CURLINFO_HEADER_OUT:
		label = "HDR-OUT ";
		break;
	case CURLINFO_DATA_IN:
		label = "DATA-IN ";
		break;
	case CURLINFO_DATA_OUT:
		label = "DATA-OUT";
		break;
	default:
		label = NULL;
		break;
	}
	/* Print debug text */
	if (len >= sizeof(str)) {
		len = sizeof(str) - 4;
		/* End with ellipsis if line truncated */
		strcpy(str + len, "...");
	} else {
		str[len] = '\0';
	}
	memcpy(str, buf, len);
	/* Remove trailing newline */
	if (len > 0 && str[len - 1] == '\n') {
		str[len - 1] = '\0';
	}
	if (label) {
		if (context->debug_label) {
			log_base_subsystem("http_client_send",
			    LOG_AYLA_DEBUG, LOG_SUB_CLIENT,
			    "[%s] %s %s", context->debug_label, label, str);
		} else {
			log_base_subsystem("http_client_send",
			    LOG_AYLA_DEBUG, LOG_SUB_CLIENT,
			    "%s %s", label, str);
		}
	} else {
		if (context->debug_label) {
			log_base_subsystem("http_client_send",
			    LOG_AYLA_DEBUG, LOG_SUB_CLIENT,
			    "[%s]  %s", context->debug_label, str);
		} else {
			log_base_subsystem("http_client_send",
			    LOG_AYLA_DEBUG, LOG_SUB_CLIENT, "%s", str);
		}
	}
	return CURLE_OK;
}

static void http_client_curl_multi_setup(struct http_client *client)
{
	CURLMcode mcode = CURLM_OK;

	/* Set Curl socket event handler */
	mcode |= curl_multi_setopt(client->curlm, CURLMOPT_SOCKETFUNCTION,
	    http_client_curl_sock_set);
	mcode |= curl_multi_setopt(client->curlm, CURLMOPT_SOCKETDATA, client);
	/* Set Curl timer setup callback */
	mcode |= curl_multi_setopt(client->curlm, CURLMOPT_TIMERFUNCTION,
	    http_client_curl_timer_set);
	mcode |= curl_multi_setopt(client->curlm, CURLMOPT_TIMERDATA, client);

	ASSERT(mcode == CURLM_OK);
}

static void http_client_curl_setup(struct http_client_context *context)
{
	CURLcode ccode = CURLE_OK;

	ccode |= curl_easy_setopt(context->curl, CURLOPT_NOSIGNAL, 1);
	/* Speed in B/s below which transfer is considered stalled */
	ccode |= curl_easy_setopt(context->curl, CURLOPT_LOW_SPEED_LIMIT,
	    CLIENT_LOW_SPEED_THRESHOLD);
	/* Set write handler for incoming data */
	ccode |= curl_easy_setopt(context->curl, CURLOPT_WRITEFUNCTION,
	    http_client_curl_write);
	ccode |= curl_easy_setopt(context->curl, CURLOPT_WRITEDATA, context);
	/* Set read handler for outgoing data */
	ccode |= curl_easy_setopt(context->curl, CURLOPT_READFUNCTION,
	    http_client_curl_read);
	ccode |= curl_easy_setopt(context->curl, CURLOPT_READDATA, context);
	/* Set received header data handler */
	ccode |= curl_easy_setopt(context->curl, CURLOPT_HEADERFUNCTION,
	    http_client_curl_header);
	ccode |= curl_easy_setopt(context->curl, CURLOPT_HEADERDATA, context);
	/* Set Curl debug output handler */
	ccode |= curl_easy_setopt(context->curl, CURLOPT_DEBUGFUNCTION,
	    http_client_curl_debug);
	ccode |= curl_easy_setopt(context->curl, CURLOPT_DEBUGDATA, context);

	ASSERT(ccode == CURLE_OK);
}

/*
 * Allocate a new HTTP client state.  Returns a pointer to a http_client
 * on success, and NULL on failure.
 */
struct http_client *http_client_init(struct file_event_table *fd_events,
	struct timer_head *timers)
{
	static bool global_init;
	struct http_client *client;

	ASSERT(fd_events != NULL);
	ASSERT(timers != NULL);

	/* Only call global init per process */
	if (!global_init) {
		curl_global_init(CURL_GLOBAL_ALL);
		global_init = true;
	}
	client = (struct http_client *)calloc(1, sizeof(*client));
	if (!client) {
		log_err("malloc failed");
		return NULL;
	}
	client->curlm = curl_multi_init();
	if (!client->curlm) {
		log_err("curl-multi interface init failed");
		free(client);
		return NULL;
	}
	http_client_curl_multi_setup(client);
	client->fd_events = fd_events;
	client->timers = timers;
	timer_init(&client->curl_timer, http_client_curl_timeout);
	timer_init(&client->step_timer, http_client_step_timeout);
	LIST_INIT(&client->contexts);
	return client;
}

/*
 * Free resources associated with an HTTP client.  Cleans up
 * all contexts associated with the client.
 */
void http_client_cleanup(struct http_client *client)
{
	struct http_client_context *context;

	if (!client) {
		return;
	}
	/* Cleanup all contexts */
	while ((context = LIST_FIRST(&client->contexts)) != NULL) {
		http_client_context_remove(context);
	}
	timer_cancel(client->timers, &client->curl_timer);
	timer_cancel(client->timers, &client->step_timer);
	curl_multi_cleanup(client->curlm);
	free(client);
}

/*
 * Add a new context to the HTTP client.  Each client has independent config
 * and may initiate HTTP requests independently.  A single context may only
 * conduct one request at a time.
 * Returns a pointer to a client_context on success, and NULL on error.
 */
struct http_client_context *http_client_context_add(struct http_client *client)
{
	struct http_client_context *context;

	ASSERT(client != NULL);

	context = (struct http_client_context *)calloc(1, sizeof(*context));
	if (!context) {
		log_err("malloc failed");
		return NULL;
	}
	/* Keep a pointer to the client state for convenience */
	context->client = client;
	/* Create a Curl easy handle for each context */
	context->curl = curl_easy_init();
	if (!context->curl) {
		log_err("curl interface init failed");
		free(context);
		return NULL;
	}
	/* Set default Curl options */
	http_client_curl_setup(context);
	LIST_INSERT_HEAD(&client->contexts, context, entry);
	return context;
}

/*
 * Free resources associated with a context and remove it from the client.
 * If a request is in progress, it will be canceled.
 */
void http_client_context_remove(struct http_client_context *context)
{
	if (!context) {
		return;
	}
	LIST_REMOVE(context, entry);
	if (http_client_busy(context)) {
		http_client_cancel(context);
	}
	curl_easy_cleanup(context->curl);
	free(context->debug_label);
	free(context);
}

/*
 * Set functions to handle incoming and outgoing data in the body of HTTP
 * requests.  If read_func is NULL, no data will be sent.  If write_func is
 * NULL, all received data will be ignored.  A user-defined argument may be
 * included, and will be passed to the read and write functions.  Each callback
 * should return how much data it successfully handled, or -1 on error.
 */
void http_client_context_set_data_funcs(struct http_client_context *context,
	ssize_t (*read_func)(void *, size_t, size_t, void *),
	ssize_t (*write_func)(const void *, size_t, size_t, void *), void *arg)
{
	ASSERT(context != NULL);

	context->data_read = read_func;
	context->data_write = write_func;
	context->data_arg = arg;
}

/*
 * Set an overall maximum request time in seconds.  A stalled transfer time
 * limit can be set as a parameter of client_send() to customize individual
 * requests.
 */
void http_client_context_set_timeout(struct http_client_context *context,
	u32 timeout_secs)
{
	ASSERT(context != NULL);

	curl_easy_setopt(context->curl, CURLOPT_TIMEOUT, (long)timeout_secs);
}

/*
 * Set a mask of the desired debug to log for the context.  Debug types are
 * defined by the http_client_debug enumeration.  A label string may be
 * provided to differentiate debug by context.
 */
void http_client_context_set_debug(struct http_client_context *context,
	unsigned debug_mask, const char *debug_label)
{
	u8 bit;

	ASSERT(context != NULL);
	ASSERT(CURLINFO_END < sizeof(context->debug_mask) * 8);

	context->debug_mask = 0;

	for (bit = 0; bit < sizeof(debug_mask) * 8; ++bit) {
		switch (debug_mask & (1 << bit)) {
		case HTTP_CLIENT_DEBUG_INFO:
			context->debug_mask |= BIT(CURLINFO_TEXT);
			break;
		case HTTP_CLIENT_DEBUG_HEADERS:
			context->debug_mask |= BIT(CURLINFO_HEADER_IN);
			context->debug_mask |= BIT(CURLINFO_HEADER_OUT);
			break;
		case HTTP_CLIENT_DEBUG_DATA:
			context->debug_mask |= BIT(CURLINFO_DATA_IN);
			context->debug_mask |= BIT(CURLINFO_DATA_OUT);
			break;
		default:
			break;
		}
	}
	/* Set a context label for the debug output */
	free(context->debug_label);
	if (context->debug_mask && debug_label) {
		context->debug_label = strdup(debug_label);
	} else {
		context->debug_label = NULL;
	}
	/* Enable/disable Curl debug output */
	curl_easy_setopt(context->curl, CURLOPT_VERBOSE,
	    context->debug_mask ? 1L : 0L);
}

/*
 * Use a custom header field in the next request.  This must be specified
 * for each client_send() call.  Value may contain printf style format
 * specifiers.
 */
int http_client_add_header(struct http_client_context *context,
	const char *name, const char *value, ...)
{
	va_list args;
	char field[CLIENT_HEADER_FIELD_BUF_SIZE];
	size_t len;

	ASSERT(context != NULL);

	len = snprintf(field, sizeof(field), "%s: ", name);
	va_start(args, value);
	len += vsnprintf(field + len, sizeof(field) - len, value, args);
	va_end(args);
	if (len >= sizeof(field)) {
		log_err("buffer too small");
		return -1;
	}
	context->req.header = curl_slist_append(context->req.header, field);
	return context->req.header ? 0 : -1;
}

/*
 * Use custom header parsers on the response to the next request.  These
 * must be set for each client_send() call.  Parse_list must end with a NULL
 * entry.
 */
int http_client_set_header_parsers(struct http_client_context *context,
	const struct http_tag *parse_list)
{
	ASSERT(context != NULL);

	context->req.parse_list = parse_list;
	return 0;
}

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
	u32 stall_timeout)
{
	CURLMcode mcode;

	ASSERT(context != NULL);

	context->req.callback = callback;
	context->req.arg = arg;
	http_parse_init(&context->header_parser, context->req.parse_list, arg);

	curl_easy_setopt(context->curl, CURLOPT_URL, url);
	curl_easy_setopt(context->curl, CURLOPT_HTTPHEADER,
	    context->req.header);
	curl_easy_setopt(context->curl, CURLOPT_LOW_SPEED_TIME,
	    (long)stall_timeout);

	/* Setup Curl to perform the requested method */
	switch (method) {
	case HTTP_HEAD:
	case HTTP_DELETE:
		curl_easy_setopt(context->curl, CURLOPT_CUSTOMREQUEST,
		    http_method_names[method]);
		break;
	case HTTP_GET:
		/* Default method */
		break;
	case HTTP_PUT:
		curl_easy_setopt(context->curl, CURLOPT_UPLOAD, 1L);
		/* Upload size is not required */
		if (content_size >= 0) {
			curl_easy_setopt(context->curl, CURLOPT_INFILESIZE,
			    (long)content_size);
		}
		break;
	case HTTP_POST:
		curl_easy_setopt(context->curl, CURLOPT_POST, 1L);
		/* Upload size is not required */
		if (content_size >= 0) {
			curl_easy_setopt(context->curl, CURLOPT_POSTFIELDSIZE,
			    (long)content_size);
		}
		break;
	}
	/* Add this context's Curl handle to the multi stack */
	mcode = curl_multi_add_handle(context->client->curlm, context->curl);
	if (mcode != CURLM_OK) {
		log_err("add_handle failed: %s", curl_multi_strerror(mcode));
		http_client_context_reset(context);
		return -1;
	}
	++context->client->running_handles;
	context->req.started = true;
	/* Begin processing the request on the next main loop iteration */
	http_client_curl_step(context->client);
	return 0;
}

/*
 * Immediately halt the current request.  If there is no request in progress,
 * just cleans up request-specific configuration.
 * Returns 0 on success, and -1 on error.
 */
int http_client_cancel(struct http_client_context *context)
{
	CURLMcode mcode;

	ASSERT(context != NULL);

	if (!http_client_busy(context)) {
		/* Cleanup any request-specific config */
		http_client_context_reset(context);
		return 0;
	}
	mcode = curl_multi_remove_handle(context->client->curlm, context->curl);
	if (mcode != CURLM_OK) {
		log_warn("error canceling request: %s",
		    curl_multi_strerror(mcode));
	}
	--context->client->running_handles;
	http_client_request_done(context, HTTP_CLIENT_ERR_CANCELED, 0);
	return (mcode == CURLM_OK) ? 0 : -1;
}

/*
 * Return true if a request is in progress, otherwise false.
 */
bool http_client_busy(const struct http_client_context *context)
{
	return context && context->req.started;
}

/*
 * Return a pointer to a statically allocated string representation of the
 * error code.
 */
const char *http_client_err_string(enum http_client_err err)
{
	return http_client_err_names[err];
}

