/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __GATEWAY_INTERFACE_H__
#define __GATEWAY_INTERFACE_H__

#define JINT_PROTO_GATEWAY "gateway"
#define GATEWAY_PROPNAME_DELIM	":"

enum ayla_gateway_op {

	/*
	 * Ayla Gateway operations
	 */
	AG_NODE_ADD,	/* add node to the gateway */
	AG_NODE_UPDATE,	/* update an existing node on the gateway */
	AG_NODE_REMOVE,	/* remove a node from the gateway */
	AG_PROP_BATCH_SEND,	/* send batch property updates */
	AG_CONN_STATUS, /* update the connection status for node */
	AG_CONN_STATUS_REQ, /* request the connection status of node */
	AG_CONN_STATUS_RESP, /* response to AG_CONN_STATUS_REQ */
	AG_PROP_SEND,	/* send attribute updates for node */
	AG_PROP_REQ,	/* request attribute updates for node */
	AG_PROP_REQ_ALL, /* request all node properties */
	AG_PROP_REQ_TO_DEV, /* request all node to-device props */
	AG_PROP_RESP,	/* respond to AD_PROP_REQ */
	AG_PROP_UPDATE,	/* node property update */
	AG_PROP_ACK,	/* acknowledge a property update */
	AG_SCHED_UPDATE,	/* node schedule update */
	AG_NODE_FACTORY_RST, /* node factory reset */
	AG_NODE_RST_RESULT, /* result of a node factory reset */
	AG_NODE_OTA, /* node ota */
	AG_NODE_OTA_RESULT, /* result of a node ota */
	AG_NODE_OTA_URL_FETCH, /* fetch the location of a remote node OTA */
	AG_NODE_OTA_LOCAL_FETCH, /* fetch a node OTA stored locally */
	AG_NODE_OTA_REMOTE_FETCH, /* fetch a node OTA stored remotely */
	AG_ACK,		/* ack */
	AG_NAK,		    /* nak */
	AG_ECHO_FAILURE,    /* echo failure */
	AG_CONFIRM_TRUE,    /* true confirmation */
	AG_CONFIRM_FALSE,   /* false confirmation */
	AG_NODE_REG,        /* node register staus */
	AG_NODE_REG_RESULT, /* result of a node register staus sending */
	AG_NOP		/* nop, must be last */
};

#define JINT_GATEWAY_OP_NAMES {				\
	[AG_NODE_ADD] = "add_node",			\
	[AG_NODE_UPDATE] = "update_node",		\
	[AG_NODE_REMOVE] = "remove_node",		\
	[AG_PROP_BATCH_SEND] = "prop_batch_send",	\
	[AG_CONN_STATUS] = "conn_status",		\
	[AG_CONN_STATUS_REQ] = "conn_status_req",	\
	[AG_CONN_STATUS_RESP] = "conn_status_resp",	\
	[AG_PROP_SEND] = "prop_send",			\
	[AG_PROP_REQ] = "prop_req",			\
	[AG_PROP_REQ_ALL] = "prop_req_all",		\
	[AG_PROP_REQ_TO_DEV] = "prop_req_to_dev",	\
	[AG_PROP_RESP] = "prop_resp",			\
	[AG_PROP_UPDATE] = "prop_update",		\
	[AG_PROP_ACK] = "prop_ack",			\
	[AG_SCHED_UPDATE] = "sched_update",		\
	[AG_NODE_FACTORY_RST] = "node_factory_rst",	\
	[AG_NODE_RST_RESULT] = "node_rst_result",	\
	[AG_NODE_OTA] = "node_ota",			\
	[AG_NODE_OTA_RESULT] = "node_ota_result",	\
	[AG_NODE_OTA_URL_FETCH] = "node_ota_url_fetch",	\
	[AG_NODE_OTA_LOCAL_FETCH] = "node_ota_local_fetch",   \
	[AG_NODE_OTA_REMOTE_FETCH] = "node_ota_remote_fetch", \
	[AG_ACK] = "ack",				\
	[AG_NAK] = "nak",				\
	[AG_ECHO_FAILURE] = "echo_failure",		\
	[AG_CONFIRM_TRUE] = "confirm_true",		\
	[AG_CONFIRM_FALSE] = "confirm_false",		\
	[AG_NODE_REG] = "node_reg",			\
	[AG_NODE_REG_RESULT] = "node_reg_result",	\
}

extern const char * const gateway_ops[];

/*
 * For INTERNAL Ayla use ONLY
 * Break property name into subdevice_key, template_key, and template prop
 * NOTE: This function is destructive to prop_name.
 */
int gateway_break_up_node_prop_name(char *prop_name, const char **subdevice_key,
			const char **template_key, const char **template_prop);

/*
 * For INTERNAL Ayla use ONLY
 * Given an operation string, get the gateway opcode
 */
enum ayla_gateway_op gateway_op_get(const char *str);

#endif /* __GATEWAY_INTERFACE_H__ */
