/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 *
 * This code is offered as an example without any guarantee or warranty.
 */
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

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
#include "props_client.h"
#include "props_if.h"
#include "app_if.h"


enum ops_devd_state {
	ODS_NOP = 0,
	ODS_OTA_FETCH,
};

#define ODS_OP_NAMES {				\
	[ODS_NOP] = "nop",			\
	[ODS_OTA_FETCH] = "ota_url_fetch",	\
}
const char *ops_devd_ops[] = ODS_OP_NAMES;

/*
 * Operations to be executed
 */
static STAILQ_HEAD(, ops_devd_cmd) ops_devd_cmdq;
static int ops_devd_initialized;
static const struct op_funcs ops_devd_op_handlers[];
static struct timer ops_devd_step_timer;

/*
 * Global variables used by ops client code written when HTTP request status
 * was stored globally.
 */
u16 req_status;
json_t *req_resp;


static void ops_devd_pop_op_cmd(void)
{
	struct device_state *dev = &device;
	struct ops_devd_cmd *op_cmd = STAILQ_FIRST(&ops_devd_cmdq);

	if (!op_cmd) {
		return;
	}
	/* Finished processing command, so mark any remaining dests failed */
	ops_devd_mark_results(op_cmd, op_cmd->dests_target, false);
	if (!op_cmd->local) {
		/* Send command results to appd */
		if (op_cmd->dests_failed) {
			if (!op_cmd->err_type) {
				op_cmd->err_type = JINT_ERR_UNKWN;
			}
			if (op_cmd->echo) {
				app_send_echo_failure_with_args(op_cmd);
			} else {
				app_send_nak_with_args(op_cmd);
			}
		}
		if (op_cmd->confirm) {
			if (!op_cmd->dests_failed ||
			    (!op_cmd->dests_specified &&
			    !(op_cmd->dests_failed & DEST_ADS))) {
				app_send_confirm_true(op_cmd);
			} else {
				app_send_confirm_false(op_cmd,
				    op_cmd->dests_failed, op_cmd->err_type);
			}
		}
	}
	STAILQ_REMOVE_HEAD(&ops_devd_cmdq, link);
	if (op_cmd->op_handlers->finished) {
		op_cmd->op_handlers->finished(op_cmd, dev);
	}
	free(op_cmd->err_name);
	json_decref(op_cmd->op_args);
	json_decref(op_cmd->info_j);
	free(op_cmd);
	if (!STAILQ_EMPTY(&ops_devd_cmdq)) {
		ops_devd_step();
	}
}

static void ops_devd_cmd_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;
	struct ops_devd_cmd *op_cmd = (struct ops_devd_cmd *)arg;
	bool success = false;
	int rc;

	if (err != HTTP_CLIENT_ERR_NONE) {
		op_cmd->err_type = JINT_ERR_CONN_ERR;
		goto req_failed;
	}
	/* Handle specific HTTP statues */
	switch (info->http_status) {
	case HTTP_STATUS_NOT_FOUND:
		op_cmd->err_type = JINT_ERR_UNKWN_PROP;
		goto req_failed;
	case HTTP_STATUS_UNPROCESSABLE_ENTITY:
		op_cmd->err_type = JINT_ERR_BAD_VAL;
		goto req_failed;
	default:
		/* All other error statuses are considered conn errors */
		if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
			op_cmd->err_type = JINT_ERR_CONN_ERR;
			goto req_failed;
		}
		break;
	}
	if (op_cmd->op_handlers->ads_success) {
		/* Set global variables for support of old success handlers */
		req_status = info->http_status;
		if (resp_data->content == HTTP_CONTENT_JSON) {
			req_resp = ds_client_data_parse_json(resp_data);
		}
		rc = op_cmd->op_handlers->ads_success(op_cmd, dev);
		req_status = 0;
		json_decref(req_resp);
		req_resp = NULL;
		if (rc == -1) {
			/* Op has actually failed */
			op_cmd->err_type = JINT_ERR_UNKWN;
			goto req_failed;
		}
		if (rc == 1) {
			/* Op isn't complete yet */
			ops_devd_step();
			return;
		}
	}
	success = true;
req_failed:
	ops_devd_mark_results(op_cmd, DEST_ADS, success);
	ops_devd_pop_op_cmd();
}

/*
 * Execute the next operation on the queue
 */
static int ops_execute_cmd(struct ds_client *client,
	struct ops_devd_cmd *op_cmd)
{
	struct device_state *dev = &device;
	char link[DS_CLIENT_LINK_MAX_LEN] = { 0 };
	struct ds_client_req_info info;
	struct ops_buf_info buf_info;
	enum http_method method;
	int rc;

	if ((op_cmd->dests_target & DEST_ADS)) {
		if (!(dev->dests_avail & DEST_ADS)) {
			/* ADS is not available */
			op_cmd->err_type = JINT_ERR_CONN_ERR;
		} else if (dev->cloud_state != DS_CLOUD_UP) {
			/* ADS is temporarily unavailable */
			return 0;
		}
	}
	ops_devd_mark_results(op_cmd, ~dev->dests_avail & op_cmd->dests_target,
	    false);
	if (!op_cmd->dests_target) {
		/* no destinations to send to */
		rc = 1;
		goto ops_done;
	}
	memset(&buf_info, 0, sizeof(buf_info));
	rc = op_cmd->op_handlers->init(&method, link, sizeof(link), &buf_info,
	    op_cmd, dev);
	if (rc) {
		goto ops_done;
	}
	/* Setup new ds_client_req_info structure */
	memset(&info, 0, sizeof(info));
	info.method = method;
	info.raw_url = link;
	if (!buf_info.init) {
		goto send;
	}
	info.non_ayla = buf_info.non_ayla;
	switch (method) {
	case HTTP_GET:
		info.resp_file_path = buf_info.resp_file_path;
		break;
	case HTTP_PUT:
	case HTTP_POST:
		info.req_data = buf_info.req_data;
		break;
	default:
		break;
	}
send:
	if (ds_send(client, &info, ops_devd_cmd_done, op_cmd) < 0) {
		/* ADS send is last, so pop the command on send failure */
		rc = -1;
		goto ops_done;
	}
	return 0;
ops_done:
	ops_devd_pop_op_cmd();
	return rc;
}

/*
 * Process commands, sending all requests on the HTTP client dedicated to
 * application requests.  If a command execution has started, return and wait
 * for it to finish.
 */
static void ops_devd_step_timeout(struct timer *timer)
{
	struct device_state *dev = &device;
	struct ops_devd_cmd *cmd;

	if (ds_client_busy(&dev->app_client)) {
		return;
	}
	while ((cmd = STAILQ_FIRST(&ops_devd_cmdq)) != NULL) {
		if (ops_execute_cmd(&dev->app_client, cmd) == 0) {
			/* Waiting for execution to complete */
			return;
		}
	}
}

/*
 * Initialize the op interface
 */
void ops_devd_init(void)
{
	timer_init(&ops_devd_step_timer, ops_devd_step_timeout);
	STAILQ_INIT(&ops_devd_cmdq);
	ops_devd_initialized = 1;
}

/*
 * Execute the next operation in the queue.  Call this when there might be
 * pending operations that were not started, due to an error or temporary
 * connectivity loss.
 */
void ops_devd_step(void)
{
	struct device_state *dev = &device;

	if (!timer_active(&ops_devd_step_timer)) {
		timer_set(&dev->timers, &ops_devd_step_timer, 0);
	}
}

/*
 * Signifies that the property cmd has finished for that
 * "mask" (destination).
 */
void ops_devd_mark_results(struct ops_devd_cmd *op_cmd, u8 mask, bool success)
{
	if (!success) {
		op_cmd->dests_failed |= (mask & op_cmd->dests_target);
	}
	op_cmd->dests_target &= ~mask;
}

/*
 * Register a new operation to be executed
 */
int ops_devd_add(struct ops_devd_cmd *op_cmd)
{
	ASSERT(op_cmd->op_handlers != NULL);
	ASSERT(op_cmd->op_handlers->init != NULL);

	if (!ops_devd_initialized) {
		return -1;
	}
	if (STAILQ_EMPTY(&ops_devd_cmdq)) {
		ops_devd_step();
	}
	STAILQ_INSERT_TAIL(&ops_devd_cmdq, op_cmd, link);
	return 0;
}

/*
 * Get the (s3) location of an OTA image.
 */
static int ops_devd_ota_url_fetch_init(enum http_method *method, char *link,
	int link_size, struct ops_buf_info *info,
	struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	const char *url;

	url = json_get_string((json_t *)op_cmd->info_j, "url");
	if (!url || *url == '\0') {
		log_warn("missing OTA info URL");
		return -1;
	}
	snprintf(link, link_size, "%s", url);
	*method = HTTP_GET;

	return 0;
}

static int ops_devd_ota_url_fetch_success(struct ops_devd_cmd *op_cmd,
	struct device_state *dev)
{
	void (*callback)(json_t *) = (void (*)(json_t *))op_cmd->arg;
	json_t *ota_obj;

	if (!callback) {
		return 0;
	}
	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ota_obj = json_object_get(req_resp, "ota");
	if (!ota_obj) {
		log_warn("no ota object");
		return -1;
	}
	callback(ota_obj);
	return 0;
}


/*
 * Get the (ephemeral) location of an OTA on an external service (i.e. s3).
 * Pass in the ota object included in the original OTA command.
 */
int ops_devd_ota_url_fetch(json_t *ota_obj, void (*success_callback)(json_t *))
{
	struct ops_devd_cmd *op_cmd;

	op_cmd = calloc(1, sizeof(*op_cmd));
	if (!op_cmd) {
		log_err("malloc failed");
		return -1;
	}
	op_cmd->op_handlers = &ops_devd_op_handlers[ODS_OTA_FETCH];
	op_cmd->local = 1;
	op_cmd->dests_target = DEST_ADS;
	op_cmd->info_j = json_incref(ota_obj);
	op_cmd->arg = success_callback;

	return ops_devd_add(op_cmd);
}

static const struct op_funcs ops_devd_op_handlers[] = {
	[ODS_OTA_FETCH] = {&ops_devd_ops[ODS_OTA_FETCH],
	    ops_devd_ota_url_fetch_init, ops_devd_ota_url_fetch_success},
};
