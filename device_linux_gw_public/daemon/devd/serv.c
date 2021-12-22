/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#define _GNU_SOURCE 1 /* for strndup */
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/conf_io.h>
#include <ayla/nameval.h>
#include <ayla/http.h>
#include <ayla/time_utils.h>
#include <ayla/json_parser.h>
#include <ayla/uri_code.h>
#include <ayla/server.h>
#include <ayla/ayla_interface.h>
#include <ayla/wifi.h>
#include <ayla/log.h>
#include <ayla/lan_ota.h>
#include <ayla/amsg.h>
#include <ayla/msg_utils.h>

#include "ds.h"
#include "dapi.h"
#include "serv.h"
#include "props_client.h"
#include "props_if.h"
#include "msg_server.h"
#include "ds_client.h"
#include "ops_devd.h"
#include "gateway_if.h"

/* set SERVER subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_SERVER, __VA_ARGS__)

#define OTA_NAME "ota_update"

static DEF_NAME_TABLE(wifi_errors, WIFI_ERRORS);

/*
 * Arbitrary delay between sending a server response and disconnecting from
 * the network.  This is needed for requests that cause the device to
 * disconnect, because immediately disconnecting might not allow socket data
 * to be flushed and the TCP connection to be broken down..
 */
#define SERV_RESP_GRACE_PERIOD_MS	500

/* Timeout in seconds for reverse-REST command response to complete */
#define SERV_REV_REST_RESP_TIMEOUT	10

/*
 * State to store amsg send info for a message that must be sent after a
 * delay.
 */
struct serv_msg_send_delayed_state {
	char *app_name;
	uint8_t interface;
	uint8_t type;
	json_t *json;
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    json_t *, void *);
	void *resp_arg;
	uint32_t timeout_ms;

	struct timer send_timer;
};

/*
 * State to track wifi_connect when delayed connection is needed.
 */
struct serv_wifi_connect_delayed_state {
	struct server_req *req;
	json_t *msg;
};

static void serv_rev_put_end_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct serv_rev_req *rev_req = (struct serv_rev_req *)arg;

	serv_rev_req_close(rev_req);
	ds_step();
}

static void serv_rev_put_end(struct server_req *req, int status)
{
	struct device_state *dev = &device;
	struct serv_rev_req *rev_req = (struct serv_rev_req *)req->arg;
	char uri[DS_CLIENT_LINK_MAX_LEN];
	struct ds_client_req_info info = {
		.method = HTTP_PUT,
		.host = dev->ads_host,
		.uri = uri,
		.timeout_secs = SERV_REV_REST_RESP_TIMEOUT
	};

	ASSERT(rev_req->lan == NULL);

	log_debug("status %d", status);

	if (!rev_req->resp_uri) {
		log_err("missing response URI");
		goto error;
	}
	ds_client_data_init_qbuf(&info.req_data, &req->reply);
	if (req->reply_is_json) {
		info.req_data.content = HTTP_CONTENT_JSON;
	}
	snprintf(uri, sizeof(uri), "devices/%s%s?cmd_id=%d&status=%d",
	    dev->key, rev_req->resp_uri, rev_req->cmd_id, status);
	if (ds_send(&dev->client, &info, serv_rev_put_end_done,
	    rev_req) < 0) {
		log_warn("reverse-REST command response failed");
		goto error;
	}
	return;
error:
	serv_rev_put_end_done(HTTP_CLIENT_ERR_FAILED, NULL, NULL, rev_req);
}

static void serv_rev_lan_put_end(struct server_req *req, int status)
{
	struct serv_rev_req *rev_req = (struct serv_rev_req *)req->arg;

	ASSERT(rev_req->lan != NULL);

	log_debug("status %d", status);

	client_lan_cmd_resp(req, status);
	serv_rev_put_end_done(HTTP_CLIENT_ERR_NONE, NULL, NULL, rev_req);
}

static struct serv_rev_req *serv_rev_req_alloc(void)
{
	struct serv_rev_req *rev_req;
	struct server_req *req;

	req = server_req_alloc();
	if (!req) {
		return NULL;
	}
	rev_req = calloc(1, sizeof(*rev_req));
	if (!rev_req) {
		log_warn("malloc failed");
		server_req_close(req);
		return NULL;
	}
	req->put_end = serv_rev_put_end;
	req->arg = rev_req;
	rev_req->req = req;
	rev_req->source = SOURCE_ADS;
	return rev_req;
}

static struct serv_rev_req *serv_rev_lan_req_alloc(struct client_lan_reg *lan)
{
	struct serv_rev_req *rev_req;

	rev_req = serv_rev_req_alloc();
	if (rev_req && lan) {
		rev_req->source = LAN_ID_TO_SOURCE(lan->id);
		rev_req->lan = lan;
		rev_req->req->put_end = serv_rev_lan_put_end;
	}
	return rev_req;
}

/*
 * Free a reverse REST command.  Normally done by the put_end function.
 */
void serv_rev_req_close(struct serv_rev_req *rev_req)
{
	if (!rev_req) {
		return;
	}
	if (rev_req->req) {
		server_req_close(rev_req->req);
	}
	free(rev_req->resource);
	free(rev_req->resp_uri);
	free(rev_req);
}

/*
 * Get the source of a reverse-REST server request.  Returns 0 if not reverse-
 * REST, or if source is not known.
 */
static int serv_rev_get_source(struct server_req *req)
{
	struct serv_rev_req *rev_req = (struct serv_rev_req *)req->arg;

	if (!rev_req) {
		return SOURCE_LOCAL;
	}
	return rev_req->source;
}

/*
 * Parse an OTA json object and pick out the necessary pieces. Return error
 * if any parts are missing.
 */
int serv_ota_obj_parse(json_t *ota_obj, const char **ota_type,
		const char **checksum, const char **url, const char **ver,
		size_t *size)
{
	unsigned ota_size;

	if (!json_is_object(ota_obj)) {
		log_warn("no ota object");
		return -1;
	}

	*ota_type = json_get_string(ota_obj, "type");
	*checksum = json_get_string(ota_obj, "checksum");
	*url = json_get_string(ota_obj, "url");
	*ver = json_get_string(ota_obj, "ver");
	if (json_get_uint(ota_obj, "size", &ota_size) < 0) {
		log_warn("missing OTA size");
		return -1;
	}
	*size = ota_size;
	if (!*ota_type || !*checksum || !*url || !*ver) {
		log_warn("missing OTA information");
		return -1;
	}

	return 0;
}

/*
 * Configure and launch the OTA updater.
 * Return 0 for success; -1 and set errno for failure.
 */
static int serv_ota_exec(json_t *ota_obj, const char *auth_header)
{
	char ota_loc[sizeof(OTA_NAME) + 5];
	char *argv[15];
	int i = 0;
	int j;
	size_t size;
	const char *ota_type;
	const char *checksum;
	const char *url;
	const char *ver;
	char size_str[12];
	pid_t pid;
	int rc = -1;

	rc = serv_ota_obj_parse(ota_obj, &ota_type, &checksum, &url, &ver,
	    &size);
	if (rc) {
		return rc;
	}
	if (strcmp(ota_type, "host_mcu")) {
		log_warn("unsupported ota type %s", ota_type);
		errno = EINVAL;
		return rc;
	}
	log_info("starting OTA download/apply of version %s", ver);
	pid = fork();
	if (pid < 0) {
		log_err("fork failed");
		return rc;
	}
	if (pid != 0) {
		return 0;
	}
	snprintf(size_str, sizeof(size_str), "%zu", size);
	argv[i++] = OTA_NAME;
	argv[i++] = "-u";
	argv[i++] = strdup(url);
	argv[i++] = "-l";
	argv[i++] = size_str;
	argv[i++] = "-c";
	argv[i++] = strdup(checksum);
	argv[i++] = "-s";
	argv[i++] = devd_msg_sock_path;
	argv[i++] = "-ar";
	if (foreground) {
		argv[i++] = "-f";
	}
	if (debug) {
		argv[i++] = "-d";
	}
	if (auth_header) {
		argv[i++] = "-H";
		if (asprintf(&argv[i++], "%s: %s", ADS_TEMP_AUTH_FIELD,
		    auth_header) < 0) {
			log_err("malloc failed");
		}
	}
	argv[i] = NULL;
	ASSERT(i < ARRAY_LEN(argv));
	if (debug) {
		log_debug("Starting %s using args: ", OTA_NAME);
		for (j = 0; j < i; j++) {
			log_debug("%s", argv[j]);
		}
	}
	execvp(OTA_NAME, argv);

	/* perhaps running locally on VM */
	snprintf(ota_loc, sizeof(ota_loc), "./%s", OTA_NAME);
	log_warn("executing %s failed, trying %s", OTA_NAME, ota_loc);
	argv[0] = ota_loc;
	execvp(ota_loc, argv);
	log_err("unable to start %s", OTA_NAME);
	sleep(2);
	exit(1);
}

/*
 * Config.json get Reverse-Rest command from Service
 */
static void serv_conf_get(struct server_req *req)
{
	size_t arg_len = req->args ? strlen(req->args) : 0;
	char name[arg_len + 1];
	json_t *root;
	json_t *config;
	json_t *val;

	if (server_get_arg_by_name(req, "name", name, sizeof(name)) < 0) {
		log_err("invalid request: name argument missing");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	/* Only support specific config requests */
	if (strncmp(name, "sys/setup_mode", sizeof(name)) &&
	    strncmp(name, "client/server/default", sizeof(name))) {
		log_err("invalid request: forbidden");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	val = conf_get(name);
	if (!val) {
		log_err("invalid request: config does not exist");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	root = json_object();
	config = json_object();
	json_object_set_new(root, "config", config);
	json_object_set_new(config, "name", json_string(name));
	json_object_set(config, "val", val);

	server_put_json(req, root);
	json_decref(root);
	server_put_end(req, HTTP_STATUS_OK);
}

/*
 * Config.json put Reverse-Rest command from Service
 */
static void serv_conf_put(struct server_req *req)
{
	json_t *config;
	json_t *config_item;
	json_t *data;
	json_t *name;
	json_t *value;
	const char *name_str;
	int i;
	int rc;
	bool invalid = false;
	bool changed = false;

	data = req->body_json;
	if (!json_is_object(data)) {
		log_warn("no data object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	config = json_object_get(data, "config");
	if (!json_is_array(config)) {
		log_warn("no config array");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}

	for (i = 0; i < json_array_size(config); i++) {
		config_item = json_array_get(config, i);
		if (!json_is_object(config_item)) {
			log_warn("%d not an object", i);
			break;
		}
		name = json_object_get(config_item, "name");
		if (!name) {
			log_warn("%d: no name", i);
			break;
		}
		name_str = json_string_value(name);
		if (!name_str) {
			log_warn("%d: invalid name string", i);
			break;
		}
		value = json_object_get(config_item, "val");
		if (!value) {
			log_warn("%s: no value", name_str);
			break;
		}
		rc = conf_set(name_str, value);
		if (rc < 0) {
			log_err("ignoring invalid config: %s", name_str);
			invalid = true;
		} else if (!rc) {
			changed = true;
		}
	}
	if (changed) {
		conf_apply();
		conf_save();
	}
	if (!invalid) {
		server_put_end(req, HTTP_STATUS_OK);
	} else {
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
	}
}

/*
 * logclient.json put Reverse-Rest command from Service
 */
void serv_conf_log_client_put(struct server_req *req)
{
	int rc;
	const char *key;
	char path[100];
	json_t *data;
	json_t *value;
	bool invalid = false;
	bool changed = false;

	data = req->body_json;
	if (!data || !json_is_object(data)) {
		log_warn("no data object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	json_object_foreach(data, key, value) {
		snprintf(path, sizeof(path), "log/%s", key);
		rc = conf_set(path, value);
		if (rc < 0) {
			log_err("ignoring invalid log config: %s", path);
			invalid = true;
		} else if (!rc) {
			changed = true;
		}
	}
	if (changed) {
		conf_apply();
		conf_save();
	}
	if (!invalid) {
		server_put_end(req, HTTP_STATUS_OK);
	} else {
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
	}
}

/*
 * Search a JSON array of module entries and return a pointer
 * to the levels array of the appropriate one.  Create missing elements
 * of the JSON structure if missing.
 * Returns NULL on error.
 */
static json_t *serv_conf_get_log_levels(json_t *mod_array, const char *name)
{
	int i;
	json_t *mod_obj = NULL;
	json_t *mod_name_obj;
	json_t *mod_levels_array = NULL;
	const char *mod_name;

	if (!json_is_array(mod_array)) {
		return NULL;
	}
	for (i = 0; i < json_array_size(mod_array); ++i) {
		mod_obj = json_array_get(mod_array, i);
		mod_name_obj = json_object_get(mod_obj, "name");
		if (!mod_name_obj || !json_is_string(mod_name_obj)) {
			log_warn("missing mod name");
			continue;
		}
		mod_name = json_string_value(mod_name_obj);
		if (!strcmp(name, mod_name)) {
			/* found the module */
			mod_levels_array = json_object_get(mod_obj, "levels");
			break;
		}
	}
	/* if module object not found, create it */
	if (i == json_array_size(mod_array)) {
		mod_obj = json_object();
		json_object_set_new(mod_obj, "name", json_string(name));
		json_array_append_new(mod_array, mod_obj);
	}
	if (!mod_levels_array) {
		mod_levels_array = json_array();
		json_object_set_new(mod_obj, "levels", mod_levels_array);
	}
	return mod_levels_array;
}

/*
 * Perform diff function. Source data is an array of level names with
 * an integer value indicating enabled or disabled, and destination
 * is an array of enabled logging levels.  Existing levels not described
 * in the diff array are preserved.
 */
static int serv_conf_apply_log_levels(json_t *diff_array, json_t *conf_array)
{
	int i, j;
	json_t *diff_level;
	const char *diff_level_name;
	json_t *diff_level_value;
	json_t *conf_level;
	void *iter;
	int8_t diff;

	if (!json_is_array(diff_array) || !json_is_array(conf_array)) {
		return -1;
	}

	for (i = 0; i < json_array_size(diff_array); ++i) {
		diff_level = json_array_get(diff_array, i);
		if (!json_is_object(diff_level)) {
			log_warn("invalid level diff");
			continue;
		}
		iter = json_object_iter(diff_level);
		diff_level_name = json_object_iter_key(iter);
		diff_level_value = json_object_iter_value(iter);
		if (!diff_level_name) {
			log_warn("invalid level diff name");
			continue;
		}
		diff = json_integer_value(diff_level_value) ? 1 : -1;

		/* search for matching level in config */
		for (j = 0; j < json_array_size(conf_array); ++j) {
			conf_level = json_array_get(conf_array, j);
			if (!json_is_string(conf_level)) {
				log_warn("invalid level conf: "
				    "%s", diff_level_name);
				continue;
			}
			if (!strcmp(diff_level_name,
			    json_string_value(conf_level))) {
				if (diff < 0) {
					json_array_remove(conf_array, j);
					if (j > 0) {
						--j;
					}
				} else if (diff > 0) {
					/*
					 * Found level, so set diff to remove
					 * any duplicates.
					 */
					diff = -1;
				}
			}
		}
		/* level not in config, so add it if requested */
		if (diff > 0) {
			json_array_append_new(conf_array,
			    json_string(diff_level_name));
		}
	}
	return 0;
}

/*
 * logclient.json put Reverse-Rest command from Service
 */
static void serv_conf_log_mods_put(struct server_req *req)
{
	int rc = 0;
	int i;
	json_t *mods_data;
	json_t *mod_data;
	json_t *mod_name_data;
	json_t *levels_data;
	json_t *mods_conf;
	json_t *levels_conf;
	const char *name;

	mods_data = json_object_get(req->body_json, "mods");
	if (!mods_data || !json_is_array(mods_data)) {
		log_warn("missing array: mods");
		return;
	}

	mods_conf = conf_get("log/mods");
	if (!mods_conf) {
		mods_conf = json_array();
		conf_set_new("log/mods", mods_conf);
	}

	for (i = 0; i < json_array_size(mods_data); ++i) {
		mod_data = json_array_get(mods_data, i);
		mod_name_data = json_object_get(mod_data, "name");
		name = json_string_value(mod_name_data);
		levels_data = json_object_get(mod_data, "levels");
		if (!name || !levels_data || !json_is_array(levels_data)) {
			log_warn("malformed obj: mod");
			continue;
		}
		/* find or create the module in config */
		levels_conf = serv_conf_get_log_levels(mods_conf, name);
		if (!levels_conf) {
			log_warn("failed to access conf: mod %s", name);
			continue;
		}
		rc |= serv_conf_apply_log_levels(levels_data, levels_conf);
	}

	if (!rc) {
		conf_apply();
		conf_save();
		server_put_end(req, HTTP_STATUS_OK);
	} else {
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
	}
}

/*
 * getdsns.json put Reverse-Rest command from Service
 */
void serv_getdsns_put(struct server_req *req)
{
	ds_cloud_init();
	server_put_end(req, HTTP_STATUS_ACCEPTED);
}

/*
 * Callback to perform reset after request has completed.
 */
static void serv_reset_req_complete(struct server_req *req)
{
	bool factory = server_get_bool_arg_by_name(req, "factory");

	ds_reset(factory);
}

/*
 * reset.json put Reverse-Rest command from Service
 */
static void serv_reset_put(struct server_req *req)
{
	/* Reset after response has been sent */
	server_set_complete_callback(req, serv_reset_req_complete);
	server_put_end(req, HTTP_STATUS_ACCEPTED);
}

/*
 * Remote location for an OTA successfully fetched
 */
static void serv_ota_url_fetch_success(json_t *ota_obj)
{
	serv_ota_exec(ota_obj, NULL);
}

/*
 * PUT via REST from ADS to start OTA or schedule external OTA GET.
 */
static void serv_ota_put(struct server_req *req)
{
	struct device_state *dev = &device;
	json_t *obj;
	json_t *ota;
	const char *ota_source;
	int rc;

	obj = req->body_json;
	if (!json_is_object(obj)) {
		log_warn("no object");
bad_req:
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	ota = json_object_get(obj, "ota");
	if (!json_is_object(ota)) {
		log_warn("no ota object");
		goto bad_req;
	}

	ota_source = json_get_string(ota, "source");
	if (ota_source && strcmp(ota_source, "local")) {
		/* for non-local OTA, schedule GET for server info */
		rc = ops_devd_ota_url_fetch(ota, serv_ota_url_fetch_success);
	} else {
		/* initiate local OTA immediately */
		rc = serv_ota_exec(ota, dev->auth_header);
	}

	if (rc) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	} else {
		server_put_end(req, HTTP_STATUS_OK);
	}
}

/*
 * Configure and launch the OTA updater.
 * Return 0 for success; -1 and set errno for failure.
 */
static int serv_lan_ota_exec(struct lan_ota_exec_info *ota_info)
{
	char ota_loc[sizeof(OTA_NAME) + 5];
	char *argv[25];
	int i = 0;
	int j;
	char size_str[12];
	pid_t pid;
	int rc = -1;

	if (strcmp(ota_info->ota_type, "host_mcu")) {
		log_warn("unsupported ota type %s", ota_info->ota_type);
		errno = EINVAL;
		return rc;
	}
	log_info("starting LAN OTA download/apply of version %s",
		ota_info->ver);
	pid = fork();
	if (pid < 0) {
		log_err("fork failed");
		return rc;
	}
	if (pid != 0) {
		return 0;
	}
	snprintf(size_str, sizeof(size_str), "%zu", ota_info->size);
	argv[i++] = OTA_NAME;
	argv[i++] = "-u";
	argv[i++] = strdup(ota_info->url);
	argv[i++] = "-l";
	argv[i++] = size_str;
	argv[i++] = "-L";
	argv[i++] = "-D";
	argv[i++] = strdup(ota_info->dsn);
	argv[i++] = "-k";
	argv[i++] = strdup(ota_info->key);
	argv[i++] = "-c";
	argv[i++] = strdup(ota_info->checksum);
	argv[i++] = "-ar";
	if (foreground) {
		argv[i++] = "-f";
	}
	if (debug) {
		argv[i++] = "-d";
	}
	argv[i] = NULL;
	ASSERT(i < ARRAY_LEN(argv));
	if (debug) {
		log_debug("Starting %s using args: ", OTA_NAME);
		for (j = 0; j < i; j++) {
			log_debug("%s", argv[j]);
		}
	}
	execvp(OTA_NAME, argv);

	/* perhaps running locally on VM */
	snprintf(ota_loc, sizeof(ota_loc), "./%s", OTA_NAME);
	log_warn("executing %s failed, trying %s", OTA_NAME, ota_loc);
	argv[0] = ota_loc;
	execvp(ota_loc, argv);
	log_err("unable to start %s", OTA_NAME);
	sleep(2);
	exit(1);
}

/*
 * A HTTP PUT from mobile for a lan ota image
 */
static void serv_lan_ota_put(struct server_req *req)
{
	struct device_state *dev = &device;
	void *req_buf = NULL;
	size_t req_buf_len;
	struct lan_ota_exec_info ota_info = {0};
	int rc;

	req_buf = queue_buf_coalesce(&req->request);
	if (!req_buf) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		return;
	}
	req_buf_len = queue_buf_len(&req->request);

	rc = lan_ota_header_proc(req_buf, req_buf_len,
		dev->dsn, dev->pub_key, &ota_info);
	if (rc) {
		log_err("lan ota process header failed");
		goto lan_ota_handle_exit;
	}

	/* initiate lan OTA immediately */
	rc = serv_lan_ota_exec(&ota_info);

	lan_ota_free_exec_info(&ota_info);

lan_ota_handle_exit:
	if (!rc) {
		server_put_end(req, HTTP_STATUS_NO_CONTENT);
	} else if (rc == -2) {
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
	} else {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
	return;
}

/*
 * POST to open a registration window.
 */
static void serv_push_button_reg_post(struct server_req *req)
{
	if (ds_push_button_reg_start() < 0) {
		server_put_end(req, HTTP_STATUS_UNAVAIL);
		return;
	}
	server_put_end(req, HTTP_STATUS_ACCEPTED);
}

/*
 * PUT change to LAN settings.
 */
static void serv_lanip_put(struct server_req *req)
{
	json_t *data;
	json_t *lanip;

	data = req->body_json;
	if (!json_is_object(data)) {
		log_warn("no data object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	lanip = json_object_get(data, "lanip");
	if (!json_is_object(lanip)) {
		log_warn("no lanip object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	if (conf_set("lanip", lanip) < 0 ||
	    conf_apply() < 0) {
		log_warn("lanip config set failed");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	conf_save();
	server_put_end(req, HTTP_STATUS_OK);
}

/*
 * Get device service json object.
 */
static json_t *serv_ads_host(struct device_state *dev)
{
	return json_string(dev->ads_host);
}

/*
 * Get the devd portion of the status for the web server.
 */
static void serv_status_get(struct server_req *req)
{
	struct device_state *dev = &device;
	json_t *root;

	root = json_object();
	json_object_set_new(root, "dsn", json_string(dev->dsn));
	json_object_set_new(root, "device_service", serv_ads_host(dev));
	json_object_set_new(root, "last_connect_mtime",
	    json_integer(dev->conn_mtime));
	json_object_set_new(root, "last_connect_time",
	    json_integer(dev->conn_time));
	json_object_set_new(root, "mtime", json_integer(time_mtime_ms()));
	json_object_set_new(root, "version", json_string(version));
	json_object_set_new(root, "features", dev->wifi_features ?
	    json_incref(dev->wifi_features) : json_array());
	json_object_set_new(root, "build", json_string(version));
	json_object_set_new(root, "api_version",
	    json_string(CLIENT_API_VERSION));

	server_put_json(req, root);
	json_decref(root);
	server_put_end(req, HTTP_STATUS_OK);
}


/*
 * GET time.json
 */
static void serv_time_get(struct server_req *req)
{
	json_t *root;
	time_t t;
	u32 loc_time;
	char time_str[CLOCK_FMT_LEN];

	time(&t);
	loc_time = clock_local((u32 *)&t);
	clock_fmt(time_str, sizeof(time_str), loc_time);

	root = json_object();
	json_object_set_new(root, "time", json_integer(t));
	json_object_set_new(root, "mtime", json_integer(time_mtime_ms()));
	json_object_set_new(root, "set_at_mtime", json_integer(0)); /*optional*/
	json_object_set_new(root, "clksrc", json_integer(clock_source()));
	json_object_set_new(root, "localtime", json_string(time_str));
	json_object_set_new(root, "daylight_active",
	    json_integer(daylight_ayla.valid && daylight_ayla.active ? 1 : 0));
	json_object_set_new(root, "daylight_change",
	    json_integer(daylight_ayla.valid ? daylight_ayla.change : 0));

	server_put_json(req, root);
	json_decref(root);
	server_put_end(req, HTTP_STATUS_OK);
}

/*
 * PUT time.json
 */
static void serv_time_put(struct server_req *req)
{
	json_t *data;
	u32 t;

	data = req->body_json;
	if (!json_is_object(data)) {
		log_warn("no data object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	if (json_get_uint(data, "time", &t) < 0) {
		log_warn("no time object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	server_put_end(req, HTTP_STATUS_OK);

	ds_clock_set(t, CS_LOCAL);
}

/*
 * Select the most appropriate HTTP status from an amsg_err code.
 * This is useful when proxying HTTP requests via a messaging interface.
 */
static int serv_wifi_get_http_err_status(enum amsg_err err)
{
	switch (err) {
	case AMSG_ERR_NONE:
		return HTTP_STATUS_OK;
	case AMSG_ERR_PRIVS:
		return HTTP_STATUS_FORBIDDEN;
	case AMSG_ERR_DATA_CORRUPT:
		return HTTP_STATUS_BAD_REQUEST;
	case AMSG_ERR_TIMED_OUT:
	case AMSG_ERR_INTERRUPTED:
		return HTTP_STATUS_UNAVAIL;
	default:
		break;
	}
	return HTTP_STATUS_INTERNAL_ERR;
}

/*
 * URI encode some data and create a JSON string.  This facilitates sending
 * data without valid UTF-8 encoding.
 */
static json_t *serv_uri_encoded_json_string(const void *data, size_t len)
{
	char str[len * 3 + 1];	/* Reserve space for null termination */
	ssize_t str_len;

	/* % encode all non-printable ASCII characters */
	str_len = uri_encode(str, sizeof(str), (char *)data, len,
	    uri_printable_ascii_map);
	if (str_len < 0) {
		return NULL;
	}
	return json_stringn(str, str_len);
}

/*
 * Response handler for a wifi_status request.
 */
static void serv_wifi_status_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, json_t *msg_obj, void *arg)
{
	struct device_state *dev = &device;
	struct server_req *req = (struct server_req *)arg;
	json_t *obj;

	ASSERT(req != NULL);

	if (err != AMSG_ERR_NONE) {
		server_put_end(req, serv_wifi_get_http_err_status(err));
		return;
	}
	if (!msg_obj) {
		log_err("no data");
		goto error;
	}
	obj = json_object_get(msg_obj, "wifi_status");
	if (!json_is_object(obj)) {
		log_err("missing wifi_status object");
		goto error;
	}
	/* Add device info */
	json_object_set_new(obj, "dsn", json_string(dev->dsn));
	json_object_set_new(obj, "device_service", serv_ads_host(dev));
	json_object_set_new(obj, "mtime", json_integer(time_mtime_ms()));
	json_object_set_new(obj, "host_symname", json_string(dev->prod_name));
	server_put_json(req, msg_obj);
	server_put_end(req, HTTP_STATUS_OK);
	return;
error:
	server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
}

/*
 * Get Wi-Fi status info.  Forward request to cond, if available,
 * and append device info onto the returned JSON structure.
 * This is required by the mobile app, even if there is no Wi-Fi.
 */
static void serv_wifi_status_get(struct server_req *req)
{
	struct msg_client_state *wifi;
	enum amsg_err err;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	err = msg_send_json(wifi->endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_STATUS_REQ, NULL,
	    serv_wifi_status_handler, req, MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
}

/*
 * Generic response handler for Wi-Fi request forwarded to cond.  Handles
 * standard amsg errors, as well as a custom "wifi_error" providing more
 * information about a failure.
 */
static void serv_wifi_json_resp_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, json_t *msg_obj, void *arg)
{
	struct server_req *req = (struct server_req *)arg;
	unsigned wifi_err;
	json_t *err_obj;

	ASSERT(req != NULL);

	if (err != AMSG_ERR_NONE) {
		server_put_end(req, serv_wifi_get_http_err_status(err));
		return;
	}
	if (!msg_obj) {
		server_put_end(req, HTTP_STATUS_NO_CONTENT);
		return;
	}
	/* Check for Wi-Fi error response message */
	if (!json_get_uint(msg_obj, "wifi_error", &wifi_err)) {
		/*
		 * On certain errors, wifi_connect.json is expected
		 * to return a 400 status and a JSON object with a
		 * brief error message.
		 */
		if (!strcmp(req->url, "wifi_connect.json")) {
			err_obj = json_object();
			json_object_set_new(err_obj, "error",
			    json_integer(wifi_err));
			json_object_set_new(err_obj, "msg",
			    json_string(wifi_errors[wifi_err]));

			server_put_json(req, err_obj);
			server_put_end(req, HTTP_STATUS_BAD_REQUEST);
			json_decref(err_obj);
			return;
		}
		/*
		 * Handle special case where a "wifi_error" NOT_FOUND is
		 * returned to indicate the request was valid, but
		 * the resource was not found.
		 */
		if (wifi_err == WIFI_ERR_NOT_FOUND) {
			server_put_end(req, HTTP_STATUS_NOT_FOUND);
			return;
		}
		/* For all other wifi_errors, just return a 500 */
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		return;
	}
	/* Normal case: reply with response message content */
	server_put_json(req, msg_obj);
	server_put_end(req, HTTP_STATUS_OK);
}

/*
 * Timeout handler for delayed message sends.
 */
static void serv_msg_send_delayed_timeout(struct timer *timer)
{
	struct serv_msg_send_delayed_state *state =
	    CONTAINER_OF(struct serv_msg_send_delayed_state, send_timer, timer);
	struct msg_client_state *client;

	client = msg_server_lookup_client(state->app_name);
	if (!client) {
		log_warn("%s is no longer connected", state->app_name);
		goto cleanup;
	}
	msg_send_json(client->endpoint, state->interface, state->type,
	    state->json, state->resp_handler, state->resp_arg,
	    state->timeout_ms);
cleanup:
	free(state->app_name);
	json_decref(state->json);
	free(state);
}

static int serv_msg_send_delayed(const char *app_name, u32 send_delay_ms,
	uint8_t interface, uint8_t type, json_t *json,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	json_t *, void *), void *resp_arg, uint32_t timeout_ms)
{
	struct serv_msg_send_delayed_state *state;

	ASSERT(send_delay_ms > 0);

	state = (struct serv_msg_send_delayed_state *)malloc(sizeof(*state));
	if (!state) {
		log_err("malloc failed");
		return -1;
	}
	state->app_name = strdup(app_name);
	state->interface = interface;
	state->type = type;
	state->json = json_incref(json);
	state->resp_handler = resp_handler;
	state->resp_arg = resp_arg;
	state->timeout_ms = timeout_ms;
	timer_init(&state->send_timer, serv_msg_send_delayed_timeout);
	timer_set(&device.timers, &state->send_timer, send_delay_ms);
	return 0;
}

/*
 * Handle a server request for Wi-Fi profiles.
 */
static void serv_wifi_profiles(struct server_req *req)
{
	struct msg_client_state *wifi;
	enum amsg_err err;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	err = msg_send_json(wifi->endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_PROFILE_LIST_REQ, NULL,
	    serv_wifi_json_resp_handler, req, MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
}

/*
 * Handle a server request to delete a Wi-Fi profile.
 */
static void serv_wifi_profile_delete(struct server_req *req)
{
	struct msg_client_state *wifi;
	json_t *msg;
	json_t *ssid_obj;
	char ssid[WIFI_SSID_LEN + 1];	/* get_arg_by_name() null-terminates */
	ssize_t ssid_len;
	enum amsg_err err;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	ssid_len = server_get_arg_by_name(req, "ssid",
	    ssid, sizeof(ssid));
	if (ssid_len < 0) {
		log_warn("ssid argument missing or invalid");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	ssid_obj = serv_uri_encoded_json_string(ssid, ssid_len);
	if (!ssid_obj) {
		log_warn("SSID encode failed");
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		return;
	}
	msg = json_object();
	json_object_set_new(msg, "ssid", ssid_obj);
	err = msg_send_json(wifi->endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_PROFILE_DELETE, msg,
	    serv_wifi_json_resp_handler, req, MSG_TIMEOUT_DEFAULT_MS);
	json_decref(msg);
	if (err != AMSG_ERR_NONE) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
}

/*
 * Handle a server request for Wi-Fi scan results.
 */
static void serv_wifi_scan_results(struct server_req *req)
{
	struct msg_client_state *wifi;
	enum amsg_err err;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	err = msg_send_json(wifi->endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SCAN_RESULTS_REQ, NULL,
	    serv_wifi_json_resp_handler, req, MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
}

/*
 * Handle a server request to initiate a Wi-FI scan.
 */
static void serv_wifi_scan(struct server_req *req)
{
	struct msg_client_state *wifi;
	json_t *msg = NULL;
	json_t *ssid_obj;
	u8 ssid[WIFI_SSID_LEN];
	ssize_t ssid_len;
	enum amsg_err err;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	/* Optional SSID */
	ssid_len = server_get_arg_by_name(req, "ssid",
	    (char *)ssid, sizeof(ssid));
	if (ssid_len > 0) {
		ssid_obj = serv_uri_encoded_json_string(ssid, ssid_len);
		if (!ssid_obj) {
			log_warn("SSID encode failed");
			server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
			return;
		}
		msg = json_object();
		json_object_set_new(msg, "ssid", ssid_obj);
	}
	err = msg_send_json(wifi->endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SCAN_START, msg,
	    serv_wifi_json_resp_handler, req, MSG_TIMEOUT_DEFAULT_MS);
	json_decref(msg);
	if (err != AMSG_ERR_NONE) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
}

static void serv_wifi_connect_test_resp_handler(
	struct amsg_endpoint *endpoint, enum amsg_err err,
	json_t *msg_obj, void *arg)
{
	struct serv_wifi_connect_delayed_state *state =
	    (struct serv_wifi_connect_delayed_state *)arg;

	ASSERT(state != NULL);

	/* Send HTTP response */
	serv_wifi_json_resp_handler(endpoint, err, msg_obj, state->req);
	/*
	 * If test connect failed or an error response was returned,
	 * do not send the actual connect message.
	 */
	if (err != AMSG_ERR_NONE || msg_obj) {
		goto cleanup;
	}
	/* Remove the test flag from the connect message */
	if (json_object_del(state->msg, "test") < 0) {
		log_err("test wifi_connect handler called for "
		    "non-test");
		goto cleanup;
	}
	/*
	 * Send the connect message with a brief delay, to ensure the
	 * server response is completely sent prior to terminating
	 * the current network connection.
	 */
	serv_msg_send_delayed(MSG_APP_NAME_WIFI, SERV_RESP_GRACE_PERIOD_MS,
	    MSG_INTERFACE_WIFI, MSG_WIFI_CONNECT,
	    state->msg, NULL, NULL, 0);
cleanup:
	json_decref(state->msg);
	free(state);
}

/*
 * Handle a server request to connect to a Wi-Fi network.
 */
static void serv_wifi_connect(struct server_req *req)
{
	struct device_state *dev = &device;
	struct msg_client_state *wifi;
	json_t *msg;
	json_t *obj;
	char *arg;
	char *cp;
	const char *str;
	size_t len;
	size_t i;
	bool simultaneous_ap_sta = false;
	struct serv_wifi_connect_delayed_state *state;
	enum amsg_err err;
	char *msg_dbg;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}

	/* Check for "ap-sta" in feature list */
	json_array_foreach(dev->wifi_features, i, obj) {
		str = json_string_value(obj);
		if (str && !strcmp(str, "ap-sta")) {
			simultaneous_ap_sta = true;
			break;
		}
	}

	msg = json_object();
	/* Add all URL arguments to message */
	while ((arg = server_get_arg_len(req, &cp, &len)) != NULL) {
		obj = NULL;
		if (!strcmp(arg, "ssid") || !strcmp(arg, "key")) {
			/*
			 * SSIDs and keys are URI encoded to handle
			 * non-UTF characters.
			 */
			obj = serv_uri_encoded_json_string(cp, len);
			log_debug("d1 cp=%s, len=%d, obj=%p", cp, len, obj);
		} else if (!strcmp(arg, "hidden")) {
			if (!strcmp(cp, "1") || !strcmp(cp, "true")) {
				obj = json_true();
			} else {
				obj = json_false();
			}
		} else if (!strcmp(arg, "location")) {
			/* Update devd's setup location field */
			ds_update_location(cp);
		} else if (!strcmp(arg, "setup_token")) {
			/* Update devd's setup token field */
			ds_update_setup_token(cp);
		}
		if (!obj) {
			/*
			 * No special formatting required for arg,
			 * so use JSON string.
			 */
			obj = json_stringn(cp, len);
			log_debug("d2 cp=%s, len=%d, obj=%p", cp, len, obj);
		}
		json_object_set_new(msg, arg, obj);
	}

	msg_dbg = json_dumps(msg, JSON_COMPACT);
	log_debug("msg=%s", msg_dbg);
	free(msg_dbg);

	if (simultaneous_ap_sta) {
		/* Normal wifi_connect */
		err = msg_send_json(wifi->endpoint,
		    MSG_INTERFACE_WIFI, MSG_WIFI_CONNECT, msg,
		    serv_wifi_json_resp_handler, req, MSG_TIMEOUT_DEFAULT_MS);
		if (err != AMSG_ERR_NONE) {
			server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
		}
	} else {
		/*
		 * Network connectivity will drop immediately on connect if
		 * AP-STA mode is not available.
		 * To ensure HTTP response is returned, do test connect first,
		 * send HTTP response, then connect after 500 ms delay.
		 * The delay is a necessary workaround to ensure all response
		 * data is returned to the HTTP client, and the TCP/IP
		 * connection is broken down completely prior to telling cond
		 * to connect, which will disable the AP.
		 */
		json_object_set_new(msg, "test", json_true());

		state = (struct serv_wifi_connect_delayed_state *)calloc(1,
		    sizeof(*state));
		if (!state) {
			log_err("malloc failed");
			err = AMSG_ERR_MEM;
			goto error;
		}
		state->req = req;
		state->msg = json_incref(msg);
		err = msg_send_json(wifi->endpoint,
		    MSG_INTERFACE_WIFI, MSG_WIFI_CONNECT, msg,
		    serv_wifi_connect_test_resp_handler, state,
		    MSG_TIMEOUT_DEFAULT_MS);
		if (err != AMSG_ERR_NONE) {
			/* Sends HTTP resp and cleans up state */
			serv_wifi_connect_test_resp_handler(wifi->endpoint, err,
			    NULL, state);
		}
	}
error:
	json_decref(msg);
	return;
}

/*
 * Handle a server request to start WPS.
 */
static void serv_wifi_wps_start(struct server_req *req)
{
	struct msg_client_state *wifi;
	enum amsg_err err;

	wifi = msg_server_lookup_client(MSG_APP_NAME_WIFI);
	if (!wifi) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	err = msg_send_json(wifi->endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_WPS_PBC, NULL,
	    serv_wifi_json_resp_handler, req, MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		server_put_end(req, HTTP_STATUS_INTERNAL_ERR);
	}
}

/*
 * Handle a server request to stop AP mode.
 */
static void serv_wifi_ap_stop(struct server_req *req)
{
	if (!msg_server_lookup_client(MSG_APP_NAME_WIFI)) {
		log_warn("cond not connected");
		server_put_end(req, HTTP_STATUS_NOT_FOUND);
		return;
	}
	/* Respond to client immediately, before it is disconnected */
	server_put_end(req, HTTP_STATUS_NO_CONTENT);

	serv_msg_send_delayed(MSG_APP_NAME_WIFI, SERV_RESP_GRACE_PERIOD_MS,
	    MSG_INTERFACE_WIFI, MSG_WIFI_AP_STOP, NULL, NULL, NULL, 0);
}

static void serv_regtoken_json_get(struct server_req *req)
{
	struct device_state *dev = &device;
	json_t *root;

	if (!dev->template_assoc) {
		log_info("template association not done");
		server_put_end(req, HTTP_STATUS_PRE_FAIL);
		return;
	}

	root = json_object();
	if (dev->regtoken) {
		json_object_set_new(root, "regtoken",
		    json_string(dev->regtoken));
	} else {
		/* get regtoken from cloud */
		ds_get_regtoken_from_cloud();
	}
	json_object_set_new(root, "registered", json_integer(dev->registered));
	json_object_set_new(root, "registration_type",
	    json_string(dev->reg_type));
	json_object_set_new(root, "host_symname", json_string(dev->prod_name));
	server_put_json(req, root);
	json_decref(root);
	server_put_end(req, HTTP_STATUS_OK);
}

/*
 * Handle incoming reverse-REST command.
 */
void serv_json_cmd(json_t *cmd, struct client_lan_reg *lan)
{
	struct serv_rev_req *rev_req;
	const char *method;
	const char *data_str;

	rev_req = serv_rev_lan_req_alloc(lan);
	if (!rev_req) {
		log_err("req malloc err");
		return;
	}

	rev_req->req->url = json_get_string_dup(cmd, "resource");
	if (!rev_req->req->url) {
		log_warn("missing resource");
		goto invalid;
	}
	if (json_get_int(cmd, "id", &rev_req->cmd_id) < 0 &&
	    json_get_int(cmd, "cmd_id", &rev_req->cmd_id) < 0) {
		log_warn("missing cmd_id");
		goto invalid;
	}

	rev_req->resp_uri = json_get_string_dup(cmd, "uri");
	if (!rev_req->resp_uri) {
		log_warn("missing uri");
		goto invalid;
	}

	method = json_get_string(cmd, "method");

	data_str = json_get_string(cmd, "data");
	if (data_str && (data_str[0] != '\0') && strcmp(data_str, "none")) {
		rev_req->req->body_is_json = true;
		queue_buf_put(&rev_req->req->request,
		    data_str, strlen(data_str));
		rev_req->req->body_json =
		    queue_buf_parse_json(&rev_req->req->request, 0);
	}
	server_handle_req(rev_req->req, method);
	return;
invalid:
	server_put_end(rev_req->req, HTTP_STATUS_BAD_REQUEST);
}

/*
 * Server request filter allowing only requests from ADS.
 */
static bool serv_ads_only(struct server_req *req)
{
	return serv_rev_get_source(req) == SOURCE_ADS;
}

/*
 * Server request filter allowing only requests from LAN clients (mobile app).
 */
static bool serv_lan_only(struct server_req *req)
{
	return SOURCE_TO_DEST_MASK(serv_rev_get_source(req)) & DEST_LAN_APPS;
}

/*
 * Server request filter allowing only local requests when in Wi-Fi AP mode.
 */
static bool serv_ap_only(struct server_req *req)
{
	return device.wifi_ap_enabled &&
	    serv_rev_get_source(req) == SOURCE_LOCAL;
}

/*
 * Server request filter allowing only requests from LAN clients and the local
 * web server.
 */
static bool serv_local_lan(struct server_req *req)
{
	return serv_rev_get_source(req) != SOURCE_ADS;
}

/*
 * Server request filter allowing only requests from ADS and LAN clients.
 */
static bool serv_ads_lan(struct server_req *req)
{
	return serv_rev_get_source(req) != SOURCE_LOCAL;
}

/*
 * Server request filter allowing requests from ADS, LAN clients, and
 * if Wi-Fi AP mode is enabled, the local web server.
 */
static bool serv_ads_lan_ap(struct server_req *req)
{
	return device.wifi_ap_enabled || serv_ads_lan(req);
}


static const struct server_url_list serv_url_table[] = {
	{ SM_POST, "local_reg.json", client_lan_reg_post, serv_local_lan },
	{ SM_PUT, "local_reg.json", client_lan_reg_put, serv_local_lan },
	{ SM_POST, "push_button_reg.json", serv_push_button_reg_post },
	{ SM_PUT, "lanip.json", serv_lanip_put, serv_ads_only },
	{ SM_GET, "config.json", serv_conf_get, serv_ads_only },
	{ SM_PUT, "config.json", serv_conf_put, serv_ads_only },
	{ SM_PUT, "logclient.json", serv_conf_log_client_put, serv_ads_only },
	{ SM_PUT, "log_mods.json", serv_conf_log_mods_put, serv_ads_lan },
	{ SM_PUT, "getdsns.json", serv_getdsns_put, serv_ads_only },
	{ SM_PUT, "reset.json", serv_reset_put, serv_ads_lan },
	{ SM_PUT, "registration.json", ds_registration_put, serv_ads_only },
	{ SM_GET, "status.json", serv_status_get, serv_ads_lan_ap },
	{ SM_GET, "time.json", serv_time_get, serv_ads_lan_ap },
	{ SM_PUT, "time.json", serv_time_put, serv_ads_lan_ap },
	{ SM_PUT, "ota.json", serv_ota_put, serv_ads_only },
	{ SM_PUT, "lanota.json", serv_lan_ota_put, serv_local_lan },
	{ SM_GET, "property.json", prop_json_get, serv_lan_only },
	{ SM_GET, "regtoken.json", serv_regtoken_json_get, serv_local_lan },
	{ SM_GET, "wifi_status.json", serv_wifi_status_get, serv_ads_lan_ap },
	{ SM_GET, "wifi_profiles.json", serv_wifi_profiles, serv_ads_lan_ap },
	{ SM_DELETE, "wifi_profile.json", serv_wifi_profile_delete,
	    serv_ads_lan_ap},
	{ SM_GET, "wifi_scan_results.json", serv_wifi_scan_results,
	    serv_ads_lan_ap },
	{ SM_POST, "wifi_scan.json", serv_wifi_scan, serv_ads_lan_ap },
	{ SM_POST, "wifi_connect.json", serv_wifi_connect, serv_ads_lan_ap },
	{ SM_POST, "wps_pbc.json", serv_wifi_wps_start, serv_ap_only },
	{ SM_PUT, "wifi_stop_ap.json", serv_wifi_ap_stop, serv_ads_lan_ap },
	{ SM_GET, "conn_status.json", gateway_conn_status_get, serv_lan_only },
	{ SM_GET, "node_property.json", gateway_node_property_get,
	    serv_lan_only },
	{ SM_PUT, "reset_node.json", gateway_reset_node_put, serv_ads_lan },
	{ SM_PUT, "node_ota.json", gateway_node_ota_put, serv_ads_only },
	{ 0, NULL, NULL }
};

void serv_init(void)
{
	struct device_state *dev = &device;
	/*
	 * Set full permissions so web server user can execute CGI scripts
	 * that interact with the socket.
	 */
	if (server_init_local(&dev->file_events, serv_url_table,
	    devd_sock_path, S_IRWXU | S_IRWXG | S_IRWXO)) {
		exit(3);
	}
}
