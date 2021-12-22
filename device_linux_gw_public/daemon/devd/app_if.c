/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <string.h>

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/json_parser.h>
#include <ayla/socket.h>
#include <ayla/log.h>
#include <ayla/assert.h>

#include "dapi.h"
#include "serv.h"
#include "notify.h"
#include "ds.h"
#include "ops_devd.h"
#include "props_client.h"
#include "app_if.h"

#define APPD_REQUEST_TIMEOUT	10000	/* prop request to appd expires */

/* set IO subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_IO, __VA_ARGS__)

static struct appd_req *appd_req_list;
static enum app_parse_rc (*gateway_parse_data)(json_t *, int, json_t *);
int app_req_id = 1;

static int app_open(void)
{
	return socket_bind_and_listen(app_sock_path, SOCK_SEQPACKET,
	    S_IRWXU | S_IRWXG);
}

/*
 * Send a DATA nak
 */
void app_send_nak(const char *err, int id)
{
	jint_send_nak(JINT_PROTO_DATA, err, id);
}

/*
 * Send a DATA ACK
 */
void app_send_ack(int id)
{
	jint_send_ack(JINT_PROTO_DATA, id);
}

/*
 * Handle data request from app.
 */
static enum app_parse_rc app_parse_data(json_t *cmd, int recv_id, json_t *opts)
{
	const char *opstr = json_get_string(cmd, "op");
	enum ayla_data_op op;
	enum app_parse_rc rc;

	if (!opstr) {
		app_send_nak(JINT_ERR_OP, recv_id);
		return APR_DONE;
	}
	op = jint_get_data_op(opstr);
	switch (op) {
	case AD_ACK:
	case AD_NAK:
	case AD_ERROR:
	case AD_CONFIRM_TRUE:
	case AD_CONFIRM_FALSE:
		return APR_DONE;
	default:
		rc = prop_handle_data_pkt(cmd, recv_id, opts);
		if (rc != APR_DONE) {
			log_warn("rc %d for op %d", rc, op);
			return rc;
		}
		break;
	}

	return APR_DONE;
}

static void app_recv(void *arg, int sock)
{
	int buf_size = INIT_RECV_BUFSIZE;
	char *buf;
	ssize_t len = -1;
	char tmp[1];
	struct device_state *dev = arg;
	json_t *root;
	json_t *cmd;
	const char *protocol;
	json_error_t jerr;
	int recv_request_id;
	json_t *opts_obj;

	while (1) {
		buf = malloc(buf_size);
		if (!buf) {
			/* drop the packet */
			len = recv(sock, tmp, sizeof(tmp), 0);
			if (!len) {
				goto disconn;
			}
			log_err("mem err");
			jint_send_error(JINT_ERR_MEM, app_req_id);
			jint_incr_id(&app_req_id);
			return;
		}
		len = recv(sock, buf, buf_size, MSG_PEEK);
		if (!len) {
			free(buf);
disconn:
			log_warn("appd sock disconnected");
			file_event_unreg(&dev->file_events, sock, app_recv,
			    NULL, dev);
			dev->app_sock = -1;
			close(sock);
			return;
		} else if (len < buf_size) {
			break;
		}
		free(buf);
		buf_size *= 4;
		if (buf_size > MAX_RECV_BUFSIZE) {
			/* drop pkt and send err */
			recv(sock, tmp, sizeof(tmp), 0);
			jint_send_error(JINT_ERR_PKTSIZE, app_req_id);
			jint_incr_id(&app_req_id);
			return;
		}
	}
	if (len < 0) {
		log_err("recv err");
		free(buf);
		return;
	}

	/* drop the socket pkt, already in buffer due to PEEKs */
	recv(sock, tmp, sizeof(tmp), 0);
	root = json_loadb(buf, len, 0, &jerr);
	free(buf);
	if (!root) {
inval_json:
		jint_json_dump(__func__, root);
		jint_send_error(JINT_ERR_INVAL_JSON, app_req_id);
		jint_incr_id(&app_req_id);
		json_decref(root);
		return;
	}
	if (debug) {
		jint_json_dump(__func__, root);
	}
	cmd = json_object_get(root, "cmd");
	if (!cmd) {
		goto inval_json;
	}
	protocol = json_get_string(cmd, "proto");
	if (!protocol) {
		goto inval_json;
	}
	if (json_get_int(cmd, "id", &recv_request_id)) {
		goto inval_json;
	}
	opts_obj = json_object_get(cmd, "opts");
	if (!strcmp(protocol, JINT_PROTO_DATA)) {
		app_parse_data(cmd, recv_request_id, opts_obj);
	} else if (!strcmp(protocol, JINT_PROTO_GATEWAY) &&
	    gateway_parse_data) {
		gateway_parse_data(cmd, recv_request_id, opts_obj);
	} else {
		app_send_nak(JINT_ERR_UNKWN_PROTO, recv_request_id);
	}
	json_decref(root);
}

static int app_send(struct device_state *dev, void *buf, size_t len)
{
	int sock;

	sock = dev->app_sock;
	if (sock < 0) {
		log_err("app socket not connected");
		return -1;
	}
	if (send(sock, buf, len, 0) < 0) {
		log_err("send failed: %m");
		return -1;
	}
	return 0;
}

/*
 * Send JSON object to appd
 */
int app_send_json(json_t *obj)
{
	char *str;
	int rc;

	str = json_dumps(obj, JSON_COMPACT);
	log_debug("%s", str);
	rc = app_send(&device, str, strlen(str));
	free(str);
	return rc;
}

static void app_accept(void *arg, int fd)
{
	struct device_state *dev = arg;
	int sock;

	sock = socket_accept(fd);
	if (dev->app_sock >= 0) {
		log_info("app socket %d already connected, "
		    "close this new socket %d", dev->app_sock, sock);
		close(sock);
		return;
	}
	dev->app_sock = sock;
	file_event_reg(&dev->file_events, sock, app_recv, NULL, dev);
	log_info("app socket connected");
}

/*
 * Initialize the app subsystem. This subsystem sends messages to appd
 */
void app_init(void)
{
	struct device_state *dev = &device;
	int sock;

	dev->app_sock = -1;
	sock = app_open();
	if (sock < 0) {
		log_err("failed to open socket interface");
		exit(2);
	}
	file_event_reg(&dev->file_events, sock, app_accept, NULL, dev);
	jint_init(app_send_json);
}

/*
 * Notify appd that an echo failed
 */
void app_send_echo_failure_with_args(struct ops_devd_cmd *op_cmd)
{
	json_t *args = json_array();
	json_t *error;

	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	error = jint_create_error(op_cmd->err_name, op_cmd->dests_failed,
	    op_cmd->err_type, *op_cmd->op_handlers->op_name,
	    op_cmd->op_args);
	if (!error) {
		return;
	}
	json_array_append_new(args, error);
	jint_send_echo_failure_with_args(op_cmd->proto, args, app_req_id);
	json_decref(args);
	jint_incr_id(&app_req_id);
}

/*
 * Send NAK to appd for an operation
 */
void app_send_nak_with_args(struct ops_devd_cmd *op_cmd)
{
	json_t *args = json_array();
	json_t *error;

	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	error = jint_create_error(op_cmd->err_name, op_cmd->dests_failed,
	    op_cmd->err_type, *op_cmd->op_handlers->op_name,
	    op_cmd->op_args);
	if (!error) {
		return;
	}
	json_array_append_new(args, error);
	/* use the same req_id that the operation was done with */
	jint_send_nak_with_args(op_cmd->proto, args, op_cmd->req_id);
	json_decref(args);
}

/*
 * Send NAK to appd for an operation
 */
void app_send_nak_for_inval_args(struct ops_devd_cmd *op_cmd)
{
	op_cmd->err_type = JINT_ERR_INVAL_ARGS;
	op_cmd->dests_failed = op_cmd->dests_target;
	app_send_nak_with_args(op_cmd);
}

/*
 * Send schedules to appd
 */
void app_send_sched(json_t *scheds, int dests)
{
	jint_send_sched(app_req_id, scheds, NULL, dests);
	jint_incr_id(&app_req_id);
}

void app_send_confirm_true(struct ops_devd_cmd *op_cmd)
{
	jint_send_confirm_true(op_cmd->proto, op_cmd->req_id);
}

void app_send_confirm_false(struct ops_devd_cmd *op_cmd, int dests,
			const char *err)
{
	jint_send_confirm_false(op_cmd->proto, op_cmd->req_id, dests, err);
}

/*
 * Free all the pending requests
 */
void app_req_delete_all(void)
{
	struct appd_req *ar;

	while ((ar = appd_req_list) != NULL) {
		server_put_end(ar->req, HTTP_STATUS_INTERNAL_ERR);
		appd_req_list = ar->next;
		free(ar);
	}
}

/*
 * Find and remove request from list.
 */
struct appd_req *app_req_delete(u16 req_id)
{
	struct device_state *dev = &device;
	struct appd_req **pp;
	struct appd_req *ar;

	for (pp = &appd_req_list; (ar = *pp) != NULL; pp = &ar->next) {
		if (ar->req_id == req_id) {
			timer_cancel(&dev->timers, &ar->wait_timer);
			*pp = ar->next;
			ar->next = NULL;
			return ar;
		}
	}
	return NULL;
}

/*
 * Timeout for a prop get request to appd
 */
static void app_get_timeout(struct timer *timer)
{
	struct appd_req *ar =
	    CONTAINER_OF(struct appd_req, wait_timer, timer);
	struct server_req *req;

	ar = app_req_delete(ar->req_id);
	req = ar->req;
	server_put_end(req, HTTP_STATUS_UNAVAIL);
	free(ar);
}

/*
 * Allocate new request.
 */
struct appd_req *app_req_alloc(void)
{
	struct appd_req *ar;

	ar = calloc(1, sizeof(*ar));
	if (ar) {
		ar->req_id = app_req_id;
	}
	timer_init(&ar->wait_timer, app_get_timeout);
	return ar;
}

/*
 * Add request to list.
 */
void app_req_add(struct appd_req *ar)
{
	struct device_state *dev = &device;

	timer_set(&dev->timers, &ar->wait_timer, APPD_REQUEST_TIMEOUT);
	ar->next = appd_req_list;
	appd_req_list = ar;
}

/*
 * Register a handler for 'gateway' packets
 */
void app_register_gateway_handler(enum app_parse_rc
				(*gw_parser)(json_t *, int, json_t *))
{
	gateway_parse_data = gw_parser;
}
