/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/buffer.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>

/* Start value for unique ID generator function */
#define MSG_UNIQUE_ID_START	10000

/*
 * Structure to pass JSON-specific response handler as an argument.
 */
struct msg_json_arg {
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    json_t *, void *);
	void *resp_arg;
};

/*
 * Custom response handler for msg_send_json() calls.
 */
static void msg_json_resp_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, const struct amsg_msg_info *info, void *resp_arg)
{
	struct msg_json_arg *arg = (struct msg_json_arg *)resp_arg;
	json_t *msg_obj;

	ASSERT(arg != NULL);

	if (err == AMSG_ERR_NONE && info->payload_size) {
		msg_obj = msg_parse_json(info);
	} else {
		msg_obj = NULL;
	}
	arg->resp_handler(endpoint, err, msg_obj, arg->resp_arg);
	json_decref(msg_obj);
	free(arg);
}

/*
 * Internal function using amsg to send a message with a JSON payload.
 */
static enum amsg_err msg_send_json_internal(enum amsg_err (*amsg_send_func)(
	struct amsg_endpoint *, uint8_t, uint8_t, const void *, size_t,
	void (*)(struct amsg_endpoint *, enum amsg_err,
	const struct amsg_msg_info *, void *), void *, uint32_t),
	struct amsg_endpoint *endpoint, uint8_t interface, uint8_t type,
	const json_t *json, void (*resp_handler)(struct amsg_endpoint *,
	enum amsg_err, json_t *, void *), void *resp_arg, uint32_t timeout_ms)
{
	char *payload;
	size_t payload_len;
	struct msg_json_arg *arg;
	enum amsg_err err;

	ASSERT(endpoint != NULL);

	if (json) {
		payload = json_dumps(json, JSON_COMPACT);
		if (!payload) {
			return AMSG_ERR_MEM;
		}
		payload_len = strlen(payload);
	} else {
		payload = NULL;
		payload_len = 0;
	}
	if (resp_handler) {
		/* Use custom JSON response handler */
		arg = (struct msg_json_arg *)malloc(
		    sizeof(struct msg_json_arg));
		arg->resp_handler = resp_handler;
		arg->resp_arg = resp_arg;
		err = amsg_send_func(endpoint, interface, type, payload,
		    payload_len, msg_json_resp_handler, arg, timeout_ms);
		if (err != AMSG_ERR_NONE) {
			free(arg);
		}
	} else {
		err = amsg_send_func(endpoint, interface, type, payload,
		    payload_len, NULL, resp_arg, timeout_ms);
	}
	free(payload);
	return err;
}

/*
 * Internal function using amsg to send a message with a queue_buf payload.
 */
static enum amsg_err msg_send_qbuf_internal(enum amsg_err (*amsg_send_func)(
	struct amsg_endpoint *, uint8_t, uint8_t, const void *, size_t,
	void (*)(struct amsg_endpoint *, enum amsg_err,
	const struct amsg_msg_info *, void *), void *, uint32_t),
	struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	void *payload;
	size_t payload_len;

	ASSERT(endpoint != NULL);
	ASSERT(qbuf != NULL);

	payload = queue_buf_coalesce(qbuf);
	payload_len = queue_buf_len(qbuf);
	if (!payload && payload_len) {
		return AMSG_ERR_MEM;
	}
	return amsg_send_func(endpoint, interface, type, payload, payload_len,
	    resp_handler, resp_arg, timeout_ms);
}

/*
 * Return a process-unique integer.  This is useful for things like
 * assigning amsg user-data IDs.
 */
int msg_generate_unique_id(void)
{
	static int id = MSG_UNIQUE_ID_START;

	/* Handle wrap around */
	if (id + 1 < MSG_UNIQUE_ID_START) {
		id = MSG_UNIQUE_ID_START;
	}
	return id++;
}

/*
 * Wrapper for amsg_send() to facilitate sending asynchronous messages
 * with JSON payloads.  This function assumes both the message and its
 * response is a JSON encoded string, and handles generation and parsing
 * of the message payloads.
 */
enum amsg_err msg_send_json(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, const json_t *json,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    json_t *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	return msg_send_json_internal(amsg_send, endpoint, interface, type,
	    json, resp_handler, resp_arg, timeout_ms);
}

/*
 * Wrapper for amsg_send_sync() to facilitate sending asynchronous messages
 * with JSON payloads.  This function assumes both the message and its
 * response is a JSON encoded string, and handles generation and parsing
 * of the message payloads.
 */
enum amsg_err msg_send_json_sync(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, const json_t *json,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    json_t *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	return msg_send_json_internal(amsg_send_sync, endpoint, interface, type,
	    json, resp_handler, resp_arg, timeout_ms);
}

/*
 * Wrapper for amsg_send_response() to facilitate replying to messages with
 * JSON payloads.
 */
enum amsg_err msg_send_json_resp(struct amsg_resp_info **resp_info_ptr,
	uint8_t interface, uint8_t type, const json_t *json)
{
	char *payload;
	size_t payload_len;
	enum amsg_err err;

	ASSERT(resp_info_ptr != NULL);
	ASSERT(json != NULL);

	if (!*resp_info_ptr) {
		return AMSG_ERR_APPLICATION;
	}
	payload = json_dumps(json, JSON_COMPACT);
	if (!payload) {
		return AMSG_ERR_MEM;
	}
	payload_len = strlen(payload);
	err = amsg_send_resp(resp_info_ptr, interface, type,
	    payload, payload_len);
	free(payload);
	return err;
}

/*
 * Helper function to parse a JSON message payload.
 * Returns NULL if parsing fails.
 */
json_t *msg_parse_json(const struct amsg_msg_info *info)
{
	json_error_t error;
	json_t *msg_obj;

	if (!info->payload_size) {
		log_warn("empty message");
		return NULL;
	}
	/* Parse any valid JSON including UTF-8 strings with NULL bytes */
	msg_obj = json_loadb(info->payload, info->payload_size, JSON_ALLOW_NUL,
	    &error);
	if (!msg_obj) {
		log_err("JSON parse error at line %d: %s",
		    error.line, error.text);
		return NULL;
	}
	return msg_obj;
}

/*
 * Wrapper for amsg_send() to facilitate sending asynchronous messages
 * with payloads held in queue_bufs.
 */
enum amsg_err msg_send_qbuf(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	return msg_send_qbuf_internal(amsg_send, endpoint, interface, type,
	    qbuf, resp_handler, resp_arg, timeout_ms);
}

/*
 * Wrapper for amsg_send_sync() to facilitate sending asynchronous messages
 * with payloads held in queue_bufs.
 */
enum amsg_err msg_send_qbuf_sync(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	return msg_send_qbuf_internal(amsg_send_sync, endpoint, interface, type,
	    qbuf, resp_handler, resp_arg, timeout_ms);
}

/*
 * Wrapper for amsg_send_response() to facilitate replying to messages with
 * payloads held in queue_bufs.
 */
enum amsg_err msg_send_qbuf_resp(struct amsg_resp_info **resp_info_ptr,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf)
{
	void *payload;
	size_t payload_len;

	ASSERT(resp_info_ptr != NULL);
	ASSERT(qbuf != NULL);

	if (!*resp_info_ptr) {
		return AMSG_ERR_APPLICATION;
	}
	payload = queue_buf_coalesce(qbuf);
	payload_len = queue_buf_len(qbuf);
	if (!payload && payload_len) {
		return AMSG_ERR_MEM;
	}
	return amsg_send_resp(resp_info_ptr, interface, type,
	    payload, payload_len);
}

/*
 * Sends an application info message.  This is used by a message server to
 * find and associate state for a client application.
 */
int msg_send_app_info(struct amsg_endpoint *endpoint, const char *app_name)
{
	u8 msg_buf[sizeof(struct msg_app_info) + strlen(app_name) + 1];
	struct msg_app_info *app_info = (struct msg_app_info *)msg_buf;
	enum amsg_err err;

	ASSERT(endpoint != NULL);

	app_info->pid = getpid();
	strcpy(app_info->name, app_name);
	err = amsg_send_sync(endpoint, MSG_INTERFACE_APPLICATION,
	    MSG_APP_INFO, msg_buf, sizeof(msg_buf), NULL, NULL,
	    MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		log_err("amsg_send_sync returned: %s", amsg_err_string(err));
		return -1;
	}
	return 0;
}

/*
 * Sends an template version message.  This is used by a message server to
 * associate template for a client application.
 */
int msg_send_template_ver(struct amsg_endpoint *endpoint,
	const char *template_ver)
{
	u8 msg_buf[sizeof(struct msg_template_ver) + strlen(template_ver) + 1];
	struct msg_template_ver *ver = (struct msg_template_ver *)msg_buf;
	enum amsg_err err;

	ASSERT(endpoint != NULL);

	strcpy(ver->template_ver, template_ver);
	err = amsg_send_sync(endpoint, MSG_INTERFACE_APPLICATION,
	    MSG_APP_TEMPLATE_VER, msg_buf, sizeof(msg_buf), NULL, NULL,
	    MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		log_err("amsg_send_sync returned: %s", amsg_err_string(err));
		return -1;
	}
	return 0;
}

