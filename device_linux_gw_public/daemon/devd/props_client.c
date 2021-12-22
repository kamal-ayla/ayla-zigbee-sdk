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
#include <ayla/json_parser.h>
#include <ayla/timer.h>

#include "ds.h"
#include "ds_client.h"
#include "dapi.h"
#include "serv.h"
#include "ops_devd.h"
#include "props_if.h"
#include "app_if.h"
#include "props_client.h"

enum prop_client_state {
	PCS_NOP = 0,
	PCS_GET_PROP,
	PCS_GET_ALL_PROPS,
	PCS_GET_TODEV_PROPS,
	PCS_PROP_SEND,
	PCS_PROP_BATCH_SEND,
	PCS_AUTO_ECHO,
	PCS_POST_DP_CREATE,
	PCS_PUT_DP_SEND,
	PCS_PUT_DP_CLOSE,
	PCS_GET_DP_REQ,
	PCS_GET_DP_REQ_FROM_FILE,
	PCS_PUT_DP_FETCHED,
	PCS_PUT_ACK,
	PCS_POST_MSG_ID,
	PCS_PUT_MSG_DATA,
	PCS_GET_MSG_ID,
	PCS_GET_MSG_DATA,
};

#define PROP_LOC_URL_LEN	500
#define PROP_FILE_URL_LEN	500
#define LAN_PROP_SEND_URL	"property/datapoint.json"
#define LAN_PROP_ACK_URL	"property/datapoint/ack.json"
#define BATCH_DPS_URL		"https://%s/%s/dsns/%s/batch_datapoints.json"

/*
 * Property information for prop operations
 */
struct prop_cmd {
	json_t *prop_info;
	const char *prop_name;
	enum ayla_data_op op;
};

static const struct op_funcs prop_op_handlers[];

static struct prop_file_datapoint_info {
	char location[PROP_LOC_URL_LEN];
	char file[PROP_FILE_URL_LEN];
} prop_latest_file_info;

/*
 * This function should never be called.
 */
static int prop_nop_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	log_err("bad call to nop");

	ASSERT_NOTREACHED();
	return 1;
}

/*
 * Free a prop_cmd structure
 */
static void prop_free_prop_cmd(void *arg)
{
	struct prop_cmd *pcmd = arg;

	if (pcmd && pcmd->prop_info) {
		json_decref(pcmd->prop_info);
	}
	free(pcmd);
}
/*
 * Generic function when a property operation finishes.
 */
static int prop_generic_op_finished(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	prop_free_prop_cmd(pcmd);

	return 0;
}

/*
 * If location is a URL, return the datapoint id, otherwise return location
 */
static char *prop_get_dp_id(const char *location)
{
	char *id;

	id = strrchr(location, '/');
	if (id) {
		return id + 1;
	}
	id = (char *)&location[0];
	return id;
}

/*
 * Given a location in format, <prop_id>/<dp_id>.xml, returns a string
 * in the format properties/<prop_id>/datapoints/<dp_id>.xml
 */
static int prop_convert_loc_to_url_str(struct device_state *dev,
	const char *loc, char *dest, int dest_len)
{
	char location[PROP_LOC_URL_LEN + 1];
	char *dp_id;

	if (!loc) {
		return -1;
	}
	dp_id = prop_get_dp_id(loc);
	if (dp_id == loc) {
		return -1;
	}
	strncpy(location, loc, dp_id - loc);
	location[dp_id - loc - 1] = '\0';

	snprintf(dest, dest_len,
	    "https://%s/devices/%s/properties%s/datapoints/%s",
	    dev->ads_host, dev->key, location, dp_id);

	return 0;
}

/*
 * Batch update successfully sent to the service
 */
int prop_batch_post_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	json_t *batch_dps_j;
	json_t *batch_dp_j;
	json_t *cmd_op_args;
	u8 cmd_dests_failed;
	int batch_id;
	int status;
	int i;

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	batch_dps_j = json_object_get(req_resp, "batch_datapoints");
	if (!batch_dps_j) {
bad_resp:
		log_warn("bad resp");
		return -1;
	}
	cmd_op_args = op_cmd->op_args;
	cmd_dests_failed = op_cmd->dests_failed;
	for (i = 0; i < json_array_size(batch_dps_j); i++) {
		batch_dp_j = json_array_get(batch_dps_j, i);
		if (json_get_int(batch_dp_j, "batch_id", &batch_id) < 0 ||
		    json_get_int(batch_dp_j, "status", &status) < 0) {
			goto bad_resp;
		}
		if (status != HTTP_STATUS_OK) {
			if (status == HTTP_STATUS_NOT_FOUND) {
				op_cmd->err_type = JINT_ERR_UNKWN_PROP;
			} else {
				op_cmd->err_type = JINT_ERR_CONN_ERR;
			}
			/* Send nak for each failed prop */
			op_cmd->op_args = batch_dp_j;
			op_cmd->dests_failed = DEST_ADS;
			app_send_nak_with_args(op_cmd);
			/* Restore original cmd values */
			op_cmd->op_args = cmd_op_args;
			op_cmd->dests_failed = cmd_dests_failed;
		}
	}

	return 0;
}

/*
 * New file datapoint successfully marked fetched
 */
static int prop_file_dp_mark_fetched_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	dev->get_cmds = dev->par_content;
	dev->par_content = 0;

	return 0;
}

/*
 * Close a file datapoint
 */
static int prop_file_dp_mark_fetched(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *location;
	json_t *dp_obj;
	json_t *dp_fetched_j;

	location = json_get_string(pcmd->prop_info, "location");
	if (prop_convert_loc_to_url_str(dev, location, link, link_size)) {
		return -1;
	}
	dp_obj = json_object();
	dp_fetched_j = json_object();
	REQUIRE(dp_obj, REQUIRE_MSG_ALLOCATION);
	REQUIRE(dp_fetched_j, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(dp_fetched_j, "fetched", json_true());
	json_object_set_new(dp_obj, "datapoint", dp_fetched_j);

	ds_client_data_init_json(&info->req_data, dp_obj);
	json_decref(dp_obj);
	info->init = 1;
	*method = HTTP_PUT;

	return 0;
}

/*
 * Get the file datapoint from the "file" location (i.e. s3 service)
 */
static int prop_file_dp_req_from_file(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *value;

	value = json_get_string(pcmd->prop_info, "value");
	if (!value) {
		return -1;
	}
	info->resp_file_path = value;
	info->non_ayla = 1;
	info->init = 1;
	snprintf(link, link_size, "%s", prop_latest_file_info.file);
	*method = HTTP_GET;

	return 0;
}

/*
 * New file datapoint successfully requested. Get the datapoint from s3.
 */
static int prop_file_dp_req_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	const char *file;
	json_t *datapoint;

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ds_json_dump(__func__, req_resp);
	datapoint = json_object_get(req_resp, "datapoint");
	if (!datapoint) {
bad_response:
		log_warn("bad response");
		return -1;
	}
	file  = json_get_string(datapoint, "file");
	if (!file) {
		goto bad_response;
	}
	snprintf(prop_latest_file_info.file, sizeof(prop_latest_file_info.file),
	    "%s", file);
	op_cmd->op_handlers = &prop_op_handlers[PCS_GET_DP_REQ_FROM_FILE];

	return 1;
}

/*
 * Request a file datapoint
 */
static int prop_file_dp_req(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *location;

	location = json_get_string(pcmd->prop_info, "location");
	if (prop_convert_loc_to_url_str(dev, location, link, link_size)) {
		return -1;
	}

	*method = HTTP_GET;

	return 0;
}

/*
 * Close a file datapoint
 */
static int prop_file_dp_close(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	prop_convert_loc_to_url_str(dev, prop_latest_file_info.location,
	    link, link_size);

	*method = HTTP_PUT;

	return 0;
}

/*
 * New file datapoint successfully sent
 */
static int prop_file_dp_send_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	op_cmd->op_handlers = &prop_op_handlers[PCS_PUT_DP_CLOSE];

	return 1;
}

/*
 * Upload information of a file datapoint
 */
static int prop_file_dp_send(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *location;
	const char *value;

	location = json_get_string(pcmd->prop_info, "location");
	value = json_get_string(pcmd->prop_info, "value");
	if (strcmp(location, prop_latest_file_info.location)) {
		log_debug("unknown location");
		return -1;
	}
	if (!ds_client_data_init_file(&info->req_data, value, "r")) {
		return -1;
	}
	info->non_ayla = 1;
	info->init = 1;
	snprintf(link, link_size, "%s", prop_latest_file_info.file);
	*method = HTTP_PUT;

	return 0;
}

/*
 * New file datapoint successfully created
 */
static int prop_file_dp_create_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *location;
	const char *file;
	json_t *datapoint;

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ds_json_dump(__func__, req_resp);
	datapoint = json_object_get(req_resp, "datapoint");
	if (!datapoint) {
bad_response:
		log_warn("bad response");
		return -1;
	}
	location = json_get_string(datapoint, "location");
	file  = json_get_string(datapoint, "file");
	if (!location || !file) {
		goto bad_response;
	}
	snprintf(prop_latest_file_info.location,
	    sizeof(prop_latest_file_info.location), "%s", location);
	snprintf(prop_latest_file_info.file,
	    sizeof(prop_latest_file_info.file), "%s", file);
	prop_send_file_location_info(op_cmd->req_id, pcmd->prop_name, location);

	return 0;
}

/*
 * Create a file datapoint
 */
static int prop_file_dp_create(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	prop_curl_buf_info_setup(info, pcmd->prop_info);
	snprintf(link, link_size,
	    "https://%s/devices/%s/properties/%s/datapoints.json",
	    dev->ads_host, dev->key, pcmd->prop_name);

	*method = HTTP_POST;

	return 0;
}

/*
 * Setup datapoint POST buffer for message property sends
 */
int prop_msg_info_setup(struct ops_buf_info *info, json_t *prop_info)
{
	json_t *obj_name;
	json_t *obj_base_type;
	json_t *obj_dev_time_ms;
	json_t *obj_metadata;
	json_t *dp_obj;
	json_t *if_obj;

	obj_name = json_object_get(prop_info, "name");
	if (!obj_name) {
		log_err("cannot find name object");
		return -1;
	}
	obj_base_type = json_object_get(prop_info, "base_type");
	if (!obj_base_type) {
		log_err("cannot find base_type object");
		return -1;
	}
	obj_dev_time_ms = json_object_get(prop_info, "dev_time_ms");
	if (!obj_dev_time_ms) {
		log_err("cannot find dev_time_ms object");
		return -1;
	}

	dp_obj = json_object();
	json_object_set(dp_obj, "name", obj_name);
	json_object_set(dp_obj, "base_type", obj_base_type);
	json_object_set(dp_obj, "dev_time_ms", obj_dev_time_ms);

	obj_metadata = json_object_get(prop_info, "metadata");
	if (obj_metadata) {
		json_object_set(dp_obj, "metadata", obj_metadata);
	}

	if_obj = json_object();
	json_object_set_new(if_obj, "datapoint", dp_obj);

	ds_client_data_init_json(&info->req_data, if_obj);
	json_decref(if_obj);
	info->init = 1;

	return 0;
}

/*
 * Create a message prop datapoint
 */
static int prop_msg_id_create(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	if (prop_msg_info_setup(info, pcmd->prop_info) < 0) {
		log_warn("prop_msg_info_setup failed");
		return -1;
	}

	snprintf(link, link_size,
	    "https://%s/devices/%s/properties/%s/datapoints.json",
	    dev->ads_host, dev->key, pcmd->prop_name);

	*method = HTTP_POST;
	log_debug("POST link %s", link);

	return 0;
}

/*
 * New prop datapoint successfully created
 */
static int prop_post_msg_id_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	json_t *datapoint;
	const char *location;

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ds_json_dump(__func__, req_resp);
	datapoint = json_object_get(req_resp, "datapoint");
	if (!datapoint) {
		log_warn("bad response");
		return -1;
	}
	location = json_get_string(datapoint, "location");
	if (!location) {
		log_warn("bad response");
		return -1;
	}

	json_object_set_new(pcmd->prop_info,
	    "location", json_string(location));

	log_debug("name %s, location %s", pcmd->prop_name, location);
	op_cmd->op_handlers = &prop_op_handlers[PCS_PUT_MSG_DATA];

	return 1;
}

/*
 * Create a put msg data request
 */
static int prop_put_msg_data(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *location;
	const char *value;
	char *dp_id;
	char dp_loc[PROP_LOC_URL_LEN + 1];

	location = json_get_string(pcmd->prop_info, "location");
	if (!location) {
		log_err("Cannot find location info");
		return -1;
	}

	dp_id = strrchr(location, '/');
	if (dp_id) {
		dp_id += 1;
	} else {
		log_err("Cannot find dp_id info");
		return -1;
	}

	strncpy(dp_loc, location, dp_id - location);
	dp_loc[dp_id - location - 1] = '\0';

	snprintf(link, link_size,
	    "https://%s/devices/%s/properties%s/message_datapoints/%s",
	    dev->ads_host, dev->key, dp_loc, dp_id);

	jint_json_dump(__func__, pcmd->prop_info);
	value = json_get_string(pcmd->prop_info, "value");
	if (!value) {
		log_err("Cannot find value info");
		return -1;
	}

	if (!ds_client_data_init_file(&info->req_data, value, "r")) {
		log_err("file data init failed");
		return -1;
	}
	info->init = 1;

	*method = HTTP_PUT;

	return 0;
}

/*
 * Send the message property op info to appd
 */
int prop_send_msg_op_info(const enum ayla_data_op op, const int req_id,
			const char *prop_name, const char *path)
{
	json_t *root = jint_new_cmd(JINT_PROTO_DATA, data_ops[op], req_id);
	json_t *cmd = json_object_get(root, "cmd");
	json_t *args = json_array();
	json_t *property = json_object();
	json_t *prop_info = json_object();
	json_t *opts = json_object();

	if (!root || !args || !property || !prop_info || !opts) {
		log_err("mem err");
		json_decref(root);
		json_decref(args);
		json_decref(property);
		json_decref(prop_info);
		json_decref(opts);
		return -1;
	}
	json_object_set_new(prop_info, "base_type",
	    json_string(data_types[ATLV_MSG_BIN]));
	json_object_set_new(prop_info, "name", json_string(prop_name));
	json_object_set_new(prop_info, "path", json_string(path));
	json_object_set_new(property, "property", prop_info);
	json_array_append_new(args, property);
	json_object_set_new(cmd, "args", args);
	json_object_set_new(opts, "source", json_integer(DEST_ADS));
	json_object_set(cmd, "opts", opts);
	app_send_json(root);
	json_decref(root);

	return 0;
}

/*
 * Get message property data, save to file
 */
static int prop_get_msg_data(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *location;
	const char *value;

	location = json_get_string(pcmd->prop_info, "location");
	if (!location) {
		log_err("Cannot find location info");
		return -1;
	}
	prop_convert_loc_to_url_str(dev, location, link, link_size);

	log_debug("GET link %s", link);
	jint_json_dump(__func__, pcmd->prop_info);

	value = json_get_string(pcmd->prop_info, "value");
	if (!value) {
		log_err("Cannot find value info");
		return -1;
	}
	info->resp_file_path = value;
	info->init = 1;

	*method = HTTP_GET;
	return 0;
}

/*
 * GET message property data succeeded
 */
static int prop_get_msg_data_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	const char *value;

	value  = json_get_string(pcmd->prop_info, "value");
	if (!value) {
		log_err("Cannot find value info");
		return -1;
	}
	prop_send_msg_op_info(AD_MSG_GET, op_cmd->req_id,
	    pcmd->prop_name, value);

	return 0;
}

/*
 * GET properties.json succeeded
 */
static int prop_get_props_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	json_t *commands;

	if (!req_resp) {
		return -1;
	}
	commands = json_object_get(req_resp, "commands");
	if (!json_is_object(commands)) {
		return -1;
	}
	ds_parse_props(dev, commands, prop_send_prop_resp, NULL, op_cmd);
	dev->par_content = (req_status == HTTP_STATUS_PAR_CONTENT);

	return 0;
}

/*
 * Get props ?input=true and ?all=true
 * Returns 0 on success
 * Returns -1 on failure
 */
static int prop_get_props_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	snprintf(link, link_size,
	    "https://%s/devices/%s/commands.json?%s",
	    dev->ads_host, dev->key,
	    (pcmd->op == AD_PROP_REQ_ALL) ? "all=true" : "input=true");

	*method = HTTP_GET;

	return 0;
}

/*
 * Helper function for prop_get_success. Shared by props and gateway clients.
 */
int prop_get_success_helper(struct ops_devd_cmd *op_cmd,
		struct device_state *dev, void (*prop_resp)(void *, json_t *))
{
	json_t *properties;

	ASSERT(prop_resp);

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ds_json_dump(__func__, req_resp);

	properties = json_array();
	json_array_append(properties, req_resp);
	prop_resp(op_cmd, properties);
	json_decref(properties);

	return 0;
}

/*
 * Prop get succeeded
 * Handle incoming response to GET property/<prop_name>.json
 * Examples:
 * "property": {
 *      "base_type": "boolean",
 *      "data_updated_at": "2014-02-04T18:38:36Z",
 *      "device_key": 2981,
 *      "direction": "input",
 *      "display_name": "Blue_LED",
 *      "key": 34973,
 *      "name": "Blue_LED",
 *      "product_name": "LinuxDev",
 *      "read_only": false,
 *      "scope": "user",
 *      "track_only_changes": false,
 *      "value": 1
 *  }
 *
 */
static int prop_get_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	return prop_get_success_helper(op_cmd, dev, prop_send_prop_resp);
}

/*
 * Initialize a prop get.
 */
static int prop_get_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	snprintf(link, link_size,
	    "https://%s/devices/%s/properties/%s.json",
	    dev->ads_host, dev->key, pcmd->prop_name);

	*method = HTTP_GET;

	return 0;
}

/*
 * Send a property object to LAN clients based on the target
 */
void prop_send_prop_to_lan_clients(struct ops_devd_cmd *op_cmd,
	struct device_state *dev, u8 targets, json_t *prop_info,
	const char *url)
{
	struct client_lan_reg *lan;
	char lan_url[CLIENT_LAN_MAX_URL_LEN];
	int rc;
	int i;

	for (i = 1, lan = client_lan_reg; i < CLIENT_LAN_REGS; i++, lan++) {
		if (!(targets & BIT(i))) {
			continue;
		}
		if (!lan->uri[0]) {
			log_err("missing lan %d", i);
			ops_devd_mark_results(op_cmd, BIT(i), false);
			continue;
		}
		snprintf(lan_url, sizeof(lan_url), "%s%s", url,
		    op_cmd->echo ? "?echo=true" : "");
		rc = client_lan_post(dev, lan, prop_info, lan_url,
		    &op_cmd->err_type);
		ops_devd_mark_results(op_cmd, BIT(i), !rc);
	}
}

/*
 * Helper function for sending prop updates/echoes to a LAN client
 */
static int prop_send_lan_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	if (op_cmd->dests_target > 1) {
		/*
		 * Post to LAN clients first.
		 */
		prop_send_prop_to_lan_clients(op_cmd, dev, op_cmd->dests_target,
		    pcmd->prop_info, LAN_PROP_SEND_URL);
	}

	return 0;
}

/*
 * Setup datapoint POST/PUT buffer for property sends and acks
 */
void prop_curl_buf_info_setup(struct ops_buf_info *info, json_t *prop_info)
{
	json_t *dp_obj;

	dp_obj = json_object();
	json_object_set(dp_obj, "datapoint", prop_info);
	ds_client_data_init_json(&info->req_data, dp_obj);
	json_decref(dp_obj);
	info->init = 1;
}

/*
 * Helper function for intializing a prop send/echo to ADS
 */
static void prop_send_ads_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	prop_curl_buf_info_setup(handler->info, pcmd->prop_info);
	*handler->method = HTTP_POST;

	snprintf(handler->link, handler->link_size,
	    "https://%s/devices/%s/properties/%s/datapoints.json%s",
	    dev->ads_host, dev->key, pcmd->prop_name,
	    op_cmd->echo ? "?echo=true" : "");
}

/*
 * Shared by props and gateway. Init function for sending prop information
 * to LAN and ADS.
 */
int prop_send_init_execute(struct prop_send_handlers *handler)
{
	struct device_state *dev = &device;
	int rc;

	if (!handler) {
		return -1;
	}
	rc = handler->lan_init_helper(dev, handler);
	if (rc) {
		return rc;
	}
	if (handler->op_cmd->dests_target & DEST_ADS) {
		/* sending to ADS, construct link for ADS */
		handler->ads_init_helper(dev, handler);
		return 0;
	}
	return 1;
}

/*
 * Initialize a prop send/echo.
 * Returns 0 on success
 * Returns -1 on failure
 * Returns 1 if success but no operation needed on ADS
 */
static int prop_send_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_send_handlers handler = {
	    prop_send_lan_init_helper,
	    prop_send_ads_init_helper,
	    method,
	    link,
	    link_size,
	    info,
	    op_cmd
	};

	return prop_send_init_execute(&handler);
}

/*
 * Sends batch prop updates to LAN clients
 */
void prop_send_batch_to_lan_clients(struct ops_devd_cmd *op_cmd,
	struct device_state *dev, json_t *prop_info_arr_j,
	const char *url)
{
	struct client_lan_reg *lan;
	char lan_url[CLIENT_LAN_MAX_URL_LEN];
	json_t *prop_info;
	int rc;
	int i;
	int j;

	for (i = 1, lan = client_lan_reg; i < CLIENT_LAN_REGS; i++, lan++) {
		if (!(op_cmd->dests_target & BIT(i))) {
			continue;
		}
		if (!lan->uri[0]) {
			log_err("missing lan %d", i);
			ops_devd_mark_results(op_cmd, BIT(i), false);
			continue;
		}
		snprintf(lan_url, sizeof(lan_url), "%s%s", url,
		    op_cmd->echo ? "?echo=true" : "");
		rc = 0;
		for (j = 0; j < json_array_size(prop_info_arr_j); j++) {
			prop_info =
			    json_object_get(json_array_get(prop_info_arr_j, j),
			    "property");
			if (!prop_info) {
				log_warn("no prop obj found");
				continue;
			}
			rc |= client_lan_post(dev, lan, prop_info, lan_url,
			    &op_cmd->err_type);
		}
		ops_devd_mark_results(op_cmd, BIT(i), !rc);
	}
}

/*
 * Helper function for sending batch prop updates to a LAN client
 */
static int prop_batch_send_lan_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	u8 dests = op_cmd->dests_target;

	if (dests & ~DEST_ADS) {
		prop_send_batch_to_lan_clients(op_cmd, dev, pcmd->prop_info,
		    LAN_PROP_SEND_URL);
	}

	return 0;
}

/*
 * Contruct the payload + set the link for a batch datapoint
 */
void prop_batch_payload_construct(struct device_state *dev,
	struct prop_send_handlers *handler, const char *base_url,
	json_t *raw_payload)
{
	json_t *dp_obj;

	dp_obj = json_object();
	json_object_set(dp_obj, "batch_datapoints", raw_payload);
	ds_client_data_init_json(&handler->info->req_data, dp_obj);
	json_decref(dp_obj);
	handler->info->init = 1;
	*handler->method = HTTP_POST;

	snprintf(handler->link, handler->link_size,
	    base_url, dev->ads_host, ADS_API_VERSION, dev->dsn);
}

/*
 * Helper function for intializing a batch prop send/echo to ADS
 */
static void prop_batch_send_ads_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;

	prop_batch_payload_construct(dev, handler, BATCH_DPS_URL,
	    pcmd->prop_info);
}

/*
 * Initialize a batch datapoint send
 * Returns 0 on success
 * Returns -1 on failure
 * Returns 1 if success but no operation needed on ADS
 */
static int prop_batch_send_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_send_handlers handler = {
	    prop_batch_send_lan_init_helper,
	    prop_batch_send_ads_init_helper,
	    method,
	    link,
	    link_size,
	    info,
	    op_cmd
	};

	return prop_send_init_execute(&handler);
}

/*
 * Shared by props and gateway. Init function for acknowledgment of a property
 * update.
 */
int prop_ack_init_execute(struct prop_send_handlers *handler,
			    const char *lan_ack_url, const char *dsn)
{
	struct device_state *dev = &device;
	struct client_lan_reg *lan;
	u8 targets;
	int rc;
	int status;
	json_t *id_j;
	json_t *ack_message_j;
	json_t *ack_info_j;
	json_t *ack_status_j;
	const char *id;
	const char *prop_name;

	id_j = json_object_get(handler->prop_info, "id");
	ack_message_j = json_object_get(handler->prop_info, "ack_message");
	id = json_string_value(id_j);
	prop_name = json_get_string(handler->prop_info, "name");

	if (!id || !ack_message_j || !prop_name ||
	    json_get_int(handler->prop_info, "status", &status) < 0) {
		log_err("bad ack info");
		return -1;
	}
	ack_info_j = json_object();
	REQUIRE(ack_info_j, REQUIRE_MSG_ALLOCATION);
	json_object_set(ack_info_j, "id", id_j);
	if (!status) {
		ack_status_j = json_integer(HTTP_STATUS_OK);
	} else {
		ack_status_j = json_integer(HTTP_STATUS_BAD_REQUEST);
	}
	json_object_set_new(ack_info_j, "ack_status", ack_status_j);
	json_object_set(ack_info_j, "ack_message", ack_message_j);
	json_object_del(handler->prop_info, "id");
	json_object_del(handler->prop_info, "status");
	/* include the ack status and ack message info in the echo */
	json_object_set(handler->prop_info, "ack_status", ack_status_j);

	targets = handler->op_cmd->dests_target;
	if (handler->op_cmd->source == SOURCE_ADS) {
		/* don't echo prop back to ADS if that was source */
		targets &= ~DEST_ADS;
	} else {
		/* ack to LAN client */
		lan = &client_lan_reg[handler->op_cmd->source - 2];
		targets &= ~LAN_ID_TO_DEST_MASK(lan->id);
		if (!lan->uri[0]) {
			log_debug("no lan client to ack");
			json_decref(ack_info_j);
			/* no big deal, just mark it success */
			ops_devd_mark_results(handler->op_cmd, lan->id, true);
			/* don't echo the update to the source LAN client */
			if (!targets) {
				return 1;
			}
			goto send_echoes;
		}
		client_lan_post(dev, lan, ack_info_j, lan_ack_url, NULL);
		json_decref(ack_info_j);
		/* no big deal even if the post fails, always mark it success */
		ops_devd_mark_results(handler->op_cmd, lan->id, true);
	}
send_echoes:
	if (targets && !status) {
		/* for props that need explicit acks, only echo on success */
		handler->op_cmd->echo = 1;
		handler->op_cmd->dests_target = targets;
		rc = handler->lan_init_helper(dev, handler);
		if (rc) {
			return rc;
		}
		if (targets & DEST_ADS) {
			/* sending to ADS */
			handler->ads_init_helper(dev, handler);
			return rc;
		}
	} else {
		/* for failures, no need to echo to destinations */
		ops_devd_mark_results(handler->op_cmd, targets, true);
	}
	if (handler->op_cmd->source == DEST_ADS) {
		if (!(dev->dests_avail & DEST_ADS)) {
			ops_devd_mark_results(handler->op_cmd, DEST_ADS, false);
			return -1;
		}
		prop_curl_buf_info_setup(handler->info, ack_info_j);
		*handler->method = HTTP_PUT;
		snprintf(handler->link, handler->link_size,
		    "https://%s/%s/dsns/%s/properties/%s/datapoints/%s.json",
		    dev->ads_host, ADS_API_VERSION, dsn, prop_name, id);
		json_decref(ack_info_j);
		return 0;
	}
	return 1;
}

/*
 * Acknowledgment a property datapoint
 */
static int prop_ack_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_cmd *pcmd = (struct prop_cmd *)op_cmd->arg;
	struct prop_send_handlers handlers = {
	    prop_send_lan_init_helper,
	    prop_send_ads_init_helper,
	    method,
	    link,
	    link_size,
	    info,
	    op_cmd,
	    pcmd->prop_info
	};

	return prop_ack_init_execute(&handlers, LAN_PROP_ACK_URL, dev->dsn);
}

/*
 * Helper function for parsing a cmd and create a prop_cmd structure
 */
static enum app_parse_rc prop_init_pcmd_struct(struct prop_cmd *pup,
	json_t *cmd, enum ayla_data_op op, int recv_id)
{
	json_t *args;
	json_t *arg;
	const char *type;
	bool bool_val;

	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
inval_args:
		app_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
		return APR_ERR;
	}
	pup->op = op;
	if (op == AD_PROP_BATCH_SEND) {
		pup->prop_info = args;
		pup->prop_name = data_ops[AD_PROP_BATCH_SEND];
		json_incref(pup->prop_info);
		return APR_DONE;
	}
	/* can only handle one update in the array */
	arg = json_array_get(args, 0);
	if (!json_is_object(arg)) {
		goto inval_args;
	}
	pup->prop_info = json_object_get(arg, "property");
	if (!pup->prop_info) {
		goto inval_args;
	}
	pup->prop_name = json_get_string(pup->prop_info, "name");
	if (!pup->prop_name) {
		goto inval_args;
	}
	if (op == AD_PROP_SEND) {
		type = json_get_string(pup->prop_info, "base_type");
		if (!type) {
			goto inval_args;
		}
		if (!strcmp(type, "boolean")) {
			if (json_get_bool(pup->prop_info, "value",
			    &bool_val) < 0) {
				goto inval_args;
			}
		}
	}
	json_incref(pup->prop_info);
	return APR_DONE;
}

/*
 * Setup an internal pcmd to echo property updates
 * if other destinations exist
 */
void prop_prepare_echo(struct device_state *dev, json_t *prop_info, int source)
{
	struct ops_devd_cmd *op_cmd;
	struct prop_cmd *pcmd;
	u8 echo_dest = dev->dests_avail & ~SOURCE_TO_DEST_MASK(source);

	if (!echo_dest) {
		return;
	}
	pcmd = calloc(1, sizeof(*pcmd));
	op_cmd = calloc(1, sizeof(*op_cmd));
	REQUIRE(pcmd, REQUIRE_MSG_ALLOCATION);
	REQUIRE(op_cmd, REQUIRE_MSG_ALLOCATION);
	op_cmd->proto = JINT_PROTO_DATA;
	op_cmd->dests_target = echo_dest;
	op_cmd->echo = 1;
	pcmd->op = AD_AUTO_ECHO;
	pcmd->prop_name = json_get_string(prop_info, "name");
	pcmd->prop_info = json_incref(prop_info);
	if (debug) {
		log_debug("echoing %s to %d", pcmd->prop_name,
		    op_cmd->dests_target);
		ds_json_dump(__func__, prop_info);
	}
	op_cmd->arg = pcmd;
	if (pcmd->prop_name) {
		op_cmd->err_name = strdup(pcmd->prop_name);
	}
	op_cmd->op_handlers = &prop_op_handlers[PCS_AUTO_ECHO];
	ops_devd_add(op_cmd);
}

/*
 * Get the dests from the opts object.
 */
int prop_get_dests_helper(struct device_state *dev, json_t *opts,
			u8 *dests_specified)
{
	json_t *dests_j;
	int dests_given;

	*dests_specified = 0;
	dests_j = json_object_get(opts, "dests");
	if (json_is_integer(dests_j)) {
		dests_given = json_integer_value(dests_j);
		if (!dests_given) {
			goto dests_avail;
		}
		*dests_specified = 1;
		if (!(dests_given & ~DEST_LAN_APPS)) {
			/* appd wants to send to mobile apps only */
			/* filter out any destinations that don't exist */
			dests_given &= dev->dests_avail;
		}
		return dests_given;
	}
dests_avail:
	return dev->dests_avail | DEST_ADS;
}

/*
 * Handle a property operation
 */
enum app_parse_rc prop_handle_data_pkt(json_t *cmd, int recv_id, json_t *opts)
{
	const char *opstr = json_get_string(cmd, "op");
	enum ayla_data_op op = jint_get_data_op(opstr);
	struct ops_devd_cmd *op_cmd;
	struct device_state *dev = &device;
	struct prop_cmd *pup = NULL;
	enum app_parse_rc rc;
	u8 confirm = 0;
	u8 echo = 0;
	int source = 0;
	u8 dests = DEST_ADS;
	enum prop_client_state pcs;
	const char *location;
	const char *value;
	u8 dests_specified = 0;
	const char *type;

	switch (op) {
	case AD_PROP_RESP:
		prop_handle_prop_resp(cmd, recv_id);
		return APR_DONE;
	case AD_PROP_SEND:
	case AD_PROP_REQ:
	case AD_PROP_ACK:
	case AD_PROP_BATCH_SEND:
		pup = calloc(1, sizeof(*pup));
		REQUIRE(pup, REQUIRE_MSG_ALLOCATION);
		rc = prop_init_pcmd_struct(pup, cmd, op, recv_id);
		if (rc != APR_DONE) {
err:
			prop_free_prop_cmd(pup);
			if (confirm) {
				jint_send_confirm_false(JINT_PROTO_DATA,
				    recv_id, dests, JINT_ERR_INVAL_ARGS);
			}
			return APR_ERR;
		}
		confirm = json_is_true(json_object_get(opts, "confirm"));
		echo = json_is_true(json_object_get(opts, "echo"));
		switch (op) {
		case AD_PROP_SEND:
		case AD_PROP_BATCH_SEND:
			dests = prop_get_dests_helper(dev, opts,
			    &dests_specified);
			app_send_ack(recv_id);
			pcs = (op == AD_PROP_SEND) ? PCS_PROP_SEND :
			    PCS_PROP_BATCH_SEND;
			if (op != AD_PROP_SEND) {
				break;
			}
			type = json_get_string(pup->prop_info, "base_type");
			if (!type) {
				break;
			}
			if (!strcmp(type, data_types[ATLV_MSG_BIN])) {
				pcs = PCS_POST_MSG_ID;
			}
			break;
		case AD_PROP_REQ:
			pcs = PCS_GET_PROP;
			type = json_get_string(pup->prop_info, "base_type");
			if (!type) {
				break;
			}
			if (strcmp(type, data_types[ATLV_MSG_BIN])) {
				break;
			}
			location = json_get_string(pup->prop_info, "location");
			value = json_get_string(pup->prop_info, "value");
			if (!location || !value) {
				break;
			}
			pcs = PCS_GET_MSG_DATA;
			break;
		case AD_PROP_ACK:
			json_get_int(opts, "source", &source);
			if (!source) {
				goto err;
			}
			dests = dev->dests_avail | SOURCE_TO_DEST_MASK(source);
			app_send_ack(recv_id);
			pcs = PCS_PUT_ACK;
			break;
		default:
			goto err;
		}
		break;
	case AD_DP_SEND:
		pup = calloc(1, sizeof(*pup));
		REQUIRE(pup, REQUIRE_MSG_ALLOCATION);
		rc = prop_init_pcmd_struct(pup, cmd, op, recv_id);
		if (rc != APR_DONE) {
			goto err;
		}
		location = json_get_string(pup->prop_info, "location");
		value = json_get_string(pup->prop_info, "value");
		if (!location || !value) {
inval_args:
			app_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			goto err;
		}
		confirm = json_is_true(json_object_get(opts, "confirm"));
		echo = json_is_true(json_object_get(opts, "echo"));
		app_send_ack(recv_id);
		pcs = PCS_PUT_DP_SEND;
		break;
	case AD_DP_CREATE:
	case AD_DP_REQ:
	case AD_DP_FETCHED:
		pup = calloc(1, sizeof(*pup));
		REQUIRE(pup, REQUIRE_MSG_ALLOCATION);
		rc = prop_init_pcmd_struct(pup, cmd, op, recv_id);
		if (rc != APR_DONE) {
			goto err;
		}
		location = json_get_string(pup->prop_info, "location");
		value = json_get_string(pup->prop_info, "value");
		if ((op == AD_DP_REQ && (!location || !value)) ||
		    (op == AD_DP_FETCHED && !value)) {
			goto inval_args;
		}
		confirm = json_is_true(json_object_get(opts, "confirm"));
		echo = json_is_true(json_object_get(opts, "echo"));
		app_send_ack(recv_id);
		switch (op) {
		case AD_DP_CREATE:
			pcs = PCS_POST_DP_CREATE;
			break;
		case AD_DP_REQ:
			pcs = PCS_GET_DP_REQ;
			break;
		case AD_DP_FETCHED:
			pcs = PCS_PUT_DP_FETCHED;
			break;
		default:
			goto err;
		}
		break;
	case AD_PROP_REQ_ALL:
	case AD_PROP_REQ_TO_DEV:
		pup = calloc(1, sizeof(*pup));
		REQUIRE(pup, REQUIRE_MSG_ALLOCATION);
		pup->op = op;
		switch (op) {
		case AD_PROP_REQ_ALL:
			pcs = PCS_GET_ALL_PROPS;
			break;
		default:
			pcs = PCS_GET_TODEV_PROPS;
			break;
		}
		break;
	case AD_PROP_REQ_FROM_DEV:
	case AD_NOP:
		return APR_DONE;
	default:
		log_err("can't process opcode %d", op);
		return APR_ERR;
	}

	op_cmd = calloc(1, sizeof(*op_cmd));
	REQUIRE(op_cmd, REQUIRE_MSG_ALLOCATION);
	op_cmd->proto = JINT_PROTO_DATA;
	op_cmd->req_id = recv_id;
	op_cmd->confirm = confirm;
	op_cmd->source = source;
	op_cmd->echo = echo;
	op_cmd->arg = pup;
	op_cmd->dests_specified = dests_specified;
	if (pup->prop_name) {
		op_cmd->err_name = strdup(pup->prop_name);
	}
	op_cmd->dests_target = dests;
	op_cmd->op_handlers = &prop_op_handlers[pcs];
	ops_devd_add(op_cmd);

	return APR_DONE;
}


static const struct op_funcs prop_op_handlers[] = {
	[PCS_NOP] = {NULL, prop_nop_init},
	[PCS_GET_PROP] = {&data_ops[AD_PROP_REQ],
	    prop_get_init, prop_get_success, prop_generic_op_finished},
	[PCS_GET_ALL_PROPS] = {&data_ops[AD_PROP_REQ_ALL],
	    prop_get_props_init, prop_get_props_success,
	    prop_generic_op_finished},
	[PCS_GET_TODEV_PROPS] = {&data_ops[AD_PROP_REQ_TO_DEV],
	    prop_get_props_init, prop_get_props_success,
	    prop_generic_op_finished},
	[PCS_PROP_SEND] = {&data_ops[AD_PROP_SEND],
	    prop_send_init, NULL, prop_generic_op_finished},
	[PCS_PROP_BATCH_SEND] = {&data_ops[AD_PROP_BATCH_SEND],
	    prop_batch_send_init, prop_batch_post_success,
	    prop_generic_op_finished},
	[PCS_AUTO_ECHO] = {&data_ops[AD_ECHO_FAILURE],
	    prop_send_init, NULL, prop_generic_op_finished},
	[PCS_POST_DP_CREATE] = {&data_ops[AD_DP_CREATE],
	    prop_file_dp_create, prop_file_dp_create_success,
	    prop_generic_op_finished},
	[PCS_PUT_DP_SEND] = {&data_ops[AD_DP_SEND],
	    prop_file_dp_send, prop_file_dp_send_success,
	    prop_generic_op_finished},
	[PCS_PUT_DP_CLOSE] = {&data_ops[AD_DP_CLOSE],
	    prop_file_dp_close, NULL, prop_generic_op_finished},
	[PCS_GET_DP_REQ] = {&data_ops[AD_DP_REQ],
	    prop_file_dp_req, prop_file_dp_req_success,
	    prop_generic_op_finished},
	[PCS_GET_DP_REQ_FROM_FILE] = {&data_ops[AD_DP_REQ_FROM_FILE],
	    prop_file_dp_req_from_file, NULL, prop_generic_op_finished},
	[PCS_PUT_DP_FETCHED] = {&data_ops[AD_DP_FETCHED],
	    prop_file_dp_mark_fetched, prop_file_dp_mark_fetched_success,
	    prop_generic_op_finished},
	[PCS_PUT_ACK] = {&data_ops[AD_PROP_ACK],
	    prop_ack_init, NULL, prop_generic_op_finished},
	[PCS_POST_MSG_ID] = {&data_ops[AD_PROP_SEND],
	    prop_msg_id_create, prop_post_msg_id_success,
	    prop_generic_op_finished},
	[PCS_PUT_MSG_DATA] = {&data_ops[AD_PROP_SEND],
	    prop_put_msg_data, NULL, prop_generic_op_finished},
	[PCS_GET_MSG_DATA] = {&data_ops[AD_MSG_GET],
	    prop_get_msg_data, prop_get_msg_data_success,
	    prop_generic_op_finished},
};
