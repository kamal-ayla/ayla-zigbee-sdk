/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/queue.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/base64.h>
#include <ayla/file_io.h>
#include <ayla/log.h>
#include <ayla/socket.h>
#include <ayla/msg_utils.h>

#include <app/ops.h>
#include <app/props.h>
#include <app/data.h>

#include <ayla/gateway_interface.h>
#include <app/gateway.h>
#include "gateway_internal.h"
#include "msg_client_internal.h"
#include "ops_internal.h"
#include "props_internal.h"
#include "data_internal.h"
#include "pthread.h"

struct file_event_table *data_file_events;
static int data_sock = -1;
static int data_req_id;
static void (*data_sched_handler)(const char *, const void *,
	size_t, json_t *);

static void data_recv(void *arg, int sock);

/*
 * Close socket to devd
 */
static void data_socket_close(void)
{
	if (data_sock < 0) {
		return;
	}
	file_event_unreg(data_file_events, data_sock, data_recv, NULL, NULL);
	close(data_sock);
	data_sock = -1;

	/* need to reconnect devd after the data socket close */
	app_set_reconnect();
}
/*
 * Open socket to devd
 */
static int data_socket_open(const char *socket_path)
{
	int sock, count = 0;

	if (data_sock >= 0) {
		log_warn("re-initializing data socket");
		data_socket_close();
	}
	/* Connect to devd's legacy socket interface */
	while ((sock = socket_connect(socket_path, SOCK_SEQPACKET)) < 0) {
		sleep(1);
		count++;
		log_warn("retrying %d socket connection to devd", count);
		if (count > 2) {
			return -1;
		}
	}
	if (file_event_reg(data_file_events, sock, data_recv, NULL, NULL) < 0) {
		close(sock);
		return -1;
	}
	data_sock = sock;
	return 0;
}

static int data_send(int sock, void *buf, size_t len)
{
	if (send(sock, buf, len, 0) < 0) {
		log_err("send failed: %m");
		return -1;
	}
	return 0;
}

/*
 * Send json through socket
 */
int data_send_json(json_t *obj)
{
	char *str;
	int rc;

	str = json_dumps(obj, JSON_COMPACT);
	if (!str) {
		log_warn("bad json obj");
		return 0;
	}
	log_debug("%s", str);
	rc = data_send(data_sock, str, strlen(str));
	free(str);
	return rc;
}

/*
 * Send a DATA nak
 */
static void data_send_nak(const char *err, int id)
{
	jint_send_nak(JINT_PROTO_DATA, err, id);
}

static int data_process_sched_update(json_t *cmd, int req_id)
{
	const char *name;
	const char *type;
	const char *value;
	int i;
	json_t *args;
	json_t *arg;
	json_t *val;
	json_t *schedobj;

	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
inval_args:
		data_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		return -1;
	}
	for (i = 0; i < json_array_size(args); i++) {
		arg = json_array_get(args, i);
		if (!json_is_object(arg)) {
			goto inval_args;
		}
		schedobj = json_object_get(arg, "schedule");
		if (!schedobj) {
			goto inval_args;
		}
		name = json_get_string(schedobj, "name");
		if (!name) {
			data_send_nak(JINT_ERR_UNKWN_PROP, req_id);
			continue;
		}
		type = json_get_string(schedobj, "base_type");
		if (!type || strcmp(data_types[ATLV_SCHED], type)) {
			data_send_nak(JINT_ERR_INVAL_TYPE, req_id);
			continue;
		}
		val = json_object_get(schedobj, "value");
		if (!val) {
bad_value:
			data_send_nak(JINT_ERR_BAD_VAL, req_id);
			continue;
		}
		if (json_is_null(val)) {
			data_sched_handler(name, NULL, 0,
			    json_object_get(schedobj, "metadata"));
			continue;
		}
		if (!json_is_string(val)) {
			goto bad_value;
		}
		value = json_string_value(val);
		data_sched_handler(name, value, strlen(value),
		    json_object_get(schedobj, "metadata"));
	}

	return 0;
}

static int data_message_prop_update(json_t *obj, struct prop *prop)
{
	const char *value;
	int rc;

	value = json_get_string(obj, "value");
	if (!value) {
		log_err("prop %s no value", prop->name);
		return -1;
	}
	rc = prop_request_for_message(prop, value);
	if (rc != ERR_OK) {
		log_err("prop_request_for_message %s error",
		    prop->name);
		return -1;
	}

	return 0;
}

static int data_process_prop_update(json_t *cmd, int req_id)
{
	const char *name;
	const char *type;
	int i;
	json_t *args;
	json_t *arg;
	json_t *val;
	json_t *propobj;
	struct prop *prop;
	void *ack_arg;
	json_t *opts_obj;
	struct data_obj dataobj;
	const void *dataobj_val;
	struct op_args op_args;
	int rc;

	const char *opstr = json_get_string(cmd, "op");
	enum ayla_data_op op;

	if (!opstr) {
		goto op_err;
	}
	op = jint_get_data_op(opstr);

	memset(&op_args, 0, sizeof(op_args));
	opts_obj = json_object_get(cmd, "opts");
	if (opts_obj) {
		json_get_uint8(opts_obj, "source", &op_args.source);
	}
	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
inval_args:
		data_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		return -1;
	}
	for (i = 0; i < json_array_size(args); i++) {
		arg = json_array_get(args, i);
		if (!json_is_object(arg)) {
			goto inval_args;
		}
		propobj = json_object_get(arg, "property");
		if (!propobj) {
			goto inval_args;
		}
		name = json_get_string(propobj, "name");
		if (!name) {
			data_send_nak(JINT_ERR_UNKWN_PROP, req_id);
			continue;
		}
		prop = prop_lookup(name);
		if (!prop) {
			data_send_nak(JINT_ERR_UNKWN_PROP, req_id);
			continue;
		}
		if ((prop->skip_init_update_from_cloud) && (op == AD_PROP_RESP)) {
			log_debug("||||||||||Skipping cloud init update for prop %s|||||||||||||", prop->name);
			/* skip over props that don't want update from cloud during init*/
			continue;
		}
		if (!prop->set) {
			/* skip over props that don't have set defiend */
			continue;
		}
		type = json_get_string(propobj, "base_type");
		if (!type || strcmp(data_types[prop->type], type)) {
			data_send_nak(JINT_ERR_INVAL_TYPE, req_id);
			continue;
		}
		ack_arg = NULL;
		op_args.ack_arg = NULL;
		if (json_get_string(propobj, "id")) {	/* explicit ack */
			ack_arg = ops_prop_ack_json_create(JINT_PROTO_DATA,
			    req_id, op_args.source, arg);
			if (prop->app_manages_acks) {
				op_args.ack_arg = ack_arg;
			}
		}

		/* Handle message prop */
		if (prop->type == PROP_MESSAGE) {
			rc = data_message_prop_update(propobj, prop);
			if (rc) {
				data_send_nak(JINT_ERR_UNKWN, req_id);
			}
			goto ack_and_continue;
		}

		val = json_object_get(propobj, "value");
		if (!val) {
			goto bad_value;
		}
		if (prop->pass_jsonobj) {
			rc = prop_datapoint_set(prop, val, sizeof(json_t *),
			    &op_args);
			goto ack_and_continue;
		}
		if (json_is_null(val)) {
			if (prop->reject_null) {
				goto bad_value;
			}
			rc = prop_datapoint_set(prop, NULL, 0, &op_args);
			goto ack_and_continue;
		}
		if (data_json_to_value(&dataobj, val, prop->type,
		    &dataobj_val)) {
			goto bad_value;
		}
		rc = prop_datapoint_set(prop, dataobj_val, dataobj.val_len,
		    &op_args);
		if (prop->type == PROP_BLOB) {
			free(dataobj.val.decoded_val);
		}
ack_and_continue:
		if (ack_arg && !prop->app_manages_acks) {
			ops_prop_ack_send(ack_arg, rc, 0);
		}
		continue;
bad_value:
		if (ack_arg) {
			json_decref(ack_arg);
		}
		data_send_nak(JINT_ERR_BAD_VAL, req_id);
	}

op_err:
		data_send_nak(JINT_ERR_OP, req_id);

	return 0;
}

static int data_message_prop_set(enum ayla_data_op op,
			json_t *cmd, int req_id)
{
	static u8 message_val[PROP_MSG_LEN];
	static size_t message_len;
	json_t *args;
	json_t *arg;
	json_t *prop_obj;
	const char *name;
	const char *path;
	struct prop *prop;
	ssize_t read_len;
	int rc;

	args = json_object_get(cmd, "args");
	if (!json_is_array(args)) {
		data_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		return APR_ERR;
	}
	arg = json_array_get(args, 0);
	prop_obj = json_object_get(arg, "property");
	if (!prop_obj) {
		data_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		return APR_ERR;
	}

	name  = json_get_string(prop_obj, "name");
	if (!name) {
		data_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		return APR_ERR;
	}
	prop = prop_lookup(name);
	if (!prop) {
		data_send_nak(JINT_ERR_UNKWN_PROP, req_id);
		return APR_ERR;
	}

	path = json_get_string(prop_obj, "path");
	if (!path) {
		data_send_nak(JINT_ERR_INVAL_PATH, req_id);
		return APR_ERR;
	}

	log_debug("op %s, path %s, prop %s", data_ops[op], path, prop->name);

	read_len = file_get_content(path, message_val, PROP_MSG_LEN);
	if (read_len < 0) {
		log_err("file_get_content %s for prop %s",
		    path, prop->name);
		data_send_nak(JINT_ERR_UNKWN, req_id);
		return APR_ERR;
	}
	message_len = read_len;

	rc = prop_datapoint_set(prop, message_val, message_len, NULL);
	if (rc < 0) {
		log_err("prop_datapoint_set %s failed", prop->name);
		return APR_ERR;
	}

	return APR_DONE;
}

/*
 * Parse "data" protocol json objects
 */
static enum app_parse_rc data_recv_data(json_t *cmd, int recv_request_id)
{
	const char *opstr = json_get_string(cmd, "op");
	enum ayla_data_op op;
	const char *name;
	const char *data;
	const char *location;
	json_t *args;
	json_t *arg;
	json_t *property_j;

	if (!opstr) {
		goto op_err;
	}
	op = jint_get_data_op(opstr);
	switch (op) {
	case AD_ACK:
		break;
	case AD_ERROR:
		break;
	case AD_ECHO_FAILURE:
		if (ops_echo_failure_process(cmd, prop_echo_failure_process)) {
			goto inval_args;
		}
		break;
	case AD_PROP_RESP:
	case AD_PROP_UPDATE:
		if (data_process_prop_update(cmd, recv_request_id)) {
			return APR_ERR;
		}
		break;
	case AD_SCHED_UPDATE:
		if (!data_sched_handler) {
			goto op_err;
		}
		if (data_process_sched_update(cmd, recv_request_id)) {
			return APR_ERR;
		}
		break;
	case AD_PROP_REQ:
		args = json_object_get(cmd, "args");
		if (!json_is_array(args)) {
			goto inval_args;
		}
		arg = json_array_get(args, 0);
		name  = json_get_string(arg, "name");
		if (!name) {
			data_send_nak(JINT_ERR_UNKWN_PROP, recv_request_id);
			return APR_ERR;
		}
		data  = json_get_string(arg, "data");
		if (!data || data[0] == '\0' || !strcmp(data, "none")) {
			prop_response_by_name(name, recv_request_id, NULL);
		} else {
			prop_response_by_name(name, recv_request_id, data);
		}
		break;
	case AD_DP_LOC:
		args = json_object_get(cmd, "args");
		if (!json_is_array(args)) {
			goto inval_args;
		}
		arg = json_array_get(args, 0);
		property_j = json_object_get(arg, "property");
		if (!property_j) {
			goto inval_args;
		}
		name  = json_get_string(property_j, "name");
		if (!name) {
			data_send_nak(JINT_ERR_UNKWN_PROP, recv_request_id);
			return APR_ERR;
		}
		location = json_get_string(property_j, "location");
		if (!location) {
			return APR_ERR;
		}
		if (prop_location_for_file_dp(name, location)) {
			return APR_ERR;
		}
		break;
	case AD_PROP_REQ_ALL:
	case AD_PROP_REQ_TO_DEV:
	case AD_PROP_REQ_FROM_DEV:
		/* TODO */
		break;
	case AD_MSG_GET:
		if (data_message_prop_set(op, cmd, recv_request_id)) {
			return APR_ERR;
		}
		break;
	default:
		log_err("can't process opcode %d", op);
		data_send_nak(JINT_ERR_OP, recv_request_id);
		return APR_ERR;
	}
	return APR_DONE;
op_err:
	data_send_nak(JINT_ERR_OP, recv_request_id);
	return APR_ERR;
inval_args:
	data_send_nak(JINT_ERR_INVAL_ARGS, recv_request_id);
	return APR_ERR;
}

/*
 * Parse 'cmd' object
 */
enum app_parse_rc data_cmd_parse(json_t *cmd, const char *protocol,
				int recv_request_id)
{
	enum app_parse_rc rc = APR_DONE;

	if (!strcmp(protocol, JINT_PROTO_DATA)) {
		rc = data_recv_data(cmd, recv_request_id);
	} else if (!strcmp(protocol, JINT_PROTO_GATEWAY)) {
		rc = gw_cmd_parse(cmd, recv_request_id);
	} else {
		data_send_nak(JINT_ERR_UNKWN_PROTO, recv_request_id);
	}
	return rc;
}

/*
 * Receive and process data from devd socket
 */
static void data_recv(void *arg, int sock)
{
	int buf_size = INIT_RECV_BUFSIZE;
	char *buf;
	ssize_t len = -1;
	char tmp[1];
	json_t *root;
	json_t *cmd;
	const char *protocol;
	json_error_t jerr;
	int recv_request_id;
	const char *opstr;
	enum ayla_data_op op;
	char *str_dbg;

	while (1) {
		buf = malloc(buf_size);
		if (!buf) {
			/* drop the packet */
			len = recv(sock, tmp, sizeof(tmp), 0);
			log_debug("APPD_DEBUG : drop the packet recv %s thrdId[%lu]",tmp,pthread_self());
			if (len <= 0) {
				goto disconn;
			}
			log_err("mem err");
			jint_send_error(JINT_ERR_MEM, data_req_id);
			log_debug("APPD_DEBUG : drop the packet data_req_id %d",data_req_id);
			jint_incr_id(&data_req_id);
			return;
		}
		memset(buf,0,buf_size);
		len = recv(sock, buf, buf_size, MSG_PEEK);
		log_debug("APPD_DEBUG : recv buf %s buf_size %d",buf,buf_size);
		if (len <= 0) {
			if (buf) {
				free(buf);
			}
disconn:
			log_warn("devd sock disconnected");
			data_socket_close();
			return;
		}
		if (len < buf_size) {
			break;
		}
		free(buf);
		buf_size *= 4;
		if (buf_size > MAX_RECV_BUFSIZE) {
			/* drop pkt and send err */
			recv(sock, tmp, sizeof(tmp), 0);
			log_debug("APPD_DEBUG : drop pkt and send err tmp %s",tmp);
			jint_send_error(JINT_ERR_PKTSIZE, data_req_id);
			log_debug("APPD_DEBUG : data_req_id %d",data_req_id);
			jint_incr_id(&data_req_id);
			return;
		}
	}
	if (len < 0) {
		log_err("recv err");
		free(buf);
		return;
	}

	memset(tmp,0,sizeof(tmp));
	/* drop the socket pkt, already in buffer due to PEEKs */
	recv(sock, tmp, sizeof(tmp), 0);
	log_debug("APDD_DEBUG : data recv before buf : %s, len : %d,",buf,len);
	root = json_loadb(buf, len, 0, &jerr);
	log_debug("APDD_DEBUG : data recv after buf : %s, len : %d,",buf,len);
	if (!root) {
		log_warn("err %s", jerr.text);
inval_json:
		jint_send_error(JINT_ERR_INVAL_JSON, data_req_id);
		jint_incr_id(&data_req_id);
		if (root) {
			jint_json_dump(__func__, root);
			json_decref(root);
		}
		free(buf);
		return;
	}

	str_dbg = json_dumps(root, JSON_COMPACT);
	log_debug("APPD DEBUG: str_bdg : %s thrdId[%lu]", str_dbg,pthread_self());
	free(str_dbg);

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
	opstr = json_get_string(cmd, "op");
	if (!opstr) {
		data_send_nak(JINT_ERR_OP, recv_request_id);
		goto cleanup;
	}
	op = jint_get_data_op(opstr);
	switch (op) {
	case AD_CONFIRM_TRUE:
		ops_true_confirmation_process(recv_request_id);
		break;
	case AD_CONFIRM_FALSE:
		ops_false_confirmation_process(cmd, recv_request_id);
		break;
	case AD_NAK:
		if (ops_nak_process(cmd, recv_request_id)) {
			data_send_nak(JINT_ERR_INVAL_ARGS, recv_request_id);
			goto cleanup;
		}
		break;
	default:
		data_cmd_parse(cmd, protocol, recv_request_id);
		break;
	}
	log_debug("data recv call end thrdId[%lu]",pthread_self());
cleanup:
	json_decref(root);
	free(buf);
}


/*
 * Get the value inside a json object based on the expected type
 */
int data_json_to_value(struct data_obj *obj, json_t *val_j, enum prop_type type,
			const void **dataobj_val)
{
	obj->type = type;
	*dataobj_val = &obj->val;

	switch (type) {
	case PROP_INTEGER:
	case PROP_BOOLEAN:
		if (!json_is_integer(val_j)) {
			return -1;
		}
		obj->val.int_val = json_integer_value(val_j);
		if (type == PROP_BOOLEAN) {
			obj->val.bool_val = obj->val.int_val != 0;
			obj->val_len = 1;
		} else {
			obj->val_len = sizeof(int);
		}
		break;
	case PROP_DECIMAL:
		if (!json_is_number(val_j)) {
			return -1;
		}
		obj->val.rval = json_number_value(val_j);
		obj->val_len = sizeof(double);
		break;
	case PROP_STRING:
	case PROP_BLOB:
	case PROP_FILE:
		if (!json_is_string(val_j)) {
			return -1;
		}
		obj->val.str_val = json_string_value(val_j);
		obj->val_len = strlen(obj->val.str_val);
		*dataobj_val = obj->val.str_val;
		if (type == PROP_BLOB) {
			obj->val.decoded_val = base64_decode(obj->val.str_val,
			    obj->val_len, &obj->val_len);
			*dataobj_val = obj->val.decoded_val;
		}
		break;
	default:
		log_debug("unsurported type %d", type);
		return -1;
	}
	return 0;
}

/*
 * Create a json object based on the value type
 */
json_t *data_type_to_json_obj(const void *val, size_t val_len,
	enum prop_type type)
{
	json_t *root;
	char *data;

	switch (type) {
	case PROP_BOOLEAN:
		root = json_integer(*(u8 *)val);
		break;
	case PROP_INTEGER:
		root = json_integer(*(int *)val);
		break;
	case PROP_STRING:
	case PROP_MESSAGE:
	case PROP_FILE:
		root = json_string((char *)val);
		break;
	case PROP_DECIMAL:
		root = json_real(*(double *)val);
		break;
	case PROP_BLOB:
		data = base64_encode(val, val_len, NULL);
		if (!data) {
			log_err("base64 failed");
			return NULL;
		}
		root = json_string(data);
		free(data);
		break;
	default:
		log_err("%d not supported", type);
		return NULL;
	}

	return root;
}

/*
 * Helper for FILE property commands
  */
static json_t *data_file_cmd_helper(const struct prop_file_def *prop_file)
{
	json_t *obj = json_object();
	json_t *prop_obj = json_object();

	json_object_set_new(obj, "property", prop_obj);

	json_object_set_new(prop_obj, "name",
	    json_string(prop_file->prop->name));

	if (prop_file->location) {
		json_object_set_new(prop_obj, "location",
		    json_string(prop_file->location));
	}
	if (prop_file->path) {
		json_object_set_new(prop_obj, "value",
		    json_string(prop_file->path));
	}
	return obj;
}

/*
 * Send command to devd
 */
int data_send_cmd(const char *proto, const char *op, json_t *args, int req_id,
		int *pcmd_req_id, const struct op_options *opts)
{
	json_t *root;
	json_t *cmd;
	int rc;

	if (!req_id) {
		req_id = data_req_id;
		jint_incr_id(&data_req_id);
	}
	if (pcmd_req_id) {
		*pcmd_req_id = req_id;
	}
	root = jint_new_cmd(proto, op, req_id);
	cmd = json_object_get(root, "cmd");
	if (opts && opts->confirm) {
		jint_confirm_set(cmd, opts->confirm);
	}
	if (opts && opts->echo) {
		jint_echo_set(cmd, opts->echo);
	}
	if (opts && opts->dests) {
		jint_dests_set(cmd, opts->dests);
	}
	if (args) {
		json_object_set_new(cmd, "args", args);
	}

	rc = data_send_json(root);
	json_decref(root);

	return rc;
}

/*
 * Create a json structure of the information inside struct prop_cmd
 */
static json_t *data_create_json_from_pcmd(const struct prop_cmd *pcmd)
{
	json_t *obj;
	json_t *prop_obj;
	json_t *metadata_obj;

	prop_obj = json_object();
	REQUIRE(prop_obj, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(prop_obj, "name", json_string(pcmd->prop->name));
	json_object_set_new(prop_obj, "base_type",
	    json_string(data_types[pcmd->prop->type]));
	if (pcmd->prop->type != PROP_FILE) {
		json_object_set_new(prop_obj, "value", data_type_to_json_obj(
		    pcmd->val, pcmd->val_len, pcmd->prop->type));
	}
	if (pcmd->opts.metadata && pcmd->opts.metadata->num_entries) {
		metadata_obj = prop_metadata_to_json(pcmd->opts.metadata);
		json_object_set_new(prop_obj, "metadata", metadata_obj);
	}
	if (pcmd->opts.dev_time_ms) {
		json_object_set_new(prop_obj, "dev_time_ms",
		    json_integer(pcmd->opts.dev_time_ms));
	}
	obj = json_object();
	REQUIRE(obj, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(obj, "property", prop_obj);
	return obj;
}

/*
 * Add message property info inside prop_obj
 */
static int data_add_msg_prop_info(json_t *prop_obj,
		struct prop_cmd *pcmd)
{
	char tmp_filename[PATH_MAX];
	int tmp_fd;

	/*
	 * Create a tmp file to save message data
	 */
	snprintf(tmp_filename, sizeof(tmp_filename),
	    "%s/appd_down_%s.XXXXXX", prop_temp_dir, pcmd->prop->name);
	tmp_fd = mkstemp(tmp_filename);
	if (tmp_fd == -1) {
		log_err("mkstemp %s err %m", tmp_filename);
		json_decref(prop_obj);
		return -1;
	}
	close(tmp_fd);

	json_object_set_new(prop_obj, "value",
	    json_string(tmp_filename));

	json_object_set_new(prop_obj, "location",
	    json_string((char *)pcmd->val));

	/* Update the location(pcmd->val) to path */
	free(pcmd->val);
	pcmd->val = strdup(tmp_filename);

	json_object_set_new(prop_obj, "base_type",
	    json_string(data_types[PROP_MESSAGE]));

	return 0;
}

/*
 * Create a json structure of the information inside struct prop_cmd
 */
static json_t *data_create_json_for_req(const struct prop_cmd *pcmd)
{
	json_t *prop_obj;
	json_t *obj;
	int ret;

	prop_obj = json_object();
	REQUIRE(prop_obj, REQUIRE_MSG_ALLOCATION);

	if ((pcmd->prop->type == PROP_MESSAGE) && pcmd->val) {
		ret = data_add_msg_prop_info(prop_obj,
		    (struct prop_cmd *)pcmd);
		if (ret) {
			log_err("data_add_msg_prop_info error");
			json_decref(prop_obj);
			return NULL;
		}
	}

	json_object_set_new(prop_obj, "name", json_string(pcmd->prop->name));

	obj = json_object();
	REQUIRE(obj, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(obj, "property", prop_obj);
	return obj;
}

/*
 * Execute a prop command. Sends, receives, etc.
 */
int data_execute_prop_cmd(const struct prop_cmd *pcmd, int *pcmd_req_id)
{
	json_t *args = NULL;
	json_t *property;
	int req_id = 0;

	if (pcmd->op == AD_PROP_RESP) {
		req_id = pcmd->req_id;
	}

	switch (pcmd->op) {
	case AD_PROP_REQ_ALL:
	case AD_PROP_REQ_TO_DEV:
	case AD_PROP_REQ_FROM_DEV:
		break;
	case AD_PROP_SEND:
	case AD_PROP_RESP:
	case AD_DP_CREATE:
		args = json_array();
		REQUIRE(args, REQUIRE_MSG_ALLOCATION);
		property = data_create_json_from_pcmd(pcmd);
		json_array_append_new(args, property);
		break;
	case AD_DP_SEND:
	case AD_DP_REQ:
	case AD_DP_FETCHED:
		args = json_array();
		REQUIRE(args, REQUIRE_MSG_ALLOCATION);
		property = data_file_cmd_helper(
		    (const struct prop_file_def *)pcmd->val);
		json_array_append_new(args, property);
		break;
	case AD_PROP_REQ:
		args = json_array();
		REQUIRE(args, REQUIRE_MSG_ALLOCATION);
		property = data_create_json_for_req(pcmd);
		if (!property) {
			log_err("prop %s op %s failed",
			    pcmd->prop->name, data_ops[pcmd->op]);
			json_decref(args);
			return -1;
		}
		json_array_append_new(args, property);
		break;
	default:
		log_err("%d op not supported", pcmd->op);
		return -1;
	}

	if (data_send_cmd(JINT_PROTO_DATA, data_ops[pcmd->op], args, req_id,
	    pcmd_req_id, &pcmd->opts)) {
		log_warn("%d op send failed", pcmd->op);
		return -1;
	}

	return 0;
}

/*
 * Execute a batch cmd
 */
int data_execute_batch_cmd(const struct prop_batch_sent_list *batch_sent_list,
			    int *pcmd_req_id)
{
	struct prop_batch_entry *batch_entry;
	json_t *args;
	json_t *batch_dp;
	json_t *property;
	json_t *dp_info;

	args = json_array();
	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	STAILQ_FOREACH(batch_entry, &batch_sent_list->batch_list->batchq,
	    link) {
		property = data_create_json_from_pcmd(batch_entry->pcmd);
		dp_info = json_object_get(property, "property");
		batch_dp = json_object();
		REQUIRE(batch_dp, REQUIRE_MSG_ALLOCATION);
		json_object_set(batch_dp, "property", dp_info);
		json_object_set_new(batch_dp, "batch_id",
		    json_integer(batch_entry->entry_id));
		json_array_append_new(args, batch_dp);
		json_decref(property);
	}

	if (data_send_cmd(JINT_PROTO_DATA, data_ops[AD_PROP_BATCH_SEND], args,
	    0, pcmd_req_id, &batch_sent_list->opts)) {
		log_warn("batch send failed");
		return -1;
	}

	return 0;
}

void data_set_schedule_handler(void (*sched_handler)(const char *,
		const void *, size_t, json_t *))
{
	data_sched_handler = sched_handler;
}

/*
 * Initialize the data client.  This provides a messaging interface for
 * sending and receiving properties and related data with devd.
 *
 * This function is a preferred alternative to data_client_init(),
 * and is intended to be called by a library to simplify application
 * initialization.
 */
int data_client_init_internal(struct file_event_table *file_events,
	const char *socket_path)
{
	data_file_events = file_events;

	/* Connect to legacy socket interface */
	if (data_socket_open(socket_path) < 0) {
		return -1;
	}
	jint_init(data_send_json);
	return 0;
}

int data_client_init_legacy(struct file_event_table *file_events,
	struct timer_head *timers, const char *socket_path)
{

	char path[PATH_MAX];
	char *cp;
	size_t path_len;

	/*
	 * Initialize and connect to the cloud client message interface here.
	 * Normally, the application or app library would do this, but since
	 * this function is meant to support legacy applications that existed
	 * before msg_client, we are bootstrapping it here.
	 */
	if (msg_client_init(file_events, timers) < 0) {
		return -1;
	}
	/*
	 * The new msg_client interface with devd uses a different socket path.
	 * Extract the socket directory from the existing socket_path to
	 * generate the client's message socket path.
	 * socket_path should look like: <socket dir>/appd/sock
	 * The client's socket path should be: <socket dir>/devd/msg_sock
	 */
	if (!file_get_dir(socket_path, path, sizeof(path))) {
		log_err("failed to get socket directory from: %s",
		    socket_path);
		return -1;
	}
	cp = strstr(path, "/" MSG_APP_NAME_APP);
	if (!cp) {
		log_err("path conversion failed: no %s directory",
		    MSG_APP_NAME_APP);
		return -1;
	}
	path_len = cp - path;
	snprintf(path + path_len, sizeof(path) - path_len, "/%s/%s",
	    MSG_APP_NAME_CLIENT, MSG_SOCKET_DEFAULT);
	/* Connect to the cloud client to enable new functionality */
	while (msg_client_connect(path) < 0) {
		sleep(5);
	}
	/* Initialize the legacy socket interface */
	return data_client_init_internal(file_events, socket_path);
}
