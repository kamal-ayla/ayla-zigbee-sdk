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
#include <ctype.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/build.h>
#include <ayla/utypes.h>
#include <ayla/http.h>
#include "libtransformer.h"
#include <inttypes.h>
#include <ayla/json_parser.h>
#include <regex.h>

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
#include <dirent.h>

/* Maximum # of datapoints allowed in a batch */
#define APPD_MAX_BATCHED_DPS				64


const char *appd_version = "zb_gatewayd " BUILD_VERSION_LABEL;
const char *appd_template_version = "zigbee_gateway_demo_v4.4";

/* ZigBee protocol property states */
static struct timer zb_permit_join_timer;
static struct timer ngrok_data_update_timer;
static unsigned int zb_join_enable;
static unsigned int zb_change_channel;
static u8 zb_join_status;
static u8 zb_network_up;
static char zb_bind_cmd[PROP_STRING_LEN + 1];
static char zb_bind_result[PROP_STRING_LEN + 1];
static unsigned int zb_num_nodes;

/* add info*/
static unsigned int num_nodes;
static unsigned int bh_num_nodes;


/* system info*/
#define UPTIME_LEN						50
static u8  get_sysinfo_status;
static unsigned int controller_status;
static unsigned int mesh_controller_status;
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
#define COMMAND_LENGTH	100
extern char command[COMMAND_LENGTH];
extern char data[DATA_SIZE];
static pthread_t vnode_poll_thread = (pthread_t)NULL;
static unsigned int wifi_sta_info_update_period_min;

static int appd_check_wifi_sta_data_deviation(char *name, char *value);
static int appd_send_wifi_sta_data(char *name, char *value);
static int appd_is_ngrok_installed(void);
int appd_mesh_controller_status();
/* Station properties  */
static int wifi_sta_info_update;
static int wifi_sta_channel;
static int wifi_sta_noise;
static int wifi_sta_RSSI;
static int gw_wifi_txop_5g;
static int gw_wifi_txop_2g;
static char wifi_sta_associated_BSSID[WIFI_STA_ADDR_LEN];
static char wifi_sta_associated_SSID[WIFI_STA_ADDR_LEN];
static char gw_wifi_BSSID_fronthaul_5G[WIFI_STA_ADDR_LEN];
static char gw_wifi_BSSID_fronthaul_2G[WIFI_STA_ADDR_LEN];
static char gw_wifi_BSSID_backhaul[WIFI_STA_ADDR_LEN];
static char device_mac_address[WIFI_STA_ADDR_LEN];
static char em_parent_mac_address[WIFI_STA_ADDR_LEN];
static char em_backhaul_type[WIFI_STA_ADDR_LEN];
static char gw_agent_almac_address[WIFI_STA_ADDR_LEN];
static char gw_ctrl_almac_address[WIFI_STA_ADDR_LEN];

#define WIFI_STA_RSSI			"wifi_sta_RSSI"
#define WIFI_STA_NOISE			"wifi_sta_noise"
#define WIFI_STA_CHANNEL		"wifi_sta_channel"
#define WIFI_STA_ASSOCIATED_SSID	"wifi_sta_associated_SSID"
#define WIFI_STA_ASSOCIATED_BSSID	"wifi_sta_associated_BSSID"
#define GW_WIFI_BSSID_FRONTHAUL_5G      "gw_wifi_BSSID_fronthaul_5G"
#define GW_WIFI_BSSID_FRONTHAUL_2G      "gw_wifi_BSSID_fronthaul_2G"
#define GW_WIFI_BSSID_BACKHAUL          "gw_wifi_BSSID_backhaul"
#define DEVICE_MAC_ADDRESS		"device_mac_address"
#define EM_PARENT_MAC_ADDRESS           "em_parent_mac_address"
#define EM_BACKHAUL_TYPE		"em_backhaul_type"
#define GW_AGENT_ALMAC_ADDRESS          "gw_agent_almac_address"
#define GW_CTRL_ALMAC_ADDRESS           "gw_ctrl_almac_address"
#define GW_WIFI_TXOP_5G			"gw_wifi_txop_5g"
#define GW_WIFI_TXOP_2G			"gw_wifi_txop_2g"

#define WIFI_GET_STA_RSSI               "get_stainfo.sh -sta_rssi"
#define WIFI_GET_STA_NOISE              "get_stainfo.sh -sta_noise"
#define WIFI_GET_STA_CHANNEL            "get_stainfo.sh -sta_channel"
#define WIFI_GET_STA_ASSOCIATED_SSID    "get_stainfo.sh -sta_ssid"
#define WIFI_GET_STA_ASSOCIATED_BSSID   "get_stainfo.sh -sta_bssid"
#define GW_WIFI_GET_BSSID_FRONTHAUL_5G  "get_stainfo.sh -sta_bssid_fronthaul_5G"
#define GW_WIFI_GET_BSSID_FRONTHAUL_2G  "get_stainfo.sh -sta_bssid_fronthaul_2G"
#define GW_WIFI_GET_BSSID_BACKHAUL      "get_stainfo.sh -sta_bssid_backhaul"
#define GET_DEVICE_MAC_ADDRESS		"get_stainfo.sh -sta_device_mac"
#define GET_EM_PARENT_MAC_ADDRESS       "get_stainfo.sh -sta_parent_mac"
#define GET_EM_BACKHAUL_TYPE            "get_stainfo.sh -sta_bh_type"
#define GET_GW_AGENT_ALMAC_ADDRESS      "get_stainfo.sh -sta_agent_almac"
#define GET_GW_CTRL_ALMAC_ADDRESS       "get_stainfo.sh -sta_controller_almac"
#define GET_GW_WIFI_TXOP_5G		"get_stainfo.sh -sta_txop_5g"
#define GET_GW_WIFI_TXOP_2G		"get_stainfo.sh -sta_txop_2g"

#define CHANNEL_COMMAND_LEN 80
static int channel_2ghz;
static int channel_5ghz;
unsigned int channel_2g_tmp;
unsigned int channel_5g_tmp;
static char channel_command[CHANNEL_COMMAND_LEN];

#define GET_TWO_GHZ_CHANNEL_VALUE "uci get wireless.radio1.channel"
#define GET_FIVE_GHZ_CHANNEL_VALUE "uci get wireless.radio0.channel"
#define SET_TWO_GHZ_CHANNEL_VALUE "uci set wireless.radio1.channel=%d"
#define SET_FIVE_GHZ_CHANNEL_VALUE "uci set wireless.radio0.channel=%d"
#define UCI_COMMIT "uci commit"
#define RESTART_WLAN_MGR "/etc/init.d/wlan_mgr restart"


/* backhaul optimization */
#define OPTIMIZE_COMMAND_LEN 80
static unsigned int bh_optimization;
static char bh_optimization_command[OPTIMIZE_COMMAND_LEN];

#define BH_OPTIMIZE "uci set smartmesh.sm_steering.bhsta_optimization=%d"
#define GET_BH_OPTIMIZATION "uci get smartmesh.sm_steering.bhsta_optimization"


/* channel scanning properties */
//Multi-channel scanning 
#define CHANNEL_SCAN_CMD_LEN 80
static unsigned int multi_channel_scan;
static char multi_channel_buf[CHANNEL_SCAN_CMD_LEN];

#define MULTI_CHANNEL_SCAN_CMD "uci set smartmesh.sm_agent.multi_channel_support=%d"
#define GET_MULTI_CHANNEL_SCAN "uci get smartmesh.sm_agent.multi_channel_support"


//Single channel scanning
static unsigned int single_channel_scan;
static char single_channel_buf[CHANNEL_SCAN_CMD_LEN];

#define SINGLE_CHANNEL_SCAN_CMD "uci set smartmesh.sm_agent.channel_scan_capability=%d"
#define GET_SINGLE_CHANNEL_SCAN "uci get smartmesh.sm_agent.channel_scan_capability"



/* ssid and key properties */
#define SSID_LEN 80
#define KEY_LEN 80

// 5GHZ variables
static char ssid_5ghz[SSID_LEN];
static char ssid_key_5ghz[KEY_LEN];
//static char ssid_5ghz_get[SSID_LEN];

// 2GHZ variables
static char ssid_2ghz[SSID_LEN];
static char ssid_key_2ghz[KEY_LEN];
//static char ssid_2ghz_get[SSID_LEN];

#define TWO_GHZ_SET_SSID "uci set mesh_broker.cred0.ssid=\"%s\""
#define TWO_GHZ_SET_KEY "uci set mesh_broker.cred0.wpa_psk_key=\"%s\""
#define FIVE_GHZ_SET_SSID "uci set mesh_broker.cred1.ssid=\"%s\""
#define FIVE_GHZ_SET_KEY "uci set mesh_broker.cred1.wpa_psk_key=\"%s\""
#define RESTART_MESH_BROKER "/etc/init.d/mesh-broker restart"

#define TWO_GHZ_GET_SSID  "uci get mesh_broker.cred0.ssid"
#define FIVE_GHZ_GET_SSID "uci get mesh_broker.cred1.ssid"
#define TWO_GHZ_GET_KEY   "uci get mesh_broker.cred0.wpa_psk_key"
#define FIVE_GHZ_GET_KEY  "uci get mesh_broker.cred1.wpa_psk_key"

/* whitelist property */
#define WHITELIST_LEN 100

//whitelist variable
#define GW_WHITELIST_STATE                 "gw_whitelist_state"
#define GW_WHITELIST_ACTIVE                "gw_whitelist_active"
#define GW_WHITELIST_BSSID		   "gw_whitelist_bssid"
#define GW_WHITELIST_MAC_ADDR              "gw_whitelist_mac_address"
static char whitelist_cmd_buf[WHITELIST_LEN];
static char whitelist_mac_addr[20];
static char gw_whitelist_bssid[20];
static int gw_whitelist_state;
static int gw_whitelist_active;
#define WHITELIST_CMD "ubus call wireless.supplicant.whitelist connect '{\"name\":\"wl0\",\"bssid\":\"%s\"}'"
#define WHITELIST_CLEAR "ubus call wireless.supplicant.whitelist clear '{\"name\":\"wl0\"}'"
#define WHITELIST_ACTIVE "ubus call wireless.supplicant.whitelist get | grep \"whitelist_active\" | awk '{print $2}' | cut -b 1"
#define WHITELIST_BSSID "ubus call wireless.supplicant.whitelist get | grep \"whitelist_bssid\" | awk '{print $2}' | cut -b 2-18"
#define WHITELIST_BSS_ID "ubus call wireless.supplicant.whitelist get | grep \"bssid\" | awk '{print $2}' | cut -b 2-18"
#define WHITELIST_STATE "ubus call wireless.supplicant.whitelist get | grep \"whitelist_state\" | awk '{print $2}' | cut -b 1"
static int appd_properties_get(void);
static int appd_update_whitelist_get();
static int appd_ngrok_update(void);

/* Backhaul STA  */

static u8 gw_wifi_bh_sta;
static int gw_wifi_bh_apscan;
static int gw_wifi_bh_bss_state;
#define BACKHAUL_BSS_STATE "wl -i wl0 bss"
#define SET_BACKHAUL_STA_ENABLE "ubus call wireless.supplicant.apscan set '{\"name\":\"wl0\",\"state\":\"1\"}'"
#define SET_BACKHAUL_STA_DISABLE "ubus call wireless.supplicant.apscan set '{\"name\":\"wl0\",\"state\":\"0\"}'"
#define GET_BACKHAUL_STA "ubus call wireless.supplicant.apscan get '{\"name\":\"wl0\"}' | grep \"ap_scan_mode\" | awk '{print $2}'"
static int appd_update_wifi_bh_sta();

/* ngrok properties */
#define AUTH_COMMAND_LEN			80
#define NGROK_STATUS_LEN			30
#define SET_AUTHTOKEN_LEN			55
static char auth_command[AUTH_COMMAND_LEN];
static u8 ngrok_enable;
static int ngrok_port;
static unsigned int timeout_flag;
static unsigned int ngrok_update_counter;
static char ngrok_status[NGROK_STATUS_LEN];
static char ngrok_error_status[NGROK_STATUS_LEN];
static char ngrok_hostname[NGROK_STATUS_LEN];
static char ngrok_set_authtoken[SET_AUTHTOKEN_LEN];
#define GET_AUTHTOKEN					"ngrok-cli -get_authtoken"
#define IS_NGROK_INSTALLED				"which ngrok; echo $?"
#define GET_NGROK_START					"ngrok-cli -start"
#define GET_NGROK_STOP					"ngrok-cli -stop"
#define GET_NGROK_STATUS				"ngrok-cli -status"
#define GET_NGROK_ERROR_STATUS                          "ngrok-cli -start 2>&1 | grep ERR_NGROK | awk '/ERROR/ {print $2}'"
#define GET_NGROK_HOST_NAME				"ngrok-cli -host_name"
#define GET_NGROK_PORT_NUM				"ngrok-cli -port_num"
#define SET_NGROK_AUTHTOKEN				"ngrok-cli -set_authtoken %s"

/* Schedule Reboot */
#define SCHEDULE_REBOOT                         	50
#define CLEAR_CRON					"crontab -l | grep -v '/bin/gw_schedule_reboot.sh' | crontab -"
#define SCHEDULE_RBT_REASON                             "transformer-cli set rpc.system.scheduledrebootreason \"GUI\""
#define SCHEDULE_RBT_APPLY                              "transformer-cli apply"
static char schedule_reboot[SCHEDULE_REBOOT];

/* WPS button */
#define WPS_BUTTON_LEN                                  50
static char gw_led_status[WPS_BUTTON_LEN];
static int gw_wps_button;
static struct timer gw_led_status_timer;

/* Firmware properties **/
#define RADIO_FW_LEN                                    50
static char radio1_fw_version[RADIO_FW_LEN];
static char radio2_fw_version[RADIO_FW_LEN];
static char radio0_fw_version[RADIO_FW_LEN];

/*Network up time command*/
static char network_up_time[40];
#define GET_NETWORK_UP_TIME				"get_stainfo.sh -backhaul_nw_up_time"

/* Node property batch list */
static struct gw_node_prop_batch_list *node_batched_dps;

/* file location of the latest value of file_up */
static char file_upload_path[512];


/* data convert to the UPPER CASE */
void uppercase_convert(char str[])
{
    int i;
    for(i=0; str[i]!='\0'; i++)
    {
        if(str[i]>='a' && str[i]<='z')
        {
            str[i] = toupper(str[i]);
        }
    }
    strcpy(data,str);
}

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

/* Get whitelist state, active in init */

static int appd_update_whitelist_get()
{
	char tmp_string[20];
	int tmp;
        FILE *fp;

	tmp = gw_whitelist_active;
	fp = popen(WHITELIST_ACTIVE,"r");
        if (fp == NULL) {
                log_err("IOT_DEBUG poll get: whitelist active failed");
                gw_whitelist_active=0;
        } else {
                fscanf(fp, "%d", &gw_whitelist_active);
        }
        pclose(fp);

        log_debug("IOT_DEBUG: appd update whitelist active %d::tmp : %d",gw_whitelist_active,tmp);

        if (tmp != gw_whitelist_active) {
		prop_send_by_name("gw_whitelist_active");
		log_debug("IOT_DEBUG : Change in  gw_whitelist_active");
        }

	tmp=gw_whitelist_state;
        fp = popen(WHITELIST_STATE,"r");
        if (fp == NULL) {
                log_err("IOT_DEBUG: appd update whitelist state failed");
                gw_whitelist_state = 0;
        } else {
                fscanf(fp, "%d", &gw_whitelist_state);

        }
        pclose(fp);
	log_debug("IOT_DEBUG: appd update whitelist state %d tmp %d",gw_whitelist_state,tmp);

        if (tmp != gw_whitelist_state) {	
		log_debug("IOT_DEBUG : change in gw_whitelist_state");
		prop_send_by_name("gw_whitelist_state");
        }

	strcpy(tmp_string,gw_whitelist_bssid);
        fp = popen(WHITELIST_BSSID,"r");
        if (fp == NULL) {
                log_err("IOT_DEBUG: appd update whitelist bssid failed");
                strcpy(gw_whitelist_bssid,"00:00:00:00:00:00");
        } else {
                fscanf(fp, "%[^\n]", gw_whitelist_bssid);
		log_debug("IOT_DEBUG: gw_whitelist_bssid poll %s",gw_whitelist_bssid);
		if(strlen(gw_whitelist_bssid)==0){
			strcpy(gw_whitelist_bssid,"00:00:00:00:00:00");
		}
        }
        pclose(fp);
        log_debug("IOT_DEBUG: appd update whitelist bssid %s",gw_whitelist_bssid);
        if(strlen(gw_whitelist_bssid)==17){
		if (strcmp(tmp_string,gw_whitelist_bssid)) {
			log_debug("IOT_DEBUG: appd update different mac address whitelist_bssid_previous_mac_addr %s gw_whitelist_bssid %s",tmp_string,gw_whitelist_bssid);
			prop_send_by_name("gw_whitelist_bssid");

                }
        }
        else {
		log_debug("IOT_DEBUG: Invalid mac length");
        }

	return 0;

}


static int appd_update_wifi_bh_sta()
{
        FILE *fp;
        char bss_state[10];
	int tmp=0;

	tmp=gw_wifi_bh_apscan;
	log_debug("IOT_DEBUG: GET_BACKHAUL_STA %s",GET_BACKHAUL_STA);
                fp = popen(GET_BACKHAUL_STA,"r");
                if (fp == NULL) {
                        log_err("Erro : get backhaul enable ");
                } else {
                        fscanf(fp, "%d", &gw_wifi_bh_apscan);
                }
                log_debug("IOT_DEBUG: appd update wifi bh STA apscan %d tmp %d",gw_wifi_bh_apscan,tmp);
                pclose(fp);

                if (tmp != gw_wifi_bh_apscan) {
			log_debug("IOT_DEBUG : appd update wifi bh sta change in gw_wifi_bh_poll_apscan");
			prop_send_by_name("gw_wifi_bh_apscan");
                }

                        log_debug("IOT_DEBUG: BACKHAUL_BSS_STATE %s",BACKHAUL_BSS_STATE);
                        fp = popen(BACKHAUL_BSS_STATE,"r");
                        if (fp == NULL) {
                                log_err("IOT_DEBUG: ERROR Backhaul BSS state ");
                                strcpy(bss_state,"NULL");
                        } else {
                                fscanf(fp,"%[^\n]",bss_state);
                        }
                        pclose(fp);
			tmp=gw_wifi_bh_bss_state;
                        log_debug("IOT_DEBUG: backhaul bss state %s",bss_state);
                        if(!strcmp(bss_state,"up")){
                                log_debug("IOT_DEBUG: gw wifi bh bss state is up");
                                gw_wifi_bh_bss_state=1;
                        }
                        else if(!strcmp(bss_state,"down")){
                                log_debug("IOT_DEBUG: gw wifi bss state is down");
                                gw_wifi_bh_bss_state=0;
                        }
			else{
				log_debug("IOT_DEBUG : BH BSS STATE ERROR");
			}
			
			log_debug("IOI_DEBUG: tmp %d gw_wifi_bh_bss_state %d",tmp,gw_wifi_bh_bss_state);

                	if (tmp != gw_wifi_bh_bss_state) {
				log_debug("IOT_DEBUG : appd wifi bh STA change in gw_wifi_bh_bss_state");
				prop_send_by_name("gw_wifi_bh_bss_state");
        	        }

			

	return 0;

}


/*
 * To Initalize the properties
 */
void appd_prop_init()
{
	prop_send_by_name("controller_status");
	prop_send_by_name("up_time");
	prop_send_by_name("radio1_fw_version");
        prop_send_by_name("radio2_fw_version");
        prop_send_by_name("radio0_fw_version");
	appd_properties_get();
}

static int appd_ngrok_update(void)
{


#if 1  
   FILE *fp;	
   FILE *fp1;
   FILE *fp2;
   FILE *fp3;

   unsigned int tmp;
   char tmp_ngrok[80];

   memset(tmp_ngrok,'\0',sizeof(tmp_ngrok));
   strcpy(tmp_ngrok,ngrok_status);

   log_debug("IOT_DEBUG : GET_NGROK_STATUS %s",GET_NGROK_STATUS);
   fp = popen(GET_NGROK_STATUS,"r");
   if (fp == NULL || appd_is_ngrok_installed()) {
      log_err("Get ngrok status failed");
      strcpy(ngrok_status , "ngrok not installed");
   } else {
      memset(ngrok_status,'\0',sizeof(ngrok_status));	   
      fscanf(fp, "%[^\n]", ngrok_status);
      log_debug("Get ngrok_status value : %s during sysinfo tmp %s",ngrok_status,tmp_ngrok);
      if ( strcmp(tmp_ngrok, ngrok_status ) ) {
           prop_send_by_name("ngrok_status");
      }      
   }
   pclose(fp);
   
   memset(tmp_ngrok,'\0',sizeof(tmp_ngrok));
   strcpy(tmp_ngrok,ngrok_hostname);

   log_debug("IOT_DEBUG: GET_NGROK_HOST_NAME");
   fp1 = popen(GET_NGROK_HOST_NAME,"r");
   if (fp1 == NULL || appd_is_ngrok_installed()) {
      log_err("Get ngrok hostname failed");
       strcpy(ngrok_hostname, "0");
   } else {
       memset(ngrok_hostname,'\0',sizeof(ngrok_hostname));
       fscanf(fp1, "%[^\n]", ngrok_hostname);
       log_debug("Get ngrok_hostname value : %s  during sysinfo",ngrok_hostname);
       if ( strcmp(tmp_ngrok, ngrok_hostname ) ) {
           prop_send_by_name("ngrok_hostname");
       }       
   }
   pclose(fp1);

   tmp = ngrok_port;
   
   log_debug("IOT_DEBUG; GET_NGROK_PORT_NUM");
   fp2 = popen(GET_NGROK_PORT_NUM,"r");
   if (fp2 == NULL || appd_is_ngrok_installed()) {
      log_err("Get ngrok port failed");
      ngrok_port=0;
   } else {
      fscanf(fp2, "%d", &ngrok_port);
      log_debug("Get ngrok_port value : %d during sysinfo",ngrok_port);
      if(tmp != ngrok_port) {
	 prop_send_by_name("ngrok_port");
      }
   }
   pclose(fp2);

   memset(tmp_ngrok,' ',sizeof(tmp_ngrok));
   if ( strcmp(ngrok_error_status, "") ) {
       strcpy(tmp_ngrok,ngrok_error_status);
   }
  
 
   log_debug("IOT_DEBUG : GET_NGROK_ERROR_STATUS status %s:: %d",ngrok_status,ngrok_enable);
   if((!strcmp(ngrok_status,"ngrok not running")) && (ngrok_enable == 1) && ngrok_port!=0){
   fp3 = popen(GET_NGROK_ERROR_STATUS,"r");
   if (fp3 == NULL || appd_is_ngrok_installed()) {
       log_err("Get ngrok error status failed");
       strcpy(ngrok_error_status , "ngrok not installed");
   } else {
       memset(ngrok_error_status, ' ', sizeof(ngrok_error_status));
       fscanf(fp3, "%[^\n]", ngrok_error_status);
       log_debug("Get ngrok error status  : %s during sysinfo",ngrok_error_status);

       if(strcmp(ngrok_error_status, "") == 0 ) {
          memset(ngrok_error_status, ' ', sizeof(ngrok_error_status));
          strcpy(ngrok_error_status , "NONE");
	  log_debug("Get ngrok error status  1: %s during sysinfo",ngrok_error_status);
       }
       
       if ( strcmp(tmp_ngrok, ngrok_error_status) ) {
           prop_send_by_name("ngrok_error_status");
       }
   }
   pclose(fp3);
   }
   if(!strcmp(ngrok_status,"ngrok not installed")){
	prop_send_by_name("ngrok_port");
	prop_send_by_name("ngrok_status");
   	prop_send_by_name("ngrok_hostname");	   

   }
#endif

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
      prop_send_by_name("radio1_fw_version");
      prop_send_by_name("radio2_fw_version");
      prop_send_by_name("radio0_fw_version");
      prop_send_by_name("gw_wifi_bh_uptime");
      prop_send_by_name("gw_led_status");
      
      if(appd_is_ngrok_installed > 0) {
         appd_ngrok_update();
      }
      else {
	prop_send_by_name("ngrok_status");
      }
      appd_properties_get();
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

	int tmp;
	tmp=controller_status;
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

	if(tmp==controller_status){
		return 0;
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
 *To get the ngrok info
 */
static int appd_ngrok_enable(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{

        if (prop_arg_set(prop, val, len, args) != ERR_OK) {
                log_err("prop_arg_set returned error");
                return -1;
        }

	FILE *fp;

        fp = popen(GET_NGROK_STATUS,"r");
        if (fp == NULL || appd_is_ngrok_installed()) {
                log_err("Get ngrok status failed");
                strcpy(ngrok_status , "ngrok not installed");
        } else {
                fscanf(fp, "%[^\n]", ngrok_status);
        }
        pclose(fp);

        if ((strcmp(ngrok_status,"ngrok not running") == 0) && ngrok_enable == 1) {

                memset(command,'\0',sizeof(command));
                memset(data,'\0',sizeof(data));
                sprintf(command, GET_NGROK_STOP);
                exec_systemcmd(command, data, DATA_SIZE);

                memset(command,'\0',sizeof(command));
                memset(data,'\0',sizeof(data));
                sprintf(command, GET_NGROK_START);
                exec_systemcmd(command, data, DATA_SIZE);
        } else if ((strcmp(ngrok_status,"ngrok not running") == 0) && ngrok_enable == 0) {
                 log_debug("ngrok status : ngrok not running and ngrok enable set to 0");
        } else if ((strcmp(ngrok_status,"ngrok running") == 0) && ngrok_enable == 1) {
                 log_debug("ngrok status: ngrok running and ngrok enable set to 1");
        } else if ((strcmp(ngrok_status,"ngrok running") == 0) && ngrok_enable == 0) {

                memset(command,'\0',sizeof(command));
                memset(data,'\0',sizeof(data));
                sprintf(command, GET_NGROK_STOP);
                exec_systemcmd(command, data, DATA_SIZE);
                log_debug("ngrok status : ngrok running and ngrok enable set to 0");
        } else {
                log_debug("get ngrok_info failed");
        }
	timer_set(app_get_timers(), &ngrok_data_update_timer, 4000);
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
        
	FILE *fp;

	fp = popen(GET_NGROK_PORT_NUM,"r");
        if (fp == NULL || appd_is_ngrok_installed()) {
                log_err("Get ngrok port failed");
        } else {
                fscanf(fp, "%d", &ngrok_port);
                log_debug("Get ngrok_port value in appd_ngrok_data_update : %d",ngrok_port);
        
                pclose(fp);
       }

        if ((strcmp(ngrok_status,"ngrok running") == 0) && ngrok_port == 0 && (ngrok_update_counter < 4)){
		timer_set(app_get_timers(), &ngrok_data_update_timer, 2000);
		log_debug("port 0 adding again & ngrok_update_counter = %d",ngrok_update_counter);
		ngrok_update_counter++;
        } else {
		timeout_flag = 0;
                ngrok_update_counter = 0;
                log_debug("either number of iterations completed or port number available");
		
		prop_send_by_name("ngrok_port");
		prop_send_by_name("ngrok_hostname");
		
		if ((ngrok_enable == 1) && (( ngrok_port == 0) || ( strcmp(ngrok_hostname, "io timeout\"") == 0))) {
			timeout_flag = 1;
			log_debug(" ngrok port number : %d, host name : %s",ngrok_port,ngrok_hostname);
			prop_send_by_name("ngrok_error_status");
		}
		else {
			log_debug(" Available port number : %d, host name : %s",ngrok_port,ngrok_hostname);
			prop_send_by_name("ngrok_error_status");
		}
	}    
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
		log_debug("Get ngrok_hostname value : %s",ngrok_hostname);
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
	} else {
		fscanf(fp, "%d", &ngrok_port);
		log_debug("Get ngrok_port value : %d",ngrok_port);
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
 *To get the Ngrok Error Status.
 */
static enum err_t appd_ngrok_error_status_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
        FILE *fp;

   if((strcmp(ngrok_status,"ngrok not running") == 0) && (ngrok_enable == 1)){

   fp = popen(GET_NGROK_ERROR_STATUS,"r");
      if (fp == NULL || appd_is_ngrok_installed()) {
          log_err("Get ngrok error status failed");
          strcpy(ngrok_error_status , "ngrok not installed");
      } else {
          memset(ngrok_error_status, ' ', sizeof(ngrok_error_status));
          fscanf(fp, "%[^\n]", ngrok_error_status);

          if(strcmp(ngrok_error_status, "") == 0 ) {
              memset(ngrok_error_status, ' ', sizeof(ngrok_error_status));
              strcpy(ngrok_error_status , "NONE");
          } 
      }
      pclose(fp);
   }
   else {
      memset(ngrok_error_status, ' ', sizeof(ngrok_error_status));
      strcpy(ngrok_error_status , "NONE");
      log_debug("ngrok error status : %s",ngrok_error_status);
   }

   if ((timeout_flag == 1 ) && (strcmp(ngrok_error_status, "NONE") == 0)) {
	   
      memset(ngrok_error_status, ' ', sizeof(ngrok_error_status));
      strcpy(ngrok_error_status , "TIMEOUT");
      log_debug("timeout flag :%d, and ngrok error status : %s",timeout_flag, ngrok_error_status);
      timeout_flag = 0;
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

        snprintf(auth_command, sizeof(auth_command), SET_NGROK_AUTHTOKEN, ngrok_set_authtoken);
	
	fp = popen(auth_command, "r");
	if (fp == NULL) {
           log_err("set ngrok authtoken failed");
	   exit(1);
	}
	pclose(fp);
	
	return 0;
}


/*
 * backhaul STA Disable
 *
 */
static int appd_backhaul_sta(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{
	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
                        log_err("prop_arg_set returned error");
                        return -1;
        }
	FILE *fp;
	
	unsigned int status;
	status = appd_mesh_controller_status();

	if((status == 0)) {// && (gw_wifi_bh_apscan!=gw_wifi_bh_sta)){
	log_debug("IOT_DEBUG: gw_wifi_bh_sta %d",gw_wifi_bh_sta);
	if(gw_wifi_bh_sta){
		log_debug("IOT_DEBUG: SET_BACKHAUL_STA %s",SET_BACKHAUL_STA_ENABLE);
		fp = popen(SET_BACKHAUL_STA_ENABLE,"r");
		if (fp == NULL) {
			log_err("IOT_DEBUG: Error set backhaul enable ");
		}
		pclose(fp);

	}
	else {
		log_debug("IOT_DEBUG: SET_BACKHAUL_STA_DISABLE %s",SET_BACKHAUL_STA_DISABLE);
		fp = popen(SET_BACKHAUL_STA_DISABLE,"r");
                if (fp == NULL) {
                        log_err("Error set backhaul disable ");
                }
                pclose(fp);
                
		}
	}
	return 0;
}

/*
 *WPS data update timer & gw led status update
 */

static void appd_gw_wps_status_update(struct timer *timer_gw_led_status)
{
        timer_cancel(app_get_timers(), timer_gw_led_status);
	gw_wps_button=0;
        prop_send_by_name("gw_wps_button");
        log_debug("Timer wps button");
        prop_send_by_name("gw_led_status");
}

/*
 *To get the wps status.
 */
static enum err_t appd_gw_led_status_send(struct prop *prop, int req_id,
                   const struct op_options *opts)
{
        long int red,red_delay,orange,orange_delay,green,green_delay;
        char status[50];
        log_debug("GW led status");
        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command,"cat /sys/class/leds/wps\\:red/brightness");
        exec_systemcmd(command, data, DATA_SIZE);
        red=atoi(data);
        //log_debug("Red %d",red);

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command,"cat /sys/class/leds/wps\\:red/delay_on");
        exec_systemcmd(command, data, DATA_SIZE);
        red_delay=atoi(data);
        //log_debug("Red_delay %d",red_delay);

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command,"cat /sys/class/leds/wps\\:orange/brightness");
        exec_systemcmd(command, data, DATA_SIZE);
        orange=atoi(data);
        //log_debug("Orange %d",orange);

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command,"cat /sys/class/leds/wps\\:orange/delay_on");
        exec_systemcmd(command, data, DATA_SIZE);
        orange_delay=atoi(data);
        //log_debug("orange_delay %d",orange_delay);

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command,"cat /sys/class/leds/wps\\:green/brightness");
        exec_systemcmd(command, data, DATA_SIZE);
        green=atoi(data);
        //log_debug("green %d",green);

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command,"cat /sys/class/leds/wps/\\:green/delay_on");
        exec_systemcmd(command, data, DATA_SIZE);
        green_delay=atoi(data);
        //log_debug("Green_delay %d",green_delay);

        sprintf(status,"%s_%s,%s_%s,%s_%s",(red==255) ? "RED" : "0",(red_delay==1000) ? "HIGH" : (red_delay==250) ? "SHORT" : "0"  ,(orange==255) ? "ORANGE" : "0",(orange_delay==1000) ? "HIGH" : (orange_delay==250) ? "SHORT" : "0",(green==255)?"GREEN":"0",(green_delay==1000) ? "HIGH" : (green_delay==250) ? "SHORT" : "0" );
	if(!strcmp(gw_led_status,status)){
		log_debug("IOT_DEBUG: gw_led_status there is no change in status");
		return 0;
	}
        strcpy(gw_led_status , status);
        return prop_arg_send(prop, req_id, opts);

}


/*
 * To Know the mesh controller status
 */
int appd_mesh_controller_status()
{
    FILE *fp;
    fp = popen(GET_MESH_CONTROLLER_STATUS_GDNT,"r");
    if (fp == NULL) {
            log_err("Mesh controller status get failed");
            exit(1);
       }
       fscanf(fp, "%d", &mesh_controller_status);
       if ( mesh_controller_status > 1 ){
                mesh_controller_status = 1;
       }
       pclose(fp);

       return mesh_controller_status;

}

/*
 *Set 2ghz channel value from cloud.
 */
static int appd_channel_2ghz(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{
   FILE *fp;
   FILE *fp1;
   FILE *fp2;

   unsigned int status;
   int channel;

   if (prop_arg_set(prop, val, len, args) != ERR_OK) {
      log_err("prop_arg_set returned error");
      return -1;
   }

   // To get 2ghz channel value
   fp = popen(GET_TWO_GHZ_CHANNEL_VALUE,"r");
   if (fp == NULL) {
      log_err("Get 2ghz channel failed");
      exit(1);
   } else {
      fscanf(fp, "%d", &channel);
      log_debug("get 2ghz channel value for verification: %d",channel);
   }
   pclose(fp);

   status=appd_mesh_controller_status();

   if(status == 1 && channel != channel_2ghz)
   {
      if(channel_2ghz > 0 && channel_2ghz < 12){
         log_debug("set channel 2ghz : %d",channel_2ghz);
         snprintf(channel_command, sizeof(channel_command), SET_TWO_GHZ_CHANNEL_VALUE, channel_2ghz);
         fp = popen(channel_command, "r");
         if (fp == NULL) {
            log_err("set 2 ghz channel value failed");
            exit(1);
         }
         pclose(fp);
         fp1 = popen(UCI_COMMIT, "r");
         if( fp1 == NULL) {
            log_err("uci commit command failed");
            exit(1);
         }
         pclose(fp1);
         fp2 = popen(RESTART_WLAN_MGR, "r");
         if( fp2 == NULL) {
            log_err("restart wlan mgr failed");
            exit(1);
         }
         pclose(fp2);
      }
      else {
             log_debug("Failed to set Invalid entry for channel 2ghz");
      }
   }
   else {
	   log_debug("set channel 2ghz failed : Either gateway configured as an agent or tried to set with exisitng value !!!");
   }
   
   // To get 2ghz channel value
   fp = popen(GET_TWO_GHZ_CHANNEL_VALUE,"r");
   if (fp == NULL) {
      log_err("Get 2ghz channel failed");
      exit(1);
   } else {
      fscanf(fp, "%d", &channel_2ghz);
      log_debug("get 2ghz channel value : %d",channel_2ghz);
      prop_send_by_name("gw_wifi_channel_2G");
   }
   pclose(fp);
	
   return 0;
}

/*
 *Set 5ghz channel value from cloud.
 */
static int appd_channel_5ghz(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{
 
   FILE *fp;
   FILE *fp1;
   FILE *fp2;
   
   unsigned int status;
   int channel;
   unsigned int channels[]={36,40,44,48,52,56,60,64,100,104,108,112,116,132,136,149,153,157,161};
   unsigned int check_flag = 0;
   unsigned int size = 0;
   unsigned int i=0;
   if (prop_arg_set(prop, val, len, args) != ERR_OK) {
      log_err("prop_arg_set returned error");
      return -1;
   }

   // To get 5ghz channel value
   fp = popen(GET_FIVE_GHZ_CHANNEL_VALUE,"r");
   if (fp == NULL) {
      log_err("Get 5ghz channel failed");
      exit(1);
   } else {
      fscanf(fp, "%d", &channel);
      log_debug("get 5ghz channel value for verification : %d",channel);
   }
   pclose(fp);
   
   status=appd_mesh_controller_status();

   if(status == 1 && channel != channel_5ghz)
   {
      log_debug("set channel for 5ghz : %d",channel_5ghz);
   
      size=sizeof(channels)/sizeof(channels[0]);

      for(i=0;i<size;i++){
         if(channel_5ghz == channels[i])
         {
            log_debug("matched value with one of the allocated channel : %d\n",channels[i]);
	    check_flag = 1;
	 }
      }

      if(check_flag == 1){

         snprintf(channel_command, sizeof(channel_command), SET_FIVE_GHZ_CHANNEL_VALUE, channel_5ghz);
         fp = popen(channel_command, "r");
	 if (fp == NULL) {
	   log_err("set 2 ghz channel value failed");
           exit(1);
	 }
	 pclose(fp);
         fp1 = popen(UCI_COMMIT, "r");
         if( fp1 == NULL) {
           log_err("uci commit command failed");
           exit(1);
	 }
         pclose(fp1);
         fp2 = popen(RESTART_WLAN_MGR, "r");
         if( fp2 == NULL) {
            log_err("restart wlan mgr failed");
            exit(1);
	}
	pclose(fp2);
     }
     else {
            log_debug("Failed to set Invalid entry for channel 5ghz");
     }
   }
   else	{
	   log_debug("set channel 5ghz failed : Either gateway configured as an agent or tried to set with existing value!!!");
   }
	
   // To get 5ghz channel value
   fp = popen(GET_FIVE_GHZ_CHANNEL_VALUE,"r");
   if (fp == NULL) {
      log_err("Get 5ghz channel failed");
      exit(1);
   } else {
             fscanf(fp, "%d", &channel_5ghz);
             log_debug("get 5ghz channel value : %d",channel_5ghz);
             prop_send_by_name("gw_wifi_channel_5G");
  }
  pclose(fp);
	
  return 0;
}


/*
 *enable/disable backhaul optimization from cloud.
 */

static int appd_bh_optimization(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{

   FILE *fp;
   FILE *fp1;
   FILE *fp2;
   FILE *fp3;

   int optimize;

   if(prop_arg_set(prop, val, len, args) != ERR_OK) {
       log_err("prop_arg_set returned error");
       return -1;
   }

   // To get backhaul optimization value
   fp3 = popen(GET_BH_OPTIMIZATION,"r");
   if(fp3 == NULL) {
      log_err("Get backhaul optimization failed");
      exit(1);
   } else {
      fscanf(fp3, "%d", &optimize);
      log_debug("backhaul optimization enable/disable : %d",optimize);
   }
   pclose(fp3);
   

   if(bh_optimization > 1) {
       bh_optimization = 1;
   }

   if(optimize != bh_optimization) {

     log_debug("backhaul optmization set value : %d",bh_optimization);

     snprintf(bh_optimization_command, sizeof(bh_optimization_command), BH_OPTIMIZE, bh_optimization);

     fp = popen(bh_optimization_command, "r");
     if(fp == NULL) {
        log_err("enable/disable backhaul optimization command failed");
        exit(1);
     }
     pclose(fp);

     fp1 = popen(UCI_COMMIT, "r");
     if(fp1 == NULL) {
        log_err("uci commit command failed");
        exit(1);
     }
     pclose(fp1);

     fp2 = popen(RESTART_MESH_BROKER, "r");
     if(fp2 == NULL) {
        log_err("restart mesh broker failed");
        exit(1);
     }
     pclose(fp2);
  }
  else {
	log_debug("backhaul optimization failed due to tried with exisitng value to set in the device !!!");
  }

  // To get backhaul optimization value
  fp3 = popen(GET_BH_OPTIMIZATION,"r");
  if(fp3 == NULL) {
     log_err("Get backhaul optimization failed");
     exit(1);
  } else {
     fscanf(fp3, "%d", &bh_optimization);
     log_debug("backhaul optimization enable/disable : %d",bh_optimization);
  }
  pclose(fp3);
  prop_send_by_name("gw_wifi_bh_optimization");    

  return 0;
}


/*
 *enable/disable multi channel scan from cloud.
 */

static int appd_multi_channel_scan(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{
  FILE *fp;
  FILE *fp1;
  FILE *fp2;
  FILE *fp3;

  int multi;

  if(prop_arg_set(prop, val, len, args) != ERR_OK) {
     log_err("prop_arg_set returned error");
     return -1;
  }
  
  if(multi_channel_scan > 1) {
     multi_channel_scan = 1;
  }

  // To get multi channel value
  fp3 = popen(GET_MULTI_CHANNEL_SCAN,"r");
  if(fp3 == NULL) {
     log_err("Get multi channel scan failed");
     exit(1);
  } else {
     fscanf(fp3, "%d", &multi);
     log_debug("Get multi channel scan value for verification : %d",multi);
  }
  pclose(fp3);
   

  if(multi != multi_channel_scan) {

     log_debug("Multi channel scan set value : %d",multi_channel_scan);

     snprintf(multi_channel_buf, sizeof(multi_channel_buf), MULTI_CHANNEL_SCAN_CMD, multi_channel_scan);

     fp = popen(multi_channel_buf, "r");
     if (fp == NULL) {
        log_err("enable/disable multi channel scan command failed");
        exit(1);
     }
     pclose(fp);
   
     fp1 = popen(UCI_COMMIT, "r");
     if(fp1 == NULL) {
        log_err("uci commit command failed");
        exit(1);
     }
     pclose(fp1);

     fp2 = popen(RESTART_MESH_BROKER, "r");
     if(fp2 == NULL) {
        log_err("restart mesh broker failed");
        exit(1);
     }
     pclose(fp2);
  }
  else {
	  log_debug("multi channel scan failed due to tried with existing value to set in the device !!!");
  }
   
  // To get multi channel value
  fp3 = popen(GET_MULTI_CHANNEL_SCAN,"r");
  if(fp3 == NULL) {
     log_err("Get multi channel scan failed");
     exit(1);
  } else {
     fscanf(fp3, "%d", &multi_channel_scan);
     log_debug("Get multi channel scan value : %d",multi_channel_scan);
  }
  pclose(fp3);
  prop_send_by_name("gw_wifi_multi_channel_scan");
      
  return 0;
}

/*
 *enable/disable single channel scan from cloud.
 */

static int appd_single_channel_scan(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{

	
  FILE *fp;
  FILE *fp1;
  FILE *fp2;
  FILE *fp3;

  unsigned int single;

  if(prop_arg_set(prop, val, len, args) != ERR_OK) {
     log_err("prop_arg_set returned error");
     return -1;
  }

  if(single_channel_scan > 1) {
     single_channel_scan = 1;
  }

  // To get single channel value
  fp3 = popen(GET_SINGLE_CHANNEL_SCAN,"r");
  if(fp3 == NULL) {
     log_err("Get single channel scan failed");
     exit(1);
  } else {
     fscanf(fp3, "%d", &single);
     log_debug("Get single channel scan value for verification: %d",single);
  }
  pclose(fp3);

  if(single != single_channel_scan) {
  
     log_debug("Single channel scan set value : %d",single_channel_scan);

     snprintf(single_channel_buf, sizeof(single_channel_buf), SINGLE_CHANNEL_SCAN_CMD, single_channel_scan);

     fp = popen(single_channel_buf, "r");
     if (fp == NULL) {
        log_err("enable/disable single channel scan command failed");
        exit(1);
     }
     pclose(fp);
     fp1 = popen(UCI_COMMIT, "r");
     if(fp1 == NULL) {
        log_err("uci commit command failed");
        exit(1);
     }
     pclose(fp1);

     fp2 = popen(RESTART_MESH_BROKER, "r");
     if(fp2 == NULL) {
        log_err("restart mesh broker failed");
        exit(1);
     }
     pclose(fp2);
  }
  else {
	  log_debug("single channel scan failed due to tried with exisitng value to set in the device!!!");
  }
  
  // To get single channel value
  fp3 = popen(GET_SINGLE_CHANNEL_SCAN,"r");
  if(fp3 == NULL) {
     log_err("Get single channel scan failed");
     exit(1);
  } else {
     fscanf(fp3, "%d", &single_channel_scan);
     log_debug("Get single channel scan value : %d",single_channel_scan);
  }
  pclose(fp3);
  prop_send_by_name("gw_wifi_single_channel_scan");
   
  return 0;
}



/*
 *Set 2GHZ SSID  from cloud.
 */
static int appd_ssid_2ghz(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{

   FILE *fp;
   FILE *fp1;

   char two_ghz_ssid_command[150];
   unsigned int validate;
   unsigned int status;
   char ssid[50];

   memset(two_ghz_ssid_command,'\0',sizeof(two_ghz_ssid_command));

   if (prop_arg_set(prop, val, len, args) != ERR_OK) {
      log_err("prop_arg_set returned error");
     return -1;
  }

  // To get 2ghz ssid
  fp = popen(TWO_GHZ_GET_SSID,"r");
  if (fp == NULL) {
     log_err("Get 2ghz ssid  failed");
     exit(1);
  } else {
     memset(ssid,'\0',sizeof(ssid));
     fscanf(fp, "%[^\n]", ssid);
     log_debug("ssid 2ghz get value for verification : %s",ssid);
  }
  pclose(fp);
	
  status=appd_mesh_controller_status();

  if(status == 1 && strcmp(ssid,ssid_2ghz) != 0) {
     validate = strlen(ssid_2ghz);
     log_debug("set ssid 2ghz value : %s and size of the value : %d",ssid_2ghz,validate);

     if(validate  > 0 && validate < 33 && ssid_2ghz[0] != ' ' && ssid_2ghz[validate-1] != ' '){

        snprintf(two_ghz_ssid_command, sizeof(two_ghz_ssid_command), TWO_GHZ_SET_SSID, ssid_2ghz);
   
	fp = popen(two_ghz_ssid_command, "r");
	if (fp == NULL) {
           log_err("set 2.4GHZ ssid failed");
	   exit(1);
	}
	pclose(fp);

	memset(ssid_2ghz,'\0',sizeof(ssid_2ghz));

	fp1 = popen(RESTART_MESH_BROKER, "r");
	if( fp1 == NULL) {
	   log_err("restart mesh broker failed");
	   exit(1);
	}
	pclose(fp1);
     } 
     else {
	 log_err("Invalid ssid 2ghz");
    }
  }
  else {
        log_debug("set ssid 2ghz failed due to gateway configured as an agent or tried with exisitng value to set in the device !!!");
  }

  // To get 2ghz ssid
  fp = popen(TWO_GHZ_GET_SSID,"r");
  if (fp == NULL) {
     log_err("Get 2ghz ssid  failed");
     exit(1);
  } else {
        memset(ssid_2ghz,'\0',sizeof(ssid_2ghz));
	fscanf(fp, "%[^\n]", ssid_2ghz);
	log_debug("ssid 2ghz get value : %s",ssid_2ghz);
  }
  prop_send_by_name("gw_wifi_ssid_2G");
  pclose(fp);

  return 0;
}

/*
 *Set 2GHZ SSID KEY  from cloud.
 */
static int appd_ssid_key_2ghz(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{
	
  FILE *fp;
  FILE *fp1;

  char two_ghz_key_command[150];
  unsigned int validate;
  unsigned int status;
  unsigned int value = 1;
  char ssid_key[100];
  
  // Variable to store initial regex()
  regex_t reegex;
  memset(two_ghz_key_command,'\0',sizeof(two_ghz_key_command));

  if (prop_arg_set(prop, val, len, args) != ERR_OK) {
     log_err("prop_arg_set returned error");
     return -1;
  }

  // To get 2ghz ssid key
  fp = popen(TWO_GHZ_GET_KEY,"r");
  if (fp == NULL) {
     log_err("Get 2ghz ssid key failed");
     exit(1);
  } else {
     memset(ssid_key,'\0',sizeof(ssid_key));
     fscanf(fp, "%[^\n]", ssid_key);
     log_debug("ssid key 2ghz get value for verification : %s",ssid_key_2ghz);
  }
  pclose(fp);

  status=appd_mesh_controller_status();

  if(status == 1 && strcmp(ssid_key,ssid_key_2ghz) != 0) {
     validate = strlen(ssid_key_2ghz);
     log_debug("set ssid 2ghz key value : %s and size of the value : %d",ssid_key_2ghz,validate);

     value = regcomp( &reegex, "^[%x]+$", 0);
     value = regexec( &reegex, ssid_2ghz, 0, NULL, 0);

     if(validate  > 7 && validate < 64 && value != 0 ){

        snprintf(two_ghz_key_command, sizeof(two_ghz_key_command), TWO_GHZ_SET_KEY, ssid_key_2ghz);

	fp = popen(two_ghz_key_command, "r");
	if (fp == NULL) {
	   log_err("set 2.4GHZ ssid failed");
	   exit(1);
	}
	pclose(fp);

        memset(ssid_key_2ghz,'\0',sizeof(ssid_key_2ghz));			
			
	fp1 = popen(RESTART_MESH_BROKER, "r");
	if( fp1 == NULL) {
	   log_err("restart mesh broker failed");
	   exit(1);
	}
	pclose(fp1);
     }
     else {
        log_err("Invalid ssid key 2ghz");
     }
  }
  else {
        log_debug("set ssid 2ghz key failed due to gateway configured as an agent  or tried with exisitng value to set in the device !!!");
  }
	

  // To get 2ghz ssid key
  fp = popen(TWO_GHZ_GET_KEY,"r");
  if (fp == NULL) {
     log_err("Get 2ghz ssid key failed");
     exit(1);
  } else {
     memset(ssid_key_2ghz,'\0',sizeof(ssid_key_2ghz));
     fscanf(fp, "%[^\n]", ssid_key_2ghz);
     log_debug("ssid key 2ghz get value : %s",ssid_key_2ghz);
  }
  prop_send_by_name("gw_wifi_ssid_key_2G");
  pclose(fp);

  return 0;

}


/*
 *Set 5GHZ SSID  from cloud.
 */
static int appd_ssid_5ghz(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{

  FILE *fp;
  FILE *fp1;

  char five_ghz_ssid_command[150];
  unsigned int validate;
  unsigned int status;
  char ssid[50];

  memset(five_ghz_ssid_command,'\0',sizeof(five_ghz_ssid_command));

  if (prop_arg_set(prop, val, len, args) != ERR_OK) {
     log_err("prop_arg_set returned error");
     return -1;
  }

  fp = popen(FIVE_GHZ_GET_SSID,"r");
  if (fp == NULL) {
     log_err("Get 5ghz ssid  failed");
     exit(1);
  } else {
     memset(ssid,'\0',sizeof(ssid));
     fscanf(fp, "%[^\n]", ssid);
     log_debug("ssid 5ghz get value for verification: %s",ssid);
  }
	
  status=appd_mesh_controller_status();

  if(status == 1 && strcmp(ssid,ssid_5ghz) != 0) {
     validate = strlen(ssid_5ghz);
     log_debug("set ssid 5ghz value : %s and size of the value : %d",ssid_5ghz,validate);

     if(validate  > 0 && validate < 33  && ssid_5ghz[0] != ' ' && ssid_5ghz[validate-1] != ' '){
        snprintf(five_ghz_ssid_command, sizeof(five_ghz_ssid_command), FIVE_GHZ_SET_SSID, ssid_5ghz);

	   fp = popen(five_ghz_ssid_command, "r");
	   if (fp == NULL) {
	      log_err("set 5GHZ ssid failed");
	      exit(1);
	   }
	   pclose(fp);

	   memset(ssid_5ghz,'\0',sizeof(ssid_5ghz));

	   fp1 = popen(RESTART_MESH_BROKER, "r");
	   if( fp1 == NULL) {
	      log_err("restart mesh broker failed");
	      exit(1);
	   }
	   pclose(fp1);
	}
	else {
	   log_err("Invalid ssid 5ghz");
	}
     }
     else {
	   log_debug("set ssid 5ghz failed due to gateway configured as an agent  or tried with exisitng value to set in the device !!!");
     }
	
     // To get 5ghz ssid
     fp = popen(FIVE_GHZ_GET_SSID,"r");
     if (fp == NULL) {
        log_err("Get 5ghz ssid  failed");
	exit(1);
     } else {
        memset(ssid_5ghz,'\0',sizeof(ssid_5ghz));
	fscanf(fp, "%[^\n]", ssid_5ghz);
	log_debug("ssid 5ghz get value : %s",ssid_5ghz);
     }
     prop_send_by_name("gw_wifi_ssid_5G");
     pclose(fp);

     return 0;
}


/*
 *Set 5GHZ SSID KEY  from cloud.
 */
static int appd_ssid_key_5ghz(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{
	
  FILE *fp;
  FILE *fp1;

  char five_ghz_key_command[150];
  unsigned int validate;
  unsigned int status;
  unsigned int value = 1;
  char ssid_key[100];

  // Variable to store initial regex()
  regex_t reegex;	

  memset(five_ghz_key_command,'\0',sizeof(five_ghz_key_command));

  if (prop_arg_set(prop, val, len, args) != ERR_OK) {
     log_err("prop_arg_set returned error");
     return -1;
  }


  fp = popen(FIVE_GHZ_GET_KEY,"r");
  if (fp == NULL) {
     log_err("Get 5ghz ssid key failed");
     exit(1);
  } else {
     memset(ssid_key,'\0',sizeof(ssid_key));
     fscanf(fp, "%[^\n]", ssid_key);
     log_debug("ssid key 5ghz get value for verification: %s",ssid_key);
  }
  

  status=appd_mesh_controller_status();

  if(status == 1 && strcmp(ssid_key, ssid_key_5ghz) != 0) {

     validate = strlen(ssid_key_5ghz);

     log_debug("set ssid 5ghz key value : %s and size of the value : %d",ssid_key_5ghz,validate);

     value = regcomp( &reegex, "^[%x]+$", 0);
     value = regexec( &reegex, ssid_2ghz, 0, NULL, 0);

     if(validate  > 7 && validate < 64 && value != 0){

        snprintf(five_ghz_key_command, sizeof(five_ghz_key_command), FIVE_GHZ_SET_KEY, ssid_key_5ghz);

	fp = popen(five_ghz_key_command, "r");
	if (fp == NULL) {
	   log_err("set 5GHZ ssid key failed");
	   exit(1);
	}
	pclose(fp);

	memset(ssid_key_5ghz,'\0',sizeof(ssid_key_5ghz));

	fp1 = popen(RESTART_MESH_BROKER, "r");
	if( fp1 == NULL) {
	   log_err("restart mesh broker failed");
	   exit(1);
	}
	pclose(fp1);
     }
     else {
           log_err("Invalid ssid key 5ghz");
     }
  }
  else {
        log_debug("set ssid 5ghz key failed due to gateway configured as an agent  or tried with exisitng value to set in the device !!!");
  }

  // To get 5ghz ssid key
  fp = popen(FIVE_GHZ_GET_KEY,"r");
  if (fp == NULL) {
     log_err("Get 5ghz ssid key failed");
     exit(1);
  } else {
     memset(ssid_key_5ghz,'\0',sizeof(ssid_key_5ghz));
     fscanf(fp, "%[^\n]", ssid_key_5ghz);
     log_debug("ssid key 5ghz get value : %s",ssid_key_5ghz);
  }
  prop_send_by_name("gw_wifi_ssid_key_5G");
  pclose(fp);
  
  return 0;
}

/* Get whitelist mac */

/*static void appd_whitelist_get(struct timer *timer_gw_whitelist_timer)
{

	
	char whitelist_bssid[20];
	log_debug("IOT_DEBUG whitelist get timer cancel");
	timer_cancel(app_get_timers(), timer_gw_whitelist_timer);
        
	FILE *fp;
	FILE *fp1;
	FILE *fp2;
        
	fp = popen(WHITELIST_ACTIVE,"r");
        if (fp == NULL) {
                log_err("IOT_DEBUG whitelist active failed");
		gw_whitelist_active=0;
        } else {
                fscanf(fp, "%d", &gw_whitelist_active);
        }
	pclose(fp);

         log_debug("IOT_DEBUG timer get: whitelist active %d::",gw_whitelist_active);
	 prop_send_by_name("gw_whitelist_active");
	 
	 fp1 = popen(WHITELIST_STATE,"r");
                if (fp1 == NULL) {
                        log_err("IOT_DEBUG timer get: whitelist state failed");
			gw_whitelist_state=0;
                } else {
                        fscanf(fp1, "%d", &gw_whitelist_state);
         	}
		pclose(fp1);
         log_debug("IOT_DEBUG timer get: whitelist state %d",gw_whitelist_state);
	 prop_send_by_name("gw_whitelist_state");
         
	 if(gw_whitelist_active==1){
		fp2 = popen(WHITELIST_BSSID,"r");
		if (fp2 == NULL) {
                	log_err("IOT_DEBUG timer get: whitelist bssid failed");
			strcpy(whitelist_bssid,"00:00:00:00:00:00");
        	} else {
			fscanf(fp2, "%[^\n]", whitelist_bssid);
        	}
		pclose(fp2);
		log_debug("IOT_DEBUG timer get: whitelist bssid %s",whitelist_bssid);
                if(!strcmp(whitelist_bssid,whitelist_cmd_buf)){
                        strcpy(whitelist_mac_addr,whitelist_bssid);
                        prop_send_by_name("gw_whitelist_mac_address");
			
                }
         }
        else{
         memset(whitelist_mac_addr,'\0',sizeof(whitelist_mac_addr));
         strcpy(whitelist_mac_addr,"00:00:00:00:00:00");
         prop_send_by_name("gw_whitelist_mac_address");
        }

}*/

/*
 *Connect with specified mac address.
 */

static int appd_whitelist_mac_address(struct prop *prop, const void *val,
                                size_t len, const struct op_args *args)
{

   FILE *fp;

   unsigned int validate;
   unsigned int status;
   char whitelist_bssid[20];

   if(prop_arg_set(prop, val, len, args) != ERR_OK) {
       log_err("prop_arg_set returned error");
       return -1;
   }

   status=appd_mesh_controller_status();
   log_debug("IOT_DEBUG: whitelist mesh controller status %d",status);
   if(status == 0) {

      validate = strlen(whitelist_mac_addr);


      if(validate == 17 && (!strcmp(whitelist_mac_addr,"00:00:00:00:00:00"))){
	      log_debug("IOT_DEBUG: WHITELIST clear");
		fp = popen(WHITELIST_CLEAR,"r");
		if (fp == NULL) {
			log_err("whitelist active failed");
		}
		pclose(fp);
		strcpy(gw_whitelist_bssid,"00:00:00:00:00:00");
		prop_send_by_name("gw_whitelist_bssid");
      }
      if(validate == 17 && strcmp(whitelist_mac_addr,"00:00:00:00:00:00") != 0){

	log_debug("IOT_DEBUG: whitelist connect mac address value : %s and size of the value : %d",whitelist_mac_addr,validate);
	fp = popen(WHITELIST_ACTIVE,"r");
        if (fp == NULL) {
                log_err("whitelist active failed");
                gw_whitelist_active=0;
        } else {
                fscanf(fp, "%d", &gw_whitelist_active);
        }
        pclose(fp);
	log_debug("IOT_DEBUG: gw_whitelist_active %d",gw_whitelist_active);

	log_debug("IOT_DEBUG: bssid get cmd: %s",WHITELIST_BSS_ID);
	fp = popen(WHITELIST_BSS_ID,"r");
	if (fp == NULL) {
		log_err("IOT_DEBUG timer get: whitelist bssid failed");
                strcpy(whitelist_bssid,"00:00:00:00:00");
		
        } else {
                fscanf(fp, "%[^\n]", whitelist_bssid);
        }
        pclose(fp);
        log_debug("IOT_DEBUG bssid %s:::  whitelist_mac_addr %s",whitelist_bssid,whitelist_mac_addr);


	if((gw_whitelist_active==1) && (!strcmp(whitelist_bssid,whitelist_mac_addr))){
		log_debug("IOT_DEBUG whitelist config already done, activating 10 sec timers to get the whitelist state");

	}else
	{
		if(strlen(whitelist_bssid)==17){
                log_debug("IOT_DEBUG whitelist bssid %s :: whitelist_mac_addr %s",whitelist_bssid,whitelist_mac_addr);
		snprintf(whitelist_cmd_buf, sizeof(whitelist_cmd_buf), WHITELIST_CMD, whitelist_mac_addr);

	        log_debug("IOT_DEBUG whitelist command : %s",whitelist_cmd_buf);

		fp = popen(whitelist_cmd_buf, "r");
	        if (fp == NULL) {
        	log_err("whitelist connect command failed");
            		exit(1);
         	}
         	pclose(fp);

  //       	timer_set(app_get_timers(), &gw_whitelist_timer, 180000);
		}
	}

      }
      else {
	 log_debug("Invalid mac address try again");     
	 strcpy(gw_whitelist_bssid,"00:00:00:00:00:00");
      }
   }
   else {
      log_debug("whitelist command not allowed due to extender act as a controller"); 
      strcpy(gw_whitelist_bssid,"00:00:00:00:00:00");
	   
   }
   return 0;
}
   


/*
 *To get 2GHZ SSID and TEST function SRINI.
 */
static int appd_properties_get(void)
{
        FILE *fp;
	FILE *fp1;
	FILE *fp2;
	FILE *fp3;
	FILE *fp4;
	FILE *fp5;
	FILE *fp6;
        FILE *fp7;
        FILE *fp8;

	int tmp;
	char tmp_string[80];
	tmp=channel_2ghz;	
	// To get 2ghz channel value
	fp = popen(GET_TWO_GHZ_CHANNEL_VALUE,"r");
	if (fp == NULL) {
			log_err("Get 2ghz channel failed");
			exit(1);
	} else {
		fscanf(fp, "%d", &channel_2ghz);
		log_debug("get 2ghz channel value : %d",channel_2ghz);
	}
	pclose(fp);

	if(tmp!=channel_2ghz){
		prop_send_by_name("gw_wifi_channel_2G");		
	}

	tmp=channel_5ghz;
	// To get 5ghz channel value
	fp1 = popen(GET_FIVE_GHZ_CHANNEL_VALUE,"r");
	if (fp1 == NULL) {
			log_err("Get 5ghz channel failed");
			exit(1);
	} else {
		fscanf(fp1, "%d", &channel_5ghz);
		log_debug("get 5ghz channel value : %d",channel_5ghz);
	}
	pclose(fp1);

	if(tmp!=channel_5ghz){
                prop_send_by_name("gw_wifi_channel_5G");
        }

	strcpy(tmp_string,ssid_2ghz);
	// To get 2ghz ssid 
	fp2 = popen(TWO_GHZ_GET_SSID,"r");
	if (fp2 == NULL) {
			log_err("Get 2ghz ssid  failed");
			exit(1);
	} else {
		memset(ssid_2ghz,'\0',sizeof(ssid_2ghz));	
		fscanf(fp2, "%[^\n]", ssid_2ghz);
		log_debug("ssid 2ghz get value : %s",ssid_2ghz);
	}
	pclose(fp2);

	if(strcmp(tmp_string,ssid_2ghz)){
		prop_send_by_name("gw_wifi_ssid_2G");
	}

	strcpy(tmp_string,ssid_5ghz);
	// To get 5ghz ssid
	fp3 = popen(FIVE_GHZ_GET_SSID,"r");
	if (fp3 == NULL) {
			log_err("Get 5ghz ssid  failed");
			exit(1);
	} else {
		memset(ssid_5ghz,'\0',sizeof(ssid_5ghz));	
		fscanf(fp, "%[^\n]", ssid_5ghz);
		log_debug("ssid 5ghz get value : %s",ssid_5ghz);
	}
	pclose(fp3);

	if(strcmp(tmp_string,ssid_5ghz)){
                prop_send_by_name("gw_wifi_ssid_5G");
        }

	tmp=bh_optimization;
	// To get backhaul optimization value
	fp4 = popen(GET_BH_OPTIMIZATION,"r");
	if (fp4 == NULL) {
			log_err("Get backhaul optimization failed");
			exit(1);
	} else {
		fscanf(fp4, "%d", &bh_optimization);
		log_debug("backhaul optimization enable/disable : %d",bh_optimization);
	}
	pclose(fp4);

	if(tmp!=bh_optimization){
		prop_send_by_name("gw_wifi_bh_optimization");
	}

	tmp=multi_channel_scan;
        // To get multi channel value
        fp5 = popen(GET_MULTI_CHANNEL_SCAN,"r");
        if (fp5 == NULL) {
                        log_err("Get multi channel scan failed");
                        exit(1);
        } else {
                fscanf(fp5, "%d", &multi_channel_scan);
                log_debug("Get multi channel scan value : %d",multi_channel_scan);
        }
        pclose(fp5);

	if(tmp!=multi_channel_scan){
		prop_send_by_name("gw_wifi_multi_channel_scan");
        }

	tmp=single_channel_scan;

        // To get single channel value
        fp6 = popen(GET_SINGLE_CHANNEL_SCAN,"r");
        if (fp6 == NULL) {
                        log_err("Get single channel scan failed");
                        exit(1);
        } else {
                fscanf(fp6, "%d", &single_channel_scan);
                log_debug("Get single channel scan value : %d",single_channel_scan);
        }
        pclose(fp6);

	if(tmp!=single_channel_scan){
		prop_send_by_name("gw_wifi_single_channel_scan");
        }

	strcpy(tmp_string,ssid_key_2ghz);
        // To get 2ghz ssid key
        fp7 = popen(TWO_GHZ_GET_KEY,"r");
        if (fp7 == NULL) {
                        log_err("Get 2ghz ssid key failed");
                        exit(1);
        } else {
                memset(ssid_key_2ghz,'\0',sizeof(ssid_key_2ghz));
                fscanf(fp7, "%[^\n]", ssid_key_2ghz);
                log_debug("Get ssid key 2ghz value : %s",ssid_key_2ghz);
        }
	pclose(fp7);

	if(strcmp(tmp_string,ssid_key_2ghz)){
		prop_send_by_name("gw_wifi_ssid_key_2G");
	}

	strcpy(tmp_string,ssid_key_5ghz);
        // To get 5ghz ssid key
        fp8 = popen(FIVE_GHZ_GET_KEY,"r");
        if (fp8 == NULL) {
                        log_err("Get 5ghz ssid key failed");
                        exit(1);
        } else {
                memset(ssid_key_5ghz,'\0',sizeof(ssid_key_5ghz));
                fscanf(fp8, "%[^\n]", ssid_key_5ghz);
                log_debug("Get ssid key 5ghz value : %s",ssid_key_5ghz);
        }
        pclose(fp8);

	if(strcmp(tmp_string,ssid_key_5ghz)){
		prop_send_by_name("gw_wifi_ssid_key_5G");
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

	log_debug("IOT_DEBUG_SET_POLL: wifi_sta_info_update %d, WIFI_STA_MIN_UPDATE_PERIOD_MINS %d, wifi_sta_info_update %d, WIFI_STA_MAX_UPDATE_PERIOD_MINS %d",wifi_sta_info_update,WIFI_STA_MIN_UPDATE_PERIOD_MINS,wifi_sta_info_update,WIFI_STA_MAX_UPDATE_PERIOD_MINS);
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
        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
	sprintf(command, WIFI_GET_STA_RSSI);
	exec_systemcmd(command, data, DATA_SIZE);
	if (appd_check_wifi_sta_data_deviation(WIFI_STA_RSSI, data)) {
		appd_send_wifi_sta_data(WIFI_STA_RSSI, data);
	}

        memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
	sprintf(command, WIFI_GET_STA_NOISE);
	exec_systemcmd(command, data, DATA_SIZE);
	if (appd_check_wifi_sta_data_deviation(WIFI_STA_NOISE, data)) {
		appd_send_wifi_sta_data(WIFI_STA_NOISE, data);
        }


	if (wifi_sta_info_update_period_min >= wifi_sta_info_update || wifi_sta_info_update_period_min == 0) {

                memset(command,'\0',sizeof(command));
	        memset(data,'\0',sizeof(data));
		sprintf(command, WIFI_GET_STA_RSSI);
		exec_systemcmd(command, data, DATA_SIZE);
		appd_send_wifi_sta_data(WIFI_STA_RSSI, data);


                memset(command,'\0',sizeof(command));
          	memset(data,'\0',sizeof(data));
		sprintf(command, WIFI_GET_STA_NOISE);
		exec_systemcmd(command, data, DATA_SIZE);
		appd_send_wifi_sta_data(WIFI_STA_NOISE, data);

		wifi_sta_info_update_period_min = 0;
	}

	wifi_sta_info_update_period_min += 1;

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
	sprintf(command, WIFI_GET_STA_CHANNEL);
	exec_systemcmd(command, data, DATA_SIZE);
	appd_send_wifi_sta_data(WIFI_STA_CHANNEL, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
	sprintf(command, WIFI_GET_STA_ASSOCIATED_SSID);
	exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
	appd_send_wifi_sta_data(WIFI_STA_ASSOCIATED_SSID, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
	sprintf(command, WIFI_GET_STA_ASSOCIATED_BSSID);
	exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
	appd_send_wifi_sta_data(WIFI_STA_ASSOCIATED_BSSID, data);
	
	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
	sprintf(command, GW_WIFI_GET_BSSID_FRONTHAUL_5G);
	exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
	appd_send_wifi_sta_data(GW_WIFI_BSSID_FRONTHAUL_5G, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
        sprintf(command, GW_WIFI_GET_BSSID_FRONTHAUL_2G);
        exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
        appd_send_wifi_sta_data(GW_WIFI_BSSID_FRONTHAUL_2G, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
	sprintf(command, GW_WIFI_GET_BSSID_BACKHAUL);
	exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
	appd_send_wifi_sta_data(GW_WIFI_BSSID_BACKHAUL, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
	sprintf(command, GET_DEVICE_MAC_ADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
        appd_send_wifi_sta_data(DEVICE_MAC_ADDRESS, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
        sprintf(command, GET_EM_PARENT_MAC_ADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
        appd_send_wifi_sta_data(EM_PARENT_MAC_ADDRESS, data);

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
        sprintf(command, GET_EM_BACKHAUL_TYPE);
        exec_systemcmd(command, data, DATA_SIZE);
	uppercase_convert(data);
        appd_send_wifi_sta_data(EM_BACKHAUL_TYPE, data);	

	memset(command,'\0',sizeof(command));
	memset(data,'\0',sizeof(data));
        sprintf(command, GET_GW_AGENT_ALMAC_ADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);
        uppercase_convert(data);
        appd_send_wifi_sta_data(GW_AGENT_ALMAC_ADDRESS, data);

        memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command, GET_GW_CTRL_ALMAC_ADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);
        uppercase_convert(data);
        appd_send_wifi_sta_data(GW_CTRL_ALMAC_ADDRESS, data);

	memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command, GET_GW_WIFI_TXOP_5G);
        exec_systemcmd(command, data, DATA_SIZE);
        if (appd_check_wifi_sta_data_deviation(GW_WIFI_TXOP_5G, data)) {
                appd_send_wifi_sta_data(GW_WIFI_TXOP_5G, data);
        }
	
	memset(command,'\0',sizeof(command));
        memset(data,'\0',sizeof(data));
        sprintf(command, GET_GW_WIFI_TXOP_2G);
        exec_systemcmd(command, data, DATA_SIZE);
	if (appd_check_wifi_sta_data_deviation(GW_WIFI_TXOP_2G, data)) {
        	appd_send_wifi_sta_data(GW_WIFI_TXOP_2G, data);	
	}
        if(0 == appd_mesh_controller_status()){
		appd_update_wifi_bh_sta();
		appd_update_whitelist_get();

        }
	
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

	} else if (!strcmp(name, GW_WIFI_TXOP_2G)) {

                tmp = atoi(value);

                deviation = abs(abs(tmp) - abs(gw_wifi_txop_2g));

                if (deviation >= ATT_DATA_DEVIATION_DB) {
                        return 1;
                } else {
                        return 0;
                }
 
        } else if (!strcmp(name, GW_WIFI_TXOP_5G)) {

                tmp = atoi(value);

                deviation = abs(abs(tmp) - abs(gw_wifi_txop_5g));

                if (deviation >= ATT_DATA_DEVIATION_DB)  {
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
	}else if (!strcmp(name, EM_BACKHAUL_TYPE)) {

                if (!strcmp(value, em_backhaul_type)) {
                        return 0;
                }
                strncpy(em_backhaul_type, value, WIFI_STA_ADDR_LEN);
                em_backhaul_type[WIFI_STA_ADDR_LEN - 1] = '\0';

                prop_send_by_name(name);
	}else if (!strcmp(name, GW_AGENT_ALMAC_ADDRESS)) {

                if (!strcmp(value, gw_agent_almac_address)) {
                        return 0;
                }
                strncpy(gw_agent_almac_address, value, WIFI_STA_ADDR_LEN);
                gw_agent_almac_address[WIFI_STA_ADDR_LEN - 1] = '\0';

                prop_send_by_name(name);
        }else if (!strcmp(name, GW_CTRL_ALMAC_ADDRESS)) {

                if (!strcmp(value, gw_ctrl_almac_address)) {
                        return 0;
                }
                strncpy(gw_ctrl_almac_address, value, WIFI_STA_ADDR_LEN);
                gw_ctrl_almac_address[WIFI_STA_ADDR_LEN - 1] = '\0';

                prop_send_by_name(name);
        }else if (!strcmp(name, GW_WIFI_TXOP_5G)) {

                tmp = atoi(value);

                if (tmp == gw_wifi_txop_5g) {
                      return 0;
                }

                gw_wifi_txop_5g = tmp;

                prop_send_by_name(name);

        }else if (!strcmp(name, GW_WIFI_TXOP_2G)) {

                tmp = atoi(value);

                if (tmp == gw_wifi_txop_2g) {
                      return 0;
                }

                gw_wifi_txop_2g = tmp;

                prop_send_by_name(name);
        }


	return 0;
}

/* schedule reboot */
static int appd_schedule_reboot(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{

        if (prop_arg_set(prop, val, len, args) != ERR_OK) {
                log_err("prop_arg_set returned error");
                return -1;
        }
        log_debug("schedule reboot: %s",schedule_reboot);
        char *delim = ":";
        char * x [] = {"0","1","2","3","4","5","6","0,1","0,2","0,3","0,4","0,5","0,6","1,2","1,3","1,4","1,5","1,6","2,3","2,4","2,5","2,6","3,4","3,5","3,6","4,5","4,6","5,6","0,1,2","0,1,3","0,1,4","0,1,5","0,1,6","0,2,3","0,2,4","0,2,5","0,2,6","0,3,4","0,3,5","0,3,6","0,4,5","0,4,6","0,5,6","1,2,3","1,2,4","1,2,5","1,2,6","1,3,4","1,3,5","1,3,6","1,4,5","1,4,6","1,5,6","2,3,4","2,3,5","2,3,6","2,4,5","2,4,6","2,5,6","3,4,5","3,4,6","3,5,6","4,5,6","0,1,2,3","0,1,2,4","0,1,2,5","0,1,2,6","0,2,3,4","0,2,3,5","0,2,3,6","0,2,4,5","0,2,4,6","0,2,5,6","1,2,3,4","1,2,3,5","1,2,3,6","1,3,4,5","1,3,4,6","1,3,5,6","1,4,5,6","2,3,4,5","2,3,4,6","2,4,5,6","3,4,5,6","0,1,2,3,4","0,1,2,3,5","0,1,2,3,6","0,2,3,4,5","0,2,3,4,5","0,3,4,5,6","1,2,3,4,5","1,2,3,4,6","2,3,4,5,6","0,1,2,3,4,5,6","0-2","0-3","0-4","0-5","0-6","1-3","1-4","1-5","1-6","2-4","2-5","2-6","3-5","3-6","4-6"};

        if(!strcmp(schedule_reboot,"0")){
                memset(command,'\0',sizeof(command));
                memset(data,'\0',sizeof(data));
                sprintf(command, CLEAR_CRON);
                exec_systemcmd(command, data, DATA_SIZE);
                log_debug("disable reboot : %s ",command);
        }
        else{
                unsigned count = 0;
                char *token = strtok(schedule_reboot,delim);
                int min,hour,i;
                char day[5],cron[200];
                count++;

                while(token != NULL)
                {
                        if(count==1){
                                min=atoi(token);
                        }
                        if(count==2){
                                hour=atoi(token);
                        }
                        if((count==3)&&(min<60)&&(hour<24)){
                                memset(day,0,sizeof(day));
                                strcpy(day,token);
                                memset(cron,0,sizeof(cron));
                                int len = sizeof(x)/sizeof(x[0]);
                                for(i = 0; i < len; ++i){
                                        if(!strcmp(x[i], day)){
						memset(command,'\0',sizeof(command));
                				memset(data,'\0',sizeof(data));
                				sprintf(command, CLEAR_CRON);
                				exec_systemcmd(command, data, DATA_SIZE);
						memset(cron,'\0',sizeof(cron));
                                                memset(data,'\0',sizeof(data));
						sprintf(cron,"echo \"%02d %02d * * %s /bin/gw_schedule_reboot.sh\" >>  /etc/crontabs/root",min,hour,day);
                                                exec_systemcmd(cron, data, DATA_SIZE);
						memset(command,'\0',sizeof(command));
                                                memset(data,'\0',sizeof(data));
                                                sprintf(command, SCHEDULE_RBT_REASON);
                                                exec_systemcmd(command, data, DATA_SIZE);
						memset(command,'\0',sizeof(command));
                                                memset(data,'\0',sizeof(data));
                                                sprintf(command, SCHEDULE_RBT_APPLY);
                                                exec_systemcmd(command, data, DATA_SIZE);
                                                log_debug("cron %s",cron);
                                        }
                                }
                        }
                        token = strtok(NULL,delim);
                        count++;

                }
        }
        return 0;
}

/*
 *To enable gw wps button and update the gw led status after 110 seconds
 */
static int appd_gw_wps_button(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{
        if (prop_arg_set(prop, val, len, args) != ERR_OK) {
                log_err("prop_arg_set returned error");
                return -1;
        }

        if (gw_wps_button == 1) {
                log_debug("WPS button pressed");
                memset(command,'\0',sizeof(command));
                memset(data,'\0',sizeof(data));
                sprintf(command, "wps_button_pressed.sh");
                exec_systemcmd(command, data, DATA_SIZE);
                timer_set(app_get_timers(), &gw_led_status_timer, 110000);
        } else if (gw_wps_button== 0) {
                timer_set(app_get_timers(), &gw_led_status_timer, 1000);
        } else {
                log_debug("wps button wrong info");
        }
        return 0;
}

/*
 *  *To get Network up time.
 *   */
static enum err_t get_gw_wifi_bh_uptime(struct prop *prop, int req_id,
                                   const struct op_options *opts)
{
	int day, hour, minutes, seconds;
	unsigned long int nw_up_time = 0;
	char *temp = NULL;
	char tmp_network_up_time[40];
	strcpy(tmp_network_up_time,network_up_time);
	memset(network_up_time,0x00, sizeof(network_up_time));
	if(1 == appd_mesh_controller_status())
	{
		log_debug("Gateway configured as controller");
	}else{
			memset(command,'\0',sizeof(command));
			memset(data,'\0',sizeof(data));
			sprintf(command, GET_NETWORK_UP_TIME);
			exec_systemcmd(command, data, DATA_SIZE);

			if(strlen(data) > 0)
			{
				nw_up_time = strtoul(data, &temp, 0);
				if(0 == nw_up_time)
				{
					sprintf(network_up_time,"%s","N/A");
				}else{
					/*day conversion*/
					day = nw_up_time / (24 * 3600);

					/*Hours conversion*/
					nw_up_time = nw_up_time % (24 * 3600);
					hour = nw_up_time / 3600;

					/*Minutes conversion*/
					nw_up_time %= 3600;
					minutes = nw_up_time / 60 ;

					/*Seconds conversion*/
					nw_up_time %= 60;
					seconds = nw_up_time;

					if(0 == day)
					{
						sprintf(network_up_time,"%d Hrs %d min %d sec",hour, minutes, seconds);
					}else{
						sprintf(network_up_time,"%d days %d Hrs %d min %d sec",day, hour, minutes, seconds);
					}
				}
			}else{
				sprintf(network_up_time,"%s","N/A");
			}
			log_debug("network up time: %s",network_up_time);
		}
	if(!strcmp(tmp_network_up_time,network_up_time)){
		return 0;
	}
    return prop_arg_send(prop, req_id, opts);
}

/*
 *  *To get the Radio BLE FW.
 *   */
static enum err_t appd_ble_fw(struct prop *prop, int req_id,
				   const struct op_options *opts)
{
	const char *radio_fw_versions;
	json_t*radio_fw_versions_obj_json;
	json_error_t error;
	radio_fw_versions_obj_json = json_load_file("/etc/config/radio_fw_version.conf", 0, &error);
	if(!radio_fw_versions_obj_json) {
		log_debug("radio_fw_version.conf is not having a valid json");
	}else{
		json_t*config_obj_json;
		config_obj_json=json_object_get(radio_fw_versions_obj_json,"config");
		radio_fw_versions=json_dumps(config_obj_json,JSON_COMPACT);
		log_debug("Config: %s",radio_fw_versions);
		json_t*config_versions_obj_json;
		config_versions_obj_json=json_object_get(config_obj_json,"radio_FW_version");
		radio_fw_versions=json_dumps(config_versions_obj_json,JSON_COMPACT);
		log_debug("FW versions: %s",radio_fw_versions);
		json_t*config_versions_radio1_obj_json;
		config_versions_radio1_obj_json=json_object_get(config_versions_obj_json,"radio1");
		radio_fw_versions=json_string_value(config_versions_radio1_obj_json);
		if(!strcmp(radio1_fw_version,radio_fw_versions)){
			log_debug("IOT_DEBUG: radio1_fw_version there is no change in version");
			return 0;
		} else {
		strcpy(radio1_fw_version,radio_fw_versions);
		log_debug("Radio1 FW version: %s",radio1_fw_version);
		}
	}
	return prop_arg_send(prop, req_id, opts);
}

/*
 *  *To get the Radio ZIGBEE FW.
 *   */
static enum err_t appd_zigbee_fw(struct prop *prop, int req_id,
				   const struct op_options *opts)
{
	const char *radio_fw_versions;
	json_t*radio_fw_versions_obj_json;
	json_error_t error;
	radio_fw_versions_obj_json = json_load_file("/etc/config/radio_fw_version.conf", 0, &error);
	if(!radio_fw_versions_obj_json) {
	/*the error variable contains error information*/
		log_debug("radio_fw_version.conf is not having a valid json");
	}else{
		json_t*config_obj_json;
		config_obj_json=json_object_get(radio_fw_versions_obj_json,"config");
		radio_fw_versions=json_dumps(config_obj_json,JSON_COMPACT);
		log_debug("Config: %s",radio_fw_versions);
		json_t*config_versions_obj_json;
		config_versions_obj_json=json_object_get(config_obj_json,"radio_FW_version");
		radio_fw_versions=json_dumps(config_versions_obj_json,JSON_COMPACT);
		log_debug("FW versions: %s",radio_fw_versions);
		json_t*config_versions_radio2_obj_json;
		config_versions_radio2_obj_json=json_object_get(config_versions_obj_json,"radio2");
		radio_fw_versions=json_string_value(config_versions_radio2_obj_json);
		if(!strcmp(radio2_fw_version,radio_fw_versions)){
			log_debug("IOT_DEBUG: radio2_fw_version there is no change in version");
                        return 0;
                } else {
		strcpy(radio2_fw_version,radio_fw_versions);
		log_debug("Radio2 FW version: %s",radio2_fw_version);
		}
	}
	return prop_arg_send(prop, req_id, opts);
}

/*
 *  *To get the Radio ZWAVE FW.
 *   */
static enum err_t appd_zwave_fw(struct prop *prop, int req_id,
				   const struct op_options *opts)
{
	const char *radio_fw_versions;
	json_t*radio_fw_versions_obj_json;
	json_error_t error;
	radio_fw_versions_obj_json = json_load_file("/etc/config/radio_fw_version.conf", 0, &error);
	if(!radio_fw_versions_obj_json) {
	/*the error variable contains error information*/
		log_debug("radio_fw_version.conf is not having a valid json");
	}else{
		json_t*config_obj_json;
		config_obj_json=json_object_get(radio_fw_versions_obj_json,"config");
		radio_fw_versions=json_dumps(config_obj_json,JSON_COMPACT);
		log_debug("Config: %s",radio_fw_versions);
		json_t*config_versions_obj_json;
		config_versions_obj_json=json_object_get(config_obj_json,"radio_FW_version");
		radio_fw_versions=json_dumps(config_versions_obj_json,JSON_COMPACT);
		log_debug("FW versions: %s",radio_fw_versions);
		json_t*config_versions_radio0_obj_json;
		config_versions_radio0_obj_json=json_object_get(config_versions_obj_json,"radio0");
		radio_fw_versions=json_string_value(config_versions_radio0_obj_json);
		if(!strcmp(radio0_fw_version,radio_fw_versions)){
			log_debug("IOT_DEBUG: radio0_fw_version there is no change in version");
                        return 0;
                } else {
		strcpy(radio0_fw_version,radio_fw_versions);
		log_debug("Radio0 FW version: %s",radio0_fw_version);
		}
	}
	return prop_arg_send(prop, req_id, opts);
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
	if(!strcmp(cpu_usage,buffer)){
		return 0;
	}
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
	char tmp[100];
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
	if(!strcmp(tmp,ram_usage)){
		return 0;
	}
	return prop_arg_send(prop, req_id, opts);
}

/*
 * node number structure
 */
struct tag_num_nodes {
	unsigned int zb_num_nodes;
	unsigned int bh_num_nodes;
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
	} else if (node->interface == GI_VT) {
		num->bh_num_nodes++;
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
	num->bh_num_nodes = 0;
	num->all_num_nodes = 0;
	node_foreach(appd_check_node_type, num);
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
	//num_nodes = node_count();
	return prop_arg_send(prop, req_id, opts);
}

/*
 * File upload complete callback
 */
static int file_upload_confirm_cb(struct prop *prop, const void *val,
	size_t len, const struct op_options *opts,
	const struct confirm_info *confirm_info)
{
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		log_info("%s upload succeeded (requested at %llu)",
		    prop->name, opts->dev_time_ms);
	} else {
		log_info("%s upload to %d failed with err %u "
		    "(requested at %llu)", prop->name, DEST_ADS,
		    confirm_info->err, opts->dev_time_ms);
	}

	remove(file_upload_path);
	return 0;
}

/*
 * Send up a FILE property
 */
static enum err_t project_sanity_script_file(struct prop *prop, int req_id,
                                   const struct op_options *opts)
{
	enum err_t ret;
	struct op_options opt = {.confirm = 1};
	struct prop_metadata *metadata;
	DIR *d;
	struct dirent *dir;

	/* Include some datapoint metadata with the file */
	metadata = prop_metadata_alloc();

	log_info("project_sanity_script_file called");

	d = opendir("/etc/ayla");
	if (d)
	{
		while ((dir = readdir(d)) != NULL)
		{
			if(strstr(dir->d_name, "cmd") != NULL){
				memset(file_upload_path,0x00, sizeof(file_upload_path));
				sprintf(file_upload_path,"/etc/ayla/%s",dir->d_name);
				log_info("file name is %s", file_upload_path);
			}
		}
		closedir(d);
	}
	
	d = opendir("/root/");
        if (d)
        {
                while ((dir = readdir(d)) != NULL)
                {
                        if(strstr(dir->d_name, "tar") != NULL){
                                memset(file_upload_path,0x00, sizeof(file_upload_path));
                                sprintf(file_upload_path,"/root/%s",dir->d_name);
                                log_info("file name is %s", file_upload_path);
                        }
                }
                closedir(d);
        }
	
	prop_metadata_add(metadata, "path", file_upload_path);
	prop_metadata_add(metadata, "trigger", prop->name);

	opt.metadata = metadata;
	ret = prop_arg_send(prop, req_id, &opt);
	prop_metadata_free(metadata);
	//remove(file_upload_path);
	return ret;
}

static enum err_t gw_sanity_script_file(struct prop *prop, int req_id,
                                   const struct op_options *opts)
{
        enum err_t ret;
        struct op_options opt = {.confirm = 1};
        struct prop_metadata *metadata;
        DIR *d;
        struct dirent *dir;

        /* Include some datapoint metadata with the file */
        metadata = prop_metadata_alloc();

        log_info("gw_sanity_script_file called");

        d = opendir("/etc/ayla");
        if (d)
        {
                while ((dir = readdir(d)) != NULL)
                {
                        if(strstr(dir->d_name, "cmd") != NULL){
                                memset(file_upload_path,0x00, sizeof(file_upload_path));
                                sprintf(file_upload_path,"/etc/ayla/%s",dir->d_name);
                                log_info("file name is %s", file_upload_path);
                        }
                }
                closedir(d);
        }

        d = opendir("/root/");
        if (d)
        {
                while ((dir = readdir(d)) != NULL)
                {
                        if(strstr(dir->d_name, "tar") != NULL){
                                memset(file_upload_path,0x00, sizeof(file_upload_path));
                                sprintf(file_upload_path,"/root/%s",dir->d_name);
                                log_info("file name is %s", file_upload_path);
                        }
                }

                closedir(d);
        }

        prop_metadata_add(metadata, "path", file_upload_path);
        prop_metadata_add(metadata, "trigger", prop->name);

        opt.metadata = metadata;
        ret = prop_arg_send(prop, req_id, &opt);
        prop_metadata_free(metadata);
        //remove(file_upload_path);
        return ret;
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
		.name = "zb_num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &zb_num_nodes,
		.len = sizeof(zb_num_nodes),
	},
	{
		.name = "num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &num_nodes,
		.len = sizeof(num_nodes),
	},
	{
		.name = "bh_num_nodes",
		.type = PROP_INTEGER,
		.send = appd_num_nodes_send,
		.arg = &bh_num_nodes,
		.len = sizeof(bh_num_nodes),
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
                .name = "em_backhaul_type",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &em_backhaul_type,
                .len = sizeof(em_backhaul_type),
        },	

        {
                .name = "gw_agent_almac_address",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &gw_agent_almac_address,
                .len = sizeof(gw_agent_almac_address),
        },

        {
                .name = "gw_ctrl_almac_address",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &gw_ctrl_almac_address,
                .len = sizeof(gw_ctrl_almac_address),
        },

	{
                .name = "gw_wifi_txop_5g",
                .type = PROP_INTEGER,
                .send = prop_arg_send,
                .arg = &gw_wifi_txop_5g,
                .len = sizeof(gw_wifi_txop_5g),
        },

	{
                .name = "gw_wifi_txop_2g",
                .type = PROP_INTEGER,
                .send = prop_arg_send,
                .arg = &gw_wifi_txop_2g,
                .len = sizeof(gw_wifi_txop_2g),
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
                .name = "ngrok_error_status",
                .type = PROP_STRING,
                .send = appd_ngrok_error_status_send,
                .arg = &ngrok_error_status,
                .len = sizeof(ngrok_error_status),
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
                .name = "gw_wifi_channel_2G",
                .type = PROP_INTEGER,
                .set = appd_channel_2ghz,
                .send = prop_arg_send,
                .arg = &channel_2ghz,
                .len = sizeof(channel_2ghz),
		.skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_wifi_channel_5G",
                .type = PROP_INTEGER,
                .set = appd_channel_5ghz,
                .send = prop_arg_send,
                .arg = &channel_5ghz,
                .len = sizeof(channel_5ghz),
		.skip_init_update_from_cloud = 1,
        },
	
        {
                .name = "gw_wifi_bh_optimization",
                .type = PROP_INTEGER,
                .set = appd_bh_optimization,
                .send = prop_arg_send,
                .arg = &bh_optimization,
                .len = sizeof(bh_optimization),
		.skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_wifi_multi_channel_scan",
                .type = PROP_INTEGER,
                .set = appd_multi_channel_scan,
                .send = prop_arg_send,
                .arg = &multi_channel_scan,
                .len = sizeof(multi_channel_scan),
                .skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_wifi_single_channel_scan",
                .type = PROP_INTEGER,
                .set = appd_single_channel_scan,
                .send = prop_arg_send,
                .arg = &single_channel_scan,
                .len = sizeof(single_channel_scan),
                .skip_init_update_from_cloud = 1,
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
	},
	/* ssid and key properties */
        {
                .name = "gw_wifi_ssid_2G",
                .type = PROP_STRING,
                .set = appd_ssid_2ghz,
		.send = prop_arg_send,
                .arg = &ssid_2ghz,
                .len = sizeof(ssid_2ghz),
		.skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_wifi_ssid_key_2G",
                .type = PROP_STRING,
                .set = appd_ssid_key_2ghz,
		.send = prop_arg_send,
                .arg = &ssid_key_2ghz,
                .len = sizeof(ssid_key_2ghz),
		.skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_wifi_ssid_5G",
                .type = PROP_STRING,
                .set = appd_ssid_5ghz,
		.send = prop_arg_send,
                .arg = &ssid_5ghz,
                .len = sizeof(ssid_5ghz),
		.skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_wifi_ssid_key_5G",
                .type = PROP_STRING,
                .set = appd_ssid_key_5ghz,
		.send = prop_arg_send,
                .arg = &ssid_key_5ghz,
                .len = sizeof(ssid_key_5ghz),
		.skip_init_update_from_cloud = 1,
        },
        {
                .name = "gw_whitelist_mac_address",
                .type = PROP_STRING,
                .set = appd_whitelist_mac_address,
                .send = prop_arg_send,
                .arg = &whitelist_mac_addr,
                .len = sizeof(ssid_key_5ghz),
                .skip_init_update_from_cloud = 1,
        },
	{
                .name = "gw_whitelist_active",
                .type = PROP_INTEGER,
                .send = prop_arg_send,
                .arg = &gw_whitelist_active,
                .len = sizeof(gw_whitelist_active),
        },
	{
                .name = "gw_whitelist_state",
                .type = PROP_INTEGER,
                .send = prop_arg_send,
                .arg = &gw_whitelist_state,
                .len = sizeof(gw_whitelist_state),
        },
	{
                .name = "gw_whitelist_bssid",
                .type = PROP_STRING,
                .send = prop_arg_send,
                .arg = &gw_whitelist_bssid,
                .len = sizeof(gw_whitelist_bssid),
        },

	/* backhaul sta enable/disable */
	{
                .name = "gw_wifi_bh_sta",
                .type = PROP_BOOLEAN,
                .set = appd_backhaul_sta,
                .send = prop_arg_send,
                .arg = &gw_wifi_bh_sta,
		.len = sizeof(gw_wifi_bh_sta),
		.skip_init_update_from_cloud = 1,
        },	

	{
                .name = "gw_wifi_bh_apscan",
                .type = PROP_INTEGER,
                .send = prop_arg_send,
                .arg = &gw_wifi_bh_apscan,
                .len = sizeof(gw_wifi_bh_apscan),
        },	

	{
                .name = "gw_wifi_bh_bss_state",
                .type = PROP_INTEGER,
                .send = prop_arg_send,
                .arg = &gw_wifi_bh_bss_state,
                .len = sizeof(gw_wifi_bh_bss_state),
        },

        /* Radio information */
        {
                .name = "radio1_fw_version",
                .type = PROP_STRING,
                .send = appd_ble_fw,
                .arg = &radio1_fw_version,
                .len = sizeof(radio1_fw_version),
        },

        {
                .name = "radio2_fw_version",
                .type = PROP_STRING,
                .send = appd_zigbee_fw,
                .arg = &radio2_fw_version,
                .len = sizeof(radio2_fw_version),
        },

        {
                .name = "radio0_fw_version",
                .type = PROP_STRING,
                .send = appd_zwave_fw,
                .arg = &radio0_fw_version,
                .len = sizeof(radio0_fw_version),
        },

	{
                .name = "schedule_reboot",
                .type = PROP_STRING,
                .set = appd_schedule_reboot,
                .send = prop_arg_send,
                .arg = &schedule_reboot,
                .len = sizeof(schedule_reboot),
        },

        {
                .name = "gw_wps_button",
                .type = PROP_INTEGER,
                .set = appd_gw_wps_button,
                .send = prop_arg_send,
                .arg = &gw_wps_button,
                .len = sizeof(gw_wps_button),
        },

        {
                .name = "gw_led_status",
                .type = PROP_STRING,
                .send = appd_gw_led_status_send,
                .arg = &gw_led_status,
                .len = sizeof(gw_led_status),
        },
		{
                .name = "gw_wifi_bh_uptime",
                .type = PROP_STRING,
                .send = get_gw_wifi_bh_uptime,
                .arg = &network_up_time,
                .len = sizeof(network_up_time),
        },
	{
                .name = "gw_sanity_script_file",
                .type = PROP_FILE,
                .send = gw_sanity_script_file,
                .arg = file_upload_path,
                .len = sizeof(file_upload_path),
                .confirm_cb = file_upload_confirm_cb,
        },
	{
                .name = "project_sanity_script_file",
                .type = PROP_FILE,
                .send = project_sanity_script_file,
                .arg = file_upload_path,
                .len = sizeof(file_upload_path),
                .confirm_cb = file_upload_confirm_cb,
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
	timer_init(&gw_led_status_timer, appd_gw_wps_status_update);
	appd_prop_init();

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
	struct tag_num_nodes num;

	/* Handle network stack events */
	zb_poll();

	appd_update_num_nodes(&num);

	/* Post accurate node count to cloud */
	/*if (num_nodes != node_count()) {
		prop_send_by_name("num_nodes");
	}*/
	if (zb_num_nodes != num.zb_num_nodes) {
		zb_num_nodes = num.zb_num_nodes;
		prop_send_by_name("zb_num_nodes");
	}
	if (bh_num_nodes != num.bh_num_nodes) {
		bh_num_nodes = num.bh_num_nodes;
		prop_send_by_name("bh_num_nodes");
	}
	if (num_nodes != num.all_num_nodes) {
		num_nodes = num.all_num_nodes;
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
