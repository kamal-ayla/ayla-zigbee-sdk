/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __JSON_INTERFACE_H__
#define __JSON_INTERFACE_H__

#define INIT_RECV_BUFSIZE	1000	/* 1K */
#define MAX_RECV_BUFSIZE	256000	/* 256K */

#define JINT_PROTO_DATA "data"
#define JINT_PROTO_GATEWAY "gateway"

#define JINT_TYPE_NAMES {		\
	[ATLV_INT] = "integer",		\
	[ATLV_UTF8] = "string",		\
	[ATLV_CONF] = "config",		\
	[ATLV_BOOL] = "boolean",	\
	[ATLV_BIN] = "binary",		\
	[ATLV_SCHED] = "schedule",	\
	[ATLV_FLOAT] = "decimal",	\
	[ATLV_MSG_BIN] = "message",	\
	[ATLV_FILE] = "file",		\
}

#define JINT_ERR_MEM "mem_err"
#define JINT_ERR_PKTSIZE "pkt_size_too_large"
#define JINT_ERR_OP "invalid_op"
#define JINT_ERR_INVAL_ARGS "invalid_args"
#define JINT_ERR_INVAL_TYPE "invalid_type"
#define JINT_ERR_UNKWN_PROP "unknown_prop"
#define JINT_ERR_BAD_VAL "bad_value"
#define JINT_ERR_INVAL_JSON "invalid_json"
#define JINT_ERR_INVAL_PATH "invalid_path"
#define JINT_ERR_UNKWN_PROTO "unknown_protocol"
#define JINT_ERR_CONN_ERR "conn_err"
#define JINT_ERR_UNKWN "unknown_err"

extern const char * const data_types[];
extern const char * const data_ops[];

/*
 * Generate a new cmd
 */
json_t *jint_new_cmd(const char *proto, const char *op, int req_id);

/*
 * Add/remove "confirm" tag to json object. If json object is null, a new one
 * is created.
 */
void jint_confirm_set(json_t *cmd, int confirm);

/*
 * Add/remove "echo" tag to json object. If json object is null, a new one
 * is created.
 */
void jint_echo_set(json_t *cmd, int echo);

/*
 * Add "source" tag to json object.  This is represented as an index, where
 * ADS is 1, LAN client #1 is 2, etc.
 */
void jint_source_set(json_t *cmd, int source);

/*
 * Add "destinations" tag to json object.  This is a bitmask, where ADS is
 * 1 << 0, LAN client #1 is 1 << 1, etc.
 */
void jint_dests_set(json_t *cmd, int dests);

/*
 * Send echo failure with the args given
 */
int jint_send_echo_failure_with_args(const char *proto, json_t *args,
				int req_id);
/*
 * Send nak with the args given
 */
int jint_send_nak_with_args(const char *proto, json_t *args, int req_id);

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
		const char *err_op, json_t *op_args);
/*
 * A generic nak. To send a more complicated nak, build it using jint_build_nak
 * and then send it using jint_send_built_nak;
 */
int jint_send_nak(const char *proto, const char *err, int req_id);

/*
 * Send Ack. Specific to a request id
 */
int jint_send_ack(const char *proto, int req_id);

/*
 * Send TRUE Confirmation. Specific to a request id
 */
int jint_send_confirm_true(const char *proto, int req_id);

/*
 * Send FALSE Confirmation. Specific to a request id
 */
int jint_send_confirm_false(const char *proto, int req_id, int dests,
			    const char *err);

/*
 * Send property response to a prop request
 */
int jint_send_prop_resp(const char *proto, int req_id, json_t *props,
			int source);
/*
 * Send property update
 */
int jint_send_prop(const char *proto, const char *op, int req_id, json_t *props,
	int confirm, int source);

/*
 * Send schedule update
 */
int jint_send_sched(int req_id, json_t *props, json_t *opts, int dests);

/*
 * Send general error
 */
int jint_send_error(const char *err, int req_id);

/*
 * Helper functions for JSON Interface. Shared between Devd and Appd
 */
void jint_json_dump(const char *prefix, json_t *root);

/*
 * Increment the request id. Req Id can never be 0 because a 0
 * means to use any request id in the reply.
 */
void jint_incr_id(int *req_id);

/*
 * Return the data op given it's json string
 */
enum ayla_data_op jint_get_data_op(const char *str);

/*
 * Return the data type given it's string form
 */
enum ayla_tlv_type jint_get_data_type(const char *str);

/*
 * Initalize the json interface, set the send function
 */
void jint_init(int (*send)(json_t *));


#endif /* __JSON_INTERFACE_H__ */
