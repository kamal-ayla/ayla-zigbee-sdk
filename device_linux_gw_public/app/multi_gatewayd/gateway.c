/*
 * Copyright 2017-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

/*
 * Multi Protocol Gateway Demo
 *
 * This gateway application hooks into a generic node management interface
 * and a simple multi protocol gateway implementation to demonstrate good
 * practices when creating a gateway.
 *
 * This demo should provide a model for how to begin implementing a gateway
 * application supporting physical nodes connected over a wireless network.
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/build.h>
#include <ayla/utypes.h>
#include <ayla/http.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/time_utils.h>
#include <ayla/timer.h>
#include <ayla/gateway_interface.h>

#include <app/app.h>
#include <app/ops.h>
#include <app/props.h>
#include <app/gateway.h>

#include "gateway.h"
#include "node.h"
#include "zigbee/zb_interface.h"
#include "bluetooth/bt_interface.h"



/* Maximum # of datapoints allowed in a batch */
#define APPD_MAX_BATCHED_DPS				64

/* Maximum # of scan results */
#define APPD_MAX_SCAN_RESULTS				20
#define APPD_MAX_SCAN_VALID_MS				120000

/* Holdoff time between scan results posts */
#define APPD_SCAN_RESULT_SEND_HOLDOFF_MS	5000



/*
 * Structure for tracking discovered Bluetooth nodes prior to sending them
 * via the appropriate property.  String properties accept a maximum of 1024
 * bytes of data, and the scan results use json format.
 */
struct scan_result {
	char addr[18];	/* Enough room for 6-byte HW address w/delims */
	char name[28];	/* Max len to allow 27-byte */
	char type[16]; /* Max len to allow 15-byte */
	s8 rssi;	/* Maximum 3 digits plus sign */
	u64 updated_ms;
};


const char *appd_version = "multi_protocol_gatewayd " BUILD_VERSION_LABEL;
const char *appd_template_version = "multi_protocol_gateway_demo_v1.1";

/* ZigBee protocol property states */
static struct timer zb_permit_join_timer;
static unsigned int zb_join_enable;
static unsigned int zb_num_nodes;
static u8 zb_join_status;
static u8 zb_network_up;
static char zb_bind_cmd[PROP_STRING_LEN + 1];
static char zb_bind_result[PROP_STRING_LEN + 1];


/* Bluetooth protocol property states */
static unsigned int bt_scan_enable;
static u8 bt_scan_status;
static struct timer bt_scan_stop_timer;
static struct timer bt_scan_result_timer;
static struct scan_result bt_scan_results[APPD_MAX_SCAN_RESULTS];
static u64 bt_scan_results_sent;
static int bt_connect_passkey;	/* Passkey for current pair attempt */
static unsigned int bt_num_nodes;
static char bt_connect_result[PROP_STRING_LEN + 1];
static char bt_disconnect_result[PROP_STRING_LEN + 1];
static unsigned int num_nodes;


/* Node property batch list */
static struct gw_node_prop_batch_list *node_batched_dps;


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
		node_prop_send_all_set(node, PROP_FROM_DEVICE);
	}

	/* Call the gw_node_reg_cb callback function to send the result */
	gw_node_reg_cb(addr, cookie, 1, 0);
}

enum bt_op_conn_result {
	BT_OP_CONN_UNKNOWN_ERROR = -2,   /* unknown error */
	BT_OP_CONN_NO_DEVICE = -1,	     /* no device found */
	BT_OP_CONN_SUCCESS = 0,      /* operation connect node success */
	BT_OP_CONN_ADD_SUCCESS = 1,	    /* operation node add success */
	BT_OP_CONN_UPDATE_SUCCESS = 2    /* operation node update success */
};

/*
 * Set the bt_connect_result after connected a device.
 */
static void app_bt_set_connect_result(const char *addr,
	enum bt_dev_op_err_code err_code)
{
	enum bt_op_conn_result result;
	char *error;

	ASSERT(addr != NULL);

	switch (err_code) {
	case BT_DEV_OP_UNKNOWN_ERROR:
		result = BT_OP_CONN_UNKNOWN_ERROR;
		error = "unknown error";
		break;
	case BT_DEV_OP_NO_DEVICE:
		result = BT_OP_CONN_NO_DEVICE;
		error = "no device found";
		break;
	case BT_DEV_OP_CONNECT_SUCCESS:
		result = BT_OP_CONN_SUCCESS;
		error = "connected";
		break;
	case BT_DEV_OP_ADD_DONE:
		result = BT_OP_CONN_ADD_SUCCESS;
		error = "added";
		break;
	case BT_DEV_OP_UPDATE_DONE:
		result = BT_OP_CONN_UPDATE_SUCCESS;
		error = "updated";
		break;
	default:
		result = BT_OP_CONN_UNKNOWN_ERROR;
		error = "unknown error";
		break;
	}

	snprintf(bt_connect_result, PROP_STRING_LEN,
	    "{\"bd_addr\":\"%s\",\"status_code\":%d,"
	    "\"status_detail\":\"%s\"}", addr, result, error);

	prop_send_by_name("bt_connect_result");
}

enum bt_op_disc_result {
	BT_OP_DISC_UNKNOWN_ERROR = -2,   /* unknown error */
	BT_OP_DISC_NO_NODE = -1,	 /* no device found */
	BT_OP_DISC_SUCCESS = 0,  /* operation disconnect node success */
	BT_OP_DISC_INPROGRESS = 1, /* operation disconnect is in progress */
};

/*
 * Set the bt_disconnect_result after disconnected a device.
 */
static void app_bt_set_disconnect_result(const char *addr,
	enum bt_dev_op_err_code err_code)
{
	enum bt_op_disc_result result;
	char *error;

	switch (err_code) {
	case BT_DEV_OP_UNKNOWN_ERROR:
		result = BT_OP_DISC_UNKNOWN_ERROR;
		error = "unknown error";
		break;
	case BT_DEV_OP_IN_PROGRESS:
		result = BT_OP_DISC_INPROGRESS;
		error = "disconnecting";
		break;
	case BT_DEV_OP_NO_NODE:
		result = BT_OP_DISC_NO_NODE;
		error = "cannot find node";
		break;
	case BT_DEV_OP_CONNECT_SUCCESS:
		result = BT_OP_DISC_SUCCESS;
		error = "disconnected";
		break;
	default:
		result = BT_OP_DISC_UNKNOWN_ERROR;
		error = "unknown error";
		break;
	}

	snprintf(bt_disconnect_result, PROP_STRING_LEN,
	    "{\"bd_addr\":\"%s\",\"status_code\":%d,"
	    "\"status_detail\":\"%s\"}", addr, result, error);

	prop_send_by_name("bt_disconnect_result");
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
		/* Update BLE node connect result */
		if ((node->interface == GI_BLE)
		    && (type == CAT_ADDR)) {
			enum bt_dev_op_err_code err_code;
			if (confirm_info->status == CONF_STAT_SUCCESS) {
				err_code = BT_DEV_OP_ADD_DONE;
			} else {
				err_code = BT_DEV_OP_UNKNOWN_ERROR;
			}
			log_debug("update BLE node %s bt_connect_result %d",
			    node->addr, err_code);
			app_bt_set_connect_result(node->addr, err_code);
		}
		break;
	case AG_NODE_UPDATE:
		ASSERT(node != NULL);
		if (confirm_info->status == CONF_STAT_SUCCESS) {
			log_info("node %s type %d updated in cloud"
			    " successfully", node->addr, node->interface);
		} else {
			log_warn("node %s update failed: err %u",
			    node->addr, confirm_info->err);
		}
		if (node_callback) {
			node_callback(node, confirm_info);
		}
		/* Update BLE node connect result */
		if ((node->interface == GI_BLE)
		    && (type == CAT_ADDR)) {
			enum bt_dev_op_err_code err_code;
			if (confirm_info->status == CONF_STAT_SUCCESS) {
				err_code = BT_DEV_OP_UPDATE_DONE;
			} else {
				err_code = BT_DEV_OP_UNKNOWN_ERROR;
			}
			log_debug("update BLE node %s bt_connect_result %d",
			    node->addr, err_code);
			app_bt_set_connect_result(node->addr, err_code);
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
 * Handle permit join timer timeout
 */
static void appd_zb_permit_join_timeout(struct timer *timer)
{
	timer_cancel(app_get_timers(), timer);
	if (zb_permit_join(0, false) < 0) {
		log_debug("disable permit join failed");
		timer_set(app_get_timers(), &zb_permit_join_timer, 100);
	} else {
		log_debug("disable ZigBee permit join successed");
		zb_join_status = 0;
		prop_send_by_name("zb_join_status");
	}
}

/*
 * Enable or disable node joining network.
 */
static int appd_gw_join_enable_set(struct prop *prop, const void *val,
	size_t len, const struct op_args *args)
{
	/* Cancel last join timer */
	timer_cancel(app_get_timers(), &zb_permit_join_timer);

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return -1;
	}

	if (zb_join_enable > 255) {
		log_debug("exceeded range, change to join enable forever");
		zb_join_enable = 255;
	}

	/* Permit nodes to join network */
	if (zb_permit_join(zb_join_enable, false) < 0) {
		log_debug("enabled permit join for %u seconds failed",
		    zb_join_enable);
		return -1;
	} else {
		log_debug("enabled permit join for %u seconds successed",
		    zb_join_enable);
	}

	/* Make sure the join permit to disable */
	if ((0 < zb_join_enable) && (zb_join_enable < 255)) {
		timer_set(app_get_timers(), &zb_permit_join_timer,
		    zb_join_enable * 1000);
	}

	if (zb_join_enable) {
		zb_join_status = 1;
	} else {
		zb_join_status = 0;
	}
	prop_send_by_name("zb_join_status");

	/* Disable join after enabled join  */
	zb_join_enable = 0;
	prop_send_by_name("zb_join_enable");

	return 0;
}

/*
 * Bind a node with another node
 */
static int appd_gw_bind_cmd_set(struct prop *prop, const void *val,
	size_t len, const struct op_args *args)
{
	int ret;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return -1;
	}

	if (!strlen(zb_bind_cmd)) {
		log_info("node addr not set");
		return 0;
	}

	ret = zb_gw_bind_prop_handler(zb_bind_cmd,
	    zb_bind_result, sizeof(zb_bind_result));

	if (ret < 0) {
		prop_send_by_name("zb_bind_result");
	}

	return 0;
}

/*
 * Send the current join state.
 */
static int appd_gw_join_enable_send(struct prop *prop, int req_id,
	const struct op_options *opts)
{
	return prop_val_send(prop, req_id,
	    &zb_join_enable, sizeof(zb_join_enable), opts);
}

/*
 * Send an updated scan list.
 */
static void appd_bt_scan_result_timeout(struct timer *timer)
{
	prop_send_by_name("bt_scan_results");
}

/*
 * Callback invoked by the Bluetooth interface when a device is discovered or
 * updated.  This updates the bt_scan_results list as needed.
 */
static void app_bt_scan_result_update(const struct bt_scan_result *result)
{
	struct scan_result *r;
	struct scan_result *insert = NULL;
	u64 now_ms = time_mtime_ms();
	u64 elapsed_ms;
	int len;

	ASSERT(result != NULL);

	if (!result->addr[0] ||
	    result->rssi < INT8_MIN || result->rssi > INT8_MAX) {
		/* Ignore incomplete scan results */
		return;
	}

	log_debug("result: addr=%s name=%s type=%s RSSI=%hd",
	    result->addr, result->name, result->type, result->rssi);

	/* Find same record */
	for (r = bt_scan_results;
	    r < (&bt_scan_results[APPD_MAX_SCAN_RESULTS]); ++r) {
		if (!r->addr[0]) {
			break;
		}
		if (!strcmp(r->addr, result->addr)) {
			if (result->rssi == r->rssi) {
				log_debug("Record %s already exists",
				    r->addr);
				return;
			}
			/* delete record */
			len = ((&bt_scan_results[APPD_MAX_SCAN_RESULTS]
			    - (r + 1)) * sizeof(struct scan_result));
			memmove(r, r + 1, len);
			memset(&bt_scan_results[APPD_MAX_SCAN_RESULTS - 1],
			    0 , sizeof(struct scan_result));
			break;
		}
	}

	/* Find the insert position in sorted scan list */
	for (r = bt_scan_results;
	    r < (&bt_scan_results[APPD_MAX_SCAN_RESULTS]); ++r) {
		if (!r->addr[0]) {
			/*insert here*/
			insert = r;
			break;
		}
		if (result->rssi >= r->rssi) {
			/* Insert in order of RSSI */
			insert = r;
			break;
		}
	}

	if (insert) {
		/* Move the insert record to next */
		len = ((&bt_scan_results[APPD_MAX_SCAN_RESULTS - 1] - insert)
		    * sizeof(struct scan_result));
		memmove(insert + 1, insert, len);

		/* Set the scan record */
		snprintf(insert->addr, sizeof(insert->addr), "%s",
		    result->addr);
		if (result->name) {
			snprintf(insert->name, sizeof(insert->name), "%s",
			    result->name);
		} else {
			insert->name[0] = '\0';
		}
		if (result->type) {
			snprintf(insert->type, sizeof(insert->type), "%s",
			    result->type);
		} else {
			insert->type[0] = '\0';
		}
		insert->rssi = result->rssi;
		insert->updated_ms = now_ms;
		log_debug("[%zu] updated: addr=%s name=%s type=%s RSSI=%hd",
		    (size_t)(insert - bt_scan_results),
		    result->addr, result->name, result->type, result->rssi);
	} else {
		/* No available scan slots */
		log_warn("discarding: addr=%s name=%s type=%s RSSI=%hd",
		    result->addr, result->name, result->type, result->rssi);
		return;
	}

	/* Sent scan results recently */
	if (timer_active(&bt_scan_result_timer)) {
		return;
	}
	elapsed_ms = now_ms - bt_scan_results_sent;
	/* If in the hold window, schedule a send for later */
	if (elapsed_ms < APPD_SCAN_RESULT_SEND_HOLDOFF_MS) {
		timer_set(app_get_timers(), &bt_scan_result_timer,
		    APPD_SCAN_RESULT_SEND_HOLDOFF_MS - elapsed_ms);
		return;
	}
	/*
	 * TODO
	 * Consider having a cloud hold timer and a shorter LAN client hold
	 * timer for better responsiveness for LAN clients.
	 */
	prop_send_by_name("bt_scan_results");
	bt_scan_results_sent = now_ms;
}

/*
 * This updates the bt_scan_results after connected a device.
 */
static void app_bt_connected_update(const char *addr)
{
	int i;

	ASSERT(addr != NULL);

	for (i = 0; i < APPD_MAX_SCAN_RESULTS; i++) {
		if (!strcmp(bt_scan_results[i].addr, addr)) {
			memmove(&bt_scan_results[i], &bt_scan_results[i + 1],
			    ((APPD_MAX_SCAN_RESULTS - 1 - i)
			    * sizeof(bt_scan_results[0])));
			memset(&bt_scan_results[APPD_MAX_SCAN_RESULTS - 1],
			    0, sizeof(bt_scan_results[0]));
		}
	}

	prop_send_by_name("bt_scan_results");
}

/*
 * Callback invoked by the Bluetooth interface when a pairing device requests
 * a passkey from the user for authentication.  The passkey must have been
 * set via the "bt_connect_passkey" property prior to the connection attempt.
 */
static int app_bt_passkey_request(const char *addr, u32 *passkey)
{
	if (bt_connect_passkey < 0) {
		log_warn("Bluetooth passkey is invalid: %d",
		    bt_connect_passkey);
		return -1;
	}
	*passkey = bt_connect_passkey;
	log_info("sending Bluetooth passkey: %06u", *passkey);
	return 0;
}

/*
 * Callback invoked by the Bluetooth interface when a pairing device requests
 * to display the passkey for authentication.  This will be posted to the
 * "bt_connect_passkey_display" property.  This allows the user to verify
 * the identity of the device.
 */
static int app_bt_passkey_display(const char *addr, u32 passkey)
{
	struct prop *prop = prop_lookup("bt_connect_passkey_display");
	char buf[11];

	log_info("verify Bluetooth passkey: %06u", passkey);
	snprintf(buf, sizeof(buf), "%06u", passkey);
	if (!prop || prop_val_send(prop, 0, buf, 0, NULL) != ERR_OK) {
		return -1;
	}
	return 0;
}

/*
 * Callback invoked by the Bluetooth interface when a pairing device requests
 * to clear the displayed the passkey.  This is expected when pairing has ended.
 */
static int app_bt_passkey_display_clear(const char *addr)
{
	struct prop *prop = prop_lookup("bt_connect_passkey_display");

	if (!prop || prop_val_send(prop, 0, "", 0, NULL) != ERR_OK) {
		return -1;
	}
	return 0;
}

/*
 * Enable or disable scanning for discoverable Bluetooth nodes.
 */
static int app_bt_scan_enable_set(struct prop *prop, const void *val,
	size_t len, const struct op_args *args)
{
	bool enable;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return -1;
	}

	log_debug("bt_scan_enable %u", bt_scan_enable);

	if (bt_scan_enable > 24 * 3600) {
		log_debug("exceeded range, change to scan enable 24 hours");
		bt_scan_enable = 24 * 3600;
	}

	enable = bt_discovery_running();
	if (bt_scan_enable) {
		if (!enable) {
			if (bt_discover(true) < 0) {
				log_debug("bt_discover error");
				bt_scan_enable = 0;
				prop_send(prop);
				return -1;
			}
		}
		timer_set(app_get_timers(),
		    &bt_scan_stop_timer, bt_scan_enable * 1000);
	} else if (enable) {
		if (bt_discover(false) < 0) {
			log_debug("bt_discover error");
			prop_send(prop);
			return -1;
		}

		timer_cancel(app_get_timers(), &bt_scan_stop_timer);

		/* Clear scan list when disabling discovery */
		memset(bt_scan_results, 0, sizeof(bt_scan_results));
		timer_cancel(app_get_timers(), &bt_scan_result_timer);
		prop_send_by_name("bt_scan_results");
	}

	if (bt_scan_enable) {
		bt_scan_status = 1;
	} else {
		bt_scan_status = 0;
	}
	prop_send_by_name("bt_scan_status");

	/* Update scan enable after enabled scan */
	bt_scan_enable = 0;
	prop_send_by_name("bt_scan_enable");

	return 0;
}

/*
 * Send the current discovery state.
 */
static int app_bt_scan_enable_send(struct prop *prop, int req_id,
	const struct op_options *opts)
{
	return prop_val_send(prop, req_id,
	    &bt_scan_enable, sizeof(bt_scan_enable), opts);
}

/*
 * Stop bluetooth scan.
 */
static void appd_bt_scan_stop_timeout(struct timer *timer)
{
	log_debug("scan stop timer timeout");

	/* Stop bluetooth scan */
	bt_discover(false);

	log_debug("disable scan");
	bt_scan_status = 0;
	prop_send_by_name("bt_scan_status");

	/* Clear scan list when disabling discovery */
	memset(bt_scan_results, 0, sizeof(bt_scan_results));
	timer_cancel(app_get_timers(), &bt_scan_result_timer);
	prop_send_by_name("bt_scan_results");
}

/*
 * Send a property ack on the connect complete callback.
 */
static void app_bt_connect_id_complete(const char *addr,
	enum node_network_result result,
	enum bt_dev_op_err_code err_code, void *arg)
{
	struct prop *prop = prop_lookup("bt_connect_id");
	int ack_msg;

	ASSERT(addr != NULL);
	ASSERT(prop != NULL);
	log_debug("addr %s, result %d, arg %p", addr, result, arg);

	if (arg) {
		/* Send the property ack, if requested */
		switch (result) {
		case NETWORK_SUCCESS:
			ack_msg = HTTP_STATUS_OK;
			break;
		case NETWORK_OFFLINE:
			ack_msg = HTTP_STATUS_GATEWAY_TIMEOUT;
			break;
		case NETWORK_UNKNOWN:
			ack_msg = HTTP_STATUS_NOT_FOUND;
			break;
		default:
			ack_msg = HTTP_STATUS_UNAVAIL;
			break;
		}
		ops_prop_ack_send(arg, result, ack_msg);
	} else {
		prop_val_send(prop, 0, "", 1, NULL);
	}

	if (result == NETWORK_SUCCESS) {
		app_bt_connected_update(addr);
		app_bt_set_connect_result(addr, err_code);
	} else {
		app_bt_set_connect_result(addr, BT_DEV_OP_UNKNOWN_ERROR);
	}
}

/*
 * Attempt to connect to a node.
 */
static int app_bt_connect_id_set(struct prop *prop, const void *val,
	size_t len, const struct op_args *args)
{
	const char *addr = (const char *)val;
	void *ack_arg = args ? args->ack_arg : NULL;
	enum bt_dev_op_err_code ret;
	struct node *nd;
	enum node_state nd_state;
	enum bt_dev_op_err_code op_code;

	ASSERT(prop->type == PROP_STRING);

	if (!addr || !addr[0]) {
		log_debug("NULL string, addr %p, addr[0] %d", addr, addr[0]);
		return 0;
	}

	nd = node_lookup(addr);
	if (nd) {
		log_debug("device %s already connected", addr);
		if (ack_arg) {
			ops_prop_ack_send(ack_arg, NETWORK_SUCCESS,
			    HTTP_STATUS_OK);
		} else {
			prop_val_send(prop, 0, "", 1, NULL);
		}

		nd_state = node_get_state(nd);
		switch (nd_state) {
		case NODE_JOINED:
		case NODE_NET_QUERY:
		case NODE_CLOUD_ADD:
			op_code = BT_DEV_OP_CONNECT_SUCCESS;
			break;
		case NODE_CLOUD_UPDATE:
			op_code = BT_DEV_OP_ADD_DONE;
			break;
		case NODE_READY:
		case NODE_NET_CONFIGURE:
			if (bt_node_check_bulb_prop(nd)) {
				op_code = BT_DEV_OP_UPDATE_DONE;
			} else {
				op_code = BT_DEV_OP_ADD_DONE;
			}
			break;
		default:
			op_code = BT_DEV_OP_UNKNOWN_ERROR;
			break;
		}
		app_bt_set_connect_result(addr, op_code);
		return 0;
	}

	log_info("connecting with %s", addr);

	ret = bt_node_connect(addr, app_bt_connect_id_complete, ack_arg);
	if (ret < 0) {
		log_err("connected with %s error", addr);
		goto error;
	}

	return 0;
error:
	if (ack_arg) {
		ops_prop_ack_send(ack_arg, NETWORK_UNKNOWN,
		    HTTP_STATUS_NOT_FOUND);
	} else {
		prop_val_send(prop, 0, "", 1, NULL);
	}

	app_bt_set_connect_result(addr, ret);
	return -1;
}

/*
 * Send a property ack on the disconnect complete callback.
 */
static void app_bt_disconnect_id_complete(const char *addr,
	enum node_network_result result, void *arg)
{
	struct prop *prop = prop_lookup("bt_disconnect_id");
	int ack_msg;

	ASSERT(prop != NULL);
	log_debug("result %d, arg %p", result, arg);

	if (arg) {
		/* Send the property ack, if requested */
		switch (result) {
		case NETWORK_SUCCESS:
			ack_msg = HTTP_STATUS_OK;
			break;
		case NETWORK_OFFLINE:
			ack_msg = HTTP_STATUS_GATEWAY_TIMEOUT;
			break;
		case NETWORK_UNKNOWN:
			ack_msg = HTTP_STATUS_NOT_FOUND;
			break;
		default:
			ack_msg = HTTP_STATUS_UNAVAIL;
			break;
		}
		ops_prop_ack_send(arg, result, ack_msg);
	} else {
		prop_val_send(prop, 0, "", 1, NULL);
	}

	if (result == NETWORK_SUCCESS) {
		app_bt_set_disconnect_result(addr, BT_DEV_OP_CONNECT_SUCCESS);
	} else {
		app_bt_set_disconnect_result(addr, BT_DEV_OP_UNKNOWN_ERROR);
	}
}

/*
 * Attempt to disconnect to a node.
 */
static int app_bt_disconnect_id_set(struct prop *prop, const void *val,
	size_t len, const struct op_args *args)
{
	const char *addr = (const char *)val;
	void *ack_arg = args ? args->ack_arg : NULL;
	enum bt_dev_op_err_code ret;

	ASSERT(prop->type == PROP_STRING);

	if (!addr || !addr[0]) {
		log_debug("NULL string, addr %p, addr[0] %d", addr, addr[0]);
		return 0;
	}

	log_info("disconnecting with %s", addr);

	ret = bt_node_disconnect(addr,
	    app_bt_disconnect_id_complete, ack_arg);
	if (ret < 0) {
		log_debug("disconnected with %s error %d", addr, ret);
		goto error;
	}

	return 0;
error:
	if (ack_arg) {
		ops_prop_ack_send(ack_arg, NETWORK_UNKNOWN,
		    HTTP_STATUS_NOT_FOUND);
	} else {
		prop_val_send(prop, 0, "", 1, NULL);
	}

	app_bt_set_disconnect_result(addr, ret);
	return -1;
}

/*
 * Compose and send a "bt_scan_results" property datapoint.  Scan results
 * are not cached, so if a connectivity failure occurs, they are lost.
 */
static int app_bt_scan_results_send(struct prop *prop, int req_id,
	const struct op_options *opts)
{
	char str[PROP_STRING_LEN + 1];
	char one[128]; /* one result max length */
	size_t len = 0;
	size_t rc;
	const struct scan_result *r;
	struct op_options send_opts = { .confirm = 1 };

	if (opts) {
		send_opts = *opts;
		send_opts.confirm = 1;
	}

	str[len] = '[';
	len++;
	for (r = bt_scan_results;
	    r < (&bt_scan_results[APPD_MAX_SCAN_RESULTS]); ++r) {
		if (!r->addr[0]) {
			break;
		}
		if (r != bt_scan_results) {
			str[len] = ',';
			len++;
		}

		memset(one, 0, sizeof(one));
		rc = snprintf(one, sizeof(one) - 1,
		    "{\"device\":{\"bd_addr\":\"%s\",\"rssi\":\"%hhd\","
		    "\"name\":\"%s\",\"type\":\"%s\"}}",
		    r->addr, r->rssi, r->name, r->type);
		if (len + rc + 2 >= sizeof(str)) {
			log_warn("truncating %zu byte scan result", len + rc);
			break;
		}
		rc = snprintf(str + len, sizeof(str) - len, "%s", one);
		len += rc;
	}
	str[len] = ']';
	str[len + 1] = '\0';

	return prop_val_send(prop, req_id, str, 0, &send_opts);
}

/*
 * node number structure
 */
struct tag_num_nodes {
	unsigned int zb_num_nodes;
	unsigned int bt_num_nodes;
	unsigned int all_num_nodes;
};

/*
 * Check node type
 */
static int appd_check_node_type(struct node *node, void *arg)
{
	struct tag_num_nodes *num;

	ASSERT(node != NULL);
	ASSERT(arg != NULL);

	num = (struct tag_num_nodes *)arg;

	if (node->interface == GI_WIFI) {
		;
	} else if (node->interface == GI_ZIGBEE) {
		num->zb_num_nodes++;
	} else if (node->interface == GI_ZWAVE) {
		;
	} else if (node->interface == GI_BLE) {
		num->bt_num_nodes++;
	} else {
		;
	}
	num->all_num_nodes++;
	return 0;
}

/*
 * Update node number
 */
static void appd_update_num_nodes(struct tag_num_nodes *num)
{
	ASSERT(num != NULL);
	num->zb_num_nodes = 0;
	num->bt_num_nodes = 0;
	num->all_num_nodes = 0;
	node_foreach(appd_check_node_type, num);
}

/*
 * Handle sending the num_nodes property.
 */
static enum err_t appd_num_nodes_send(struct prop *prop, int req_id,
			const struct op_options *opts)
{
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
	/****** ZigBee protocol properties ******/
	{
		.name = "zb_join_enable",
		.type = PROP_INTEGER,
		.set = appd_gw_join_enable_set,
		.send = appd_gw_join_enable_send,
		.arg = &zb_join_enable,
		.len = sizeof(zb_join_enable)
	},
	{
		.name = "zb_num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &zb_num_nodes,
		.len = sizeof(zb_num_nodes),
	},
	{
		.name = "zb_join_status",
		.type = PROP_BOOLEAN,
		.send = prop_arg_send,
		.arg = &zb_join_status,
		.len = sizeof(zb_join_status)
	},
	{
		.name = "zb_network_up",
		.type = PROP_BOOLEAN,
		.send = prop_arg_send,
		.arg = &zb_network_up,
		.len = sizeof(zb_network_up)
	},
	{
		.name = "zb_bind_cmd",
		.type = PROP_STRING,
		.set = appd_gw_bind_cmd_set,
		.send = prop_arg_send,
		.arg = &zb_bind_cmd,
		.len = sizeof(zb_bind_cmd)
	},
	{
		.name = "zb_bind_result",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &zb_bind_result,
		.len = sizeof(zb_bind_result)
	},
	/****** bluetooth protocol properties ******/
	{
		.name = "bt_scan_enable",
		.type = PROP_INTEGER,
		.set = app_bt_scan_enable_set,
		.send = app_bt_scan_enable_send,
		.arg = &bt_scan_enable,
		.len = sizeof(bt_scan_enable)
	},
	{
		.name = "bt_scan_status",
		.type = PROP_BOOLEAN,
		.send = prop_arg_send,
		.arg = &bt_scan_status,
		.len = sizeof(bt_scan_status)
	},
	{
		.name = "bt_connect_id",
		.type = PROP_STRING,
		.set = app_bt_connect_id_set,
		.app_manages_acks = 1
	},
	{
		.name = "bt_connect_result",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &bt_connect_result,
		.len = sizeof(bt_connect_result)
	},
	{
		.name = "bt_disconnect_id",
		.type = PROP_STRING,
		.set = app_bt_disconnect_id_set,
		.app_manages_acks = 1
	},
	{
		.name = "bt_disconnect_result",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &bt_disconnect_result,
		.len = sizeof(bt_disconnect_result)
	},
	{
		.name = "bt_connect_passkey",
		.type = PROP_INTEGER,
		.set = prop_arg_set,
		.arg = &bt_connect_passkey,
		.len = sizeof(bt_connect_passkey)
	},
	{
		.name = "bt_connect_passkey_display",
		.type = PROP_STRING,
	},
	{
		.name = "bt_scan_results",
		.type = PROP_STRING,
		.send = app_bt_scan_results_send,
	},
	{
		.name = "bt_num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &bt_num_nodes,
		.len = sizeof(bt_num_nodes),
	},
	/****** Multi protocol properties ******/
	{
		.name = "num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &num_nodes,
		.len = sizeof(num_nodes),
	}
};

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int node_query_info_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result))
{
	ASSERT(node != NULL);
	if (node->interface == GI_WIFI) {
		log_err("do not support WiFi protocol node now");
		return -1;
	} else if (node->interface == GI_ZIGBEE) {
		return zb_query_info_handler(node, callback);
	} else if (node->interface == GI_ZWAVE) {
		log_err("do not support ZWave protocol node now");
		return -1;
	} else if (node->interface == GI_BLE) {
		return bt_query_info_handler(node, callback);
	} else {
		log_err("do not support %d protocol node", node->interface);
		return -1;
	}
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int node_configure_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result))
{
	ASSERT(node != NULL);
	if (node->interface == GI_WIFI) {
		log_err("do not support WiFi protocol node now");
		return -1;
	} else if (node->interface == GI_ZIGBEE) {
		return zb_configure_handler(node, callback);
	} else if (node->interface == GI_ZWAVE) {
		log_err("do not support ZWave protocol node now");
		return -1;
	} else if (node->interface == GI_BLE) {
		return bt_configure_handler(node, callback);
	} else {
		log_err("do not support %d protocol node", node->interface);
		return -1;
	}
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int node_prop_set_handler(struct node *node, struct node_prop *prop,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result))
{
	ASSERT(node != NULL);
	if (node->interface == GI_WIFI) {
		log_err("do not support WiFi protocol node now");
		return -1;
	} else if (node->interface == GI_ZIGBEE) {
		return zb_prop_set_handler(node, prop, callback);
	} else if (node->interface == GI_ZWAVE) {
		log_err("do not support ZWave protocol node now");
		return -1;
	} else if (node->interface == GI_BLE) {
		return bt_prop_set_handler(node, prop, callback);
	} else {
		log_err("do not support %d protocol node", node->interface);
		return -1;
	}
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int node_leave_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result))
{
	ASSERT(node != NULL);
	if (node->interface == GI_WIFI) {
		log_err("do not support WiFi protocol node now");
		return -1;
	} else if (node->interface == GI_ZIGBEE) {
		return zb_leave_handler(node, callback);
	} else if (node->interface == GI_ZWAVE) {
		log_err("do not support ZWave protocol node now");
		return -1;
	} else if (node->interface == GI_BLE) {
		return bt_leave_handler(node, callback);
	} else {
		log_err("do not support %d protocol node", node->interface);
		return -1;
	}
}

/*
 * Save node info to json object
 */
json_t *node_conf_save_handler(const struct node *node)
{
	ASSERT(node != NULL);
	if (node->interface == GI_WIFI) {
		log_err("do not support WiFi protocol node now");
		return NULL;
	} else if (node->interface == GI_ZIGBEE) {
		return zb_conf_save_handler(node);
	} else if (node->interface == GI_ZWAVE) {
		log_err("do not support ZWave protocol node now");
		return NULL;
	} else if (node->interface == GI_BLE) {
		return bt_conf_save_handler(node);
	} else {
		log_err("do not support %d protocol node", node->interface);
		return NULL;
	}
}

/*
 * Restore node info from json object
 */
int node_conf_loaded_handler(struct node *node, json_t *net_state_obj)
{
	ASSERT(node != NULL);
	if (node->interface == GI_WIFI) {
		log_err("do not support WiFi protocol node now");
		return -1;
	} else if (node->interface == GI_ZIGBEE) {
		return zb_conf_loaded_handler(node, net_state_obj);
	} else if (node->interface == GI_ZWAVE) {
		log_err("do not support ZWave protocol node now");
		return -1;
	} else if (node->interface == GI_BLE) {
		return bt_conf_loaded_handler(node, net_state_obj);
	} else {
		log_err("do not support %d protocol node", node->interface);
		return -1;
	}
}

/*
 * Initialize node network callback interface
 */
void appd_node_network_callback_init(void)
{
	struct node_network_callbacks network_callbacks = {
		.node_query_info = node_query_info_handler,
		.node_configure = node_configure_handler,
		.node_prop_set = node_prop_set_handler,
		.node_leave = node_leave_handler,
		.node_conf_save = node_conf_save_handler,
		.node_conf_loaded = node_conf_loaded_handler,
	};

	/* Setup generic node management hooks */
	node_set_network_callbacks(&network_callbacks);
}

/*
 * Hook for the app library to initialize the user-defined application.
 */
int appd_init(void)
{
	struct node_cloud_callbacks cloud_callbacks = {
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
	node_set_cloud_callbacks(&cloud_callbacks);
	appd_node_network_callback_init();

	timer_init(&zb_permit_join_timer, appd_zb_permit_join_timeout);

	/* Initialize local timer structures */
	timer_init(&bt_scan_stop_timer, appd_bt_scan_stop_timeout);
	timer_init(&bt_scan_result_timer, appd_bt_scan_result_timeout);

	return 0;
}

/*
 * Hook for the app library to start the user-defined application.  Once
 * This function returns, the app library will enable receiving updates from
 * the cloud, and begin to process tasks on the main thread.
 */
int appd_start(void)
{
	struct bt_callbacks bt_callbacks = {
		.scan_update = app_bt_scan_result_update,
		.passkey_request = app_bt_passkey_request,
		.passkey_display = app_bt_passkey_display,
		.passkey_display_clear = app_bt_passkey_display_clear
	};

	log_info("application starting");

	/*
	 * Set gateway template version to select the correct cloud template.
	 */
	app_set_template_version(appd_template_version);

	/* Start the ZigBee interface */
	if (zb_start() < 0) {
		log_err("zb_start returned error");
		return -1;
	}

	/* Start the Bluetooth interface */
	if (bt_start(&bt_callbacks) < 0) {
		log_err("bt_start returned error");
		return -1;
	}

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
	node_mgmt_exit();

	/* Stop ZigBee network stack */
	zb_exit();

	bt_stop();

	bt_cleanup();
}

/*
 * Function called during each main loop iteration.  This may be used to
 * perform routine tasks that are not linked to a specific timer or file event.
 */
void appd_poll(void)
{
	struct tag_num_nodes num;

	/* Handle network stack events */
	zb_poll();

	appd_update_num_nodes(&num);

	/* Post accurate node count to cloud */
	if (zb_num_nodes != num.zb_num_nodes) {
		zb_num_nodes = num.zb_num_nodes;
		prop_send_by_name("zb_num_nodes");
	}
	if (bt_num_nodes != num.bt_num_nodes) {
		bt_num_nodes = num.bt_num_nodes;
		prop_send_by_name("bt_num_nodes");
	}
	if (num_nodes != num.all_num_nodes) {
		num_nodes = num.all_num_nodes;
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
	 * XXX Be aware that node_mgmt_factory_reset() kicks off a
	 * potentially asynchronous operation, and if the factory reset
	 * results in appd being terminated, all nodes may not be reset.
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
