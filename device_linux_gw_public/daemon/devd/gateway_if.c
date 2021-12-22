/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <errno.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/gateway_interface.h>
#include <ayla/json_parser.h>
#include <ayla/timer.h>
#include <ayla/server.h>

#include "dapi.h"
#include "serv.h"
#include "notify.h"
#include "ds.h"
#include "ops_devd.h"
#include "app_if.h"
#include "gateway_client.h"
#include "props_client.h"

/*
 * Send a GATEWAY nak
 */
void gateway_send_nak(const char *err, int id)
{
	jint_send_nak(JINT_PROTO_GATEWAY, err, id);
}

/*
 * Send a GATEWAY ACK
 */
void gateway_send_ack(int id)
{
	jint_send_ack(JINT_PROTO_GATEWAY, id);
}

/*
 * Handle appd's response to a conn status request
 */
void gateway_handle_conn_status_resp(json_t *cmd, int req_id)
{
	struct appd_req *ar;
	struct server_req *req;
	json_t *args;
	json_t *status_info_j;
	json_t *connection_j;
	int i;

	ar = app_req_delete(req_id);
	if (!ar) {
		log_warn("req_id %x not found", req_id);
		return;
	}
	req = ar->req;

	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
		gateway_send_nak(JINT_ERR_INVAL_ARGS, req_id);
internal_err:
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		goto finish;
	}
	for (i = 0; i < json_array_size(args); i++) {
		status_info_j = json_array_get(args, i);
		if (gateway_convert_address_to_dsn(status_info_j)) {
			goto internal_err;
		}
	}
	connection_j = json_object();
	REQUIRE(connection_j, REQUIRE_MSG_ALLOCATION);
	json_object_set(connection_j, "connection", args);
	server_put_json(req, connection_j);
	json_decref(connection_j);
	server_put_end(req, HTTP_STATUS_OK);
finish:
	free(ar);
}

/*
 * Send request to appd for connection status of nodes
 */
void gateway_conn_status_get(struct server_req *req)
{
	json_t *root;
	json_t *cmd;
	json_t *args;
	json_t *req_info;
	struct appd_req *ar;
	const char *dsn;
	const char *addr;
	json_t *address_j;
	json_t *addresses_j;
	json_t *dsns_j;
	json_t *data_j;
	int i;

	ar = app_req_alloc();
	if (!ar) {
		log_err("alloc failed");
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		return;
	}
	root = jint_new_cmd(JINT_PROTO_GATEWAY,
	    gateway_ops[AG_CONN_STATUS_REQ], ar->req_id);
	jint_incr_id(&app_req_id);

	cmd = json_object_get(root, "cmd");
	addresses_j = json_array();

	REQUIRE(addresses_j, REQUIRE_MSG_ALLOCATION);

	data_j = req->body_json;
	if (!data_j) {
bad_req:
		log_warn("bad request");
		json_decref(root);
		json_decref(addresses_j);
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	dsns_j = json_object_get(data_j, "dsns");
	if (!json_is_array(dsns_j)) {
		goto bad_req;
	}
	for (i = 0; i < json_array_size(dsns_j); i++) {
		address_j = json_array_get(dsns_j, i);
		if (!json_is_string(address_j)) {
			goto bad_req;
		}
		dsn = json_string_value(address_j);
		addr = gateway_dsn_to_addr(dsn);
		if (!addr) {
			log_warn("dsn %s not found", dsn);
			continue;
		}
		json_array_append_new(addresses_j, json_string(addr));
	}
	if (!json_array_size(addresses_j)) {
		goto bad_req;
	}

	args = json_array();
	req_info = json_object();
	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	REQUIRE(req_info, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(req_info, "addresses", addresses_j);
	json_array_append_new(args, req_info);
	json_object_set_new(cmd, "args", args);

	if (app_send_json(root)) {
		log_warn("app send failed");
		free(ar);
		server_put_end(req, HTTP_STATUS_UNAVAIL);
		json_decref(root);
		return;
	}
	json_decref(root);
	ar->req = req;
	app_req_add(ar);
}

/*
 * Send request to appd for property value of a node
 */
void gateway_node_property_get(struct server_req *req)
{
	json_t *root;
	json_t *cmd;
	json_t *args;
	json_t *req_info;
	struct appd_req *ar;
	const char *dsn;
	const char *addr;
	json_t *data_j;
	json_t *prop_info_j;
	char *prop_name = NULL;
	const char *subdevice_key;
	const char *template_key;
	const char *template_prop;
	size_t len;
	char *val;
	char *arg;

	ar = app_req_alloc();
	if (!ar) {
		log_err("alloc failed");
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		return;
	}
	while ((arg = server_get_arg_len(req, &val, &len)) != NULL) {
		if (!strcmp(arg, "name")) {
			prop_name = val;
		}
	}
	if (!prop_name || gateway_break_up_node_prop_name(prop_name,
	    &subdevice_key, &template_key, &template_prop)) {
bad_req:
		log_warn("bad request");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		free(ar);
		return;
	}
	data_j = req->body_json;
	if (!data_j) {
		goto bad_req;
	}
	dsn = json_get_string(data_j, "dsn");
	if (!dsn) {
		goto bad_req;
	}
	addr = gateway_dsn_to_addr(dsn);
	if (!addr) {
		log_err("dsn %s not found", dsn);
		server_put_end(req, HTTP_STATUS_PRE_FAIL);
		free(ar);
		return;
	}

	root = jint_new_cmd(JINT_PROTO_GATEWAY,
	    gateway_ops[AG_PROP_REQ], ar->req_id);
	jint_incr_id(&app_req_id);
	cmd = json_object_get(root, "cmd");
	args = json_array();
	req_info = json_object();
	prop_info_j = json_object();
	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	REQUIRE(req_info, REQUIRE_MSG_ALLOCATION);
	REQUIRE(prop_info_j, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(req_info, "address", json_string(addr));
	json_object_set_new(req_info, "name", json_string(template_prop));
	json_object_set_new(req_info, "subdevice_key",
	    json_string(subdevice_key));
	json_object_set_new(req_info, "template_key",
	    json_string(template_key));
	json_object_set_new(prop_info_j, "property", req_info);
	json_array_append_new(args, prop_info_j);
	json_object_set_new(cmd, "args", args);

	if (app_send_json(root)) {
		log_debug("app send failed");
		server_put_end(req, HTTP_STATUS_UNAVAIL);
		json_decref(root);
		free(ar);
		return;
	}
	json_decref(root);
	ar->req = req;
	app_req_add(ar);
}

/*
 * Handle appd's response to node property request
 */
void gateway_handle_property_resp(json_t *cmd, int req_id)
{
	struct appd_req *ar;
	struct server_req *req;
	json_t *args;
	json_t *arg;
	json_t *prop_info_j;
	const char *status;

	ar = app_req_delete(req_id);
	if (!ar) {
		log_warn("req_id %x not found", req_id);
		return;
	}
	req = ar->req;

	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
		gateway_send_nak(JINT_ERR_INVAL_ARGS, req_id);
internal_err:
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		goto finish;
	}
	arg = json_array_get(args, 0);
	prop_info_j = json_object_get(arg, "property");
	if (!prop_info_j) {
		goto internal_err;
	}
	status = json_get_string(prop_info_j, "status");
	if (status && !strcmp(status, JINT_ERR_UNKWN_PROP)) {
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		goto finish;
	}
	if (gateway_convert_address_to_dsn(prop_info_j) ||
	    gateway_prop_info_to_name(prop_info_j, NULL)) {
		goto internal_err;
	}
	server_put_json(req, prop_info_j);
	server_put_end(req, HTTP_STATUS_OK);
finish:
	free(ar);
}

/*
 * Convert node properties coming from the cloud/app into a format that appd
 * can understand (i.e. address, template_key, subdevice_key, name).
 * An option json_array can be given (result_j) which will be populated with
 * all successfully converted props.
 */
static void gateway_dsn_prop_to_addr_prop(json_t *props_j, json_t *result_j)
{
	int i;
	json_t *elem;
	json_t *node_prop;
	char *prop_name;
	const char *subdevice_key;
	const char *template_key;
	const char *template_prop;
	const char *node_dsn;

	for (i = 0; i < json_array_size(props_j); i++) {
		elem = json_array_get(props_j, i);
		node_prop = json_object_get(elem, "property");
		if (!node_prop) {
			log_warn("property obj missing");
			continue;
		}
		node_dsn = json_get_string(node_prop, "dsn");
		if (!node_dsn) {
			log_warn("node dsn missing");
			continue;
		}
		if (gateway_convert_dsn_to_address(node_prop)) {
			continue;
		}
		/* split the name up into individual pieces for appd */
		prop_name = json_get_string_dup(node_prop, "name");
		if (!prop_name || gateway_break_up_node_prop_name(prop_name,
		    &subdevice_key, &template_key, &template_prop)) {
			free(prop_name);
			continue;
		}
		json_object_set_new(node_prop, "name",
		    json_string(template_prop));
		json_object_set_new(node_prop, "subdevice_key",
		    json_string(subdevice_key));
		json_object_set_new(node_prop, "template_key",
		    json_string(template_key));
		free(prop_name);
		if (result_j) {
			json_array_append(result_j, elem);
		}
	}
}

/*
 * Node property updates from Cloud/LAN
 */
void gateway_process_node_update(struct device_state *dev, json_t *update_j,
	int source)
{
	json_t *elem;
	json_t *prop_j;
	int prop_size;
	json_t *appd_props;
	int i;

	appd_props = json_array();
	REQUIRE(appd_props, REQUIRE_MSG_ALLOCATION);
	gateway_dsn_prop_to_addr_prop(update_j, appd_props);
	if (!json_array_size(appd_props)) {
		json_decref(appd_props);
		return;
	}
	jint_send_prop(JINT_PROTO_GATEWAY, gateway_ops[AG_PROP_UPDATE],
	    app_req_id, appd_props, 0, source);
	jint_incr_id(&app_req_id);
	if (!dev->lan.auto_sync) {
		json_decref(appd_props);
		return;
	}

	/* echo to any LAN clients */
	prop_size = json_array_size(appd_props);
	for (i = 0; i < prop_size; i++) {
		elem = json_array_get(appd_props, i);
		prop_j = json_object_get(elem, "property");
		if (!json_is_object(prop_j)) {
			continue;
		}
		if (ds_echo_for_prop_is_needed(prop_j, source)) {
			/*
			 * don't automatically echo props that need explicit
			 * ack. They will be echoed once appd returns success
			 * for the property update.
			 */
			gateway_node_prop_prepare_echo(dev, elem, source);
		}
	}
	json_decref(appd_props);
}

/*
 * Send response to a GET node prop request to appd
 */
void gateway_send_prop_resp(void *arg, json_t *props)
{
	struct ops_devd_cmd *cmd = arg;
	json_t *appd_props;

	if (!cmd) {
		log_err("no ops given");
		return;
	}
	appd_props = json_array();
	REQUIRE(appd_props, REQUIRE_MSG_ALLOCATION);
	gateway_dsn_prop_to_addr_prop(props, appd_props);
	if (!json_array_size(appd_props)) {
		json_decref(appd_props);
		return;
	}
	jint_send_prop_resp(JINT_PROTO_GATEWAY, cmd->req_id, appd_props,
	    SOURCE_ADS);
	json_decref(appd_props);
}

/*
 * Node schedules from cloud/LAN
 */
void gateway_process_node_scheds(struct device_state *dev, json_t *scheds_j,
	int source)
{
	int sched_size;
	json_t *elem;
	json_t *appd_scheds;
	json_t *node_sched;
	const char *node_dsn;
	int i;

	sched_size = json_array_size(scheds_j);
	if (!sched_size) {
		return;
	}
	appd_scheds = json_array();
	REQUIRE(appd_scheds, REQUIRE_MSG_ALLOCATION);
	for (i = 0; i < sched_size; i++) {
		elem = json_array_get(scheds_j, i);
		node_sched = json_object_get(elem, "schedule");
		if (!node_sched) {
			log_warn("schedule obj missing");
			continue;
		}
		node_dsn = json_get_string(node_sched, "dsn");
		if (!node_dsn) {
			log_warn("node dsn missing");
			continue;
		}
		if (gateway_convert_dsn_to_address(node_sched)) {
			continue;
		}
		json_array_append(appd_scheds, elem);
	}
	if (json_array_size(appd_scheds)) {
		jint_send_prop(JINT_PROTO_GATEWAY, gateway_ops[AG_SCHED_UPDATE],
		    app_req_id, appd_scheds, 0, source);
		jint_incr_id(&app_req_id);
	}
	json_decref(appd_scheds);
}

/*
 * Send contents of a cloud command to a node
 * and finish processing the request.
 */
static void gateway_send_cmd_to_appd(struct server_req *req,
			enum ayla_gateway_op op, const char *addr)
{
	json_t *root;
	json_t *cmd;
	json_t *args;
	json_t *req_info;
	json_t *ota_j;
	const char *ver;
	struct serv_rev_req *rev_req = (struct serv_rev_req *)req->arg;

	root = jint_new_cmd(JINT_PROTO_GATEWAY, gateway_ops[op], app_req_id);
	jint_incr_id(&app_req_id);
	cmd = json_object_get(root, "cmd");
	args = json_array();
	req_info = json_object();
	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	REQUIRE(req_info, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(req_info, "address", json_string(addr));
	json_object_set_new(req_info, "cmd_id", json_integer(rev_req->cmd_id));
	json_object_set_new(req_info, "cmd_uri",
	    json_string(rev_req->resp_uri));
	json_object_set(req_info, "cmd_data", req->body_json);
	if (op == AG_NODE_OTA) {
		/* also include the version */
		ota_j = json_object_get(req->body_json, "ota");
		if (ota_j) {
			ver = json_get_string(ota_j, "ver");
			if (ver) {
				json_object_set(req_info, "version",
				    json_object_get(ota_j, "ver"));
			}
		}
	}
	json_array_append_new(args, req_info);
	json_object_set_new(cmd, "args", args);

	if (app_send_json(root)) {
		log_debug("app send failed");
		server_put_end(req, HTTP_STATUS_UNAVAIL);
		json_decref(root);
		return;
	}
	json_decref(root);
	/* Request successfully sent. Appd will respond */
	serv_rev_req_close(rev_req);
}

/*
 * Process Node Factory Reset Command
 */
void gateway_reset_node_put(struct server_req *req)
{
	json_t *data_j;
	const char *dsn;
	const char *addr;

	data_j = req->body_json;
	if (!json_is_object(data_j)) {
		log_warn("no data object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	dsn = json_get_string(data_j, "dsn");
	addr = gateway_dsn_to_addr(dsn);
	if (!addr) {
		log_warn("unknown dsn %s", dsn);
		/* given dsn is no longer registerd with this gateway */
		/* mark the node factory reset a success */
		server_put_end(req, HTTP_STATUS_OK);
		return;
	}
	gateway_send_cmd_to_appd(req, AG_NODE_FACTORY_RST, addr);
}

/*
 * Process Node OTA Command
 */
void gateway_node_ota_put(struct server_req *req)
{
	json_t *data_j;
	char dsn[MAX_DSN_LEN];
	const char *addr;
	const char *url;
	json_t *ota_j;

	data_j = req->body_json;
	if (!json_is_object(data_j)) {
		log_warn("no data object");
bad_req:
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	ota_j = json_object_get(data_j, "ota");
	if (!json_is_object(ota_j)) {
		goto bad_req;
	}
	url = json_get_string(ota_j, "url");
	if (!url) {
		goto bad_req;
	}
	url = strchr(url, '?') ;
	if (!url) {
		goto bad_req;
	}
	url++;
	if (server_get_val_from_args(url, "dsn", dsn, sizeof(dsn)) == -1) {
		goto bad_req;
	}
	addr = gateway_dsn_to_addr(dsn);
	if (!addr) {
		log_warn("unknown dsn %s", dsn);
		/* given dsn is not known to this gateway */
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	gateway_send_cmd_to_appd(req, AG_NODE_OTA, addr);
}

/*
 * Process Node Register Status Change Command
 */
void gateway_node_reg_put(struct server_req *req, const char *dsn)
{
	const char *addr;

	if (!dsn) {
		log_err("missing dsn info");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}

	addr = gateway_dsn_to_addr(dsn);
	if (!addr) {
		log_warn("unknown dsn %s", dsn);
		/* given dsn is no longer registered with this gateway */
		/* mark the node register status change a success */
		server_put_end(req, HTTP_STATUS_OK);
		return;
	}

	gateway_send_cmd_to_appd(req, AG_NODE_REG, addr);
}

