/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>

#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/log.h>
#include <ayla/json_interface.h>

/* set IO subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_IO, __VA_ARGS__)

/*
 * Helper functions for JSON Interface. Shared between Devd and Appd
 */

const char * const data_types[] = JINT_TYPE_NAMES;
const char * const data_ops[] = JINT_DATA_OP_NAMES;

static int (*jint_send)(json_t *);
/*
 * Generate a new cmd
 */
json_t *jint_new_cmd(const char *proto, const char *op, int req_id)
{
	json_t *cmd = json_object();
	json_t *root = json_object();
	json_t *protocol = json_string(proto);
	json_t *req = json_integer(req_id);
	json_t *operation = json_string(op);

	REQUIRE(cmd, REQUIRE_MSG_ALLOCATION);
	REQUIRE(root, REQUIRE_MSG_ALLOCATION);
	REQUIRE(protocol, REQUIRE_MSG_ALLOCATION);
	REQUIRE(req, REQUIRE_MSG_ALLOCATION);
	REQUIRE(operation, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(cmd, "proto", protocol);
	json_object_set_new(cmd, "id", req);
	json_object_set_new(cmd, "op", operation);
	json_object_set_new(root, "cmd", cmd);

	return root;
}

/*
 * Helper function for creating an obj if it doesn't exists
 */
static json_t *jint_add_obj_if_needed(json_t *root, const char *key)
{
	json_t *target = json_object_get(root, key);

	if (target == NULL) {
		target = json_object();
		REQUIRE(target, REQUIRE_MSG_ALLOCATION);
		json_object_set_new(root, key, target);
	}
	return target;
}

/*
 * Helper function for adding/removing options
 */
static void jint_option_set(json_t *cmd, const char *key, int value)
{
	json_t *opts = jint_add_obj_if_needed(cmd, "opts");

	if (value) {
		json_object_set_new(opts, key, json_true());
	} else {
		json_object_set_new(opts, key, json_false());
	}
}

/*
 * Add/remove "confirm" tag to json object. If json object is null, a new one
 * is created.
 */
void jint_confirm_set(json_t *cmd, int confirm)
{
	jint_option_set(cmd, "confirm", confirm);
}

/*
 * Add/remove "echo" tag to json object. If json object is null, a new one
 * is created.
 */
void jint_echo_set(json_t *cmd, int echo)
{
	jint_option_set(cmd, "echo", echo);
}

/*
 * Add "source" tag to json object.  This is represented as an index, where
 * ADS is 1, LAN client #1 is 2, etc.
 */
void jint_source_set(json_t *cmd, int source)
{
	json_t *opts = jint_add_obj_if_needed(cmd, "opts");

	json_object_set_new(opts, "source", json_integer(source));
}

/*
 * Add "destinations" tag to json object.  This is a bitmask, where ADS is
 * 1 << 0, LAN client #1 is 1 << 1, etc.
 */
void jint_dests_set(json_t *cmd, int dests)
{
	json_t *opts = jint_add_obj_if_needed(cmd, "opts");

	json_object_set_new(opts, "dests", json_integer(dests));
}

/*
 * Creates args with err.
 */
static json_t *jint_error_set(const char *err)
{
	json_t *args = json_array();
	json_t *error = json_object();
	json_t *errj = json_string(err);

	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	REQUIRE(error, REQUIRE_MSG_ALLOCATION);
	REQUIRE(errj, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(error, "err", errj);
	json_array_append_new(args, error);

	return args;
}

/*
 * Creates an error json object for a property.
 * name is the name of the property and dests is a bit mask of the
 * failed dests for that property.
 * Example:
 *{
 *      "err": "conn_err
 *      "op": ”prop_send”,
 *      "name": "Blue_LED",
 *      "dests”: 1,
 *      "op_args": [
 *        {
 *         "property": {
 *           "address”: "54”,
 *           "subdevice_key”: "2”,
 *           "template_key": "0x06”,
 *           "name”: "0”,
 *           "base_type": "boolean",
 *           "value": 1
 *            }
 *           }
 *           ]
 *      }
 */
json_t *jint_create_error(const char *name, int dests, const char *err,
		const char *err_op, json_t *op_args)
{
	json_t *error = json_object();

	REQUIRE(error, REQUIRE_MSG_ALLOCATION);
	if (err) {
		json_object_set_new(error, "err", json_string(err));
	}
	if (name) {
		json_object_set_new(error, "name", json_string(name));
	}
	if (dests) {
		json_object_set_new(error, "dests", json_integer(dests));
	}
	if (err_op) {
		json_object_set_new(error, "op", json_string(err_op));
	}
	if (op_args) {
		json_object_set(error, "op_args", op_args);
	}

	return error;
}

/*
 * Helper function for passing the json to the send function
 * Decref the root after
 */
static int jint_send_json(json_t *root)
{
	int rc;

	if (jint_send) {
		rc = jint_send(root);
	} else {
		rc = -1;
	}
	json_decref(root);
	return rc;
}

/*
 * Send echo failure with the args given.
 */
int jint_send_echo_failure_with_args(const char *proto, json_t *args,
				int req_id)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_ECHO_FAILURE], req_id);
	json_t *cmd;

	cmd = json_object_get(root, "cmd");
	json_object_set(cmd, "args", args);

	return jint_send_json(root);
}

/*
 * Send nak with the args given.
 */
int jint_send_nak_with_args(const char *proto, json_t *args, int req_id)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_NAK], req_id);
	json_t *cmd;

	cmd = json_object_get(root, "cmd");
	json_object_set(cmd, "args", args);

	return jint_send_json(root);
}

/*
 * A generic nak. To send a more complicated nak, build it and send using
 * jint_send_nak_with_args
 */
int jint_send_nak(const char *proto, const char *err, int req_id)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_NAK], req_id);
	json_t *cmd = json_object_get(root, "cmd");
	json_t *args = jint_error_set(err);

	json_object_set_new(cmd, "args", args);

	return jint_send_json(root);
}

/*
 * Send Ack. Specific to a request id
 */
int jint_send_ack(const char *proto, int req_id)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_ACK], req_id);

	return jint_send_json(root);
}

/*
 * Send TRUE Confirmation. Specific to a request id
 */
int jint_send_confirm_true(const char *proto, int req_id)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_CONFIRM_TRUE], req_id);

	return jint_send_json(root);
}

/*
 * Send FALSE Confirmation. Specific to a request id
 */
int jint_send_confirm_false(const char *proto, int req_id, int dests,
			    const char *err)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_CONFIRM_FALSE], req_id);
	json_t *cmd = json_object_get(root, "cmd");
	json_t *args = json_array();
	json_t *err_j;

	REQUIRE(args, REQUIRE_MSG_ALLOCATION);

	err_j = jint_create_error(NULL, dests, err, NULL, NULL);
	json_array_append_new(args, err_j);
	json_object_set_new(cmd, "args", args);
	return jint_send_json(root);
}

/*
 * Send property response to a prop request
 */
int jint_send_prop_resp(const char *proto, int req_id, json_t *props,
			int source)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_PROP_RESP], req_id);
	json_t *cmd = json_object_get(root, "cmd");

	json_incref(props);
	json_object_set_new(cmd, "args", props);
	if (source) {
		jint_source_set(cmd, source);
	}
	return jint_send_json(root);
}

/*
 * Send property update
 */
int jint_send_prop(const char *proto, const char *op, int req_id, json_t *props,
	int confirm, int source)
{
	json_t *root = jint_new_cmd(proto, op, req_id);
	json_t *cmd = json_object_get(root, "cmd");

	json_object_set(cmd, "args", props);
	if (confirm) {
		jint_confirm_set(cmd, confirm);
	}
	if (source) {
		jint_source_set(cmd, source);
	}
	return jint_send_json(root);
}

/*
 * Send schedule update
 */
int jint_send_sched(int req_id, json_t *props, json_t *opts, int dests)
{
	json_t *root = jint_new_cmd(JINT_PROTO_DATA,
	    data_ops[AD_SCHED_UPDATE], req_id);
	json_t *cmd = json_object_get(root, "cmd");

	json_object_set(cmd, "args", props);
	if (opts) {
		json_object_set(cmd, "opts", opts);
	}
	if (dests) {
		json_object_set_new(cmd, "dests", json_integer(dests));
	}
	return jint_send_json(root);
}

/*
 * Send general error
 */
int jint_send_error(const char *err, int req_id)
{
	json_t *root = jint_new_cmd(JINT_PROTO_DATA,
	    data_ops[AD_ERROR], req_id);
	json_t *cmd = json_object_get(root, "cmd");
	json_t *args = jint_error_set(err);

	json_object_set_new(cmd, "args", args);
	return jint_send_json(root);
}

/*
 * Dump out the json to log debug IO subsystem
 */
void jint_json_dump(const char *prefix, json_t *root)
{
	char *out;

	if (!root) {
		log_base(prefix, LOG_AYLA_DEBUG, "null");
		return;
	}
	if (!log_debug_enabled()) {
		return;
	}
	out = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
	log_base(prefix, LOG_AYLA_DEBUG, "%s", out);
	free(out);
}

/*
 * Increment the request id. Req Id can never be 0 because a 0
 * means to use any request id in the reply.
 */
void jint_incr_id(int *req_id)
{
	(*req_id)++;
	if (*req_id == 0) {
		*req_id = 1;
	}
}

/*
 * Return the data op given it's json string
 */
enum ayla_data_op jint_get_data_op(const char *str)
{
	int i;

	for (i = 0; i < ARRAY_LEN(data_ops); i++) {
		if (data_ops[i] && !strcmp(str, data_ops[i])) {
			return (enum ayla_data_op)i;
		}
	}
	return AD_NOP;
}

/*
 * Return the data type given it's string form
 */
enum ayla_tlv_type jint_get_data_type(const char *str)
{
	int i;

	for (i = 0; i < ARRAY_LEN(data_types); i++) {
		if (data_types[i] && !strcmp(str, data_types[i])) {
			return (enum ayla_tlv_type)i;
		}
	}
	return ATLV_INVALID;
}

/*
 * Initalize the json interface, set the send function
 */
void jint_init(int (*send)(json_t *))
{
	jint_send = send;
}
