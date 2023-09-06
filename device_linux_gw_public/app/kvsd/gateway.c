/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

/*
 * Generic Gateway Demo
 *
 * This gateway application hooks into a generic node management interface
 * and a node simulator to demonstrate good practices when creating a
 * generic gateway.  The node simulator creates simulated node instances
 * that behave in a similar fashion to a real node.  The node simulator
 * takes inputs and makes calls back up the stack without the need for
 * physical hardware, making it an ideal tool to demonstrate and test
 * generic gateway features.
 *
 * This demo should provide a model for how to begin implementing a gateway
 * application supporting physical nodes connected over a wireless network.
 * When integrating a wireless network stack, the node simulator would be
 * replaced by a network interface layer implementing the generic node
 * management interface and adapting it to the wireless network to be
 * supported.
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <ayla/utypes.h>
#include <ayla/http.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/build.h>
#include <ayla/gateway_interface.h>
#include <ayla/timer.h>
#include <ayla/conf_io.h>

#include <app/app.h>
#include <app/ops.h>
#include <app/props.h>
#include <app/gateway.h>
#include <signal.h>

#include "gateway.h"
#include "node.h"
#include "node_camera.h"
#include "video_stream.h"
#include "utils.h"


/* Maximum # of datapoints allowed in a batch */
#define APPD_MAX_BATCHED_DPS	64

#define CAM_NODES_MAX			4		/* Max number of camera nodes */

const char *appd_version = "gatewayd " BUILD_VERSION_LABEL;
const char *appd_template_version = "kvs_test_0.14";

/* Gateway property states */
static int add_camera_nodes;		/* Add Camer node request */
static unsigned num_nodes;		/* Current number of nodes */

/* Node property batch list */
static struct gw_node_prop_batch_list *node_batched_dps;

static struct timer update_timer;

/* Forward declarations */
static void appd_update_timeout(struct timer *timer);

static int appd_prop_ads_failure_cb(struct prop *prop, const void *val,
                                    size_t len, const struct op_options *opts);

static int kvs_streams_json(struct prop *prop, const void *val, size_t len,
                            const struct op_args *args);
static int webrtc_signaling_channels_json (struct prop *prop, const void *val, size_t len,
                                           const struct op_args *args);

static void stream_proc_kill();

/*
 * Send the appd software version.
 */
static enum err_t appd_send_version(struct prop *prop, int req_id,
	const struct op_options *opts)
{
	return prop_val_send(prop, req_id, appd_version, 0, NULL);
}

/*
 * Handler called by the generic node management layer to add the node
 * to the cloud.  This is invoked after the node has been queried and
 * and node management has populated its property tree.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked by the confirmation handler.
 */
static int appd_node_add_handler(struct node *node,
	void (*callback)(struct node *, const struct confirm_info *))
{
	struct gw_node gw_node;
	struct op_options opts = { .confirm = 1, .arg = callback };
	int rc;

	if (node_populate_gw_node(node, &gw_node) < 0) {
		log_err("failed to populate node info for %s", node->addr);
		return -1;
	}

	log_info("sending add_node for %s", node->addr);
	rc = gw_node_add(&gw_node, NULL, &opts);
	gw_node_free(&gw_node, 0);
	return rc;
}

/*
 * Handler called by the generic node management layer to remove a node
 * from the cloud.  This is called if the node left the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked by the confirmation handler.
 */
static int appd_node_remove_handler(struct node *node,
	void (*callback)(struct node *, const struct confirm_info *))
{
	struct op_options opts = { .confirm = 1, .arg = callback };

	log_info("sending remove_node for %s", node->addr);
	return gw_node_remove(node->addr, &opts);
}

/*
 * Handler called by the generic node management layer to update a node
 * in the cloud.  This is called when the node's information or property
 * tree is changed.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked by the confirmation handler.
 */
static int appd_node_update_handler(struct node *node,
	void (*callback)(struct node *, const struct confirm_info *))
{
	struct gw_node gw_node;
	struct op_options opts = { .confirm = 1, .arg = callback };
	int rc;

	if (node_populate_gw_node(node, &gw_node) < 0) {
		log_err("failed to populate node info for %s", node->addr);
		return -1;
	}
	log_info("sending update_node for %s", node->addr);
	rc = gw_node_update(&gw_node, NULL, &opts);
	gw_node_free(&gw_node, 0);
	return rc;
}

/*
 * Handler called by the generic node management layer to send the online
 * status of a node to the cloud.  It is up to the node management layer
 * to determine the online status using the method best suited to the type of
 * network the node is on.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked by the confirmation handler.
 */
static int appd_node_conn_send_handler(struct node *node,
	void (*callback)(struct node *, const struct confirm_info *))
{
	struct op_options opts = { .confirm = 1, .arg = callback };

	log_info("sending conn_status %s for %s",
	    node->online ? "ONLINE" : "OFFLINE", node->addr);
	return gw_node_conn_status_send(node->addr, node->online, &opts);
}

/*
 * Handler called by the generic node management layer to send all batched
 * datapoints.  The generic node management layer requests datapoints be
 * appended to a batch by setting the batch_append parameter of the
 * node_prop_send callback.
 */
static int appd_node_prop_batch_send(void)
{
	if (!node_batched_dps) {
		return 0;
	}
	return gw_node_prop_batch_send(&node_batched_dps, NULL, NULL);
}

/*
 * Handler called by the generic node management layer to send a
 * datapoint.  If the batch_append parameter is set, the datapoint should
 * be appended to a list to be sent later.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked by the confirmation handler.
 */
static int appd_node_prop_send_handler(const struct node *node,
	const struct node_prop *prop,
	void (*callback)(struct node *, struct node_prop *,
	const struct confirm_info *), bool batch_append)
{
	struct gw_node_prop_batch_list *batch_list;
	struct gw_node_prop gw_prop;
	struct op_options opts = { .confirm = 1, .arg = callback };

	node_populate_gw_prop(node, prop, &gw_prop);

	if (batch_append) {
		log_info("batching node property: %s::%s:%s:%s = %s",
		    node->addr, prop->subdevice->key, prop->template->key,
		    prop->name, prop_val_to_str(prop->val, prop->type));
		batch_list = gw_node_prop_batch_append(node_batched_dps,
		    &gw_prop, prop->type, prop->val, prop->val_size, &opts);
		if (!batch_list) {
			return -1;
		}
		node_batched_dps = batch_list;
		/* Immediately send batch if maximum size is reached */
		if (node_batched_dps->batchq_len >= APPD_MAX_BATCHED_DPS) {
			log_debug("maximum batch size reached for %s",
			    node->addr);
			return appd_node_prop_batch_send();
		}
		return 0;
	}
	log_info("sending node property: %s::%s:%s:%s = %s",
	    node->addr, prop->subdevice->key, prop->template->key, prop->name,
	    prop_val_to_str(prop->val, prop->type));
	return gw_node_prop_send(&gw_prop, prop->type, prop->val,
	    prop->val_size, 0, &opts);
}

/*
 * Handler called by the generic gateway API to set a node property.
 * This function validates the parameters and passes the property
 * update to the generic node management layer to process.  Optional
 * property acknowledgments are supported.
 */
static int appd_node_props_set_handler(struct gw_node_prop *prop,
	enum prop_type type, const void *val, size_t val_len,
	const struct op_args *args)
{
	struct node *node;
	struct node_prop *node_prop;
	int status = 0;
	int ack_msg = HTTP_STATUS_OK; /* Using HTTP status codes for acks */

	node = node_lookup(prop->addr);
	if (!node) {
		log_warn("no node with addr: %s", prop->addr);
		status = -1;
		ack_msg = HTTP_STATUS_NOT_FOUND;
		goto done;
	}
	node_prop = node_prop_lookup(node, prop->subdevice_key,
	    prop->template_key, prop->name);
	if (!node_prop) {
		log_warn("node %s does not support property: %s", node->addr,
		    prop->name);
		status = -1;
		ack_msg = HTTP_STATUS_NOT_FOUND;
		goto done;
	}
	if (node_prop->dir != PROP_TO_DEVICE) {
		log_err("node property %s is read-only", prop->name);
		status = -1;
		ack_msg = HTTP_STATUS_METHOD_NOT_ALLOWED;
		goto done;
	}
	if (type != node_prop->type) {
		log_err("node property %s value type mismatch", prop->name);
		status = -1;
		ack_msg = HTTP_STATUS_UNPROCESSABLE_ENTITY;
		goto done;
	}
	if (val_len > node_prop->val_size) {
		log_err("node property %s value too large: got %zuB, "
		    "expected %zuB", prop->name, val_len, node_prop->val_size);
		status = -1;
		ack_msg = HTTP_STATUS_UNPROCESSABLE_ENTITY;
		goto done;
	}
	log_info("setting node property: %s::%s:%s:%s = %s",
	    prop->addr, prop->subdevice_key, prop->template_key, prop->name,
	    prop_val_to_str(val, type));
	status = node_prop_set(node, node_prop, val, val_len);
	if (status < 0) {
		ack_msg = HTTP_STATUS_INTERNAL_ERR;
	}
done:
	/* Send property ack if requested */
	if (args && args->ack_arg) {
		ops_prop_ack_send(args->ack_arg, status, ack_msg);
	}
	return status;
}

/*
 * Handler called by the generic gateway API to get the value of a node
 * property. This function returns the latest value of the property cached
 * by the generic node management layer.
 */
static int appd_node_prop_get_handler(struct gw_node_prop *prop, int req_id,
	const char *arg)
{
	struct node *node;
	struct node_prop *node_prop;

	node = node_lookup(prop->addr);
	if (!node) {
		log_warn("no node with addr: %s", prop->addr);
		return -1;
	}
	node_prop = node_prop_lookup(node, prop->subdevice_key,
	    prop->template_key, prop->name);
	if (!node_prop) {
		log_warn("node %s does not support property: %s", node->addr,
		    prop->name);
		return -1;
	}
	log_info("sending node property: %s::%s:%s:%s = %s",
	    prop->addr, prop->subdevice_key, prop->template_key, prop->name,
	    prop_val_to_str(node_prop->val, node_prop->type));
	return gw_node_prop_send(prop, node_prop->type, node_prop->val,
	    node_prop->val_size, req_id, NULL);
}

/*
 * Handler called by the generic gateway API to get the online status of a node.
 * This function returns the latest node status cached by the generic
 * node management layer.
 */
static int appd_node_conn_get_handler(const char *addr)
{
	struct node *node;

	node = node_lookup(addr);
	if (!node) {
		log_warn("no node with addr: %s", addr);
		return -1;
	}
	log_info("reporting node connection state: addr=%s status=%s",
	    addr, node->online ? "ONLINE" : "OFFLINE");
	return node->online ? 1 : 0;
}

/*
 * Handler called by the generic gateway API to factory reset a node.
 * This function expects the following actions:
 * 1. Reset the node's state to defaults.
 * 2. Remove the node from the network.
 * 3. Call gw_node_rst_cb to indicate the result.
 * If the operation reported success, the node will be removed from the cloud.
 */
static void appd_gw_node_reset_handler(const char *addr, void *cookie)
{
	struct node *node = node_lookup(addr);

	if (!node) {
		log_warn("no node with addr: %s", addr);
		/*
		 * Setting success flag even if node is not found, so library
		 * cleans up any node state.
		 */
		gw_node_rst_cb(addr, cookie, 1, 404);
		return;
	}
	log_info("factory reset node %s", addr);
	/*
	 * Ayla node factory reset operation both clears the node state and
	 * removes the node from the gateway.
	 */
	node_factory_reset(node);
	node_remove(node);
	gw_node_rst_cb(addr, cookie, 1, 0);
}

/*
 * Handler called by the generic gateway API to confirm a pending node OTA.
 * For this demo, the OTA is rejected if it matches the current node
 * version (if there is one), otherwise it is downloaded.
 */
static void appd_gw_node_ota_handler(const char *addr, const char *ver,
				void *cookie)
{
	char ota_path[PATH_MAX];
	struct node *node;

	node = node_lookup(addr);
	if (!node) {
		log_warn("no node with addr: %s", addr);
		gw_node_ota_cb(addr, cookie, NULL, NULL);
		return;
	}
	if (node->version[0] && !strcmp(node->version, ver)) {
		log_warn("rejecting OTA: same as existing version %s", ver);
		gw_node_ota_cb(addr, cookie, NULL, NULL);
		return;
	}
	/* Create unique file in RAM for this node */
	snprintf(ota_path, sizeof(ota_path), "/tmp/%s_ota.img", node->addr);
	log_info("received node OTA update: version %s for node %s", ver, addr);
	gw_node_ota_cb(addr, cookie, ota_path, NULL);
}

/*
 * Handler called by the generic gateway API to handle the node
 * register status change. This function expects the following actions:
 * 1. Send the node's register status to node.
 * 2. Call gw_node_rst_cb to indicate the result.
 */
static void appd_gw_node_reg_handler(const char *addr, bool stat,
				void *cookie)
{
	struct node *node = node_lookup(addr);

	if (!node) {
		log_warn("no node with addr: %s", addr);
		/*
		 * Setting success flag even if node is not found, so library
		 * cleans up any node state.
		 */
		gw_node_reg_cb(addr, cookie, 1, 404);
		return;
	}

	log_info("node %s register status change to %d", addr, stat);

	if (stat) {
		/*
		 * Send the register status change event to node,
		 * node sends all properties to cloud if node is registered,
		 * but because all nodes were from market for this demo,
		 * just send node all from-device properties to cloud in here.
		 */
		 node_prop_send_all_set(node, 1);
	}

	/* Call the gw_node_reg_cb callback function to send the result */
	gw_node_reg_cb(addr, cookie, 1, 0);
}

/*
 * The node operation confirm handler is called to indicate the success
 * or failure of any operation with the op_options.confirm flag set.
 * In this demo gateway app, the generic node management code requires
 * a callback for every cloud operation it initiates.
 */
int appd_node_ops_confirm_handler(enum ayla_gateway_op op,
    enum gw_confirm_arg_type type, const void *arg,
    const struct op_options *opts, const struct confirm_info *confirm_info)
{
	struct gw_node_prop_dp *node_dp;
	struct gw_node_ota_info *node_ota = NULL;
	struct node *node = NULL;
	struct node_prop *prop = NULL;
	static void (*node_callback)(struct node *,
	    const struct confirm_info *) = NULL;
	static void (*node_prop_callback)(struct node *, struct node_prop *,
	    const struct confirm_info *) = NULL;

	ASSERT(arg != NULL);

	/* Lookup relevant info about the confirmation */
	switch (type) {
	case CAT_NODEPROP_DP:
		node_dp = (struct gw_node_prop_dp *)arg;
		node = node_lookup(node_dp->prop->addr);
		if (!node) {
			log_warn("non-existent node: %s", node_dp->prop->addr);
			return -1;
		}
		prop = node_prop_lookup(node, node_dp->prop->subdevice_key,
		    node_dp->prop->template_key, node_dp->prop->name);
		if (!prop) {
			log_warn("non-existent property: %s::%s:%s:%s",
			    node->addr, node_dp->prop->subdevice_key,
			    node_dp->prop->template_key, node_dp->prop->name);
			return -1;
		}
		node_prop_callback = opts ? opts->arg : NULL;
		break;
	case CAT_ADDR:
		node = node_lookup((const char *)arg);
		if (!node) {
			log_warn("non-existent node: %s", (const char *)arg);
			return -1;
		}
		node_callback = opts ? opts->arg : NULL;
		break;
	case CAT_NODE_OTA_INFO:
		node_ota = (struct gw_node_ota_info *)arg;
		node = node_lookup(node_ota->addr);
		if (!node) {
			log_warn("non-existent node: %s", node_ota->addr);
			return -1;
		}
		node_callback = opts ? opts->arg : NULL;
		break;
	default:
		goto not_supported;
	}
	/* Apply confirmation action */
	switch (op) {
	case AG_NODE_ADD:
		ASSERT(node != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			log_info("node %s added to cloud successfully",
			    node->addr);
		} else {
			log_warn("node %s add failed: err %u", node->addr,
			    confirm_info->err);
		}
		if (node_callback) {
			node_callback(node, confirm_info);
		}
		break;
	case AG_NODE_UPDATE:
		ASSERT(node != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			log_info("node %s updated in cloud successfully",
			    node->addr);
		} else {
			log_warn("node %s update failed: err %u",
			    node->addr, confirm_info->err);
		}
		if (node_callback) {
			node_callback(node, confirm_info);
		}
		break;
	case AG_NODE_REMOVE:
		ASSERT(node != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			log_info("node %s removed from cloud successfully",
			    node->addr);
		} else {
			log_warn("node %s remove failed: err %u",
			    node->addr, confirm_info->err);
		}
		if (node_callback) {
			node_callback(node, confirm_info);
		}
		break;
	case AG_CONN_STATUS:
		ASSERT(node != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			log_info("node %s connection status sent successfully",
			    node->addr);
		} else {
			log_warn("node %s connection status send failed: "
			    "err %u", node->addr, confirm_info->err);
		}
		if (node_callback) {
			node_callback(node, confirm_info);
		}
		break;
	case AG_PROP_SEND:
		ASSERT(node != NULL);
		ASSERT(prop != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			log_info("node prop %s::%s:%s:%s sent "
			    "successfully to dests %02X",
			    node->addr, prop->subdevice->key,
			    prop->template->key, prop->name,
			    confirm_info->dests);
		} else {
			log_warn("node prop %s::%s:%s:%s send "
			    "failed to dests %02X: err %u",
			    node->addr, prop->subdevice->key,
			    prop->template->key, prop->name,
			    confirm_info->dests, confirm_info->err);
		}
		if (node_prop_callback) {
			node_prop_callback(node, prop, confirm_info);
		}
		break;
	case AG_NODE_OTA_RESULT:
		ASSERT(node_ota != NULL);
		ASSERT(node != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			if (node_ota->save_location) {
				log_info("node %s downloaded OTA version %s "
				    "successfully to location %s", node->addr,
				    node_ota->version, node_ota->save_location);
				/*
				 * Tell generic node management to apply
				 * the node OTA.
				 */
				node_ota_apply(node, node_ota->version,
				    node_ota->save_location);
			} else {
				log_info("node %s discarded OTA version %s",
				    node->addr, node_ota->version);
			}
		} else {
			if (node_ota->save_location) {
				log_warn("node %s failed to downloaded OTA "
				    "version %s to location %s: err %u",
				    node->addr, node_ota->version,
				    node_ota->save_location, confirm_info->err);
			} else {
				log_warn("node %s failed to discard OTA "
				    "version %s: err %u", node->addr,
				    node_ota->version, confirm_info->err);
			}
		}
		break;
	default:
		goto not_supported;
	}
	return 0;
not_supported:
	log_warn("confirm handler not supported for %s",
	    gateway_ops[op]);
	return -1;
}

/*
 * Helper for setting add_*_nodes properties.  This tells the node
 * simulator to simulate a number of nodes joining or leaving the network.
 * Once the operation has been completed successfully, the updated property
 * value is echoed back.
 */
static int appd_gw_add_nodes_set_helper(struct prop *prop,
	const void *val, size_t len, const struct op_args *args,
	enum camera_node_type type, u32 sample_secs)
{
	int *num_nodes = (int *)prop->arg;
	size_t curr_node_count = node_count();
	if((*num_nodes) + curr_node_count >= CAM_NODES_MAX) {
		log_err("Cannot add %d nodes, max node count is %d",
			(*num_nodes), CAM_NODES_MAX);
		return -1;
	}

	int status = 0;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		status = -1;
		goto done;
	}
	if (!*num_nodes) {
		goto done;
	}
	while (*num_nodes != 0) {
		if (*num_nodes > 0) {
			/* Tell the node simulator to simulate a node joining */
			if (cam_node_add(type, sample_secs) < 0) {
				log_err("Failed to create simulated node");
				break;
			}
			--(*num_nodes);
		} else {
			/* Tell the node simulator to simulate a node leaving */
			if (cam_node_remove(type) < 0) {
				log_err("Failed to remove a simulated node");
				*num_nodes = 0;
				break;
			}
			++(*num_nodes);
		}
	}
	/* Echo back the updated property value */
	prop_send(prop);
done:
	/* Send property ack if requested */
	if (args && args->ack_arg) {
		ops_prop_ack_send(args->ack_arg, status, 0);
	}
	return (*num_nodes != 0) ? -1 : 0;
}

/*
 * Set handler for add_tstat_nodes property.
 */
static int appd_gw_add_camera_nodes_set(struct prop *prop,
	const void *val, size_t len, const struct op_args *args)
{
	return appd_gw_add_nodes_set_helper(prop, val, len, args,
	    CAMERA, 0);
}

/*
 * Handle sending the num_nodes property.
 */
static enum err_t appd_send_num_nodes(struct prop *prop, int req_id,
			const struct op_options *opts)
{
	num_nodes = node_count();
	return prop_arg_send(prop, req_id, opts);
}

/*
 * Sample gateway properties template.
 */
static struct prop appd_gw_prop_table[] = {
	/* Application software version property */
	{
		.name = "version",
		.type = PROP_STRING,
		.send = appd_send_version
	},
    {
        .name = "signaling_channels",
        .set = webrtc_signaling_channels_json,
        .type = PROP_JSON,
        .ads_failure_cb = appd_prop_ads_failure_cb,
    },
    {
        .name = "kvs_streams",
        .set = kvs_streams_json,
        .type = PROP_JSON,
        .ads_failure_cb = appd_prop_ads_failure_cb,
    },
	/****** Gateway properties ******/
	{
		.name = "add_camera_nodes",
		.type = PROP_INTEGER,
		.set = appd_gw_add_camera_nodes_set,
		.send = prop_arg_send,
		.arg = &add_camera_nodes,
		.len = sizeof(add_camera_nodes),
		.app_manages_acks = 1
	},
	{
		.name = "log",
		.type = PROP_STRING,
	},
	{
		.name = "num_nodes",
		.type = PROP_INTEGER,
		.send = appd_send_num_nodes,
		.arg = &num_nodes,
		.len = sizeof(num_nodes),
	},
};

void handle_sigterm(int sig)
{
	stream_proc_kill();
	exit(0);
}

void setup_sigterm()
{
	struct sigaction action;
	action.sa_handler = handle_sigterm;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGKILL, &action, NULL);
	sigaction(SIGINT, &action, NULL);
}


/*
 * Hook for the app library to initialize the user-defined application.
 */
int appd_init(void)
{
	struct node_cloud_callbacks node_callbacks = {
		.node_add = appd_node_add_handler,
		.node_remove = appd_node_remove_handler,
		.node_update_info = appd_node_update_handler,
		.node_conn_status = appd_node_conn_send_handler,
		.node_prop_send = appd_node_prop_send_handler,
		.node_prop_batch_send = appd_node_prop_batch_send
	};

	log_info("application initializing");

	/* Load property table */
	prop_add(appd_gw_prop_table, ARRAY_LEN(appd_gw_prop_table));

	/* This demo manages acks for ack enabled properties */
	gw_node_prop_set_handler_set(&appd_node_props_set_handler, 1);
	gw_node_prop_get_handler_set(&appd_node_prop_get_handler);
	gw_node_conn_get_handler_set(&appd_node_conn_get_handler);
	gw_node_rst_handler_set(&appd_gw_node_reset_handler);
	gw_node_ota_handler_set(&appd_gw_node_ota_handler);
	gw_node_reg_handler_set(&appd_gw_node_reg_handler);
	gw_confirm_handler_set(&appd_node_ops_confirm_handler);

	/*
	 * Initialize generic node management and register callbacks.
	 */
	node_mgmt_init(app_get_timers());
	node_set_cloud_callbacks(&node_callbacks);

	/* Initialize the node simulator */
	cam_init(app_get_timers());

	setup_sigterm();

	/* Kill other instances of video stream processes that might be still running (error) */
	stream_proc_kill();

	return 0;
}

/*
 * Hook for the app library to start the user-defined application.  Once
 * This function returns, the app library will enable receiving updates from
 * the cloud, and begin to process tasks on the main thread.
 */
int appd_start(void)
{
	log_info("application starting");

	/*
	 * Set gateway template version to select the correct cloud template.
	 */
	app_set_template_version(appd_template_version);

    timer_init(&update_timer, appd_update_timeout);
    timer_set(app_get_timers(), &update_timer, 5000);

	/* Start gateway node simulator for demo appd */
	cam_start();

	return 0;
}

/*
 * Hook for the app library to notify the user-defined application that the
 * process is about to terminate.
 */
void appd_exit(int status)
{
	log_info("application exiting with status: %d", status);

	/* Cleanup */
	cam_stop();
	node_mgmt_exit();
}

/*
 * Function called during each main loop iteration.  This may be used to
 * perform routine tasks that are not linked to a specific timer or file event.
 */
void appd_poll(void)
{
	/* Post accurate node count to cloud */
	if (num_nodes != node_count()) {
		prop_send_by_name("num_nodes");
	}
	return;
}

/*
 * Hook for the app library to notify the user-defined application that a
 * factory reset is about to occur.
 */
void appd_factory_reset(void)
{
	log_info("application factory reset");

	/*
	 * XXX this works for the simulator, but be aware that
	 * node_mgmt_factory_reset() kicks off a potentially asynchronous
	 * operation, and if the factory reset results in appd being
	 * terminated, all nodes may not be reset.
	 */
	node_mgmt_factory_reset();
}

/*
 * Hook for the app library to notify the user-defined application that the
 * the connectivity status has changed.
 */
void appd_connectivity_event(enum app_conn_type type, bool up)
{
	static bool first_connection = true;

	log_info("%s connection %s", app_conn_type_strings[type],
	    up ? "UP" : "DOWN");

	/* Some tasks should be performed when first connecting to the cloud */
	if (type == APP_CONN_CLOUD && up) {
		if (first_connection) {
			/*
			 * Send all from-device properties to update the
			 * service on first connection.  This is helpful to
			 * ensure that the application's startup state is
			 * immediately synchronized with the cloud.
			 */
			prop_send_from_dev(true);

			/* Request all to-device properties from the cloud */
			prop_request_to_dev();

			first_connection = false;
		}
		/*
		 * Run the node management state machine in case any operations
		 * failed while offline and were scheduled to retry.
		 */
		node_sync_all();
	}
}

/*
 * Hook for the app library to notify the user-defined application that the
 * the user registration status has changed.
 */
void appd_registration_event(bool registered)
{
	log_info("device user %s", registered ? "registered" : "unregistered");

	if (registered) {
		/*
		 * Send all from-device properties to update the service after
		 * user registration.  This is helpful to ensure that the
		 * device's current state is visible to the new user, since
		 * the cloud hides all user-level property datapoints created
		 * prior to user registration.
		 */
		prop_send_from_dev(true);
	}
}

/*
 * Compose and send a "log" property datapoint.  The log entry is not
 * cached, so if a connectivity failure occurs, its value is lost.
 */
int appd_log_entry(const char *func, const char *msg, ...)
{
	va_list args;
	static char log[PROP_STRING_LEN + 1];
	size_t log_len;
	struct prop *log_prop = prop_lookup("log");

	ASSERT(log_prop != NULL);

	va_start(args, msg);
	log_len = vsnprintf(log, sizeof(log), msg, args);
	if (log_len >= sizeof(log)) {
		log_len = sizeof(log) - 1;
		/* End with ellipsis to indicate truncation occurred */
		memcpy(log + log_len - strlen("..."), "...", strlen("..."));
	}
	va_end(args);
	log_base(func, LOG_AYLA_INFO, "%s", log);
	return prop_val_send(log_prop, 0, log, log_len, NULL);
}

/*
 * Handle update timer timeout
 */
void appd_update_timeout(struct timer *timer)
{
    timer_set(app_get_timers(), &update_timer, 5000);
}

/*
 * Ads failure callback for properties. Called whenever a particular property
 * update failed to reach the cloud due to connectivity loss.
 */
static int appd_prop_ads_failure_cb(struct prop *prop, const void *val,
                                    size_t len, const struct op_options *opts)
{
    if (!prop) {
        log_debug("NULL prop argument");
        return 0;
    }

    log_info("%s = %s failed to send to ADS at %llu",
             prop->name,
             prop_val_to_str(val ? val : prop->arg, prop->type),
             opts ? opts->dev_time_ms : 0);
    return 0;
}

static const char* get_cam_addr_from_stream_name(const char* stream_name)
{
	const char* pos = strchr(stream_name, '-');
	if (pos == NULL) {
		return NULL;
	}
	++pos;
	
	return pos;
}

static void free_ptrstr(char** str)
{
	if (*str) {
		free(*str);
		*str = NULL;
	}
}

static int kvs_streams_json(struct prop *prop, const void *val, size_t len,
                            const struct op_args *args)
{
	struct hls_data kvsdata;
	memset(&kvsdata, 0, sizeof(struct hls_data));

    json_t *dev_node_a, *dev_node, *credentials;
    void * itr;
    int retention_days , expiration_time;

    json_t *info = (json_t *)val;

    log_debug("we have received the kvs streams json");

    itr = json_object_iter(info);
    if(itr)
    {
		log_debug("info has iterations");
        dev_node_a = json_object_iter_value(itr);

        dev_node = json_array_get(dev_node_a,0);
        //ds_json_dump(__func__, dev_node);
        if (!dev_node || !json_is_object(dev_node)) {
            log_err("no kvs streaming object");
            return -1;
        }
        credentials = json_object_get(dev_node,"credentials");
        if (!credentials|| !json_is_object(credentials)) {
            log_err("credentials is not object");
            return -1;
        }

        kvsdata.kvs_channel_name = json_get_string_dup(dev_node, "name");
        log_debug2("kvs_channel_name '%s'",kvsdata.kvs_channel_name);

        kvsdata.arn = json_get_string_dup(dev_node, "arn");
        log_debug2("arn '%s'", kvsdata.arn);

        kvsdata.region = json_get_string_dup(dev_node, "region");
        log_debug2("region '%s'",kvsdata.region);

		kvsdata.access_key_id = json_get_string_dup(credentials, "access_key_id");
        log_debug2("access_key_id '%s'",kvsdata.access_key_id);

		kvsdata.secret_access_key= json_get_string_dup(credentials, "secret_access_key");
        log_debug2("secret_access_key '%s'",kvsdata.secret_access_key);

        kvsdata.session_token = json_get_string_dup(credentials, "session_token");

		json_get_int(dev_node, "retention_days", &retention_days);
        kvsdata.retention_days = retention_days;
        log_debug2("retention days '%d' and '%d' ", retention_days , kvsdata.retention_days);

		json_get_int(credentials, "expiration", &expiration_time);
        kvsdata.expiration_time= expiration_time;
        log_debug2("expiration time '%d' and '%d' ", expiration_time , kvsdata.expiration_time);
    }
	else {
		return -1;
	}

    log_debug("kvs streaming channel info is parsed ");

	const char* cam_node_name = get_cam_addr_from_stream_name(kvsdata.kvs_channel_name);
	struct node* node = node_lookup(cam_node_name);
	struct cam_node_state* node_state = (struct cam_node_state *)node_state_get(node, STATE_SLOT_NET);

	free_ptrstr(&node_state->hls_data.kvs_channel_name);
	free_ptrstr(&node_state->hls_data.arn);
	free_ptrstr(&node_state->hls_data.region);
	free_ptrstr(&node_state->hls_data.access_key_id);
	free_ptrstr(&node_state->hls_data.secret_access_key);
	free_ptrstr(&node_state->hls_data.session_token);

	memcpy(&node_state->hls_data, &kvsdata, sizeof(kvsdata));

	log_debug("kvs stream info for cam node addr: %s", cam_node_name);

	conf_save();

    return 0;
}

static int webrtc_signaling_channels_json (struct prop *prop, const void *val, size_t len,
                                           const struct op_args *args)
{
	struct webrtc_data webrtcdata;
	memset(&webrtcdata, 0, sizeof(struct webrtc_data));

	json_t *dev_node_a, *dev_node, *credentials;
	void * itr;
	int expiration_time;

	json_t *info = (json_t *)val;

	log_debug("we have received the webrtc signaling channel json");

    itr = json_object_iter(info);
    if(itr)
    {
		log_debug("info has iterations");
        dev_node_a = json_object_iter_value(itr);

        dev_node = json_array_get(dev_node_a,0);
        //ds_json_dump(__func__, dev_node);
        if (!dev_node || !json_is_object(dev_node)) {
            log_err("no signalling channel object");
            return -1;
        }
        credentials = json_object_get(dev_node,"credentials");
        if (!credentials|| !json_is_object(credentials)) {
            log_err("credentials is not object");
            return -1;
        }

        webrtcdata.webrtc_channel_name = json_get_string_dup(dev_node,
                                                              "name");
        log_debug2("webrtc_channel_name '%s'",webrtcdata.webrtc_channel_name);
		
        webrtcdata.arn = json_get_string_dup(dev_node, "arn");
        log_debug2("arn '%s'", webrtcdata.arn);

        webrtcdata.region = json_get_string_dup(dev_node, "region");
        log_debug2("region '%s'",webrtcdata.region);

        webrtcdata.access_key_id = json_get_string_dup(credentials, "access_key_id");
        log_debug2("access_key_id '%s'",webrtcdata.access_key_id);

        webrtcdata.secret_access_key= json_get_string_dup(credentials, "secret_access_key");
        log_debug2("secret_access_key '%s'",webrtcdata.secret_access_key);

        webrtcdata.session_token = json_get_string_dup(credentials, "session_token");
        json_get_int(credentials, "expiration", &expiration_time);

        webrtcdata.expiration_time = expiration_time;
        log_debug2("expiration time '%d' and '%d' ", expiration_time , webrtcdata.expiration_time);
    }
	else {
		return -1;
	}

    log_debug("webrtc signalling channel info is parsed ");

	const char* cam_node_name = get_cam_addr_from_stream_name(webrtcdata.webrtc_channel_name);
	struct node* node = node_lookup(cam_node_name);
	struct cam_node_state* node_state = (struct cam_node_state *)node_state_get(node, STATE_SLOT_NET);

	free_ptrstr(&node_state->webrtc_data.webrtc_channel_name);
	free_ptrstr(&node_state->webrtc_data.arn);
	free_ptrstr(&node_state->webrtc_data.region);
	free_ptrstr(&node_state->webrtc_data.access_key_id);
	free_ptrstr(&node_state->webrtc_data.secret_access_key);
	free_ptrstr(&node_state->webrtc_data.session_token);

	memcpy(&node_state->webrtc_data, &webrtcdata, sizeof(webrtcdata));

	log_debug("kvs stream info for cam node addr: %s", cam_node_name);

	conf_save();

	return 0;
}

static void stream_proc_kill()
{
	kill_all_proc(HLS_STREAM_APP, 1000);
	kill_all_proc(WEBRTC_STREAM_APP, 1000);
	kill_all_proc(MASTER_STREAM_APP, 1000);
}

