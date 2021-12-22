/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_OPS_DEVD_H__
#define __AYLA_OPS_DEVD_H__

#define ADS_API_VERSION		"dev/v1"

struct ops_buf_info {
	struct ds_client_data req_data;	/* Request data info */
	const char *resp_file_path;	/* Set to write response to file */
	bool non_ayla;	/* Set to 1 if curl command shouldn't include auth */
	u8 init:1;	/* Set to 1 if this block has been initialized */
};

struct ops_devd_cmd;
struct op_funcs {
	const char * const *op_name;
	int (*init)(enum http_method *method, char *link,
	    int link_size, struct ops_buf_info *info,
	    struct ops_devd_cmd *op_cmd, struct device_state *dev);
	int (*ads_success)(struct ops_devd_cmd *op_cmd,
	    struct device_state *dev);
	int (*finished)(struct ops_devd_cmd *op_cmd, struct device_state *dev);
};

struct ops_devd_cmd {
	STAILQ_ENTRY(ops_devd_cmd) link;
	const struct op_funcs *op_handlers;
	const char *err_type;		/* err type if err occurs */
	const char *proto;
	void *arg;
	json_t *info_j;
	char *err_name;			/* string to send to appd on failures */
	int req_id;
	int source;
	json_t *op_args;
	u8 dests_target;		/* target dest mask for prop update */
	u8 dests_failed;		/* failed destination targets */
	u8 echo:1;
	u8 confirm:1;
	u8 dests_specified:1;		/* 1 if appd specified dests */
	u8 local:1;			/* 1 if local (not from appd) */
};

/*
 * Status and response JSON of most recent request to ADS.  Needed for support
 * of legacy code in props/gateway client  TODO remove when no longer needed.
 */
extern u16 req_status;
extern json_t *req_resp;

/*
 * Process commands.
 * If a command execution has started, return and wait for it to finish.
 */
int ops_devd_process_cmds(struct device_state *dev);

/*
 * Register a new operation to be executed
 */
int ops_devd_add(struct ops_devd_cmd *op_cmd);

/*
 * Initialize the op interface
 */
void ops_devd_init(void);

/*
 * Execute the next operation in the queue.  Call this when there might be
 * pending operations that were not started, due to an error or temporary
 * connectivity loss.
 */
void ops_devd_step(void);

/*
 * Signifies that the property cmd has finished for that
 * "mask" (destination).
 */
void ops_devd_mark_results(struct ops_devd_cmd *op_cmd, bool mask, u8 success);

/*
 * Get the (ephemeral) location of an OTA on an external service (i.e. s3).
 * Pass in the ota object included in the original OTA command.
 */
int ops_devd_ota_url_fetch(json_t *ota_obj, void (*success_callback)(json_t *));

#endif /*  __AYLA_OPS_DEVD_H__ */
