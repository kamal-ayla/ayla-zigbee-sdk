/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

/*
 * ZigBee Gateway Demo
 *
 * This gateway application hooks into a generic node management interface
 * and a simple ZigBee gateway implementation to demonstrate good practices
 * when creating a gateway.
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

#include "opkg.h"


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
#include "zb_interface.h"

#include <libgen.h>
#include <stdlib.h>
/* Maximum # of datapoints allowed in a batch */
#define APPD_MAX_BATCHED_DPS				64


const char *appd_version = "zb_gatewayd " BUILD_VERSION_LABEL;
const char *appd_template_version = "zigbee_gateway_demo_v2.1";

/* ZigBee protocol property states */
static struct timer zb_permit_join_timer;
static unsigned int zb_join_enable;
static unsigned int zb_change_channel;
static u8 zb_join_status;
static u8 zb_network_up;
static char zb_bind_cmd[PROP_STRING_LEN + 1];
static char zb_bind_result[PROP_STRING_LEN + 1];
static unsigned int num_nodes;

/* system info*/
static u8  get_sysinfo_status;
static unsigned int controller_status;
static char up_time[50];
#define GET_MESH_CONTROLLER_STATUS "uci get multiap.controller.enabled"
#define GET_DEVICE_UPTIME "/bin/get_sysinfo.sh"



/* Node property batch list */
static struct gw_node_prop_batch_list *node_batched_dps;


/*
 * Send the appd software version.
 */
static enum err_t appd_send_version(struct prop *prop, int req_id,
	const struct op_options *opts)
{
/**********************************************/
	pkg_t *pkg;
	setenv("OFFLINE_ROOT", "/", 0);
		log_debug(" after setenv \n");
	if (opkg_new()) {
		log_debug("opkg_new() failed.\n");
		print_error_list();
		return 1;
	}

	pkg = opkg_find_package("ayla-zigbee-sdk", NULL, NULL, NULL);
	char *v;
		if (pkg) {
				v = pkg_version_str_alloc(pkg);
				log_debug("Name:         %s\n"
					"Version:      %s\n",
					pkg->name,
					v);
			} else{
				log_debug("error finding package\n");
				return prop_val_send(prop, req_id, appd_version, 0, NULL);
			}


		char ayla_new_appd_version[100];

   		strcpy(ayla_new_appd_version,pkg->name);
   		strcat(ayla_new_appd_version, " - ");
   		strcat(ayla_new_appd_version, v);
		strcat(ayla_new_appd_version, "/");

		strcat(ayla_new_appd_version, appd_version);
		log_debug("*******************************%s\n", ayla_new_appd_version);
		free(v);
		opkg_free();

	   
/**********************************************/
	//return prop_val_send(prop, req_id, appd_version, 0, NULL);//David's code
	return prop_val_send(prop, req_id, ayla_new_appd_version, 0, NULL); //Added by Saritha for showing HW version on Dashboard	
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
 * Set zigbee channel
 */
static int appd_gw_change_channel_set(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
                log_err("prop_arg_set returned error");
                return -1;
        }

	/* set zigbee channel */

	if (zb_network_channel_change(zb_change_channel) < 0) {
		log_debug("set zigbee channel to %u failed", zb_change_channel);
                return -1;
	} else {
		log_debug("set zigbee channel to %u success", zb_change_channel);
	}

	prop_send_by_name("zb_change_channel");

	return 0;
}

/*
 *To get the sysinfo
 */
static int appd_sysinfo_set(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{
	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
			log_err("prop_arg_set returned error");
			return -1;
	}

	if (get_sysinfo_status) {
			prop_send_by_name("controller_status");
			prop_send_by_name("up_time");
			log_debug("get sysinfo success");
	}

	get_sysinfo_status = 0;
	prop_send_by_name("get_sysinfo_status");

	return 0;
}

/*
 * To get the controller status
 */
static enum err_t appd_controller_status_send(struct prop *prop, int req_id,
                  const struct op_options *opts)
{
	FILE *fp;

	fp = popen(GET_MESH_CONTROLLER_STATUS,"r");
	if (fp == NULL) {
		log_err("Mesh controller status get failed");
		exit(1);
	}
	fscanf(fp, "%d", &controller_status);
	pclose(fp);
	return prop_arg_send(prop, req_id, opts);

}

/*
 *To get the device uptime.
 */
static enum err_t appd_uptime_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
	FILE *fp;

	fp = popen(GET_DEVICE_UPTIME,"r");
	if (fp == NULL) {
		log_err("Get device uptime  failed");
		exit(1);
	}
	fscanf(fp, "%[^\n]", up_time);
	pclose(fp);
	return prop_arg_send(prop, req_id, opts);
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
 * Handle sending the num_nodes property.
 */
static enum err_t appd_num_nodes_send(struct prop *prop, int req_id,
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
		.name = "zb_change_channel",
		.type = PROP_INTEGER,
		.set = appd_gw_change_channel_set,
		.send = prop_arg_send,
		.arg = &zb_change_channel,
		.len = sizeof(zb_change_channel)
        },
	{
		.name = "num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &num_nodes,
		.len = sizeof(num_nodes),
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
		/* system information */
	{
		.name = "get_sysinfo_status",
		.type = PROP_BOOLEAN,
		.set = appd_sysinfo_set,
		.send = prop_arg_send,
		.arg = &get_sysinfo_status,
		.len = sizeof(get_sysinfo_status),
	},
	{
		.name = "controller_status",
		.type = PROP_BOOLEAN,
		.send = appd_controller_status_send,
		.arg = &controller_status,
		.len = sizeof(controller_status),
	},
	{
		.name = "up_time",
		.type = PROP_STRING,
		.send = appd_uptime_send,
		.arg = &up_time,
		.len = sizeof(up_time),
	}
};

/*
 * Initialize node network callback interface
 */
void appd_node_network_callback_init(void)
{
	struct node_network_callbacks network_callbacks = {
		.node_query_info = zb_query_info_handler,
		.node_configure = zb_configure_handler,
		.node_prop_set = zb_prop_set_handler,
		.node_leave = zb_leave_handler,
		.node_conf_save = zb_conf_save_handler,
		.node_conf_loaded = zb_conf_loaded_handler,
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

	/* Start the ZigBee interface */
	if (zb_start() < 0) {
		log_err("zb_start returned error");
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

}

/*
 * Function called during each main loop iteration.  This may be used to
 * perform routine tasks that are not linked to a specific timer or file event.
 */
void appd_poll(void)
{
	/* Handle network stack events */
	zb_poll();

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
