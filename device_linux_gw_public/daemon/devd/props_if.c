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

#include <ayla/utypes.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/json_parser.h>
#include <ayla/log.h>
#include <ayla/timer.h>

#include "notify.h"
#include "dapi.h"
#include "serv.h"
#include "ds.h"
#include "ops_devd.h"
#include "app_if.h"
#include "props_client.h"
#include "props_if.h"

/*
 * Send the location of a file datapoint created for a FILE property
 */
int prop_send_file_location_info(const int req_id, const char *prop_name,
				const char *location)
{
	json_t *root = jint_new_cmd(JINT_PROTO_DATA, data_ops[AD_DP_LOC],
	    req_id);
	json_t *cmd = json_object_get(root, "cmd");
	json_t *args = json_array();
	json_t *property = json_object();
	json_t *prop_info = json_object();

	if (!root || !args || !property || !prop_info) {
		log_err("mem err");
		json_decref(root);
		json_decref(args);
		json_decref(property);
		json_decref(prop_info);
		return -1;
	}
	json_object_set_new(prop_info, "name", json_string(prop_name));
	json_object_set_new(prop_info, "location", json_string(location));
	json_object_set_new(property, "property", prop_info);
	json_array_append_new(args, property);
	json_object_set_new(cmd, "args", args);
	app_send_json(root);
	json_decref(root);

	return 0;
}

/*
 * Send response to a GET prop request to appd
 */
void prop_send_prop_resp(void *arg, json_t *props)
{
	struct ops_devd_cmd *cmd = arg;

	if (!cmd) {
		log_err("no ops given");
		return;
	}

	jint_send_prop_resp(JINT_PROTO_DATA, cmd->req_id, props, SOURCE_ADS);
}

/*
 * Send property updates to appd. Originated from ADS or a client lan app
 */
void prop_send_prop_update(json_t *props, int source)
{
	jint_send_prop(JINT_PROTO_DATA, data_ops[AD_PROP_UPDATE], app_req_id,
	    props, 0, source);
	jint_incr_id(&app_req_id);
}

/*
 * Handle appd's response to a prop get request
 */
void prop_handle_prop_resp(json_t *cmd, int req_id)
{
	struct appd_req *ar;
	struct server_req *req;
	json_t *args;
	json_t *arg;
	json_t *propobj;

	ar = app_req_delete(req_id);
	if (!ar) {
		log_warn("req_id %x not found", req_id);
		return;
	}
	req = ar->req;

	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
		app_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		goto finish;
	}

	arg = json_array_get(args, 0);
	if (!json_is_object(arg)) {
		app_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		goto finish;
	}
	propobj = json_object_get(arg, "property");
	if (!propobj) {
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
	} else {
		server_put_json(req, propobj);
		server_put_end(req, HTTP_STATUS_OK);
	}
finish:
	free(ar);
}

/*
 * Send request to appd for a single property.
 * Request has args name=<prop>
 */
void prop_json_get(struct server_req *req)
{
	json_t *root;
	json_t *cmd;
	json_t *args;
	json_t *prop_info;
	struct appd_req *ar;
	char *name = NULL;
	size_t len;
	char *val;
	char *arg;
	char *data_str;

	ar = app_req_alloc();
	if (!ar) {
		log_err("alloc failed");
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		return;
	}

	while ((arg = server_get_arg_len(req, &val, &len)) != NULL) {
		if (!strcmp(arg, "name")) {
			name = val;
		}
	}
	root = jint_new_cmd(JINT_PROTO_DATA, data_ops[AD_PROP_REQ],
	    ar->req_id);
	jint_incr_id(&app_req_id);

	cmd = json_object_get(root, "cmd");
	args = json_array();
	prop_info = json_object();
	json_object_set_new(prop_info, "name", json_string(name));
	if (req->body_json) {
		data_str = json_dumps(req->body_json, JSON_COMPACT);
		json_object_set_new(prop_info, "data", json_string(data_str));
		free(data_str);
	}
	json_array_append_new(args, prop_info);
	json_object_set_new(cmd, "args", args);

	if (app_send_json(root)) {
		log_debug("app send for %s failed", name);
		free(ar);
		server_put_end(req, HTTP_STATUS_UNAVAIL);
		json_decref(root);
		return;
	}
	json_decref(root);
	ar->req = req;
	app_req_add(ar);
}
