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
#include "libtransformer.h"
#include <inttypes.h>
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
#include "att/vt_interface.h"
#include "att/att_interface.h"

#include "pthread.h"
#include <libgen.h>
#include <stdlib.h>
/* Maximum # of datapoints allowed in a batch */
#define APPD_MAX_BATCHED_DPS				64


const char *appd_version = "zb_gatewayd " BUILD_VERSION_LABEL;
const char *appd_template_version = "zigbee_gateway_demo_v2.6";

/* ZigBee protocol property states */
static struct timer zb_permit_join_timer;
static struct timer ngrok_data_update_timer;
static unsigned int zb_join_enable;
static unsigned int zb_change_channel;
static u8 zb_join_status;
static u8 zb_network_up;
static char zb_bind_cmd[PROP_STRING_LEN + 1];
static char zb_bind_result[PROP_STRING_LEN + 1];
static unsigned int num_nodes;


/* system info*/
#define UPTIME_LEN						50
static u8  get_sysinfo_status;
static unsigned int controller_status;
static char board_model[100];
static char ram_usage[100];
static char cpu_usage[5];
static char up_time[UPTIME_LEN];
//#define GET_MESH_CONTROLLER_STATUS "uci get multiap.controller.enabled"
#define GET_MESH_CONTROLLER_STATUS_GCNT "uci get multiap.controller.enabled"
#define GET_MESH_CONTROLLER_STATUS_GDNT "uci get mesh_broker.mesh_common.controller_enabled"
#define BOARD_TYPE   "uci get version.@version[0].product"
#define GET_DEVICE_UPTIME "/bin/get_sysinfo.sh"
#define GET_RAM_FREE "transformer-cli get sys.mem.RAMFree | grep -o '[0-9]*'"
#define GET_RAM_USED "transformer-cli get sys.mem.RAMUsed | grep -o '[0-9]*'"
#define GET_RAM_TOTAL "transformer-cli get sys.mem.RAMTotal | grep -o '[0-9]*'"
#define GET_CURRENT_CPU_USAGE "transformer-cli get sys.proc.CurrentCPUUsage | grep -o '[0-9]*'"
#define GET_AYLA_VERSION "opkg list | grep ayla"

#define WIFI_STA_ADDR_LEN               50
extern char command[COMMAND_LEN];
extern char data[DATA_SIZE];
static pthread_t vnode_poll_thread = (pthread_t)NULL;
static unsigned int wifi_sta_info_update_period_min;

static int appd_check_wifi_sta_data_deviation(char *name, char *value);
static int appd_send_wifi_sta_data(char *name, char *value);

/* Station properties  */
static int wifi_sta_info_update;
static int wifi_sta_channel;
static int wifi_sta_noise;
static int wifi_sta_RSSI;
static char wifi_sta_associated_BSSID[WIFI_STA_ADDR_LEN];
static char wifi_sta_associated_SSID[WIFI_STA_ADDR_LEN];
static char gw_wifi_BSSID_fronthaul_5G[WIFI_STA_ADDR_LEN];
static char gw_wifi_BSSID_fronthaul_2G[WIFI_STA_ADDR_LEN];
static char gw_wifi_BSSID_backhaul[WIFI_STA_ADDR_LEN];
static char device_mac_address[WIFI_STA_ADDR_LEN];
static char em_parent_mac_address[WIFI_STA_ADDR_LEN];
static char em_bh_type[WIFI_STA_ADDR_LEN];


#define WIFI_STA_RSSI			"wifi_sta_RSSI"
#define WIFI_STA_NOISE			"wifi_sta_noise"
#define WIFI_STA_CHANNEL		"wifi_sta_channel"
#define WIFI_STA_ASSOCIATED_SSID	"wifi_sta_associated_SSID"
#define WIFI_STA_ASSOCIATED_BSSID	"wifi_sta_associated_BSSID"
#define GW_WIFI_BSSID_FRONTHAUL_5G      "gw_wifi_BSSID_fronthaul_5G"
#define GW_WIFI_BSSID_FRONTHAUL_2G      "gw_wifi_BSSID_fronthaul_2G"
#define GW_WIFI_BSSID_BACKHAUL       "gw_wifi_BSSID_backhaul"
#define DEVICE_MAC_ADDRESS		"device_mac_address"
#define EM_PARENT_MAC_ADDRESS           "em_parent_mac_address"
#define EM_BH_TYPE		        "em_bh_type"

#define WIFI_GET_STA_RSSI               "get_stainfo.sh -sta_rssi"
#define WIFI_GET_STA_NOISE              "get_stainfo.sh -sta_noise"
#define WIFI_GET_STA_CHANNEL            "get_stainfo.sh -sta_channel"
#define WIFI_GET_STA_ASSOCIATED_SSID    "get_stainfo.sh -sta_ssid"
#define WIFI_GET_STA_ASSOCIATED_BSSID   "get_stainfo.sh -sta_bssid"
#define GW_WIFI_GET_BSSID_FRONTHAUL_5G         "get_stainfo.sh -sta_bssid_fronthaul_5G"
#define GW_WIFI_GET_BSSID_FRONTHAUL_2G         "get_stainfo.sh -sta_bssid_fronthaul_2G"
#define GW_WIFI_GET_BSSID_BACKHAUL          "get_stainfo.sh -sta_bssid_backhaul"
//#define GET_MESH_CONTROLLER_STATUS       "get_stainfo.sh -sta_controller_status"
#define GET_DEVICE_MAC_ADDRESS		   "get_stainfo.sh -sta_device_mac"
#define GET_EM_PARENT_MAC_ADDRESS          "get_stainfo.sh -sta_parent_mac"
#define GET_EM_BH_TYPE             	   "get_stainfo.sh -sta_bh_type"

/* ngrok properties */
#define AUTH_COMMAND_LEN			80
#define NGROK_STATUS_LEN			30
#define SET_AUTHTOKEN_LEN			55
static char auth_command[AUTH_COMMAND_LEN];
static u8 ngrok_enable;
static int ngrok_port;
static char ngrok_status[NGROK_STATUS_LEN];
static char ngrok_hostname[NGROK_STATUS_LEN];
static char ngrok_set_authtoken[SET_AUTHTOKEN_LEN];
#define GET_AUTHTOKEN					"ngrok-cli -get_authtoken"
#define IS_NGROK_INSTALLED				"which ngrok; echo $?"
#define GET_NGROK_START					"ngrok-cli -start"
#define GET_NGROK_STOP					"ngrok-cli -stop"
#define GET_NGROK_STATUS				"ngrok-cli -status"
#define GET_NGROK_HOST_NAME				"ngrok-cli -host_name"
#define GET_NGROK_PORT_NUM				"ngrok-cli -port_num"
#define SET_NGROK_AUTHTOKEN				"ngrok-cli -set_authtoken %s"


/* Node property batch list */
static struct gw_node_prop_batch_list *node_batched_dps;

/*
 * Send the appd software version.
 */
static enum err_t appd_send_version(struct prop *prop, int req_id,
	const struct op_options *opts)
{
#if 0	
/**********************************************/
	pkg_t *pkg;
	setenv("OFFLINE_ROOT", "/", 0);
		log_debug(" after setenv\n");
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
		log_debug("*******************************%s\n", ayla_new_appd_version);
		free(v);
		opkg_free();

#endif

        FILE *fp;
        char ayla_new_appd_version[100];
        fp = popen(GET_AYLA_VERSION,"r");
        if (fp == NULL) {
                log_err("Ayla version get failed");
                exit(1);
        }
        fscanf(fp, "%[^\n]", ayla_new_appd_version);
        pclose(fp);
        log_debug("*******************************%s\n", ayla_new_appd_version);

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
	log_debug("**********************************appd_gw_join_enable_set******************************************");

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
	/*Form network if not already*/
	if(zb_network_up == 0){
		log_debug("**********************************appd_gw_join_enable_set zb_network_up is 0******************************************");
		if (zb_network_form() < 0) {
		log_debug(" zb_network_form failed");
		return -1;
		} else {
		log_debug("zb_network_form success");
	}

	}
	if(zb_network_up == 1){
		log_debug("**********************************appd_gw_join_enable_set zb_network_up is 1******************************************");

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
 *To get the sysinfo:
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
			prop_send_by_name("ram_usage");
			prop_send_by_name("cpu_usage");
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
	FILE *fp1;
	FILE *fp2;

        fp1 = popen(BOARD_TYPE,"r");
        if (fp1 == NULL) {
                log_err("Board Type Command failed");
                exit(1);
        }
        fscanf(fp1, "%s", board_model);
	pclose(fp1);


	if ( strcmp (board_model, "gcnt-5_extender_orion") == 0 ){


		fp = popen(GET_MESH_CONTROLLER_STATUS_GCNT,"r");
		if (fp == NULL) {
			log_err("Mesh controller status get failed");
			exit(1);
		}
		fscanf(fp, "%d", &controller_status);
		pclose(fp);
	}

	if ( strcmp (board_model, "gdnt-r_extender") == 0 ){

              fp2 = popen(GET_MESH_CONTROLLER_STATUS_GDNT,"r");
              if (fp2 == NULL) {
                      log_err("Mesh controller status get failed");
                       exit(1);
              }
              fscanf(fp2, "%d", &controller_status);
	      if ( controller_status > 1 ){
		      controller_status = 1;
	      }
	      pclose(fp2);
        }

	
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
 *To check whether ngrok is installed in device.
 */

static int appd_is_ngrok_installed(void)
{
	FILE *fp;
	int status = 0;

	fp = popen(IS_NGROK_INSTALLED,"r");
	if (fp == NULL) {
		log_err("Failed to get ngrok installed status");
	} else {
		fscanf(fp, "%d", &status);
		pclose(fp);
	}

	return status;
}

/*
 *To get the authtoken.
 */
static int appd_ngrok_send_authtoken(void)
{
	FILE *fp;
	char get_authtoken[SET_AUTHTOKEN_LEN];

	fp = popen(GET_AUTHTOKEN,"r");
	if(fp == NULL || appd_is_ngrok_installed()) {
		log_err("Failed to get authtoken");
		strcpy(ngrok_set_authtoken, "0");
	} else {
		fscanf(fp, "%[^\n]", get_authtoken);
		strcpy(ngrok_set_authtoken,get_authtoken);
	}

	prop_send_by_name("ngrok_set_authtoken");
	return 0;
}



/*
 *To get the ngrok info
 */
static int appd_ngrok_enable(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return -1;
	}

	if (ngrok_enable == 1) {
		system(GET_NGROK_START);
		timer_set(app_get_timers(), &ngrok_data_update_timer, 2000);
	} else if (ngrok_enable == 0) {
		system(GET_NGROK_STOP);
		timer_set(app_get_timers(), &ngrok_data_update_timer, 2000);
	} else {
		log_debug("get ngrok_info failed");
	}
	return 0;
}

/*
 *Ngrok data update timer
 */

static void appd_ngrok_data_update(struct timer *timer_ngrok_update)
{
	timer_cancel(app_get_timers(), timer_ngrok_update);

	log_debug("Updating the Ngrok data");
	prop_send_by_name("ngrok_status");
	prop_send_by_name("ngrok_hostname");
	prop_send_by_name("ngrok_port");
	appd_ngrok_send_authtoken();
}

/*
 *To get the Ngrok Hostname.
 */
static enum err_t appd_ngrok_hostname_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
	FILE *fp;

	fp = popen(GET_NGROK_HOST_NAME,"r");
	if (fp == NULL || appd_is_ngrok_installed()) {
		log_err("Get ngrok hostname failed");
		strcpy(ngrok_hostname, "0");
	} else {
		fscanf(fp, "%[^\n]", ngrok_hostname);
		pclose(fp);
	}

	return prop_arg_send(prop, req_id, opts);

}

/*
 *To get the Ngrok Port.
 */
static enum err_t appd_ngrok_port_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
	FILE *fp;

	fp = popen(GET_NGROK_PORT_NUM,"r");
	if (fp == NULL || appd_is_ngrok_installed()) {
		log_err("Get ngrok port failed");
		ngrok_port = 0;
		ngrok_enable = 0;
		prop_send_by_name("ngrok_enable");
	} else {
		fscanf(fp, "%d", &ngrok_port);
		if (ngrok_port == 0) {
			ngrok_enable = 0;
			prop_send_by_name("ngrok_enable");
		}
		pclose(fp);
	}
	return prop_arg_send(prop, req_id, opts);

}

/*
 *To get the Ngrok Status.
 */
static enum err_t appd_ngrok_status_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
	FILE *fp;

	fp = popen(GET_NGROK_STATUS,"r");
	if (fp == NULL || appd_is_ngrok_installed()) {
		log_err("Get ngrok status failed");
		strcpy(ngrok_status , "ngrok not installed");
	} else {
		fscanf(fp, "%[^\n]", ngrok_status);
		pclose(fp);
	}
	return prop_arg_send(prop, req_id, opts);

}

/*
 *Set ngrok authtoken from cloud.
 */
static int appd_ngrok_set_authtoken(struct prop *prop, const void *val,
				size_t len, const struct op_args *args)
{
	FILE *fp;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return -1;
	}

	if(strlen(ngrok_set_authtoken) >= 10) {
		snprintf(auth_command, sizeof(auth_command), SET_NGROK_AUTHTOKEN,
				ngrok_set_authtoken);

		fp = popen(auth_command, "r");
		if (fp == NULL) {
			log_err("set ngrok authtoken failed");
			exit(1);
		}
		pclose(fp);
	} else {
		log_err("invalid authtoken");
	}
	return 0;
}

/*
 * Set Wifi sta_info update period
 */

static int appd_get_wifi_sta_info_update(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{


	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return -1;
        }

	if (wifi_sta_info_update < WIFI_STA_MIN_UPDATE_PERIOD_MINS) {
		wifi_sta_info_update = WIFI_STA_MIN_UPDATE_PERIOD_MINS;
		log_debug("wifi_sta_info_update value is out of range");
	} else if (wifi_sta_info_update > WIFI_STA_MAX_UPDATE_PERIOD_MINS) {
		wifi_sta_info_update = WIFI_STA_MAX_UPDATE_PERIOD_MINS;
		log_debug("wifi_sta_info_update value is out of range");
	} else if (wifi_sta_info_update == 0) {
		wifi_sta_info_update = WIFI_STA_DEFAULT_UPDATE_PERIOD_MINS;
		log_debug("wifi_sta_info_update value set to deafult period");
	} else {
		;
	}

	prop_send_by_name("set_wifi_stainfo_update_min");

	att_set_poll_period(wifi_sta_info_update);

	return 0;
}


void appd_wifi_sta_poll()
{

	if (wifi_sta_info_update == 0) {
		 wifi_sta_info_update = WIFI_STA_DEFAULT_UPDATE_PERIOD_MINS;
		 prop_send_by_name("set_wifi_stainfo_update_min");
		 att_set_poll_period(wifi_sta_info_update);
	}

	sprintf(command, WIFI_GET_STA_RSSI);
	exec_systemcmd(command, data, DATA_SIZE);
	if (appd_check_wifi_sta_data_deviation(WIFI_STA_RSSI, data)) {
		appd_send_wifi_sta_data(WIFI_STA_RSSI, data);
	}

	sprintf(command, WIFI_GET_STA_NOISE);
	exec_systemcmd(command, data, DATA_SIZE);
	if (appd_check_wifi_sta_data_deviation(WIFI_STA_NOISE, data)) {
		appd_send_wifi_sta_data(WIFI_STA_NOISE, data);
        }


	if (wifi_sta_info_update_period_min >= wifi_sta_info_update || wifi_sta_info_update_period_min == 0) {
		sprintf(command, WIFI_GET_STA_RSSI);
		exec_systemcmd(command, data, DATA_SIZE);
		appd_send_wifi_sta_data(WIFI_STA_RSSI, data);

		sprintf(command, WIFI_GET_STA_NOISE);
		exec_systemcmd(command, data, DATA_SIZE);
		appd_send_wifi_sta_data(WIFI_STA_NOISE, data);

		wifi_sta_info_update_period_min = 0;
	}

	wifi_sta_info_update_period_min += 1;

	sprintf(command, WIFI_GET_STA_CHANNEL);
	exec_systemcmd(command, data, DATA_SIZE);
	appd_send_wifi_sta_data(WIFI_STA_CHANNEL, data);

	sprintf(command, WIFI_GET_STA_ASSOCIATED_SSID);
	exec_systemcmd(command, data, DATA_SIZE);
	appd_send_wifi_sta_data(WIFI_STA_ASSOCIATED_SSID, data);

	sprintf(command, WIFI_GET_STA_ASSOCIATED_BSSID);
	exec_systemcmd(command, data, DATA_SIZE);
	appd_send_wifi_sta_data(WIFI_STA_ASSOCIATED_BSSID, data);
	
	sprintf(command, GW_WIFI_GET_BSSID_FRONTHAUL_5G);
	exec_systemcmd(command, data, DATA_SIZE);
	appd_send_wifi_sta_data(GW_WIFI_BSSID_FRONTHAUL_5G, data);

        sprintf(command, GW_WIFI_GET_BSSID_FRONTHAUL_2G);
        exec_systemcmd(command, data, DATA_SIZE);
        appd_send_wifi_sta_data(GW_WIFI_BSSID_FRONTHAUL_2G, data);

	sprintf(command, GW_WIFI_GET_BSSID_BACKHAUL);
	exec_systemcmd(command, data, DATA_SIZE);
	appd_send_wifi_sta_data(GW_WIFI_BSSID_BACKHAUL, data);

	sprintf(command, GET_DEVICE_MAC_ADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);
        appd_send_wifi_sta_data(DEVICE_MAC_ADDRESS, data);

        sprintf(command, GET_EM_PARENT_MAC_ADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);
        appd_send_wifi_sta_data(EM_PARENT_MAC_ADDRESS, data);

        sprintf(command, GET_EM_BH_TYPE);
        exec_systemcmd(command, data, DATA_SIZE);
        appd_send_wifi_sta_data(EM_BH_TYPE, data);	
}


static int appd_check_wifi_sta_data_deviation(char *name, char *value)
{
        int tmp = 0;
	int deviation = 0;

        if (!strcmp(name, WIFI_STA_NOISE)) {

		tmp = atoi(value);

                deviation = abs(abs(tmp) - abs(wifi_sta_noise));

                if ( deviation >= ATT_DATA_DEVIATION_DB) {
                        return 1;
                } else {
                        return 0;
                }

        } else if (!strcmp(name, WIFI_STA_RSSI)) {

		tmp = atoi(value);

                deviation = abs(abs(tmp) - abs(wifi_sta_RSSI));

                if ( deviation >= ATT_DATA_DEVIATION_DB) {
                        return 1;
                } else {
                        return 0;
                }
        }

	return 0;
}


static int appd_send_wifi_sta_data(char *name, char *value)
{
	int tmp = 0;

	if (!strcmp(name, WIFI_STA_CHANNEL)) {

		tmp = atoi(value);

		if (tmp == wifi_sta_channel) {
			return 0;
		}

		wifi_sta_channel = tmp;

		prop_send_by_name(name);

	} else if (!strcmp(name, WIFI_STA_NOISE)) {

                tmp = atoi(value);

                if (tmp == wifi_sta_noise) {
                      return 0;
                }

                wifi_sta_noise = tmp;

		prop_send_by_name(name);

	} else if (!strcmp(name, WIFI_STA_RSSI)) {

                tmp = atoi(value);

                if (tmp == wifi_sta_RSSI) {
                        return 0;
                }

                wifi_sta_RSSI = tmp;

		prop_send_by_name(name);
        
	} else if (!strcmp(name, WIFI_STA_ASSOCIATED_SSID)) {
       
       		if (!strcmp(value, wifi_sta_associated_SSID)) {
                        return 0;
                }
                strncpy(wifi_sta_associated_SSID, value, WIFI_STA_ADDR_LEN);
                wifi_sta_associated_SSID[WIFI_STA_ADDR_LEN - 1] = '\0';

		prop_send_by_name(name);
        
	} else if (!strcmp(name, WIFI_STA_ASSOCIATED_BSSID)) {

                if (!strcmp(value, wifi_sta_associated_BSSID)) {
                        return 0;
                }
                strncpy(wifi_sta_associated_BSSID, value, WIFI_STA_ADDR_LEN);
                wifi_sta_associated_BSSID[WIFI_STA_ADDR_LEN - 1] = '\0'; 

		prop_send_by_name(name);
	} else if (!strcmp(name, GW_WIFI_BSSID_FRONTHAUL_5G)) {

                if (!strcmp(value, gw_wifi_BSSID_fronthaul_5G)) {
                        return 0;
                }
                strncpy(gw_wifi_BSSID_fronthaul_5G, value, WIFI_STA_ADDR_LEN);
                gw_wifi_BSSID_fronthaul_5G[WIFI_STA_ADDR_LEN - 1] = '\0'; 

		prop_send_by_name(name);
	} else if (!strcmp(name, GW_WIFI_BSSID_FRONTHAUL_2G)) {

                if (!strcmp(value, gw_wifi_BSSID_fronthaul_2G)) {
                        return 0;
                }
                strncpy(gw_wifi_BSSID_fronthaul_2G, value, WIFI_STA_ADDR_LEN);
                gw_wifi_BSSID_fronthaul_2G[WIFI_STA_ADDR_LEN - 1] = '\0'; 

		prop_send_by_name(name);		
	} else if (!strcmp(name, GW_WIFI_BSSID_BACKHAUL)) {

                if (!strcmp(value, gw_wifi_BSSID_backhaul)) {
                        return 0;
                }
                strncpy(gw_wifi_BSSID_backhaul, value, WIFI_STA_ADDR_LEN);
                gw_wifi_BSSID_backhaul[WIFI_STA_ADDR_LEN - 1] = '\0'; 

		prop_send_by_name(name);
	} else if (!strcmp(name, DEVICE_MAC_ADDRESS)) {

                if (!strcmp(value, device_mac_address)) {
                        return 0;
                }
                strncpy(device_mac_address, value, WIFI_STA_ADDR_LEN);
               	device_mac_address[WIFI_STA_ADDR_LEN - 1] = '\0';

                prop_send_by_name(name);
        }else if (!strcmp(name, EM_PARENT_MAC_ADDRESS)) {

                if (!strcmp(value, em_parent_mac_address)) {
                        return 0;
                }
                strncpy(em_parent_mac_address, value, WIFI_STA_ADDR_LEN);
                em_parent_mac_address[WIFI_STA_ADDR_LEN - 1] = '\0';

                prop_send_by_name(name);
	}else if (!strcmp(name, EM_BH_TYPE)) {

                if (!strcmp(value, em_bh_type)) {
                        return 0;
                }
                strncpy(em_bh_type, value, WIFI_STA_ADDR_LEN);
                em_bh_type[WIFI_STA_ADDR_LEN - 1] = '\0';

                prop_send_by_name(name);
	}
	

	return 0;
}

/*
 *To get the device cpu_usage.
 */
static enum err_t appd_cpu_usage_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
	FILE *fp;
	static unsigned int cpusage;
	char buffer[5];
	fp = popen(GET_CURRENT_CPU_USAGE,"r");
	if (fp == NULL) {
		log_err("Ram usage get failed");
		exit(1);
	}
	fscanf(fp, "%d", &cpusage);
	pclose(fp);
	sprintf(buffer,"%d",cpusage);
	strcpy(cpu_usage,buffer);
	strcat(cpu_usage,"%");
	log_debug(" cpu_usage is :%s\n", cpu_usage);
	return prop_arg_send(prop, req_id, opts);
}


/*
 *To get the device ram_usage.
 */
static enum err_t appd_ram_usage_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{

	FILE *fp;
	static unsigned int ram_mb, ram_kb;
	char buffer[10];
	fp = popen(GET_RAM_FREE,"r");
	if (fp == NULL) {
		log_err("Ram usage get failed");
		exit(1);
	}
	fscanf(fp, "%d", &ram_kb);
	pclose(fp);
	ram_mb = ram_kb / 1024;
    
    // Displaying output
    log_debug("***********************************%d Kilobytes = %d Megabytes", ram_kb, ram_mb);
    strcpy(ram_usage,"Free=");
	sprintf(buffer,"%d",ram_mb);
	strcat(ram_usage,buffer);
	strcat(ram_usage,"MB");

	fp = popen(GET_RAM_USED,"r");
	if (fp == NULL) {
		log_err("Ram usage get failed");
		exit(1);
	}
	fscanf(fp, "%d", &ram_kb);
	pclose(fp);
	ram_mb = ram_kb / 1024;
    
    // Displaying output
    log_debug("***********************************%d Kilobytes = %d Megabytes", ram_kb, ram_mb);
    strcat(ram_usage," Used=");
	sprintf(buffer,"%d",ram_mb);
	strcat(ram_usage,buffer);
	strcat(ram_usage,"MB");

		fp = popen(GET_RAM_TOTAL,"r");
	if (fp == NULL) {
		log_err("Ram usage get failed");
		exit(1);
	}
	fscanf(fp, "%d", &ram_kb);
	pclose(fp);
	ram_mb = ram_kb / 1024;
    
    // Displaying output
    log_debug("***********************************%d Kilobytes = %d Megabytes", ram_kb, ram_mb);
    strcat(ram_usage," Total=");
	sprintf(buffer,"%d",ram_mb);
	strcat(ram_usage,buffer);
	strcat(ram_usage,"MB");
    log_debug(" ram_usage is :%s\n", ram_usage);

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
	},
	{	.name = "set_wifi_stainfo_update_min",
		.type = PROP_INTEGER,
		.set = appd_get_wifi_sta_info_update,
		.send = prop_arg_send,
		.arg = &wifi_sta_info_update,
		.len = sizeof(wifi_sta_info_update),
	},
	/*wifi Station  properties*/
	{
		.name = "wifi_sta_channel",
		.type = PROP_INTEGER,
		.send = prop_arg_send,
		.arg = &wifi_sta_channel,
		.len = sizeof(wifi_sta_channel),
	},
	{
		.name = "wifi_sta_noise",
		.type = PROP_INTEGER,
		.send = prop_arg_send,
		.arg = &wifi_sta_noise,
		.len = sizeof(wifi_sta_noise),
	},
	{
		.name = "wifi_sta_RSSI",
		.type = PROP_INTEGER,
		.send = prop_arg_send,
		.arg = &wifi_sta_RSSI,
		.len = sizeof(wifi_sta_RSSI),
	},
	{
		.name = "wifi_sta_associated_BSSID",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &wifi_sta_associated_BSSID,
		.len = sizeof(wifi_sta_associated_BSSID),
	},
	{
		.name = "wifi_sta_associated_SSID",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &wifi_sta_associated_SSID,
		.len = sizeof(wifi_sta_associated_SSID),
	},
	
	{
		.name = "gw_wifi_BSSID_fronthaul_5G",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &gw_wifi_BSSID_fronthaul_5G,
		.len = sizeof(gw_wifi_BSSID_fronthaul_5G),
	},
	
	{
		.name = "gw_wifi_BSSID_fronthaul_2G",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &gw_wifi_BSSID_fronthaul_2G,
		.len = sizeof(gw_wifi_BSSID_fronthaul_2G),
	},	
	
	{
		.name = "gw_wifi_BSSID_backhaul",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = &gw_wifi_BSSID_backhaul,
		.len = sizeof(gw_wifi_BSSID_backhaul),
	},

        {
                .name = "device_mac_address",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &device_mac_address,
                .len = sizeof(device_mac_address),
        },

        {
                .name ="em_parent_mac_address",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &em_parent_mac_address,
                .len = sizeof(em_parent_mac_address),
        },

        {
                .name = "em_bh_type",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &em_bh_type,
                .len = sizeof(em_bh_type),
        },	
		/*  ngrok properties */
	{
		.name = "ngrok_enable",
		.type = PROP_BOOLEAN,
		.set = appd_ngrok_enable,
		.send = prop_arg_send,
		.arg = &ngrok_enable,
		.len = sizeof(ngrok_enable),
	},
	{
		.name = "ngrok_status",
		.type = PROP_STRING,
		.send = appd_ngrok_status_send,
		.arg = &ngrok_status,
		.len = sizeof(ngrok_status),
	},
	{
		.name = "ngrok_hostname",
		.type = PROP_STRING,
		.send = appd_ngrok_hostname_send,
		.arg = &ngrok_hostname,
		.len = sizeof(ngrok_hostname),
	},
	{
		.name = "ngrok_port",
		.type = PROP_INTEGER,
		.send = appd_ngrok_port_send,
		.arg = &ngrok_port,
		.len = sizeof(ngrok_port),
	},
	{
		.name = "ngrok_set_authtoken",
		.type = PROP_STRING,
		.set = appd_ngrok_set_authtoken,
		.send = prop_arg_send,
		.arg = &ngrok_set_authtoken,
		.len = sizeof(ngrok_set_authtoken),
	},
	{
		.name = "ram_usage",
		.type = PROP_STRING,
		.send = appd_ram_usage_send,
		.arg = &ram_usage,
		.len = sizeof(ram_usage),
	},
	{
		.name = "cpu_usage",
		.type = PROP_STRING,
		.send = appd_cpu_usage_send,
		.arg = &cpu_usage,
		.len = sizeof(cpu_usage),
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
        if (node->interface == GI_ZIGBEE) {
                return zb_query_info_handler(node, callback);
        } else if (node->interface == GI_VT) {
                return vt_query_info_handler(node, callback);
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
        if (node->interface == GI_ZIGBEE) {
                return zb_configure_handler(node, callback);
        } else if (node->interface == GI_VT) {
                return vt_configure_handler(node, callback);
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
        if (node->interface == GI_ZIGBEE) {
                return zb_prop_set_handler(node, prop, callback);
        } else if (node->interface == GI_VT) {
                return vt_prop_set_handler(node, prop, callback);
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
        if (node->interface == GI_ZIGBEE) {
                return zb_leave_handler(node, callback);
        } else if (node->interface == GI_VT) {
                return vt_leave_handler(node, callback);
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
        if (node->interface == GI_ZIGBEE) {
                return zb_conf_save_handler(node);
        } else if (node->interface == GI_VT) {
                return vt_conf_save_handler(node);
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
        if (node->interface == GI_ZIGBEE) {
                return zb_conf_loaded_handler(node, net_state_obj);
        } else if (node->interface == GI_VT) {
                return vt_conf_loaded_handler(node, net_state_obj);
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
	timer_init(&ngrok_data_update_timer, appd_ngrok_data_update);

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

	node_mgmt_clear_vnodes(VNODE_OEM_MODEL, &att_node_left_handler);

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

static void vnode_poll_thread_fun(void) 
{
	 while(1) {
		 att_poll();

		 appd_wifi_sta_poll();

		 sleep(60);
	 }
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


	if (!vnode_poll_thread) {
		if (pthread_create(&vnode_poll_thread, NULL, (void *)&vnode_poll_thread_fun, NULL)) {
            		pthread_cancel(vnode_poll_thread);
		}
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
