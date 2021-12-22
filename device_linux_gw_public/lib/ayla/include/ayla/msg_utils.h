/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __AYLA_MSG_UTILS_H__
#define __AYLA_MSG_UTILS_H__

#include <jansson.h>

struct queue_buf;
struct amsg_endpoint;
struct amsg_msg_info;
struct amsg_resp_info;

#define MSG_TIMEOUT_DEFAULT_MS		10000
#define MSG_SOCKET_DIR_DEFAULT		"/var/run"
#define MSG_SOCKET_DEFAULT		"msg_sock"

/*
 * Ayla common application names.  Used when reporting MSG_APP_INFO.
 */
#define MSG_APP_NAME_CLIENT	"devd"
#define MSG_APP_NAME_APP	"appd"
#define MSG_APP_NAME_WIFI	"cond"
#define MSG_APP_NAME_CLI	"acli"
#define MSG_APP_NAME_LOGGER	"logd"
#define MSG_APP_NAME_OTA	"ota"


/*
 * Return a process-unique integer.  This is useful for things like
 * assigning amsg user-data IDs.
 */
int msg_generate_unique_id(void);

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
	uint32_t timeout_ms);

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
	uint32_t timeout_ms);

/*
 * Wrapper for amsg_send_response() to facilitate replying to messages with
 * JSON payloads.
 */
enum amsg_err msg_send_json_resp(struct amsg_resp_info **resp_info_ptr,
	uint8_t interface, uint8_t type, const json_t *json);

/*
 * Helper function to parse a JSON message payload.
 * Returns NULL if parsing fails.
 */
json_t *msg_parse_json(const struct amsg_msg_info *info);

/*
 * Wrapper for amsg_send() to facilitate sending asynchronous messages
 * with payloads held in queue_bufs.
 */
enum amsg_err msg_send_qbuf(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms);

/*
 * Wrapper for amsg_send_sync() to facilitate sending asynchronous messages
 * with payloads held in queue_bufs.
 */
enum amsg_err msg_send_qbuf_sync(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms);

/*
 * Wrapper for amsg_send_response() to facilitate replying to messages with
 * payloads held in queue_bufs.
 */
enum amsg_err msg_send_qbuf_resp(struct amsg_resp_info **resp_info_ptr,
	uint8_t interface, uint8_t type, struct queue_buf *qbuf);

/*
 * Synchronously sends an application info message.  This may be used by a
 * message server to find and associate state for a client application.
 */
int msg_send_app_info(struct amsg_endpoint *endpoint, const char *app_name);

/*
 * Sends an template version message.  This is used by a message server to
 * associate template for a client application.
 */
int msg_send_template_ver(struct amsg_endpoint *endpoint,
	const char *template_ver);

#endif /* __AYLA_MSG_UTILS_H__ */

