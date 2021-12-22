/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

/* This c file provides all customer callbacks */


#include "ember_stack_include.h"

#include <ayla/log.h>
#include <ayla/assert.h>

#include "appd_interface.h"

#include <time.h>
#include <sys/time.h>



#define ANNO_NET_ADDR_OFFSET   1
#define ANNO_IEEE_ADDR_OFFSET    (ANNO_NET_ADDR_OFFSET + 2)

#define STATUS_OFFSET   1

#define IEEE_ADDR_OFFSET   2
#define NET_ADDR_OFFSET    (IEEE_ADDR_OFFSET + 8)

#define POWER_SOURCE_OFFSET   5

#define PROFILE_ID_OFFSET   6
#define DEVICE_ID_OFFSET    (PROFILE_ID_OFFSET + 2)

#define ATTRIBUTE_ID_OFFSET   3


#pragma pack(1)

/*
 * ZCL response message capability field struct
 */
struct zdo_capa {
	uint8_t alternate_pan:1;
	uint8_t device_type:1;
	uint8_t power_source:1;
	uint8_t receiver_idle:1;
	uint8_t reserved:2;
	uint8_t security:1;
	uint8_t allocate_addr:1;
};

/*
 * ZDO match descriptor request message struct
 */
struct zdo_match_req {
	uint8_t seq_no;
	uint16_t net_addr;
	uint16_t profile_id;
	uint8_t in_cnt;
	uint16_t in[0];
	uint8_t out_cnt;
	uint16_t out[0];
};

/*
 * ZDO match descriptor response message struct
 */
struct zdo_match_resp {
	uint8_t seq_no;
	uint8_t status;
	uint16_t net_addr;
	uint8_t entpoint_cnt;
	uint8_t entpoint[0];
};

/*
 * ZDO announce message struct
 */
struct zdo_anno {
	uint8_t seq_no;
	uint16_t net_addr;
	uint8_t ieee_addr[EUI64_SIZE];
	struct zdo_capa capability;
};

/*
 * ZDO ieee/network address response message struct
 */
struct zdo_addr_resp {
	uint8_t seq_no;
	uint8_t status;
	uint8_t ieee_addr[EUI64_SIZE];
	uint16_t net_addr;
};

/*
 * ZDO power descriptor response message power descriptor field struct
 */
struct zdo_power_desc {
	uint8_t current_mode:4;
	uint8_t available_source:4;
	uint8_t constant_power:1;
	uint8_t rechargeable_battery:1;
	uint8_t disposable_battery:1;
	uint8_t reserved:1;
	uint8_t source_level:4;
};

/*
 * ZDO power descriptor response message struct
 */
struct zdo_power_resp {
	uint8_t seq_no;
	uint8_t status;
	uint16_t net_addr;
	struct zdo_power_desc power_desc;
};

/*
 * ZDO simple descriptor response message struct
 */
struct zdo_simp_resp {
	uint8_t seq_no;
	uint8_t status;
	uint16_t net_addr;
	uint8_t length;
	uint8_t endpoint;
	uint16_t profileId;
	uint16_t deviceId;
	uint8_t version:4;
	uint8_t reserved:4;
	uint8_t in_cluster_cnt;
	uint16_t in_cluster[0];
	/* uint8_t out_cluster_cnt;
	uint16_t out_cluster[0]; */
};

/*
 * ZCL response message control field struct
 */
struct zcl_frm_ctrl {
	uint8_t frame_type:2;
	uint8_t manf_specific:1;
	uint8_t direction:1;
	uint8_t dis_def_resp:1;
	uint8_t reserved:3;
};

/*
 * ZCL message struct
 */
struct zcl_msg {
	struct zcl_frm_ctrl ctrl;
	uint8_t seq_no;
	uint8_t cmd_id;
	uint8_t msg[0];
};

/*
 * ZCL read attribute response message struct
 */
struct zcl_read_attr_resp {
	uint16_t attr_id;
	uint8_t status;
	uint8_t data_type;
	uint8_t value[0];
};

/*
 * ZCL write attribute message struct
 */
struct zcl_write_attr {
	uint16_t attr_id;
	uint8_t data_type;
	uint8_t value[0];
};

/*
 * ZCL write attribute response message struct
 */
struct zcl_write_attr_resp {
	uint8_t status;
	uint16_t attr_id;
};

/*
 * ZCL attribute report message struct
 */
struct zcl_attr_rept {
	uint16_t attr_id;
	uint8_t data_type;
	uint8_t value;
};

/*
 * ZCL ias zone enroll request message struct
 */
struct zcl_ias_enroll_req {
	uint16_t zone_type;
	uint16_t manufacter_code;
};

/*
 * ZCL ias zone read attribute response message struct
 */
struct zcl_ias_read_resp {
	uint16_t attr_id;
	uint8_t status;
	uint8_t data_type;
	uint8_t value;
};

/*
 * ZCL ias zone write attribute response message struct
 */
struct zcl_ias_write_resp {
	uint8_t status;
};

/*
 * ZCL default response message struct
 */
struct zcl_def_resp {
	uint8_t cmd_id;
	uint8_t status;
};

/*
 * ZDO bind response message struct
 */
struct zdo_bind_resp {
	uint8_t seq_no;
	uint8_t status;
};

#pragma pack()


static void debug_print_memory(const void *addr, uint32_t len)
{
	char buf[64];
	uint8_t *paddr = (uint8_t *)addr;
	int i, j, k;

	i = 0;
	while (i < len) {
		memset(buf, 0, sizeof(buf));
		for (j = 0; ((j < 20) && (i + j < len)); j++) {
			k = ((paddr[i + j] & 0xF0) >> 4);
			buf[j * 3 + 0] = ((k < 10)
			    ? (k + '0') : (k - 10 + 'A'));
			k = (paddr[i + j] & 0x0F);
			buf[j * 3 + 1] = ((k < 10)
			    ? (k + '0') : (k - 10 + 'A'));
			buf[j * 3 + 2] = ' ';
		}
		log_debug("%s", buf);
		i += j;
	}
}

static void debug_print_join_info(EmberNodeId newNodeId,
			EmberEUI64 newNodeEui64,
			EmberNodeId parentOfNewNode,
			EmberDeviceUpdate status,
			EmberJoinDecision decision)
{
	uint8_t *pEui = (uint8_t *)newNodeEui64;
	/* Corresponds to the EmberJoinDecision status codes */
	PGM_NO_CONST PGM_P joinDecisionText[] = {
		EMBER_JOIN_DECISION_STRINGS
	};
	/* Corresponds to the EmberDeviceUpdate status codes */
	PGM_NO_CONST PGM_P deviceUpdateText[] = {
		EMBER_DEVICE_UPDATE_STRINGS
	};

	log_debug("nodeId=0x%04X, nodeEui=%02X%02X%02X%02X%02X%02X%02X%02X, "
	    "parent nodeId=0x%04X",
	    newNodeId, pEui[7], pEui[6], pEui[5], pEui[4],
	    pEui[3], pEui[2], pEui[1], pEui[0], parentOfNewNode);

	log_debug("deviceUpdateStatus=%s, joinDecision=%s",
	    deviceUpdateText[status], joinDecisionText[decision]);
}

/*
 * Handle HA profile message
 */
static bool zbc_zdo_profile_msg_handle(uint16_t source, uint16_t cluster_id,
			uint8_t *message, uint16_t msgLen)
{
	struct zdo_match_req *match;
	uint8_t *ptr;
	struct zdo_anno *anno;
	struct zdo_addr_resp *addr_resp;
	struct zdo_power_resp *power_resp;
	struct zdo_simp_resp *simp_resp;
	struct zdo_bind_resp *bind;
	uint8_t powerLevel;
	bool ret = false;

	switch (cluster_id) {
	case MATCH_DESCRIPTORS_REQUEST:
		match = (struct zdo_match_req *)message;
		log_debug("Received match descriptor request msg seq_no=0x%02X"
		    " net_addr=0x%04X profile_id=0x%04X from source=0x%04X",
		    match->seq_no, match->net_addr, match->profile_id, source);
		ptr = ((uint8_t *)&(match->in))
		    + (match->in_cnt * sizeof(uint16_t));
		match->out_cnt = *ptr;
		match->out[0] = *(uint16_t *)(ptr + 1);
		if ((match->out_cnt == 1)
		    && (match->out[0] == ZCL_IAS_ZONE_CLUSTER_ID)) {
			appd_motion_match_handler(source, match->seq_no);
			ret = true;
		};
		break;
	case END_DEVICE_ANNOUNCE:
		anno = (struct zdo_anno *)message;
		log_debug("Received device announce msg seq_no=0x%02X"
		    " net_addr=0x%04X from source=0x%04X",
		    anno->seq_no, anno->net_addr, source);
		appd_node_join_network(anno->ieee_addr, anno->net_addr);
		break;
	case LEAVE_REQUEST:
		log_debug("Received leave request msg"
		    " from source=0x%04X", source);
		break;
	case NETWORK_ADDRESS_RESPONSE:
		addr_resp = (struct zdo_addr_resp *)message;
		if (addr_resp->status == 0) {
			log_debug("Received network address response msg"
			    " nodeEui=%02X%02X%02X%02X%02X%02X%02X%02X,"
			    " nodeId=0x%04X, source=0x%04X",
			    addr_resp->ieee_addr[7], addr_resp->ieee_addr[6],
			    addr_resp->ieee_addr[5], addr_resp->ieee_addr[4],
			    addr_resp->ieee_addr[3], addr_resp->ieee_addr[2],
			    addr_resp->ieee_addr[1], addr_resp->ieee_addr[0],
			    addr_resp->net_addr, source);
		} else {
			log_err("Received network address response"
			    " status=0x%02X from source=0x%04X",
			    addr_resp->status, source);
		}
		break;
	case IEEE_ADDRESS_RESPONSE:
		addr_resp = (struct zdo_addr_resp *)message;
		if (addr_resp->status == 0) {
			log_debug("Received IEEE address response msg"
			    " nodeEui=%02X%02X%02X%02X%02X%02X%02X%02X,"
			    " nodeId=0x%04X, source=0x%04X",
			    addr_resp->ieee_addr[7], addr_resp->ieee_addr[6],
			    addr_resp->ieee_addr[5], addr_resp->ieee_addr[4],
			    addr_resp->ieee_addr[3], addr_resp->ieee_addr[2],
			    addr_resp->ieee_addr[1], addr_resp->ieee_addr[0],
			    addr_resp->net_addr, source);
			appd_update_node_id(addr_resp->ieee_addr,
			    addr_resp->net_addr);
		} else {
			log_err("Received IEEE address response"
			    " status=0x%02X from source=0x%04X",
			    addr_resp->status, source);
		}
		break;
	case NODE_DESCRIPTOR_RESPONSE:
		log_debug("Received node descriptor response msg"
		    " from source=0x%04X", source);
		break;
	case POWER_DESCRIPTOR_RESPONSE:
		power_resp = (struct zdo_power_resp *)message;
		if (power_resp->status == 0) {
			switch (power_resp->power_desc.source_level) {
			case 0x0:
				powerLevel = 0;
				break;
			case 0xC:
				powerLevel = 100;
				break;
			default:
				powerLevel = 0;
				break;
			}
			log_debug("Received power descriptor response msg from"
			    " source=0x%04X, powerType=%d, powerLevel=%d",
			    source, power_resp->power_desc.constant_power,
			    powerLevel);
			appd_power_complete_handler(source,
			    power_resp->power_desc.constant_power, powerLevel);
		} else {
			log_err("Received power descriptor response"
			    " status=0x%02X from source=0x%04X",
			    power_resp->status, source);
		}
		break;
	case SIMPLE_DESCRIPTOR_RESPONSE:
		simp_resp = (struct zdo_simp_resp *)message;
		if (simp_resp->status == 0) {
			appd_simple_complete_handler(source,
			    simp_resp->profileId, simp_resp->deviceId);
		} else {
			log_err("Received simple descriptor response"
			    " status=0x%02X from source=0x%04X",
			    simp_resp->status, source);
		}
		break;
	case BIND_RESPONSE:
		bind = (struct zdo_bind_resp *)message;
		appd_bind_response_handler(source, bind->status, 0);
		break;
	case UNBIND_RESPONSE:
		bind = (struct zdo_bind_resp *)message;
		appd_bind_response_handler(source, bind->status, 1);
		break;
	case LEAVE_RESPONSE:
		log_debug("Received leave response msg"
		    " from source=0x%04X", source);
		break;
	default:
		log_debug("Received cluster_id=0x%04X message"
		    " from source=0x%04X", cluster_id, source);
		debug_print_memory(message, msgLen);
		break;
	}

	return ret;
}

/*
 * Handle read attribute response message
 */
static void zbc_read_attr_resp_handle(uint16_t source, uint8_t *msg)
{
	struct zcl_read_attr_resp *attr;
	char model_id[ZB_MODEL_ID_LEN] = "-";
	int len;
	uint8_t primary_power = EMBER_ZCL_POWER_SOURCE_UNKNOWN;

	attr = (struct zcl_read_attr_resp *)msg;
	log_debug("Received read attribute response attr id=0x%02X"
		    " from source=0x%04X", attr->attr_id, source);

	if (attr->attr_id == ZCL_MODEL_IDENTIFIER_ATTRIBUTE_ID) {
		if (attr->status == 0) {
			if (attr->data_type
			    == ZCL_CHAR_STRING_ATTRIBUTE_TYPE) {
				/* first byte is length for string data */
				if (attr->value[0] > (ZB_MODEL_ID_LEN - 1)) {
					len = (ZB_MODEL_ID_LEN - 1);
				} else {
					len = attr->value[0];
				}
				strncpy(model_id,
				    (char *)&attr->value[1], len);
				model_id[len] = '\0';
			} else {
				log_err("data type %d wrong", attr->data_type);
			}
		} else {
			log_err("status %d error", attr->status);
		}
		appd_model_identifier_complete_handler(source, model_id);
	} else if (attr->attr_id == ZCL_POWER_SOURCE_ATTRIBUTE_ID) {
		if (attr->status == 0) {
			if (attr->data_type == ZCL_ENUM8_ATTRIBUTE_TYPE) {
				primary_power = (attr->value[0] & 0x0F);
			} else {
				log_err("data type %d wrong", attr->data_type);
			}
		} else {
			log_err("status %d error", attr->status);
		}
		appd_power_source_complete_handler(source, primary_power);
	}
	return;
}

/*
 * Handle basic cluster message
 */
static void zbc_basic_cluster_msg_handle(uint16_t source,
			uint8_t cmd_id, uint8_t *msg)
{
	switch (cmd_id) {
	case ZCL_READ_ATTRIBUTES_RESPONSE_COMMAND_ID:
		zbc_read_attr_resp_handle(source, msg);
		break;
	default:
		log_debug("Received basic cluster type=%d message"
		    " from source=0x%04X", cmd_id, source);
		break;
	}
}

/*
 * Update power configuration report attribute
 */
static void zbc_power_cfg_report_attribute(uint16_t source, uint16_t attr_id,
			uint8_t data_type, uint8_t *data)
{
	if (attr_id == ZCL_BATTERY_PERCENTAGE_REMAINING_ATTRIBUTE_ID) {
		if (data_type == ZCL_INT8U_ATTRIBUTE_TYPE) {
			appd_update_int_prop(source, ZB_POWER_LEV_PROP_NAME,
			    data[0]);
		}
	}
}

/*
 * Update thermostat report attribute
 */
static void zbc_thermostat_report_attribute(uint16_t source, uint16_t attr_id,
			uint8_t data_type, uint8_t *data)
{
	if (attr_id == ZCL_LOCAL_TEMPERATURE_ATTRIBUTE_ID) {
		if (data_type == ZCL_INT16S_ATTRIBUTE_TYPE) {
			appd_update_decimal_prop(source, ZB_LOCAL_TEMPERATURE,
			    (*(int16_t *)data / (double)100.00));
		}
	} else if (attr_id == ZCL_OCCUPIED_COOLING_SETPOINT_ATTRIBUTE_ID) {
		if (data_type == ZCL_INT16S_ATTRIBUTE_TYPE) {
			appd_update_int_prop(source, ZB_COOLING_SETPOINT,
			    (*(int16_t *)data / 100));
		}
	} else if (attr_id == ZCL_OCCUPIED_HEATING_SETPOINT_ATTRIBUTE_ID) {
		if (data_type == ZCL_INT16S_ATTRIBUTE_TYPE) {
			appd_update_int_prop(source, ZB_HEATING_SETPOINT,
			    (*(int16_t *)data / 100));
		}
	} else if (attr_id == ZCL_SYSTEM_MODE_ATTRIBUTE_ID) {
		if (data_type == ZCL_ENUM8_ATTRIBUTE_TYPE) {
			appd_update_int_prop(source, ZB_SYSTEM_MODE,
			    *data);
		}
	}
}

/*
 * Update fan control report attribute
 */
static void zbc_fan_control_report_attribute(uint16_t source, uint16_t attr_id,
			uint8_t data_type, uint8_t *data)
{
	if (attr_id == ZCL_FAN_CONTROL_FAN_MODE_ATTRIBUTE_ID) {
		if (data_type == ZCL_ENUM8_ATTRIBUTE_TYPE) {
			appd_update_int_prop(source, ZB_FAN_MODE,
			    *data);
		}
	}
}

/*
 * Handle report attributes message
 */
static void zbc_report_attribute_msg_handle(uint16_t source,
			uint16_t clusterId, uint8_t *msg, uint16_t msg_len)
{
	struct attr_rept {
		uint16_t t_attr_id;
		uint8_t l_data_type;
		uint8_t v_data[0];
	} *attr;
	uint8_t data_len;
	int tlv_len = 0;

	if (!msg) {
		log_err("msg is NULL from source=0x%04X", source);
		return;
	}

	log_debug("Cluster 0x%04X attribute report message msg_len=%d"
	    " from source=0x%04X", clusterId, msg_len, source);

	debug_print_memory(msg, msg_len);

	while (tlv_len < msg_len) {
		attr = (struct attr_rept *)(msg + tlv_len);
		data_len = emberAfGetDataSize(attr->l_data_type);
		log_debug("attribute 0x%04X report data_len=%u"
		    " from source=0x%04X",
		    attr->t_attr_id, data_len, source);
		if (clusterId == ZCL_POWER_CONFIG_CLUSTER_ID) {
			zbc_power_cfg_report_attribute(source,
			    attr->t_attr_id, attr->l_data_type, attr->v_data);
		} else if (clusterId == ZCL_THERMOSTAT_CLUSTER_ID) {
			zbc_thermostat_report_attribute(source,
			    attr->t_attr_id, attr->l_data_type, attr->v_data);
		} else if (clusterId == ZCL_FAN_CONTROL_CLUSTER_ID) {
			zbc_fan_control_report_attribute(source,
			    attr->t_attr_id, attr->l_data_type, attr->v_data);
		}
		tlv_len = (tlv_len + 2 + 1 + data_len);
	}
}

/*
 * Handle basic cluster message
 */
static void zbc_power_config_cluster_msg_handle(uint16_t source,
			uint8_t cmd_id, uint8_t *msg, uint16_t msg_len)
{
	switch (cmd_id) {
	case ZCL_REPORT_ATTRIBUTES_COMMAND_ID:
		zbc_report_attribute_msg_handle(source,
		    ZCL_POWER_CONFIG_CLUSTER_ID, msg, msg_len);
		break;
	default:
		log_debug("Received power config cluster type=%d message,"
		    " msg_len=%d from source=0x%04X",
		    cmd_id, msg_len, source);
		break;
	}
}

/*
 * Handle on off cluster message
 */
static void zbc_on_off_cluster_msg_handle(uint16_t source,
			uint8_t frame_type, uint8_t cmd_id, uint8_t *msg)
{
	struct zcl_def_resp *def;
	struct zcl_attr_rept *attr;
	if (frame_type == ZCL_GLOBAL_COMMAND) {
		log_debug("Received global command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
		switch (cmd_id) {
		case ZCL_REPORT_ATTRIBUTES_COMMAND_ID:
			attr = (struct zcl_attr_rept *)msg;
			if (attr->attr_id == ZCL_ON_OFF_ATTRIBUTE_ID) {
				log_debug("Received On/Off attribute report"
				    " value=0x%02X, from source=0x%04X",
				    attr->value, source);
				appd_update_node_prop(source,
				    ZB_ON_OFF_PROP_NAME, &(attr->value));
				appd_ieee_query_handler(source);
			}
			break;
		case ZCL_DEFAULT_RESPONSE_COMMAND_ID:
			def = (struct zcl_def_resp *)msg;
			log_debug("Received default response cmd_id==0x%02X"
			    " status=0x%02X from source=0x%04X",
			    def->cmd_id, def->status, source);
			break;
		default:
			log_debug("Received on off cluster type %d message"
			    " from source=0x%04X", cmd_id, source);
			break;
		}
	} else if (frame_type == ZCL_CLUSTER_SPECIFIC_COMMAND) {
		log_debug("Received cluster spceific command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
	} else {
		log_debug("Received invalid frame type 0x%02X"
		    " from source=0x%04X", frame_type, source);
	}
}

/*
 * Handle level control cluster message
 */
static void zbc_level_control_cluster_msg_handle(uint16_t source,
			uint8_t frame_type, uint8_t cmd_id, uint8_t *msg)
{
	struct zcl_def_resp *def;
	if (frame_type == ZCL_GLOBAL_COMMAND) {
		log_debug("Received global command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
		switch (cmd_id) {
		case ZCL_DEFAULT_RESPONSE_COMMAND_ID:
			def = (struct zcl_def_resp *)msg;
			log_debug("Received default response cmd_id=0x%02X"
			    " status=0x%02X from source=0x%04X",
			    def->cmd_id, def->status, source);
			break;
		default:
			log_debug("Received level control cluster invalid"
			    " type %d message from source=0x%04X",
			    cmd_id, source);
			break;
		}
	} else if (frame_type == ZCL_CLUSTER_SPECIFIC_COMMAND) {
		log_debug("Received cluster spceific command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
	} else {
		log_debug("Received invalid frame type 0x%02X"
		    " from source=0x%04X", frame_type, source);
	}
}

static char *zbc_get_prop_name_by_id(uint16_t cluster_id, uint16_t attr_id)
{
	struct attr_id_2_name {
		uint16_t cluster_id;
		uint16_t attr_id;
		char *name;
	};
	static struct attr_id_2_name zb_id_name[] = {
		{
			ZCL_THERMOSTAT_CLUSTER_ID,
			ZCL_SYSTEM_MODE_ATTRIBUTE_ID,
			ZB_SYSTEM_MODE
		},
		{
			ZCL_THERMOSTAT_CLUSTER_ID,
			ZCL_OCCUPIED_COOLING_SETPOINT_ATTRIBUTE_ID,
			ZB_COOLING_SETPOINT
		},
		{
			ZCL_THERMOSTAT_CLUSTER_ID,
			ZCL_OCCUPIED_HEATING_SETPOINT_ATTRIBUTE_ID,
			ZB_HEATING_SETPOINT
		},
		/*
		{
			ZCL_THERMOSTAT_CLUSTER_ID,
			ZCL_LOCAL_TEMPERATURE_ATTRIBUTE_ID,
			ZB_LOCAL_TEMPERATURE
		},
		{
			ZCL_FAN_CONTROL_CLUSTER_ID,
			ZCL_FAN_CONTROL_FAN_MODE_ATTRIBUTE_ID,
			ZB_FAN_MODE
		},
		{
			ZCL_THERMOSTAT_UI_CONFIG_CLUSTER_ID,
			ZCL_TEMPERATURE_DISPLAY_MODE_ATTRIBUTE_ID,
			ZB_TEMPERATURE_DISPLAY
		},*/
		/*
		{
			ZCL_ON_OFF_CLUSTER_ID,
			ZCL_ON_COMMAND_ID,
			ZB_ON_OFF_PROP_NAME
		},
		{
			ZCL_LEVEL_CONTROL_CLUSTER_ID,
			ZCL_STOP_WITH_ON_OFF_COMMAND_ID,
			ZB_LEVEL_CTRL_PROP_NAME
		},*/
	};

	int i;
	for (i = 0; i < (sizeof(zb_id_name) / sizeof(zb_id_name[0])); i++) {
		if ((cluster_id == zb_id_name[i].cluster_id)
		    && (attr_id == zb_id_name[i].attr_id)) {
			return zb_id_name[i].name;
		}
	}

	return NULL;
}

/*
 * Handle thermostat cluster message
 */
static void zbc_thermostat_cluster_msg_handle(uint16_t source,
			uint8_t frame_type, uint8_t cmd_id,
			uint8_t *msg, uint16_t msg_len)
{
	struct zcl_write_attr_resp *resp;
	if (frame_type == ZCL_GLOBAL_COMMAND) {
		log_debug("Received global command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
		switch (cmd_id) {
		case ZCL_WRITE_ATTRIBUTES_RESPONSE_COMMAND_ID:
			log_debug("Received write attributes response"
			    " from source=0x%04X", source);
			resp = (struct zcl_write_attr_resp *)msg;
			if (resp->status == 0) {
				log_debug("status=0x%02X, wrote success",
				    resp->status);
			} else {
				log_debug("status=0x%02X, attribute id=0x%04X",
				    resp->status, resp->attr_id);
			}
			break;
		case ZCL_REPORT_ATTRIBUTES_COMMAND_ID:
			zbc_report_attribute_msg_handle(source,
			    ZCL_THERMOSTAT_CLUSTER_ID, msg, msg_len);
			break;
		default:
			log_debug("Received thermostat cluster invalid type"
			    " %d message from source=0x%04X", cmd_id, source);
			break;
		}
	} else if (frame_type == ZCL_CLUSTER_SPECIFIC_COMMAND) {
		log_debug("Received cluster spceific command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
	} else {
		log_debug("Received invalid frame type 0x%02X"
		    " from source=0x%04X", frame_type, source);
	}
}

/*
 * Handle fan control cluster message
 */
static void zbc_fan_control_cluster_msg_handle(uint16_t source,
			uint8_t frame_type, uint8_t cmd_id,
			uint8_t *msg, uint16_t msg_len)
{
	struct zcl_write_attr_resp *resp;
	if (frame_type == ZCL_GLOBAL_COMMAND) {
		log_debug("Received global command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
		switch (cmd_id) {
		case ZCL_WRITE_ATTRIBUTES_RESPONSE_COMMAND_ID:
			log_debug("Received write attributes response"
			    " from source=0x%04X", source);
			resp = (struct zcl_write_attr_resp *)msg;
			if (resp->status == 0) {
				log_debug("status=0x%02X, wrote success",
				    resp->status);
			} else {
				log_debug("status=0x%02X, attribute id=0x%04X",
				    resp->status, resp->attr_id);
			}
			break;
		case ZCL_REPORT_ATTRIBUTES_COMMAND_ID:
			zbc_report_attribute_msg_handle(source,
			    ZCL_FAN_CONTROL_CLUSTER_ID, msg, msg_len);
			break;
		default:
			log_debug("Received fan control cluster invalid type"
			    " %d message from source=0x%04X", cmd_id, source);
			break;
		}
	} else if (frame_type == ZCL_CLUSTER_SPECIFIC_COMMAND) {
		log_debug("Received cluster spceific command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
	} else {
		log_debug("Received invalid frame type 0x%02X"
		    " from source=0x%04X", frame_type, source);
	}
}

/*
 * Handle ias zone cluster message
 */
static bool zbc_ias_zone_cluster_msg_handle(uint16_t source,
			uint8_t frame_type, uint8_t cmd_id, uint8_t *msg)
{
	struct zcl_ias_enroll_req *enroll;
	struct zcl_ias_read_resp *read;
	struct zcl_ias_write_resp *write;
	if (frame_type == ZCL_GLOBAL_COMMAND) {
		log_debug("Received global command"
		    " id=0x%02X from source=0x%04X", cmd_id, source);
		switch (cmd_id) {
		case ZCL_READ_ATTRIBUTES_RESPONSE_COMMAND_ID:
			log_debug("Received read attribute response"
			    " from source=0x%04X", source);
			read = (struct zcl_ias_read_resp *)msg;
			appd_read_zone_state_complete_handler(source,
			    read->value);
			break;
		case ZCL_WRITE_ATTRIBUTES_RESPONSE_COMMAND_ID:
			write = (struct zcl_ias_write_resp *)msg;
			log_debug("Received write attribute response status=%d"
			    " from source=0x%04X", write->status, source);
			appd_write_cie_complete_handler(source,
			    write->status);
			break;
		default:
			log_debug("Received invalid command 0x%02X"
			    " from source=0x%04X", cmd_id, source);
			break;
		}
	} else if (frame_type == ZCL_CLUSTER_SPECIFIC_COMMAND) {
		log_debug("Received cluster specific command"
		    " id=0x%02X  from source=0x%04X", cmd_id, source);
		switch (cmd_id) {
		case ZCL_ZONE_STATUS_CHANGE_NOTIFICATION_COMMAND_ID:
			log_debug("Received status change from"
			    " source=0x%04X", source);
			appd_ias_zone_status_change_handler(source, msg);
			break;
		case ZCL_ZONE_ENROLL_REQUEST_COMMAND_ID:
			log_debug("Received enroll request from"
			    " source=0x%04X", source);
			enroll = (struct zcl_ias_enroll_req *)msg;
			appd_ias_enroll_req_handler(source,
			    enroll->zone_type, enroll->manufacter_code);
			break;
		default:
			log_debug("Received invalid command 0x%02X"
			    " from source=0x%04X", cmd_id, source);
			break;
		}
	} else {
		log_debug("Received invalid frame type 0x%02X"
		    " from source=0x%04X", frame_type, source);
	}

	return true;
}

/*
 * Handle HA profile message
 */
static bool zbc_ha_profile_msg_handle(uint16_t source, uint16_t clusterId,
				uint8_t *message, uint16_t msgLen)
{
	struct zcl_msg *zcl;
	bool ret = false;

	appd_update_as_online_status(source);

	ASSERT(message != NULL);
	zcl = (struct zcl_msg *)message;

	switch (clusterId) {
	case ZCL_BASIC_CLUSTER_ID:
		zbc_basic_cluster_msg_handle(source, zcl->cmd_id, zcl->msg);
		break;
	case ZCL_POWER_CONFIG_CLUSTER_ID:
		zbc_power_config_cluster_msg_handle(source,
		    zcl->cmd_id, zcl->msg, msgLen - 3);
		break;
	case ZCL_ON_OFF_CLUSTER_ID:
		zbc_on_off_cluster_msg_handle(source,
		    zcl->ctrl.frame_type, zcl->cmd_id, zcl->msg);
		break;
	case ZCL_LEVEL_CONTROL_CLUSTER_ID:
		zbc_level_control_cluster_msg_handle(source,
		    zcl->ctrl.frame_type, zcl->cmd_id, zcl->msg);
		break;
	case ZCL_THERMOSTAT_CLUSTER_ID:
		zbc_thermostat_cluster_msg_handle(source,
		    zcl->ctrl.frame_type, zcl->cmd_id, zcl->msg, msgLen - 3);
		break;
	case ZCL_FAN_CONTROL_CLUSTER_ID:
		zbc_fan_control_cluster_msg_handle(source,
		    zcl->ctrl.frame_type, zcl->cmd_id, zcl->msg, msgLen - 3);
		break;
	case ZCL_IAS_ZONE_CLUSTER_ID:
		ret = zbc_ias_zone_cluster_msg_handle(source,
		    zcl->ctrl.frame_type, zcl->cmd_id, zcl->msg);
		break;
	case ZCL_SIMPLE_METERING_CLUSTER_ID:
		log_debug("Received simple metering report"
		    " from source=0x%04X", source);
		appd_ieee_query_handler(source);
		break;
	default:
		log_debug("Received clusterId=0x%04X message"
		    " from source=0x%04X", clusterId, source);
		debug_print_memory(message, msgLen);
		break;
	}

	return ret;
}

/** @brief Pre Message Received
 *
 * This callback is the first in the Application Framework's message processing
 * chain. The Application Framework calls it when a message has been received
 * over the air but has not yet been parsed by the ZCL command-handling code. If
 * you wish to parse some messages that are completely outside the ZCL
 * specification or are not handled by the Application Framework's command
 * handling code, you should intercept them for parsing in this callback.

 *   This callback returns a Boolean value indicating whether or not the message
 * has been handled. If the callback returns a value of true, then the
 * Application Framework assumes that the message has been handled and it does
 * nothing else with it. If the callback returns a value of false, then the
 * application framework continues to process the message as it would with any
 * incoming message.
	Note:	This callback receives a pointer to an
 * incoming message struct. This struct allows the application framework to
 * provide a unified interface between both Host devices, which receive their
 * message through the ezspIncomingMessageHandler, and SoC devices, which
 * receive their message through emberIncomingMessageHandler.
 *
 * @param incomingMessage   Ver.: always
 */
bool emberAfPreMessageReceivedCallback(EmberAfIncomingMessage *incomingMessage)
{
	EmberAfIncomingMessage *im;
	bool ret = false;

	ASSERT(incomingMessage != NULL);
	im = incomingMessage;

	if (!(im->apsFrame)) {
		log_debug("Incoming Message type=%d, msgLen=%d, "
		    "source=0x%04X, apsFrame is NULL",
		    im->type, im->msgLen, im->source);
		return false;
	}

	log_debug("Incoming Message profileId=0x%04X, clusterId=0x%04X, "
	    "source=0x%04X, msgLen=%d",
	    im->apsFrame->profileId, im->apsFrame->clusterId,
	    im->source, im->msgLen);

	if (im->apsFrame->profileId == EMBER_ZDO_PROFILE_ID) {
		ret = zbc_zdo_profile_msg_handle(im->source,
		    im->apsFrame->clusterId, im->message, im->msgLen);
	} else if (im->apsFrame->profileId == HA_PROFILE_ID) {
		ret = zbc_ha_profile_msg_handle(im->source,
		    im->apsFrame->clusterId, im->message, im->msgLen);
	}

	return ret;
}

/** @brief Stack Status
 *
 * This function is called by the application framework from the stack status
 * handler.  This callbacks provides applications an opportunity to be notified
 * of changes to the stack status and take appropriate action.  The return code
 * from this callback is ignored by the framework.  The framework will always
 * process the stack status after the callback returns.
 *
 * @param status   Ver.: always
 */
bool emberAfStackStatusCallback(EmberStatus status)
{
	switch (status) {
	case EMBER_NETWORK_UP:
		log_debug("Coordinator network up");
		appd_update_network_status(true);
		appd_start_all_power_query();
		break;
	case EMBER_NETWORK_DOWN:
		log_debug("Coordinator network down");
		appd_update_network_status(false);
		break;
	default:
		log_debug("stackStatus 0x%x", status);
		break;
	}
	return false;
}

/** @brief Trust Center Join
 *
 * This callback is called from within the application framework's
 * implementation of emberTrustCenterJoinHandler or ezspTrustCenterJoinHandler.
 * This callback provides the same arguments passed to the
 * TrustCenterJoinHandler. For more information about the TrustCenterJoinHandler
 * please see documentation included in stack/include/trust-center.h.
 *
 * @param newNodeId   Ver.: always
 * @param newNodeEui64   Ver.: always
 * @param parentOfNewNode   Ver.: always
 * @param status   Ver.: always
 * @param decision   Ver.: always
 */
void emberAfTrustCenterJoinCallback(EmberNodeId newNodeId,
				EmberEUI64 newNodeEui64,
				EmberNodeId parentOfNewNode,
				EmberDeviceUpdate status,
				EmberJoinDecision decision)
{
	debug_print_join_info(newNodeId, newNodeEui64, parentOfNewNode,
	    status, decision);

	/*
	 * The leave message is NWK layer message,
	 * cannot get in emberAfPreMessageReceivedCallback,
	 * so handle it in here.
	 */
	if (status == EMBER_DEVICE_LEFT) {
		log_debug("Node Net Addr=0x%04X left network", newNodeId);
		appd_node_left((const uint8_t *)newNodeEui64);
	}
}

/** @brief Message Sent
 *
 * This function is called by the application framework from the message sent
 * handler, when it is informed by the stack regarding the message sent status.
 * All of the values passed to the emberMessageSentHandler are passed on to
 * this callback. This provides an opportunity for the application to verify
 * that its message has been sent successfully and take the appropriate action.
 * This callback should return a bool value of true or false. A value of true
 * indicates that the message sent notification has been handled and should not
 * be handled by the application framework.
 *
 * @param type   Ver.: always
 * @param indexOrDestination   Ver.: always
 * @param apsFrame   Ver.: always
 * @param msgLen   Ver.: always
 * @param message   Ver.: always
 * @param status   Ver.: always
 */
bool emberAfMessageSentCallback(EmberOutgoingMessageType type,
				uint16_t indexOrDestination,
				EmberApsFrame *apsFrame,
				uint16_t msgLen,
				uint8_t *message,
				EmberStatus status)
{
	struct zcl_msg *zcl;

	if (type != EMBER_OUTGOING_DIRECT) {
		log_debug("Message type not EMBER_OUTGOING_DIRECT");
		return false;
	}

	if ((apsFrame->profileId != HA_PROFILE_ID)
	    && (apsFrame->profileId != SE_PROFILE_ID)) {
		log_debug("0x%04X not HA/SE profile id, no need more handle",
		    apsFrame->profileId);
		return false;
	}

	ASSERT(message != NULL);
	zcl = (struct zcl_msg *)message;

	log_debug("node_id=0x%04X, cluster_id=0x%04X,"
	    " cmd_id=0x%02X, status=0x%02X",
	    indexOrDestination, apsFrame->clusterId,
	    zcl->cmd_id, status);

	switch (apsFrame->clusterId) {
	case ZCL_ON_OFF_CLUSTER_ID:
		if (zcl->cmd_id <= ZCL_ON_COMMAND_ID) {
			appd_prop_complete_handler(indexOrDestination,
			    ZB_ON_OFF_PROP_NAME, status);
		}
		break;
	case ZCL_LEVEL_CONTROL_CLUSTER_ID:
		if (zcl->cmd_id <= ZCL_STOP_WITH_ON_OFF_COMMAND_ID) {
			appd_prop_complete_handler(indexOrDestination,
			    ZB_LEVEL_CTRL_PROP_NAME, status);
		}
		break;
	case ZCL_THERMOSTAT_CLUSTER_ID:
		if (zcl->cmd_id == ZCL_WRITE_ATTRIBUTES_COMMAND_ID) {
			struct zcl_write_attr *wrt;
			wrt = (struct zcl_write_attr *)(zcl->msg);
			char *prop_name = zbc_get_prop_name_by_id(
			    ZCL_THERMOSTAT_CLUSTER_ID, wrt->attr_id);
			if (prop_name != NULL) {
				appd_prop_complete_handler(
				    indexOrDestination, prop_name, status);
			}
		}
		break;
	case ZCL_FAN_CONTROL_CLUSTER_ID:
		if (zcl->cmd_id == ZCL_WRITE_ATTRIBUTES_COMMAND_ID) {
			appd_prop_complete_handler(indexOrDestination,
			    ZB_FAN_MODE, status);
		}
		break;
	case ZCL_IAS_ZONE_CLUSTER_ID:
		if (zcl->cmd_id == ZCL_WRITE_ATTRIBUTES_COMMAND_ID) {
			/* if write cid request sent failed, do nothing,
			after the timer timeout,
			the request will resend again */
		}
		break;
	default:
		return false;
	}

	return false;
}

/** @brief Ncp Init
 *
 * This function is called when the network coprocessor is being initialized,
 * either at startup or upon reset.  It provides applications on opportunity to
 * perform additional configuration of the NCP.  The function is always called
 * twice when the NCP is initialized.  In the first invocation, memoryAllocation
 * will be true and the application should only issue EZSP commands that affect
 * memory allocation on the NCP.  For example, tables on the NCP can be resized
 * in the first call.  In the second invocation, memoryAllocation will be false
 * and the application should only issue EZSP commands that do not affect memory
 * allocation.  For example, tables on the NCP can be populated in the second
 * call.  This callback is not called on SoCs.
 *
 * @param memoryAllocation   Ver.: always
 */
void emberAfNcpInitCallback(bool memoryAllocation)
{
	EzspStatus setStatus;
	if (!memoryAllocation) {
		setStatus = ezspSetConfigurationValue(
		    EZSP_CONFIG_APPLICATION_ZDO_FLAGS,
		    EMBER_APP_RECEIVES_SUPPORTED_ZDO_REQUESTS);
		if (setStatus != EZSP_SUCCESS) {
			log_err("ezspSetConfigurationValue ZDO ret %d",
			    setStatus);
		} else {
			log_debug("ezspSetConfigurationValue ZDO success");
		}
	}
}

/** @brief Simple Metering Cluster Get Profile Response
 *
 *
 *
 * @param endTime   Ver.: always
 * @param status   Ver.: always
 * @param profileIntervalPeriod   Ver.: always
 * @param numberOfPeriodsDelivered   Ver.: always
 * @param intervals   Ver.: always
 */
bool emberAfSimpleMeteringClusterGetProfileResponseCallback(
				uint32_t endTime,
				uint8_t status,
				uint8_t profileIntervalPeriod,
				uint8_t numberOfPeriodsDelivered,
				uint8_t *intervals)
{
	return false;
}

/** @brief Simple Metering Cluster Request Mirror
 *
 *
 *
 */
bool emberAfSimpleMeteringClusterRequestMirrorCallback(void)
{
	return false;
}

/** @brief Simple Metering Cluster Remove Mirror
 *
 *
 *
 */
bool emberAfSimpleMeteringClusterRemoveMirrorCallback(void)
{
	return false;
}

/** @brief Simple Metering Cluster Request Fast Poll Mode Response
 *
 *
 *
 * @param appliedUpdatePeriod   Ver.: always
 * @param fastPollModeEndtime   Ver.: always
 */
bool emberAfSimpleMeteringClusterRequestFastPollModeResponseCallback(
				uint8_t appliedUpdatePeriod,
				uint32_t fastPollModeEndtime)
{
	return false;
}

/** @brief Simple Metering Cluster Supply Status Response
 *
 *
 *
 * @param providerId   Ver.: always
 * @param issuerEventId   Ver.: always
 * @param implementationDateTime   Ver.: always
 * @param supplyStatus   Ver.: always
 */
bool emberAfSimpleMeteringClusterSupplyStatusResponseCallback(
				uint32_t providerId,
				uint32_t issuerEventId,
				uint32_t implementationDateTime,
				uint8_t supplyStatus)
{
	return false;
}

void emAfPluginSimpleMeteringClientCliGetSampledData(void)
{
}

void emAfPluginSimpleMeteringClientCliLocalChangeSupply(void)
{
}

void emAfPluginSimpleMeteringClientCliSchSnapshot(void)
{
}

void emAfPluginSimpleMeteringClientCliStartSampling(void)
{
}

/** @brief Time Cluster Server Tick
 *
 * Server Tick
 *
 * @param endpoint Endpoint that is being served  Ver.: always
 */
void emberAfTimeClusterServerTickCallback(uint8_t endpoint)
{
}

/** @brief Time Cluster Server Init
 *
 * Server Init
 *
 * @param endpoint Endpoint that is being initialized  Ver.: always
 */
void emberAfTimeClusterServerInitCallback(uint8_t endpoint)
{
}

/** @brief Simple Metering Cluster Client Default Response
 *
 * This function is called when the client receives the default response from
 * the server.
 *
 * @param endpoint Destination endpoint  Ver.: always
 * @param commandId Command id  Ver.: always
 * @param status Status in default response  Ver.: always
 */
void emberAfSimpleMeteringClusterClientDefaultResponseCallback(
				uint8_t endpoint,
				uint8_t commandId,
				EmberAfStatus status)
{
}

void emAfTimeClusterServerSetCurrentTime(uint32_t utcTime)
{
	struct timeval tv;
	struct timezone tz;

	tv.tv_sec  = utcTime;
	tv.tv_usec = 0;

	tz.tz_minuteswest = 480;
	tz.tz_dsttime     = 0;

	settimeofday(&tv, &tz);
}

uint32_t emAfTimeClusterServerGetCurrentTime(void)
{
	return (uint32_t)time(NULL);
}

