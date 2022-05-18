/*
 * Copyright 2019 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */


#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>

#include <ayla/utypes.h>
#include <ayla/ayla_interface.h>
#include <ayla/ops.h>
#include <ayla/props.h>
#include <ayla/gateway_interface.h>
#include <ayla/gateway.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/timer.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>

#include "gateway.h"
#include "node.h"
#include "vt_interface.h"
#include "att_interface.h"
#include "att_interface_node.h"


#define NODE_OEM_MODEL  VNODE_OEM_MODEL


/* Fixed subdevice names, General device info props */
#define ZB_SUBDEVICE		"dev"

/* Template with mandatory virtual device info for all nodes */
#define ATT_POC_TEMPLATE_NAME		"vt_node"
#define ATT_POC_TEMPLATE_VERSION	"1.0"


#define ATT_POC_ASSOCLIST		"/tmp/assoclist.txt"

int att_poll_period_min;

char command[COMMAND_LEN];
char data[DATA_SIZE];
/*
 * Save AT&T node info
 */
struct att_node_info {
	struct node *node;		/* Pointer to node */
	uint16_t node_id;
	uint16_t device_id;
};

/*
 * AT&T node prop info
 */
struct nd_prop_info{
	uint16_t device_id;
	char *subdevive_key;
	char *template_key;
	char *template_version;
	const struct node_prop_def *prop_def;
	size_t def_size;
};

/* AT&T node prop define */
static const struct node_prop_def const att_poc_props[] = {
	{ ATT_POC_ACTIVE,		PROP_BOOLEAN,	PROP_FROM_DEVICE },
	{ ATT_POC_MACADDRESS,		PROP_STRING,	PROP_FROM_DEVICE },
        { ATT_POC_RSSI,      		PROP_INTEGER,   PROP_FROM_DEVICE },
        { ATT_POC_NOISE,     		PROP_INTEGER,   PROP_FROM_DEVICE },
	{ ATT_POC_CHANNEL, 	        PROP_INTEGER,   PROP_FROM_DEVICE },
	{ ATT_POC_STATION_TYPE,         PROP_STRING,    PROP_FROM_DEVICE },
	{ ATT_POC_SSID,    		PROP_STRING,    PROP_FROM_DEVICE },
	{ ATT_POC_PARENT_NODE,          PROP_STRING,    PROP_FROM_DEVICE },
};

/* Prop define table */
static struct nd_prop_info prop_info_array[] = {
	{
		.device_id = 0,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ATT_POC_TEMPLATE_NAME,
		.template_version = ATT_POC_TEMPLATE_VERSION,
		.prop_def = att_poc_props,
		.def_size = ARRAY_LEN(att_poc_props)
	},
};

/*
 * AT&T POC node data info
 */
struct att_poc_node_data {
	uint16_t node_id;
	uint16_t att_poll_period_min;

	bool Active;
	char MACAddress[ATT_POC_ADDR_LEN];
        int RSSI;
        int Noise;
	char StationType[ATT_POC_ADDR_LEN];
	char SSID[ATT_POC_ADDR_LEN];
	int Channel;
	char ParentNode[ATT_POC_ADDR_LEN];

	uint16_t update_Active:1;
	uint16_t update_MACAddress:1;
	uint16_t update_RSSI:1;
	uint16_t update_Noise:1;
	uint16_t update_StationType:1;
	uint16_t update_SSID:1;
	uint16_t update_Channel:1;
	uint16_t update_ParentNode:1;
};

struct att_poc_node_data att_poc_node_list[ATT_POC_NODE_MAX];

/*
 * Get AT&T node data
 */
struct att_poc_node_data *att_get_node_data(uint16_t node_id)
{
	/*int i;
	for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
		if (node_id == att_poc_node_list[i].node_id) {
			return &att_poc_node_list[i];
		}
	}
	return NULL;*/
	if (node_id <= 0) {
		return NULL;
	} else if (node_id <= ATT_POC_NODE_MAX) {
		return &att_poc_node_list[node_id - 1];
	} else {
		return NULL;
	}
}

/*
 * Get AT&T node data by MACAddress
 */
struct att_poc_node_data *att_get_node_data_by_mac(char *mac)
{
	int i;
	for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
		if (!att_poc_node_list[i].node_id) {
			continue;
		}
		if (!strcmp(mac, att_poc_node_list[i].MACAddress)) {
			return &att_poc_node_list[i];
		}
	}
	return NULL;
}

/*
 * Get AT&T avilable node
 */
uint16_t att_get_available_node_id(void)
{
        int i;
        for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
                if (!att_poc_node_list[i].node_id) {
                        return i+1;
                }
        }
	return 0;
}


/*
 * Get AT&T node id by MAC
 */
uint16_t att_get_node_index_by_mac(const char *mac)
{
        int i;
        for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
		if (!att_poc_node_list[i].node_id) {
                        continue;
                }
                if (!strcmp(mac, att_poc_node_list[i].MACAddress)) {
                        return i+1;
                }
        }
        return 0;
}

int exec_systemcmd(char *cmd, char *retBuf, int retBufSize)
{
        FILE *f;
        char *ptr = retBuf;
        int bufSize = retBufSize, bufbytes = 0, readbytes = 0;

        memset(retBuf, 0, retBufSize);

        if((f = popen(cmd, "r")) == NULL) {
                printf("popen %s error\n", cmd);
                return -1;
        }

        while(!feof(f)) {
                *ptr = 0;
                if (bufSize >= 128) {
                        bufbytes = 128;
                } else {
                        bufbytes = bufSize - 1;
                }

                fgets(ptr, bufbytes, f);
                readbytes = strlen(ptr);
                if(readbytes == 0)
                        break;
                bufSize -= readbytes;
                ptr += readbytes;
        }
        pclose(f);
        retBuf[retBufSize-1]=0;
        return 0;
}

void att_node_left_handler(const char *macaddr)
{
	snprintf(command, sizeof(command), ATT_POC_GET_ACTIVESTATUS, macaddr);
	exec_systemcmd(command, data, DATA_SIZE);

        int sta_active = (int)atol(data);
        if (!sta_active) {
		att_node_left(macaddr);
		log_debug("######### Removed the node: %s #########", macaddr);
	}
}


void att_node_left(const char *addr)
{
        struct node *att_node;
        struct att_node_info *info;

        log_debug("Node %s left network", addr);

        att_node = node_lookup(addr);
        if (!att_node) {
                log_err("Cannot find node %s", addr);
                return;
        }

        info = (struct att_node_info *)node_state_get(att_node, STATE_SLOT_NET);
        ASSERT(info != NULL);

	info->node_id = 0;
        node_left(att_node);
}

void att_set_poll_period(int att_poll_period)
{
	att_poll_period_min = att_poll_period;
}


void att_poll(void)
{
	char *line = NULL;
        size_t len = 0;
        FILE *fd = NULL;
        ssize_t read = 0;
	int i = 0;
	int is_node_exists = 0;
	char *att_macaddr = NULL;
	int mac[MAC_ADDR_LEN] = {'\0'};

	sprintf(command, ATT_POC_GET_MACADDRESS);
        exec_systemcmd(command, data, DATA_SIZE);

        fd = fopen(ATT_POC_ASSOCLIST, "r");
        if (fd) {
            while ((read = getline(&line, &len, fd)) != -1) {
		log_info("####Assoc node mac######: %s ##########", line);

		is_node_exists = 0;

		if (sscanf(line, "%02X:%02X:%02X:%02X:%02X:%02X", &mac[0], &mac[1], &mac[2], 
					&mac[3], &mac[4], &mac[5]) != MAC_ADDR_LEN) {
			continue;
		}


		for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
                	if (!att_poc_node_list[i].node_id) {
                        	continue;
                	}
                	
			if (!strcmp(line, att_poc_node_list[i].MACAddress)) {
				is_node_exists = 1;
				break;
			}
		}
			
		if (!is_node_exists) {
			vt_node_add(line);
                	vt_set_node_data(line, ATT_POC_MACADDRESS, line);
		}
	    }
	    
	    free(line);
	    fclose(fd);
	}

        for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
                if (!att_poc_node_list[i].node_id) {
                        continue;
                }
                att_macaddr = att_poc_node_list[i].MACAddress;

		log_info("#####Node mac in list : %s ###", att_macaddr);

		snprintf(command, sizeof(command), ATT_POC_GET_ACTIVESTATUS, att_macaddr);
                exec_systemcmd(command, data, DATA_SIZE);
                att_set_node_data(att_macaddr, ATT_POC_ACTIVE, data);

		int sta_active = (int)atol(data);
                if (!sta_active) {
			att_node_left(att_macaddr);
			memset(&att_poc_node_list[i], 0 , sizeof(struct att_poc_node_data));
			//att_poc_node_list[i].att_poll_period_min = 0;
			log_debug("Removing the node: %s", att_macaddr);
                        continue;
		}


		snprintf(command, sizeof(command), ATT_POC_GET_RSSI, att_macaddr);
                exec_systemcmd(command, data, DATA_SIZE);

		if (att_check_data_deviation(att_macaddr, ATT_POC_RSSI, data)) {
			att_set_node_data(att_macaddr, ATT_POC_RSSI, data);
		}

		snprintf(command, sizeof(command), ATT_POC_GET_NOISE, att_macaddr);
                exec_systemcmd(command, data, DATA_SIZE);

		if (att_check_data_deviation(att_macaddr, ATT_POC_NOISE, data)) {
			att_set_node_data(att_macaddr, ATT_POC_NOISE, data);
                }


		if (att_poc_node_list[i].att_poll_period_min >= att_poll_period_min || att_poc_node_list[i].att_poll_period_min == 0) {

			snprintf(command, sizeof(command), ATT_POC_GET_RSSI, att_macaddr);
			exec_systemcmd(command, data, DATA_SIZE);
			att_set_node_data(att_macaddr, ATT_POC_RSSI, data);

			snprintf(command, sizeof(command), ATT_POC_GET_NOISE, att_macaddr);
			exec_systemcmd(command, data, DATA_SIZE);
			att_set_node_data(att_macaddr, ATT_POC_NOISE, data);

			att_poc_node_list[i].att_poll_period_min = 0;
		}

		att_poc_node_list[i].att_poll_period_min += 1;

                snprintf(command, sizeof(command), ATT_POC_GET_STATION_TYPE, att_macaddr);
                exec_systemcmd(command, data, DATA_SIZE);
                att_set_node_data(att_macaddr, ATT_POC_STATION_TYPE, data);

                snprintf(command, sizeof(command), ATT_POC_GET_SSID, att_macaddr);
                exec_systemcmd(command, data, DATA_SIZE);
                att_set_node_data(att_macaddr, ATT_POC_SSID, data);

		snprintf(command, sizeof(command), ATT_POC_GET_CHANNEL, att_macaddr);
                exec_systemcmd(command, data, DATA_SIZE);
                att_set_node_data(att_macaddr, ATT_POC_CHANNEL, data);

                sprintf(command, ATT_POC_GET_PARENT_NODE);
                exec_systemcmd(command, data, DATA_SIZE);
                att_set_node_data(att_macaddr, ATT_POC_PARENT_NODE, data);
	}

}


/*
 * Get node prop info
 */
static struct nd_prop_info *att_get_node_prop_info(uint16_t device_id)
{
	/*int i;
	for (i = 0; i < ARRAY_LEN(prop_info_array); i++) {
		if (device_id == prop_info_array[i].device_id) {
			return &prop_info_array[i];
		}
	}
	return NULL;*/
	return &prop_info_array[0];
}

/*
 * Get node prop
 */
static struct node_prop *att_get_node_prop(struct node *vt_node, char *name)
{
	struct att_node_info *info;
	struct nd_prop_info *prop_info;
	struct node_prop *nd_prop;

	ASSERT(vt_node != NULL);
	ASSERT(name != NULL);

	info = (struct att_node_info *)node_state_get(vt_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	prop_info = att_get_node_prop_info(info->device_id);
	if (!prop_info) {
		log_err("node %s cannot find prop info", vt_node->addr);
		return NULL;
	}

	nd_prop = node_prop_lookup(vt_node, prop_info->subdevive_key,
	    prop_info->template_key, name);

	return nd_prop;
}

/*
 * Send node prop to cloud
 */
static void att_send_node_prop(struct node *vt_node, char *name, void *value)
{
	struct node_prop *nd_prop;
	int ret;

	ASSERT(vt_node != NULL);
	ASSERT(name != NULL);
	ASSERT(value != NULL);

	nd_prop = att_get_node_prop(vt_node, name);
	if (!nd_prop) {
		log_err("node %s does not have property %s",
		    vt_node->addr, name);
		return;
	}

	switch (nd_prop->type) {
	case PROP_INTEGER:
		ret = node_prop_integer_send(vt_node, nd_prop, *(int *)value);
		break;
	case PROP_STRING:
		ret = node_prop_string_send(vt_node, nd_prop, (char *)value);
		break;
	case PROP_BOOLEAN:
		ret = node_prop_boolean_send(vt_node, nd_prop, *(bool *)value);
		break;
	case PROP_DECIMAL:
		ret = node_prop_decimal_send(vt_node, nd_prop,
		    *(double *)value);
		break;
	default:
		log_err("node %s does not support type 0x%X property %s",
		    vt_node->addr, nd_prop->type, nd_prop->name);
		ret = -1;
		break;
	}

	if (ret < 0) {
		log_err("node %s sent property %s fail", vt_node->addr, name);
		return;
	}

	log_debug("node %s updated property %s success", vt_node->addr, name);
	return;
}

static void att_debug_print_node_info(const struct att_node_info *info)
{
	struct att_poc_node_data *data;

	ASSERT(info != NULL);

	log_debug("node addr=%s, version=%s, oem_model=%s, "
	    "interface=%d, power=%d, online=%d",
	    info->node->addr, info->node->version, info->node->oem_model,
	    info->node->interface, info->node->power, info->node->online);

	log_debug("node_id=0x%04X, device_id=0x%04X",
	    info->node_id, info->device_id);

	data = att_get_node_data(info->node_id);
	if (data == NULL) {
		log_debug("Cannot get node %d data", info->node_id);
		return;
	}
	log_debug("node_id=%d", data->node_id);

	log_debug("Active=%d, MACAddress=%s",data->Active, data->MACAddress);

	log_debug("RSSI=%d, Noise=%d, StationType=%s", data->RSSI, data->Noise, data->StationType);

	log_debug("SSID=%s, Channel=%d, ParentNode=%s", data->SSID, data->Channel, data->ParentNode);
}

/*
 * Get node_id by the node.
 */
uint16_t att_get_node_id(struct node *vt_node)
{
	struct att_node_info *info;
	ASSERT(vt_node != NULL);
	info = (struct att_node_info *)node_state_get(vt_node, STATE_SLOT_NET);
	ASSERT(info != NULL);
	return info->node_id;
}

/*
 * If found node_id, set the node_eui info to arg.
 */
static int att_node_id_cmp(struct node *vt_node, void *arg)
{
	struct att_node_info *iftmp, *ifarg;

	ASSERT(vt_node != NULL);
	ASSERT(arg != NULL);

	if (vt_node->interface != GI_VT) {
		return 0;
	}

	ifarg = (struct att_node_info *)arg;
	iftmp = (struct att_node_info *)node_state_get(vt_node, STATE_SLOT_NET);
	ASSERT(iftmp != NULL);
	if (iftmp->node_id == ifarg->node_id) {
		ifarg->node = iftmp->node;
		return 1;
	} else {
		return 0;
	}
}

/*
 * Get node by the node_id.
 */
static struct node *att_get_node(uint16_t node_id)
{
	struct att_node_info ifarg;
	memset(&ifarg, 0, sizeof(ifarg));
	ifarg.node_id = node_id;
	ifarg.node = NULL;
	node_foreach(att_node_id_cmp, (void *)&ifarg);
	return ifarg.node;
}

/*
 * Cleanup function for att_node_info.
 */
static void att_node_state_cleanup(void *arg)
{
	struct att_node_info *info = (struct att_node_info *)arg;
	ASSERT(info != NULL);
	log_debug("Clean node %s info", info->node->addr);
	free(info);
}

/*
 * Add node to gateway node list.
 */
int att_node_add(const char *mac_addr)
{
	struct node *vt_node;
	struct att_node_info *tmp, *info;
	struct att_poc_node_data *data;


	tmp = (struct att_node_info *)calloc(1, sizeof(struct att_node_info));
	if (!tmp) {
		log_err("malloc node_info memory failed");
		return -1;
	}

	vt_node = node_joined(mac_addr, NODE_OEM_MODEL, GI_VT, GP_MAINS, "-");
	if (!vt_node) {
		log_err("node %s add fail", mac_addr);
		free(tmp);
		return -1;
	}

	info = (struct att_node_info *)node_state_get(vt_node, STATE_SLOT_NET);
	if (info != NULL) {
		log_debug("node info already exists for node %s", mac_addr);
		free(tmp);
	} else {
		info = tmp;
		node_state_set(vt_node, STATE_SLOT_NET, info,
		    att_node_state_cleanup);
		info->node = vt_node;
	}
	
	info->node_id = att_get_node_index_by_mac(mac_addr);
	if (!info->node_id) {
		info->node_id = att_get_available_node_id();
		log_debug("node %s Getting new available node id", mac_addr);
        }


	info->device_id = info->node_id;
	data = att_get_node_data(info->node_id);
	data->node_id = info->node_id;
	strncpy(data->MACAddress, mac_addr, ATT_POC_ADDR_LEN);
        data->MACAddress[ATT_POC_ADDR_LEN - 1] = '\0';

	log_debug("node %s add success", mac_addr);
	att_debug_print_node_info(info);

	return 0;
}




/*
 * Associate a node template definition table with a node.  Used by the
 * simulator to setup a node supporting the desired characteristics.
 */
static void att_template_add(struct node *vt_node,
	const char *subdevice, const char *template, const char *version,
	const struct node_prop_def *table, size_t table_size)
{
	for (; table_size; --table_size, ++table) {
		node_prop_add(vt_node, subdevice, template, table, version);
	}
}

/*
 * Set node template
 */
static int att_set_node_template(struct node *vt_node, uint16_t device_id)
{
	struct nd_prop_info *prop_info;
	prop_info = att_get_node_prop_info(device_id);
	if (prop_info) {
		att_template_add(vt_node, prop_info->subdevive_key,
		    prop_info->template_key, prop_info->template_version,
		    prop_info->prop_def, prop_info->def_size);
		return 0;
	} else {
		log_err("Don't support device_id = 0x%04X", device_id);
		return -1;
	}
}

/*
 * Set query handle complete callback
 */
void att_set_query_complete_cb(struct node *vt_node,
		void (*callback)(struct node *, enum node_network_result))
{
	struct att_node_info *info;
	int ret;
	enum node_network_result result = NETWORK_SUCCESS;

	info = (struct att_node_info *)node_state_get(vt_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	ret = att_set_node_template(vt_node, info->device_id);
	if (ret < 0) {
		result = NETWORK_UNSUPPORTED;
	}
	log_debug("node %s", vt_node->addr);

	if (callback) {
		callback(vt_node, result);
	}
}

/*
 * Send all node props to cloud after config handle complete
 */
static int att_send_node_props(struct node *vt_node, bool update)
{
	struct att_poc_node_data *data;
	ASSERT(vt_node != NULL);

	data = att_get_node_data_by_mac(vt_node->addr);
	if (data == NULL) {
		log_debug("Cannot get node %s data", vt_node->addr);
		return -1;
	}

	node_prop_batch_begin(vt_node);

	if ((update && data->update_Active) || !update) {
		/* Must send node status first */
		node_conn_status_changed(vt_node, data->Active);

		att_send_node_prop(vt_node, ATT_POC_ACTIVE, &data->Active);
		data->update_Active = 0;
	}
	
	
	if ((update && data->update_MACAddress) || !update) {
		att_send_node_prop(vt_node, ATT_POC_MACADDRESS,
		    data->MACAddress);
		data->update_MACAddress = 0;
	}

	if ((update && data->update_RSSI) || !update) {
                att_send_node_prop(vt_node, ATT_POC_RSSI, &data->RSSI);
                data->update_RSSI = 0;
        }

	if ((update && data->update_Noise) || !update) {
                att_send_node_prop(vt_node, ATT_POC_NOISE, &data->Noise);
                data->update_Noise = 0;
        }


	if ((update && data->update_Channel) || !update) {
                att_send_node_prop(vt_node, ATT_POC_CHANNEL, &data->Channel);
                data->update_Channel = 0;
        }

	if ((update && data->update_StationType) || !update) {
                att_send_node_prop(vt_node, ATT_POC_STATION_TYPE, data->StationType);
                data->update_StationType = 0;
        }

	if ((update && data->update_SSID) || !update) {
                att_send_node_prop(vt_node, ATT_POC_SSID, data->SSID);
                data->update_SSID = 0;
        }


	if ((update && data->update_ParentNode) || !update) {
                att_send_node_prop(vt_node,  ATT_POC_PARENT_NODE, data->ParentNode);
                data->update_ParentNode = 0;
        }

	node_prop_batch_end(vt_node);
	return 0;
}

/*
 * Set config handle complete callback
 */
void att_set_config_complete_cb(struct node *vt_node,
		void (*callback)(struct node *, enum node_network_result))
{
	struct att_node_info *info;

	ASSERT(vt_node != NULL);
	info = (struct att_node_info *)node_state_get(vt_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (callback) {
		callback(vt_node, NETWORK_SUCCESS);
	}

	att_send_node_props(vt_node, false);
}

/*
 * Set leave handle complete callback
 */
void att_set_leave_complete_cb(struct node *vt_node,
		void (*callback)(struct node *, enum node_network_result))
{
	if (callback) {
		callback(vt_node, NETWORK_SUCCESS);
	}
}

/*
 * Handle prop set reply
 */
void att_prop_complete_handler(uint16_t node_id, char *name, uint8_t status)
{
	return;
}

/*
 * Appd convert node info to json object when save node info to config file
 */
json_t *att_conf_save_handler(const struct node *vt_node)
{
	struct att_node_info *info;
	json_t *info_obj;

	ASSERT(vt_node != NULL);
	if (vt_node->interface != GI_VT) {
		log_info("node %s is not virtual node", vt_node->addr);
		return NULL;
	}
	info = (struct att_node_info *)node_state_get(
	    (struct node *)vt_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	info_obj = json_object();
	if (info_obj == NULL) {
		return NULL;
	}

	json_object_set_new(info_obj, "node_id", json_integer(info->node_id));
	json_object_set_new(info_obj, "device_id",
	    json_integer(info->device_id));
	return info_obj;
}

/*
 * Appd get node info from json object when restore node info from config file
 */
int att_conf_loaded_handler(struct node *vt_node, json_t *info_obj)
{
	struct att_node_info *info;
	json_t *jobj;

	ASSERT(vt_node != NULL);
	if (vt_node->interface != GI_VT) {
		log_info("node %s is not virtual node", vt_node->addr);
		return 0;
	}
	ASSERT(info_obj != NULL);

	info = (struct att_node_info *)calloc(1, sizeof(struct att_node_info));
	if (!info) {
		log_err("malloc memory failed for %s", vt_node->addr);
		return -1;
	}

	node_state_set(vt_node, STATE_SLOT_NET, info, att_node_state_cleanup);

	info->node = vt_node;
	jobj = json_object_get(info_obj, "node_id");
	info->node_id = json_integer_value(jobj);
	jobj = json_object_get(info_obj, "device_id");
	info->device_id = json_integer_value(jobj);

	att_debug_print_node_info(info);
	return 0;
}

/*
 * Update node prop to cloud
 */
void att_update_node_prop(uint16_t node_id, char *name, void *value)
{
	struct node *vt_node;

	vt_node = att_get_node(node_id);
	if (!vt_node) {
		/* node did not join local network, do nothing */
		log_debug("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	att_send_node_prop(vt_node, name, value);
	return;
}

/*
 * Update node decimal prop
 */
void att_update_decimal_prop(uint16_t node_id, char *name, double value)
{
	struct node *vt_node;

	log_debug("decimal prop name %s, value=%lf from source=0x%04X",
	    name, value, node_id);

	vt_node = att_get_node(node_id);
	if (!vt_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	att_send_node_prop(vt_node, name, (void *)&value);
	return;
}

/*
 * Update node int prop
 */
void att_update_int_prop(uint16_t node_id, char *name, int value)
{
	struct node *vt_node;

	log_debug("int prop name %s, value=%d from source=0x%04X",
	    name, value, node_id);

	vt_node = att_get_node(node_id);
	if (!vt_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	att_send_node_prop(vt_node, name, (void *)&value);
	return;
}



int att_check_data_deviation(char *macaddr, char *name, char *value)
{
	struct att_poc_node_data *data;
        int tmp;
	int deviation = 0;

        data = att_get_node_data_by_mac(macaddr);

        if (data == NULL) {
                log_debug("Cannot get node %s data", macaddr);
                return 0;
        }

	if (!strcmp(name, ATT_POC_RSSI)) {

                tmp = atoi(value);

		deviation = abs(abs(tmp) - abs(data->RSSI));

		if ( deviation >= ATT_DATA_DEVIATION_DB) {
			return 1;
		} else {
			return 0;
		}

        } else if (!strcmp(name, ATT_POC_NOISE)) {

                tmp = atoi(value);

                deviation = abs(abs(tmp) - abs(data->Noise));

                if ( deviation >= ATT_DATA_DEVIATION_DB) {
                        return 1;
                } else {
                        return 0;
                }
	}

	return 0;
}


/*
 * Set AT&T node data
 */
int att_set_node_data(char *macaddr, char *name, char *value)
{
	struct att_poc_node_data *data;
	int tmp;

	data = att_get_node_data_by_mac(macaddr);
	
	if (data == NULL) {
		log_debug("Cannot get node %s data", macaddr);
		return -1;
	}


	if (!strcmp(name, ATT_POC_ACTIVE)) {

		tmp = atoi(value);

		if (tmp == data->Active) {
                        return 0;
                }


		data->Active = tmp;
                data->update_Active = 1;

	} else if (!strcmp(name, ATT_POC_MACADDRESS)) {
		if (!strcmp(value, data->MACAddress)) {
			return 0;
		}
		strncpy(data->MACAddress, value, ATT_POC_ADDR_LEN);
		data->MACAddress[ATT_POC_ADDR_LEN - 1] = '\0';
		data->update_MACAddress = 1;
        } else if (!strcmp(name, ATT_POC_RSSI)) {

                tmp = atoi(value);

                if (tmp == data->RSSI) {
			return 0;
                }

                data->RSSI = tmp;
                data->update_RSSI = 1;

        } else if (!strcmp(name, ATT_POC_NOISE)) {

                tmp = atoi(value);

                if (tmp == data->Noise) {
                        return 0;
                }

                data->Noise = tmp;
                data->update_Noise = 1;

        } else if (!strcmp(name, ATT_POC_CHANNEL)) {

                tmp = atoi(value);

                if (tmp == data->Channel) {
                        return 0;
                }

                data->Channel = tmp;
                data->update_Channel = 1;

        } else if (!strcmp(name, ATT_POC_SSID)) {
                if (!strcmp(value, data->SSID)) {
                        return 0;
                }
                strncpy(data->SSID, value, ATT_POC_ADDR_LEN);
                data->SSID[ATT_POC_ADDR_LEN - 1] = '\0';
                data->update_SSID = 1;
        
	} else if (!strcmp(name, ATT_POC_STATION_TYPE)) {
                if (!strcmp(value, data->StationType)) {
                        return 0;
                }
                strncpy(data->StationType, value, ATT_POC_ADDR_LEN);
                data->StationType[ATT_POC_ADDR_LEN - 1] = '\0';
                data->update_StationType = 1;
        
	} else if (!strcmp(name, ATT_POC_PARENT_NODE)) {
                if (!strcmp(value, data->ParentNode)) {
                        return 0;
                }
                strncpy(data->ParentNode, value, ATT_POC_ADDR_LEN);
                data->ParentNode[ATT_POC_ADDR_LEN - 1] = '\0';
                data->update_ParentNode = 1;
        }

	att_update_nodes_data();

	return 0;
}


/*
 * Update an AT&T node data
 */
void att_update_node_data(struct att_poc_node_data *data)
{
	struct node *vt_node;
	ASSERT(data != NULL);

	vt_node = node_lookup(data->MACAddress);
	if (vt_node) {
		if (att_send_node_props(vt_node, true) < 0) {
			log_debug("att_send_node_props %s error",
			    data->MACAddress);
		}
	} else {
		if (att_node_add(data->MACAddress) < 0) {
			log_debug("att_node_add %s, %d error",
			    data->MACAddress, data->node_id);
		}
	}
}

/*
 * Update AT&T nodes data
 */
void att_update_nodes_data(void)
{
	int i;

	for (i = 0; i < ARRAY_LEN(att_poc_node_list); i++) {
		if (att_poc_node_list[i].node_id) {
			att_update_node_data(&att_poc_node_list[i]);
		}
	}
}
