/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */


#include <unistd.h>
#include <string.h>
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
#include "ember/enums.h"
#include "ember/cluster-id.h"
#include "zb_interface.h"
#include "appd_interface.h"
#include "appd_interface_node.h"


#define NODE_OEM_MODEL    "linuxevb"


/* Fixed subdevice names, General device info props */
#define ZB_SUBDEVICE		"dev"

/* Template with mandatory ZigBee device info for all nodes */
#define ZB_TEMPLATE_LIGHT			"light"
#define ZB_TEMPLATE_DIMM_LIGHT		"dimm"
#define ZB_TEMPLATE_SMART_PLUG		"plug"
#define ZB_TEMPLATE_CONTROLLER		"ctrller"
#define ZB_TEMPLATE_IAS_ZONE			"iaszone"
#define ZB_TEMPLATE_THERMOSTAT		"thermost"

#define ZB_TEMPLATE_LIGHT_VERSION		"1.0"
#define ZB_TEMPLATE_DIMM_LIGHT_VERSION	"1.0"
#define ZB_TEMPLATE_SMART_PLUG_VERSION	"1.0"
#define ZB_TEMPLATE_CONTROLLER_VERSION	"1.0"
#define ZB_TEMPLATE_IAS_ZONE_VERSION		"1.0"
#define ZB_TEMPLATE_THERMOSTAT_VERSION	"1.0"




#define ZB_PROFILE_ID_HA			0x0104

#define ZB_DEVICE_ID_LIGHT			0x0100
#define ZB_DEVICE_ID_DIMM_LIGHT		0x0101
#define ZB_DEVICE_ID_SMART_PLUG		0x0051
#define ZB_DEVICE_ID_YIFANG_CONTROLLER	0x0103
#define ZB_DEVICE_ID_IRIS_CONTROLLER		0x0006
#define ZB_DEVICE_ID_IAS_ZONE			0x0402
#define ZB_DEVICE_ID_THERMOSTAT		0x0301


#define MAINS_POLL_PERIOD         60000
#define BATTERY_POLL_INTERVAL    10
#define POLL_TIMEOUT_COUNT       3
#define READ_TIMEOUT_COUNT       10
#define WAIT_RESP_PERIOD         30000
#define ZONE_STATE_PERIOD        3000
#define LEAVE_WAIT_PERIOD        3000


#define ZB_NODE_EUI_LEN    8

#define ZB_ALIAS_LEN        32

#define ZB_INVALID_ZONE_ID    0xFF

enum zb_thermostat_bind_state {
	ZB_BIND_NONE = 0x00,
	ZB_BIND_THERMOSTAT = 0x01,
	ZB_BIND_FAN_CONTROL  = 0x02
};

enum zb_zone_state {
	ZB_IAS_ZONE_NOT_WROTE_CIE = 0x00,
	ZB_IAS_ZONE_WROTE_CIE_DENIED = 0x01,
	ZB_IAS_ZONE_NOT_ENROLLED  = 0x02,
	ZB_IAS_ZONE_ENROLLED      = 0x03,
};

/*
 * ZCL ias zone status field struct
 */
struct zone_status_field {
	uint8_t alarm1_opened:1;
	uint8_t alarm2_opened:1;
	uint8_t tamper:1;
	uint8_t battery:1;
	uint8_t supervision_reports:1;
	uint8_t restore_reports:1;
	uint8_t trouble:1;
	uint8_t AC_mains:1;
	uint8_t test:1;
	uint8_t battery_defect:1;
	uint8_t reserved:6;
};

/*
 * ZCL ias zone status change notification struct
 */
struct zone_status_change {
	struct zone_status_field status;
	uint8_t ext_status;
	uint8_t zone_id;
	uint16_t delay;
};

/*
 * Save ZigBee node info
 */
struct zb_node_info {
	struct node *node;		/* Pointer to node */
	uint8_t node_eui[ZB_NODE_EUI_LEN];
	uint16_t node_id;
	uint16_t profile_id;
	uint16_t device_id;
	uint8_t sent_online:1;
	uint8_t node_ready:1;
	uint8_t leaving:1;
	uint8_t thermostat_bind:2;
	uint8_t reserved1:3;
	char model_id[ZB_MODEL_ID_LEN];
	char alias[ZB_ALIAS_LEN];
	uint16_t zone_type;
	uint16_t manufacter_code;
	uint8_t zone_id;
	uint8_t zone_state;
	uint32_t poll_count;
	struct timer poll_timer;
	void (*query_complete)(struct node *, enum node_network_result);
	void (*config_complete)(struct node *, enum node_network_result);
	void (*prop_complete)(struct node *, struct node_prop *,
		enum node_network_result);
	void (*leave_complete)(struct node *, enum node_network_result);
};

/*
 * ZigBee node prop info
 */
struct nd_prop_info{
	uint16_t device_id;
	char *subdevive_key;
	char *template_key;
	char *template_version;
	const struct node_prop_def *prop_def;
	size_t def_size;
};

/*
 * Delay add node queue record structure
 */
struct delay_record {
	STAILQ_ENTRY(delay_record) link;
	uint8_t node_eui[ZB_NODE_EUI_LEN];
	uint16_t node_id;
	struct timer delay;
};

static STAILQ_HEAD(, delay_record) delayq;

static bool zb_zone_id_list[ZB_INVALID_ZONE_ID];


/* Light prop define */
static const struct node_prop_def const zb_template_light[] = {
	{ ZB_ON_OFF_PROP_NAME,		PROP_BOOLEAN,	PROP_TO_DEVICE },
	{ ZB_SHORT_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_LONG_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_SRC_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_LEV_PROP_NAME,	PROP_INTEGER,	PROP_FROM_DEVICE },
	{ ZB_MODEL_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_ALIAS_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
};

/* Dimmable light prop define */
static const struct node_prop_def const zb_template_dimm_light[] = {
	{ ZB_ON_OFF_PROP_NAME,		PROP_BOOLEAN,	PROP_TO_DEVICE },
	{ ZB_LEVEL_CTRL_PROP_NAME,	PROP_INTEGER,	PROP_TO_DEVICE },
	{ ZB_SHORT_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_LONG_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_SRC_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_LEV_PROP_NAME,	PROP_INTEGER,	PROP_FROM_DEVICE },
	{ ZB_MODEL_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_ALIAS_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
};

/* Smart plug prop define */
static const struct node_prop_def const zb_template_smart_plug[] = {
	{ ZB_ON_OFF_PROP_NAME,		PROP_BOOLEAN,	PROP_TO_DEVICE },
	{ ZB_SHORT_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_LONG_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_SRC_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_LEV_PROP_NAME,	PROP_INTEGER,	PROP_FROM_DEVICE },
	{ ZB_MODEL_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_ALIAS_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
};

/* Controller prop define */
static const struct node_prop_def const zb_template_controller[] = {
	{ ZB_SHORT_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_LONG_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_SRC_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_LEV_PROP_NAME,	PROP_INTEGER,	PROP_FROM_DEVICE },
	{ ZB_MODEL_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_ALIAS_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
};

/* Door sensor prop define */
static const struct node_prop_def const zb_template_ias_zone[] = {
	{ ZB_STATUS_PROP_NAME,		PROP_BOOLEAN,	PROP_TO_DEVICE },
	{ ZB_SHORT_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_LONG_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_SRC_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_LEV_PROP_NAME,	PROP_INTEGER,	PROP_FROM_DEVICE },
	{ ZB_MODEL_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_ALIAS_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
};

/* Thermostat prop define */
static const struct node_prop_def const zb_template_thermostat[] = {
	{ ZB_SYSTEM_MODE,		PROP_INTEGER,	PROP_TO_DEVICE },
	{ ZB_COOLING_SETPOINT,		PROP_INTEGER,	PROP_TO_DEVICE },
	{ ZB_HEATING_SETPOINT,		PROP_INTEGER,	PROP_TO_DEVICE },
	{ ZB_LOCAL_TEMPERATURE,		PROP_DECIMAL,	PROP_FROM_DEVICE },
	{ ZB_FAN_MODE,			PROP_INTEGER,	PROP_TO_DEVICE },
	{ ZB_SHORT_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_LONG_ADDR_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_SRC_PROP_NAME,	PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_POWER_LEV_PROP_NAME,	PROP_INTEGER,	PROP_FROM_DEVICE },
	{ ZB_MODEL_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
	{ ZB_ALIAS_PROP_NAME,		PROP_STRING,	PROP_FROM_DEVICE },
};

/* Prop define table */
static struct nd_prop_info prop_info_array[] = {
	{
		.device_id = ZB_DEVICE_ID_LIGHT,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_LIGHT,
		.template_version = ZB_TEMPLATE_LIGHT_VERSION,
		.prop_def = zb_template_light,
		.def_size = ARRAY_LEN(zb_template_light)
	},
	{
		.device_id = ZB_DEVICE_ID_DIMM_LIGHT,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_DIMM_LIGHT,
		.template_version = ZB_TEMPLATE_DIMM_LIGHT_VERSION,
		.prop_def = zb_template_dimm_light,
		.def_size = ARRAY_LEN(zb_template_dimm_light)
	},
	{
		.device_id = ZB_DEVICE_ID_SMART_PLUG,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_SMART_PLUG,
		.template_version = ZB_TEMPLATE_SMART_PLUG_VERSION,
		.prop_def = zb_template_smart_plug,
		.def_size = ARRAY_LEN(zb_template_smart_plug)
	},
	{
		.device_id = ZB_DEVICE_ID_YIFANG_CONTROLLER,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_CONTROLLER,
		.template_version = ZB_TEMPLATE_CONTROLLER_VERSION,
		.prop_def = zb_template_controller,
		.def_size = ARRAY_LEN(zb_template_controller)
	},
	{
		.device_id = ZB_DEVICE_ID_IRIS_CONTROLLER,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_CONTROLLER,
		.template_version = ZB_TEMPLATE_CONTROLLER_VERSION,
		.prop_def = zb_template_controller,
		.def_size = ARRAY_LEN(zb_template_controller)
	},
	{
		.device_id = ZB_DEVICE_ID_IAS_ZONE,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_IAS_ZONE,
		.template_version = ZB_TEMPLATE_IAS_ZONE_VERSION,
		.prop_def = zb_template_ias_zone,
		.def_size = ARRAY_LEN(zb_template_ias_zone)
	},
	{
		.device_id = ZB_DEVICE_ID_THERMOSTAT,
		.subdevive_key = ZB_SUBDEVICE,
		.template_key = ZB_TEMPLATE_THERMOSTAT,
		.template_version = ZB_TEMPLATE_THERMOSTAT_VERSION,
		.prop_def = zb_template_thermostat,
		.def_size = ARRAY_LEN(zb_template_thermostat)
	},
};

/*
 * Add node to gateway node list.
 */
static void appd_node_add(const uint8_t *node_eui, uint16_t node_id);

/*
 * Init appd interface.
 */
void appd_interface_init(void)
{
	STAILQ_INIT(&delayq);
}

/*
 *  Appd interface exit.
 */
void appd_interface_exit(void)
{
	struct delay_record *record;
	STAILQ_FOREACH(record, &delayq, link) {
		timer_cancel(app_get_timers(), &(record->delay));
		free(record);
	}
}

/*
 * Add a record to delay queue.
 */
static void appd_add_delay_record(const uint8_t *node_eui, uint16_t node_id)
{
	struct delay_record *record;

	ASSERT(node_eui != NULL);

	STAILQ_FOREACH(record, &delayq, link) {
		if (!memcmp(record->node_eui, node_eui, ZB_NODE_EUI_LEN)) {
			record->node_id = node_id;
			log_debug("node %02X%02X%02X%02X%02X%02X%02X%02X"
			    " already in delay queue",
			    node_eui[7], node_eui[6], node_eui[5],
			    node_eui[4], node_eui[3], node_eui[2],
			    node_eui[1], node_eui[0]);
			return;
		}
	}

	record = calloc(1, sizeof(struct delay_record));
	if (!record) {
		log_err("malloc delay_record memory failed");
		return;
	}

	memcpy(record->node_eui, node_eui, ZB_NODE_EUI_LEN);
	record->node_id = node_id;

	STAILQ_INSERT_TAIL(&delayq, record, link);

	log_debug("added a delay record,"
	    " node addr %02X%02X%02X%02X%02X%02X%02X%02X, node_id=0x%04X",
	    node_eui[7], node_eui[6], node_eui[5],
	    node_eui[4], node_eui[3], node_eui[2],
	    node_eui[1], node_eui[0], node_id);

	return;
}

/*
 * Handle node add delay record
 */
static void appd_handle_delay_record(struct delay_record *record)
{
	ASSERT(record != NULL);
	log_debug("record node addr %02X%02X%02X%02X%02X%02X%02X%02X",
	    record->node_eui[7], record->node_eui[6],
	    record->node_eui[5], record->node_eui[4],
	    record->node_eui[3], record->node_eui[2],
	    record->node_eui[1], record->node_eui[0]);
	STAILQ_REMOVE(&delayq, record, delay_record, link);
	appd_node_add(record->node_eui, record->node_id);
	free(record);
}

/*
 * Handle delay timer timeout
 */
static void appd_delay_add_timeout(struct timer *timer)
{
	struct delay_record *record;
	record = CONTAINER_OF(struct delay_record, delay, timer);
	log_debug("add delay handle");
	/* Start node join network delay handle */
	appd_handle_delay_record(record);
}

/*
 * Set add delay timer
 */
static void appd_set_delay_timer(const uint8_t *node_eui)
{
	struct delay_record *record;

	log_debug("node addr %02X%02X%02X%02X%02X%02X%02X%02X",
	    node_eui[7], node_eui[6], node_eui[5],
	    node_eui[4], node_eui[3], node_eui[2],
	    node_eui[1], node_eui[0]);

	if (STAILQ_EMPTY(&delayq)) {
		log_debug("delay record queue is empty");
		return;
	}

	STAILQ_FOREACH(record, &delayq, link) {
		log_debug("record node addr %02X%02X%02X%02X%02X%02X%02X%02X",
		    record->node_eui[7], record->node_eui[6],
		    record->node_eui[5], record->node_eui[4],
		    record->node_eui[3], record->node_eui[2],
		    record->node_eui[1], record->node_eui[0]);
		if (!memcmp(record->node_eui, node_eui, ZB_NODE_EUI_LEN)) {
			timer_reset(app_get_timers(), &(record->delay),
			    appd_delay_add_timeout, 0);
			return;
		}
	}
}

/*
 * Get node prop info
 */
static struct nd_prop_info *appd_get_node_prop_info(uint16_t device_id)
{
	int i;
	for (i = 0; i < ARRAY_LEN(prop_info_array); i++) {
		if (device_id == prop_info_array[i].device_id) {
			return &prop_info_array[i];
		}
	}
	return NULL;
}

/*
 * Get node prop
 */
static struct node_prop *appd_get_node_prop(struct node *zb_node, char *name)
{
	struct zb_node_info *info;
	struct nd_prop_info *prop_info;
	struct node_prop *nd_prop;

	ASSERT(zb_node != NULL);
	ASSERT(name != NULL);

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	prop_info = appd_get_node_prop_info(info->device_id);
	if (!prop_info) {
		log_err("node %s cannot find prop info", zb_node->addr);
		return NULL;
	}

	nd_prop = node_prop_lookup(zb_node, prop_info->subdevive_key,
	    prop_info->template_key, name);

	return nd_prop;
}

/*
 * Send node prop to cloud
 */
static void appd_send_node_prop(struct node *zb_node, char *name, void *value)
{
	struct node_prop *nd_prop;
	int ret;

	ASSERT(zb_node != NULL);
	ASSERT(name != NULL);
	ASSERT(value != NULL);

	nd_prop = appd_get_node_prop(zb_node, name);
	if (!nd_prop) {
		log_err("node %s does not have property %s",
		    zb_node->addr, name);
		return;
	}

	switch (nd_prop->type) {
	case PROP_INTEGER:
		ret = node_prop_integer_send(zb_node, nd_prop, *(int *)value);
		break;
	case PROP_STRING:
		ret = node_prop_string_send(zb_node, nd_prop, (char *)value);
		break;
	case PROP_BOOLEAN:
		ret = node_prop_boolean_send(zb_node, nd_prop, *(bool *)value);
		break;
	case PROP_DECIMAL:
		ret = node_prop_decimal_send(zb_node, nd_prop,
		    *(double *)value);
		break;
	default:
		log_err("node %s does not support type 0x%X property %s",
		    zb_node->addr, nd_prop->type, nd_prop->name);
		ret = -1;
		break;
	}

	if (ret < 0) {
		log_err("node %s sent property %s fail", zb_node->addr, name);
		return;
	}

	log_debug("node %s updated property %s success", zb_node->addr, name);
	return;
}

/*
 * Send prop when the node is online
 */
static void appd_online_send_prop(struct node *zb_node,
			struct zb_node_info *info)
{
	char addr[ZB_NODE_ADDR_LEN + 1];
	char *power[] = { "MAINS", "BATTERY", "UNKOWN" };

	ASSERT(zb_node != NULL);
	ASSERT(info != NULL);

	node_prop_batch_begin(zb_node);

	memset(addr, 0, sizeof(addr));
	snprintf(addr, sizeof(addr), "0x%04X", info->node_id);
	appd_send_node_prop(zb_node, ZB_SHORT_ADDR_PROP_NAME, addr);

	appd_send_node_prop(zb_node, ZB_LONG_ADDR_PROP_NAME, zb_node->addr);
	appd_send_node_prop(zb_node, ZB_MODEL_PROP_NAME, info->model_id);
	appd_send_node_prop(zb_node, ZB_ALIAS_PROP_NAME, info->alias);

	if (zb_node->power <= GP_BATTERY) {
		appd_send_node_prop(zb_node, ZB_POWER_SRC_PROP_NAME,
		    power[zb_node->power]);
	} else {
		appd_send_node_prop(zb_node, ZB_POWER_SRC_PROP_NAME,
		    power[2]);
	}

	node_prop_batch_end(zb_node);
}

static void debug_print_node_info(const struct zb_node_info *info)
{
	char buf[96];

	ASSERT(info != NULL);

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "node addr=%s, version=%s, "
	    "oem_model=%s, interface=%d, "
	    "power=%d, online=%d",
	    info->node->addr, info->node->version,
	    info->node->oem_model, info->node->interface,
	    info->node->power, info->node->online);
	log_debug("%s", buf);

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "node_eui=%02X%02X%02X%02X%02X%02X%02X%02X,"
	    " node_id=0x%04X, profile_id=0x%04X, device_id=0x%04X",
	    info->node_eui[7], info->node_eui[6], info->node_eui[5],
	    info->node_eui[4], info->node_eui[3], info->node_eui[2],
	    info->node_eui[1], info->node_eui[0],
	    info->node_id, info->profile_id, info->device_id);
	log_debug("%s", buf);

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "sent_online=%d, node_ready=%d,"
	    " leaving=%d, thermostat_bind=%d",
	    info->sent_online, info->node_ready,
	    info->leaving, info->thermostat_bind);
	log_debug("%s", buf);

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "model_id=%s, alias=%s",
	    info->model_id, info->alias);
	log_debug("%s", buf);

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "zone_type=0x%02X, manufacter_code=0x%02X,"
	    " zone_id=%d", info->zone_type, info->manufacter_code,
	    info->zone_id);
	log_debug("%s", buf);
}

/*
 * Get node info by the node addr.
 */
static struct zb_node_info *appd_get_node_info(char *addr)
{
	struct node *zb_node;
	struct zb_node_info *info;

	ASSERT(addr != NULL);

	zb_node = node_lookup(addr);
	if (!zb_node) {
		log_info("no node with addr: %s", addr);
		return NULL;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	return info;
}

/*
 * Get node_id by the node.
 */
uint16_t appd_get_node_id(struct node *zb_node)
{
	struct zb_node_info *info;
	ASSERT(zb_node != NULL);
	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);
	return info->node_id;
}

/*
 * If found node_id, set the node_eui info to arg.
 */
static int appd_node_id_cmp(struct node *zb_node, void *arg)
{
	struct zb_node_info *iftmp, *ifarg;

	ASSERT(zb_node != NULL);
	ASSERT(arg != NULL);

	if (zb_node->interface != GI_ZIGBEE) {
		return 0;
	}

	ifarg = (struct zb_node_info *)arg;
	iftmp = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(iftmp != NULL);
	if (iftmp->node_id == ifarg->node_id) {
		ifarg->node = iftmp->node;
		return 1;
	} else {
		return 0;
	}
}

/*
 * Get node_eui by the node_id.
 */
static struct node *appd_get_node(uint16_t node_id)
{
	struct zb_node_info ifarg;
	memset(&ifarg, 0, sizeof(ifarg));
	ifarg.node_id = node_id;
	ifarg.node = NULL;
	node_foreach(appd_node_id_cmp, (void *)&ifarg);
	return ifarg.node;
}

/*
 * Cleanup function for zb_node_info.
 */
static void appd_node_state_cleanup(void *arg)
{
	struct zb_node_info *info = (struct zb_node_info *)arg;
	ASSERT(info != NULL);

	log_debug("Clean node %s info, leaving=%d",
	    info->node->addr, info->leaving);
	timer_cancel(app_get_timers(), &(info->poll_timer));
	if (info->device_id == ZB_DEVICE_ID_IAS_ZONE) {
		if (info->zone_id < ZB_INVALID_ZONE_ID) {
			zb_zone_id_list[info->zone_id] = false;
		}
	}
	if (info->leaving) {
		/* Set delay timer to hanlde node join network */
		appd_set_delay_timer(info->node_eui);
		info->leaving = false;
	}
	free(info);
}

/*
 * Update node as online status
 */
void appd_update_as_online_status(uint16_t node_id)
{
	struct node *zb_node;
	struct zb_node_info *info;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (info->node_ready == 0) {
		log_debug("node %s not ready", zb_node->addr);
		return;
	}

	if (!(zb_node->online)) {
		info->sent_online = 1;
		node_conn_status_changed(zb_node, true);
		log_debug("Updated node %s conn status as online",
		    zb_node->addr);
		appd_online_send_prop(zb_node, info);
	}

	return;
}

/*
 * Update node as offline status
 */
static void appd_update_as_offline_status(struct zb_node_info *info)
{
	ASSERT(info != NULL);

	if (!(info->sent_online) || (info->node->online)) {
		info->sent_online = 1;
		node_conn_status_changed(info->node, false);
		log_debug("Updated node %s conn status as offline",
		    info->node->addr);
	}

	return;
}

/*
 * Add node to gateway node list.
 */
static void appd_node_add(const uint8_t *node_eui, uint16_t node_id)
{
	char addr[ZB_NODE_ADDR_LEN + 1];
	struct node *zb_node;
	struct zb_node_info *tmp, *info;

	memset(addr, 0, sizeof(addr));
	snprintf(addr, sizeof(addr), "%02X%02X%02X%02X%02X%02X%02X%02X",
	    node_eui[7], node_eui[6], node_eui[5], node_eui[4],
	    node_eui[3], node_eui[2], node_eui[1], node_eui[0]);
	log_debug("Add node %s into network", addr);

	tmp = (struct zb_node_info *)calloc(1, sizeof(struct zb_node_info));
	if (!tmp) {
		log_err("malloc node_info memory failed");
		return;
	}

	zb_node = node_joined(addr, NODE_OEM_MODEL, GI_ZIGBEE, GP_MAINS, "-");
	if (!zb_node) {
		log_err("node %s added fail", addr);
		free(tmp);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	if (info != NULL) {
		log_debug("node info already exists for node %s", addr);
		free(tmp);
		timer_cancel(app_get_timers(), &(info->poll_timer));
	} else {
		info = tmp;
		node_state_set(zb_node, STATE_SLOT_NET, info,
		    appd_node_state_cleanup);
		info->node = zb_node;
		memcpy(info->node_eui, node_eui, ZB_NODE_EUI_LEN);
		info->zone_id = ZB_INVALID_ZONE_ID;
		info->zone_state = ZB_IAS_ZONE_NOT_WROTE_CIE;
	}

	info->node_id = node_id;

	log_debug("node %s added success", addr);
	debug_print_node_info(info);
}

/*
 * Check if the node is leaving
 */
static bool appd_node_leaving_check(const uint8_t *node_eui, uint16_t node_id)
{
	char addr[ZB_NODE_ADDR_LEN + 1];
	struct node *zb_node;
	struct zb_node_info *info;

	ASSERT(node_eui != NULL);

	memset(addr, 0, sizeof(addr));
	snprintf(addr, sizeof(addr), "%02X%02X%02X%02X%02X%02X%02X%02X",
	    node_eui[7], node_eui[6], node_eui[5], node_eui[4],
	    node_eui[3], node_eui[2], node_eui[1], node_eui[0]);
	log_debug("node %s joined network", addr);

	zb_node = node_lookup(addr);
	if (!zb_node) {
		log_info("no node with addr: %s", addr);
		return false;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	return info->leaving;
}

/*
 * Handle a node join network event from the network stack.
 */
void appd_node_join_network(const uint8_t *node_eui, uint16_t node_id)
{
	if (appd_node_leaving_check(node_eui, node_id)) {
		log_debug("node is leaving");
		appd_add_delay_record(node_eui, node_id);
	} else {
		appd_node_add(node_eui, node_id);
	}
}

/*
 * Handle a node left event from the network stack.
 */
void appd_node_left(const uint8_t *node_eui)
{
	char addr[ZB_NODE_ADDR_LEN + 1];
	struct node *zb_node;
	struct zb_node_info *info;

	memset(addr, 0, sizeof(addr));
	snprintf(addr, sizeof(addr), "%02X%02X%02X%02X%02X%02X%02X%02X",
	    node_eui[7], node_eui[6], node_eui[5], node_eui[4],
	    node_eui[3], node_eui[2], node_eui[1], node_eui[0]);
	log_debug("Node %s left network", addr);

	zb_node = node_lookup(addr);
	if (!zb_node) {
		log_err("Cannot find node %s", addr);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	appd_update_as_offline_status(info);

	/* Need to continue node next step handle when the leave_complete not
	NULL */
	if (info->leave_complete) {
		info->leave_complete(info->node, NETWORK_SUCCESS);
		info->leave_complete = NULL;
	} else if (info->query_complete) {
			info->query_complete(info->node, NETWORK_UNKNOWN);
			info->query_complete = NULL;
	} else {
		node_left(zb_node);
		info->leaving = true;
	}
}

/*
 * Associate a node template definition table with a node.  Used by the
 * simulator to setup a node supporting the desired characteristics.
 */
static void appd_template_add(struct node *zb_node,
	const char *subdevice, const char *template, const char *version,
	const struct node_prop_def *table, size_t table_size)
{
	for (; table_size; --table_size, ++table) {
		node_prop_add(zb_node, subdevice, template, table, version);
	}
}

/*
 * Set node template
 */
static int appd_set_node_template(struct node *zb_node, uint16_t device_id)
{
	struct nd_prop_info *prop_info;
	prop_info = appd_get_node_prop_info(device_id);
	if (prop_info) {
		appd_template_add(zb_node, prop_info->subdevive_key,
		    prop_info->template_key, prop_info->template_version,
		    prop_info->prop_def, prop_info->def_size);
		return 0;
	} else {
		log_err("Don't support device_id = 0x%04X", device_id);
		return -1;
	}
}

/*
 * Handle poll timer timeout
 */
static void appd_simple_query_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	log_debug("node %s query timeout", info->node->addr);
	zb_send_simple_request(info->node_id);
	timer_set(app_get_timers(), timer, WAIT_RESP_PERIOD);
}

/*
 * Start simple descriptor query
 */
static void appd_start_simple_query(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to get simple descriptor info */
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_simple_query_timeout, WAIT_RESP_PERIOD);
	zb_send_simple_request(info->node_id);
}

/*
 * Handle write CIE address timer timeout
 */
static void appd_write_cie_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	log_debug("node %s write CIE address timeout", info->node->addr);
	zb_send_write_cie_request(info->node_id);
	timer_set(app_get_timers(), timer, WAIT_RESP_PERIOD);
}

/*
 * Start write CIE address to node
 */
static void appd_start_write_cie(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to write CIE address */
	log_debug("node %s start to write CIE address", info->node->addr);
	info->zone_state = ZB_IAS_ZONE_NOT_WROTE_CIE;
	conf_save();	/* Save management state to config */
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_write_cie_timeout, WAIT_RESP_PERIOD);
	zb_send_write_cie_request(info->node_id);
}

/*
 * Handle read zone state timer timeout
 */
static void appd_read_zone_state_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	log_debug("node %s read zone state timeout", info->node->addr);
	zb_send_read_zone_state_request(info->node_id);
	timer_set(app_get_timers(), timer, ZONE_STATE_PERIOD);
	info->poll_count++;
	if (info->poll_count >= READ_TIMEOUT_COUNT) {
		log_info("node %s read zone state timeout %u times",
		    info->node->addr, info->poll_count);
		appd_update_as_offline_status(info);
	}
}

/*
 * Start read zone state to node
 */
static void appd_start_read_zone_state(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to wait read zone state */
	log_debug("node %s start to read zone state", info->node->addr);
	info->poll_count = 0;
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_read_zone_state_timeout, ZONE_STATE_PERIOD);
}

/*
 * Handle thermostat bind timer timeout
 */
static void appd_thermostat_bind_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	uint16_t cluster_id;

	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	log_debug("node %s thermostat bind %d timeout",
	    info->node->addr, info->thermostat_bind);
	if (info->thermostat_bind == ZB_BIND_NONE) {
		cluster_id = ZCL_THERMOSTAT_CLUSTER_ID;
	} else if (info->thermostat_bind == ZB_BIND_THERMOSTAT) {
		cluster_id = ZCL_FAN_CONTROL_CLUSTER_ID;
	} else {
		timer_cancel(app_get_timers(), &(info->poll_timer));
		return;
	}
	zb_thermostat_bind_request(info->node_id, info->node_eui,
	    cluster_id);
	timer_set(app_get_timers(), timer, WAIT_RESP_PERIOD);
}

/*
 * Start thermostat bind to node
 */
static void appd_start_thermostat_bind(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	log_debug("node %s start to thermostat bind", info->node->addr);
	info->thermostat_bind = ZB_BIND_NONE;
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_thermostat_bind_timeout, WAIT_RESP_PERIOD);
	zb_thermostat_bind_request(info->node_id, info->node_eui,
	    ZCL_THERMOSTAT_CLUSTER_ID);
}

/*
 * Handle poll timer timeout
 */
static void appd_power_query_timeout(struct timer *timer)
{
	struct zb_node_info *info;

	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	info->poll_count++;
	if (info->poll_count >= POLL_TIMEOUT_COUNT) {
		log_info("Poll node %s power timeout %u times",
		    info->node->addr, info->poll_count);
		appd_update_as_offline_status(info);
	}

	log_debug("Send power request to node %s, poll_count %u",
	    info->node->addr, info->poll_count);
	zb_send_power_request(info->node_id);
	timer_set(app_get_timers(), timer, WAIT_RESP_PERIOD);
}

/*
 * Start power descriptor poll
 */
static void appd_start_power_poll(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to get power descriptor info query */
	log_debug("node %s start to power descriptor info query",
	    info->node->addr);
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_power_query_timeout, WAIT_RESP_PERIOD);
	zb_send_power_request(info->node_id);
}

/*
 * Start power descriptor query
 */
static int appd_start_power_query(struct node *zb_node, void *arg)
{
	struct zb_node_info *info;

	ASSERT(zb_node != NULL);
	if (zb_node->interface != GI_ZIGBEE) {
		return 0;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (info->device_id == ZB_DEVICE_ID_THERMOSTAT) {
		log_debug("node %s is thermostat", zb_node->addr);
		appd_start_thermostat_bind(info);
		return 0;
	} else if ((info->device_id == ZB_DEVICE_ID_IAS_ZONE)
	    && (info->zone_state < ZB_IAS_ZONE_ENROLLED)) {
		log_debug("IAS node %s zone_state is 0x%02X",
		    zb_node->addr, info->zone_state);
		appd_start_write_cie(info);
		return 0;
	}

	appd_start_power_poll(info);
	return 0;
}

/*
 * Start all node power descriptor query for loaded node from config file
 * after ZigBee network up
 */
void appd_start_all_power_query(void)
{
	log_debug("Start all node poll power query");
	/* Start poll timer for loaded node from config file
	after ZigBee network up */
	node_foreach(appd_start_power_query, NULL);
}

/*
 * Handle power source timer timeout
 */
static void appd_power_source_query_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	log_debug("node %s get power source timeout", info->node->addr);
	zb_send_power_source_request(info->node_id);
	timer_set(app_get_timers(), timer, WAIT_RESP_PERIOD);
}

/*
 * Start power source query
 */
static void appd_start_power_source_query(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to get power source info */
	log_debug("node %s start power source query", info->node->addr);
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_power_source_query_timeout, WAIT_RESP_PERIOD);
	zb_send_power_source_request(info->node_id);
}

/*
 * Handle model identifier timer timeout
 */
static void appd_model_identifier_query_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	log_debug("node %s get model identifier timeout", info->node->addr);
	zb_send_model_identifier_request(info->node_id);
	timer_set(app_get_timers(), timer, WAIT_RESP_PERIOD);
}

/*
 * Start model identifier query
 */
static void appd_start_model_identifier_query(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to get model identifier info */
	log_debug("node %s start model identifier query", info->node->addr);
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_model_identifier_query_timeout, WAIT_RESP_PERIOD);
	zb_send_model_identifier_request(info->node_id);
}

/*
 * Handle poll timer timeout for leaving
 */
static void appd_leave_timeout(struct timer *timer)
{
	struct zb_node_info *info;
	info = CONTAINER_OF(struct zb_node_info, poll_timer, timer);
	info->poll_count++;
	if (info->poll_count >= POLL_TIMEOUT_COUNT) {
		log_info("node %s leave timeout %u times",
		    info->node->addr, info->poll_count);
		if (info->leave_complete) {
			info->leave_complete(info->node, NETWORK_SUCCESS);
			info->leave_complete = NULL;
		}
		return;
	}

	log_debug("Send leave request to node %s, poll_count %u",
	    info->node->addr, info->poll_count);
	zb_send_leave_request(info->node_id);
	timer_set(app_get_timers(), timer, LEAVE_WAIT_PERIOD);
}

/*
 * Start to leave
 */
static void appd_start_leave(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	/* Set poll timer to start leaving */
	info->poll_count = 0;
	timer_reset(app_get_timers(), &(info->poll_timer),
	    appd_leave_timeout, LEAVE_WAIT_PERIOD);
	zb_send_leave_request(info->node_id);
}

/*
 * Query step completed handler
 */
static void appd_query_complete_handler(struct zb_node_info *info)
{
	int ret;
	enum node_network_result result = NETWORK_SUCCESS;

	ASSERT(info != NULL);

	if (info->query_complete) {
		ret = appd_set_node_template(info->node, info->device_id);
		if (ret < 0) {
			result = NETWORK_UNSUPPORTED;
		}
		log_debug("node %s call query complete callback function",
		    info->node->addr);
		info->query_complete(info->node, result);
		info->query_complete = NULL;
	}

	log_debug("node %s query step completed", info->node->addr);
}

/*
 * Set query handle complete callback
 */
void appd_set_query_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result))
{
	struct zb_node_info *info;
	ASSERT(zb_node != NULL);
	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);
	info->query_complete = callback;
	appd_start_simple_query(info);
}

/*
 * Set config handle complete callback
 */
void appd_set_config_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result))
{
	struct zb_node_info *info;
	ASSERT(zb_node != NULL);
	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (callback) {
		callback(zb_node, NETWORK_SUCCESS);
	}

	/* Set node as ready state after config step finish */
	info->node_ready = 1;

	if (info->device_id == ZB_DEVICE_ID_THERMOSTAT) {
		appd_start_thermostat_bind(info);
	} else if (info->device_id == ZB_DEVICE_ID_IAS_ZONE) {
		appd_start_write_cie(info);
	} else {
		appd_start_power_poll(info);
	}
}

/*
 * Set prop set handle complete callback
 */
void appd_set_prop_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result))
{
	struct zb_node_info *info;
	ASSERT(zb_node != NULL);
	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);
	info->prop_complete = callback;
}

/*
 * Set leave handle complete callback
 */
void appd_set_leave_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result))
{
	struct zb_node_info *info;
	ASSERT(zb_node != NULL);
	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);
	info->leave_complete = callback;
	appd_start_leave(info);
}

/*
 * Handle simple descriptor reply
 */
void appd_simple_complete_handler(uint16_t node_id,
				uint16_t profile_id, uint16_t device_id)
{
	struct node *zb_node;
	struct zb_node_info *info;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	timer_cancel(app_get_timers(), &(info->poll_timer));

	if ((info->profile_id == profile_id)
	    && (info->device_id == device_id)) {
		log_info("Repeated reply msg, profile_id(0x%04X),"
		    " device_id(0x%04X) for node %s",
		    profile_id, device_id, zb_node->addr);
		return;
	}

	info->profile_id = profile_id;
	info->device_id = device_id;

	if (profile_id != ZB_PROFILE_ID_HA) {
		log_info("The profile ID(0x%04X) is not Home Automation"
		    " profile ID(0x%04X)", profile_id, ZB_PROFILE_ID_HA);
	}

	log_debug("node %s got simple descriptor successfully", zb_node->addr);

	appd_start_power_source_query(info);
}

/*
 * Handle power source info reply
 */
void appd_power_source_complete_handler(uint16_t node_id,
			uint8_t primary_power)
{
	struct node *zb_node;
	struct zb_node_info *info;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	timer_cancel(app_get_timers(), &(info->poll_timer));

	if (primary_power == EMBER_ZCL_POWER_SOURCE_BATTERY) {
		zb_node->power = GP_BATTERY;
	} else {
		zb_node->power = GP_MAINS;
	}

	log_debug("node %s got power source=0x%02X, set power=0x%02X",
	    zb_node->addr, primary_power, zb_node->power);
	debug_print_node_info(info);

	appd_start_model_identifier_query(info);
}

/*
 * Set node alias
 */
static void appd_set_node_alias(struct zb_node_info *info)
{
	struct model_id_2_name {
		char *model_id;
		char *alias;
	};
	struct dev_id_2_name {
		uint16_t device_id;
		char *alias;
	};

	struct model_id_2_name model_name[] = {
		{
			"4257050-RZHAC",
			"smart plug"
		},
		{
			"ZHA-DimmableLig",
			"dimmable light"
		},
		{
			"Wireless_Switch",
			"remote controller"
		},
		{
			"multiv4",
			"door sensor"
		},
		{
			"3325-S",
			"motion sensor"
		},
		{
			"3157100-E",
			"thermostat"
		},
	};
	struct dev_id_2_name dev_name[] = {
		{
			ZB_DEVICE_ID_LIGHT,
			"light"
		},
		{
			ZB_DEVICE_ID_DIMM_LIGHT,
			"dimmable light"
		},
		{
			ZB_DEVICE_ID_SMART_PLUG,
			"smart plug"
		},
		{
			ZB_DEVICE_ID_YIFANG_CONTROLLER,
			"on/off light switch"
		},
		{
			ZB_DEVICE_ID_IRIS_CONTROLLER,
			"remote controller"
		},
		{
			ZB_DEVICE_ID_IAS_ZONE,
			"ias zone sensor"
		},
		{
			ZB_DEVICE_ID_THERMOSTAT,
			"thermostat"
		},
	};
	int i;

	ASSERT(info != NULL);

	for (i = 0; i < (sizeof(model_name) / sizeof(model_name[0])); i++) {
		if (!strcmp(info->model_id, model_name[i].model_id)) {
			strncpy(info->alias, model_name[i].alias,
			    ZB_ALIAS_LEN);
			info->alias[ZB_ALIAS_LEN - 1] = '\0';
			log_debug("set node %s alias %s",
			    info->node->addr, info->alias);
			return;
		}
	}

	for (i = 0; i < (sizeof(dev_name) / sizeof(dev_name[0])); i++) {
		if (info->device_id == dev_name[i].device_id) {
			strncpy(info->alias, dev_name[i].alias,
			    ZB_ALIAS_LEN);
			info->alias[ZB_ALIAS_LEN - 1] = '\0';
			log_debug("set node %s alias %s",
			    info->node->addr, info->alias);
			return;
		}
	}

	return;
}

/*
 * Handle model identifier info reply
 */
void appd_model_identifier_complete_handler(uint16_t node_id, char *model_id)
{
	struct node *zb_node;
	struct zb_node_info *info;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	timer_cancel(app_get_timers(), &(info->poll_timer));

	strncpy(info->model_id, model_id, ZB_MODEL_ID_LEN);
	info->model_id[ZB_MODEL_ID_LEN - 1] = '\0';

	log_debug("node %s got model identifier %s",
	    zb_node->addr, info->model_id);
	debug_print_node_info(info);

	appd_set_node_alias(info);

	appd_query_complete_handler(info);
}

/*
 * Handle write CIE address reply
 */
void appd_write_cie_complete_handler(uint16_t node_id, uint8_t status)
{
	struct node *zb_node;
	struct zb_node_info *info;

	log_debug("node 0x%04X got write cie status 0x%02X", node_id, status);
	if (status && (status != EMBER_ZCL_STATUS_REQUEST_DENIED)) {
		return;
	}

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	timer_cancel(app_get_timers(), &(info->poll_timer));
	if (!status) {
		info->zone_state = ZB_IAS_ZONE_NOT_ENROLLED;
	} else {
		info->zone_state = ZB_IAS_ZONE_WROTE_CIE_DENIED;
	}
	conf_save();	/* Save management state to config */

	/* Check zone state */
	appd_start_read_zone_state(info);
}

/*
 * Thermostat bound complete handle
 */
static void appd_thermostat_bound_handler(struct zb_node_info *info,
			uint8_t status)
{
	uint16_t cluster_id;

	ASSERT(info != NULL);

	log_debug("node %s thermostat_bind=%d, status=%d",
	    info->node->addr, info->thermostat_bind, status);

	timer_cancel(app_get_timers(), &(info->poll_timer));

	if (!status) {
		if (info->thermostat_bind == ZB_BIND_NONE) {
			info->thermostat_bind = ZB_BIND_THERMOSTAT;
			cluster_id = ZCL_FAN_CONTROL_CLUSTER_ID;
			log_debug("node %s thermostat cluster bound"
			    " completed", info->node->addr);
			zb_thermostat_bind_request(info->node_id,
			    info->node_eui, cluster_id);
			timer_set(app_get_timers(), &(info->poll_timer),
			    WAIT_RESP_PERIOD);
		} else if (info->thermostat_bind == ZB_BIND_THERMOSTAT) {
			info->thermostat_bind = ZB_BIND_FAN_CONTROL;
			log_debug("node %s fan control cluster bound"
			    " completed", info->node->addr);
			appd_start_power_poll(info);
		} else {
			log_debug("node %s thermostat bind state %d error",
			    info->node->addr, info->thermostat_bind);
		}
	} else {
		if (info->thermostat_bind == ZB_BIND_NONE) {
			cluster_id = ZCL_THERMOSTAT_CLUSTER_ID;
			zb_thermostat_bind_request(info->node_id,
			    info->node_eui, cluster_id);
			timer_set(app_get_timers(), &(info->poll_timer),
			    WAIT_RESP_PERIOD);
		} else if (info->thermostat_bind == ZB_BIND_THERMOSTAT) {
			cluster_id = ZCL_FAN_CONTROL_CLUSTER_ID;
			zb_thermostat_bind_request(info->node_id,
			    info->node_eui, cluster_id);
			timer_set(app_get_timers(), &(info->poll_timer),
			    WAIT_RESP_PERIOD);
		} else {
			log_debug("node %s thermostat bind state %d error",
			    info->node->addr, info->thermostat_bind);
		}
	}
}

/*
 * Handle power descriptor reply
 */
void appd_power_complete_handler(uint16_t node_id,
				uint8_t powerType, uint8_t power_level)
{
	struct node *zb_node;
	struct zb_node_info *info;
	u64 msTime = MAINS_POLL_PERIOD;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	log_debug("node %s received power descriptor reply", zb_node->addr);

	if (zb_node->power == GP_BATTERY) {
		msTime *= BATTERY_POLL_INTERVAL;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	timer_set(app_get_timers(), &(info->poll_timer), msTime);
	info->poll_count = 0;

	/* Set node as ready state after got power response,
	because there is no config step when the gateway reboot */
	info->node_ready = 1;
	appd_update_as_online_status(node_id);

	/* Some devices could report attributes power configuration
	-battary percentage remaining */
	if (info->device_id != ZB_DEVICE_ID_YIFANG_CONTROLLER) {
		int level;
		level = power_level;
		appd_send_node_prop(zb_node, ZB_POWER_LEV_PROP_NAME, &level);
	}
	return;
}

/*
 * Handle prop set reply
 */
void appd_prop_complete_handler(uint16_t node_id, char *name, uint8_t status)
{
	struct node *zb_node;
	struct zb_node_info *info;
	struct nd_prop_info *prop_info;
	struct node_prop *nd_prop;
	enum node_network_result result = NETWORK_SUCCESS;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	prop_info = appd_get_node_prop_info(info->device_id);
	if (!prop_info) {
		log_err("node %s cannot find prop info", zb_node->addr);
		return;
	}

	nd_prop = node_prop_lookup(zb_node, prop_info->subdevive_key,
	    prop_info->template_key, name);

	if (!nd_prop) {
		log_err("node %s does not have property %s",
		    zb_node->addr, name);
		return;
	}

	if (status) {
		result = NETWORK_UNKNOWN;
	}

	if (info->prop_complete) {
		info->prop_complete(zb_node, nd_prop, result);
		info->prop_complete = NULL;
	}

	log_debug("node %s prop_complete_handler finished", zb_node->addr);
	return;
}

/*
 * Appd convert node info to json object when save node info to config file
 */
json_t *appd_conf_save_handler(const struct node *zb_node)
{
	struct zb_node_info *info;
	json_t *info_obj;

	ASSERT(zb_node != NULL);
	if (zb_node->interface != GI_ZIGBEE) {
		log_info("node %s is not ZigBee node", zb_node->addr);
		return NULL;
	}
	info = (struct zb_node_info *)node_state_get(
	    (struct node *)zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	info_obj = json_object();
	if (info_obj == NULL) {
		return NULL;
	}

	json_object_set_new(info_obj, "node_id", json_integer(info->node_id));
	json_object_set_new(info_obj, "profile_id",
	    json_integer(info->profile_id));
	json_object_set_new(info_obj, "device_id",
	    json_integer(info->device_id));
	json_object_set_new(info_obj, "model_id", json_string(info->model_id));
	json_object_set_new(info_obj, "zone_type",
	    json_integer(info->zone_type));
	json_object_set_new(info_obj, "manufacter_code",
	    json_integer(info->manufacter_code));
	json_object_set_new(info_obj, "zone_id",
	    json_integer(info->zone_id));
	json_object_set_new(info_obj, "zone_state",
	    json_integer(info->zone_state));
	return info_obj;
}

/*
 * convert node addr to node eui
 */
static int appd_node_addr_to_eui(char *addr, uint8_t *eui)
{
	int i;
	char digit[ZB_NODE_EUI_LEN * 2], h, l;

	ASSERT(addr != NULL);
	ASSERT(eui != NULL);

	for (i = 0; i < (ZB_NODE_EUI_LEN * 2); i++) {
		if ((addr[i] >= '0') && (addr[i] <= '9')) {
			digit[i] = (addr[i] - '0');
		} else if ((addr[i] >= 'A') && (addr[i] <= 'F')) {
			digit[i] = (addr[i] - 'A' + 10);
		} else if ((addr[i] >= 'a') && (addr[i] <= 'f')) {
			digit[i] = (addr[i] - 'a' + 10);
		} else {
			log_err("Addr %s is invalid", addr);
			memset(eui, 0, ZB_NODE_EUI_LEN);
			return -1;
		}
	}

	for (i = 0; i < ZB_NODE_EUI_LEN; i++) {
		h = digit[(i * 2) + 0];
		l = digit[(i * 2) + 1];
		eui[ZB_NODE_EUI_LEN - 1 - i] = ((h << 4) | l);
	}
	return 0;
}

/*
 * Appd get node info from json object when restore node info from config file
 */
int appd_conf_loaded_handler(struct node *zb_node, json_t *info_obj)
{
	struct zb_node_info *info;
	json_t *jobj;
	const char *model_id;

	ASSERT(zb_node != NULL);
	if (zb_node->interface != GI_ZIGBEE) {
		log_info("node %s is not ZigBee node", zb_node->addr);
		return 0;
	}
	ASSERT(info_obj != NULL);

	model_id = json_get_string(info_obj, "model_id");
	if (!model_id) {
		log_err("node does not have model id info");
		return -1;
	}

	info = (struct zb_node_info *)calloc(1, sizeof(struct zb_node_info));
	if (!info) {
		log_err("malloc memory failed for %s", zb_node->addr);
		return -1;
	}

	node_state_set(zb_node, STATE_SLOT_NET, info, appd_node_state_cleanup);

	info->node = zb_node;
	jobj = json_object_get(info_obj, "node_id");
	info->node_id = json_integer_value(jobj);
	jobj = json_object_get(info_obj, "profile_id");
	info->profile_id = json_integer_value(jobj);
	jobj = json_object_get(info_obj, "device_id");
	info->device_id = json_integer_value(jobj);

	strncpy(info->model_id, model_id, ZB_MODEL_ID_LEN);
	info->model_id[ZB_MODEL_ID_LEN - 1] = '\0';

	jobj = json_object_get(info_obj, "zone_type");
	info->zone_type = json_integer_value(jobj);
	jobj = json_object_get(info_obj, "manufacter_code");
	info->manufacter_code = json_integer_value(jobj);
	jobj = json_object_get(info_obj, "zone_id");
	info->zone_id = json_integer_value(jobj);
	jobj = json_object_get(info_obj, "zone_state");
	info->zone_state = json_integer_value(jobj);

	appd_node_addr_to_eui(zb_node->addr, info->node_eui);
	appd_set_node_alias(info);

	debug_print_node_info(info);
	return 0;
}

/*
 * Query ieee address handler
 */
void appd_ieee_query_handler(uint16_t node_id)
{
	struct node *zb_node;

	zb_node = appd_get_node(node_id);
	if (zb_node) {
		/* node already exists, do nothing */
		return;
	}

	zb_send_ieee_addr_request(node_id);
}

/*
 * Update node_id
 */
void appd_update_node_id(const uint8_t *node_eui, uint16_t node_id)
{
	char addr[ZB_NODE_ADDR_LEN + 1];
	struct node *zb_node;
	struct zb_node_info *info;

	ASSERT(node_eui != NULL);

	memset(addr, 0, sizeof(addr));
	snprintf(addr, sizeof(addr), "%02X%02X%02X%02X%02X%02X%02X%02X",
	    node_eui[7], node_eui[6], node_eui[5], node_eui[4],
	    node_eui[3], node_eui[2], node_eui[1], node_eui[0]);

	zb_node = node_lookup(addr);
	if (!zb_node) {
		log_info("no node with addr: %s", addr);
		appd_node_add(node_eui, node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (info->node_id != node_id) {
		log_debug("Update node %s node_id 0x%04X to 0x%04X",
		    addr, info->node_id, node_id);
		info->node_id = node_id;
	}
	return;
}

/*
 * Update node prop to cloud
 */
void appd_update_node_prop(uint16_t node_id, char *name, void *value)
{
	struct node *zb_node;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		/* node did not join local network, do nothing */
		log_debug("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	appd_send_node_prop(zb_node, name, value);
	return;
}

/*
 * Update gateway ZigBee network status to cloud
 */
void appd_update_network_status(bool status)
{
	struct prop *prop;

	prop = prop_lookup("zb_network_up");
	if (!prop) {
		log_err("cnnot find property zb_network_up");
		return;
	}

	if (prop_arg_set(prop, &status, sizeof(bool), NULL) != ERR_OK) {
		log_err("prop_arg_set returned error");
		return;
	}

	if (prop_send(prop) != ERR_OK) {
		log_err("prop_send returned error");
		return;
	}

	return;
}

/*
 * Get a zonde id
 */
static int appd_get_a_zone_id(uint8_t *zone_id)
{
	int i;
	ASSERT(zone_id != NULL);
	for (i = 0; i < ARRAY_LEN(zb_zone_id_list); i++) {
		if (!zb_zone_id_list[i]) {
			*zone_id = (uint8_t)i;
			zb_zone_id_list[i] = true;
			return 0;
		}
	}
	return -1;
}

/*
 * IAS zone enrolled complete handle
 */
static void appd_ias_enrolled_handler(struct zb_node_info *info)
{
	ASSERT(info != NULL);
	if (info->zone_state != ZB_IAS_ZONE_ENROLLED) {
		log_debug("node %s enroll completed", info->node->addr);
		timer_cancel(app_get_timers(), &(info->poll_timer));
		info->zone_state = ZB_IAS_ZONE_ENROLLED;
		conf_save();
		appd_start_power_poll(info);
	}
}

/*
 * Handle read zone state reply
 */
void appd_read_zone_state_complete_handler(uint16_t node_id, uint8_t state)
{
	struct node *zb_node;
	struct zb_node_info *info;

	log_debug("node 0x%04X got read zone state 0x%02X", node_id, state);

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);
	info->poll_count = 0;

	if (state == EMBER_ZCL_IAS_ZONE_STATE_ENROLLED) {
		appd_ias_enrolled_handler(info);
	}
}

/*
 * Handle IAS device enroll request
 */
void appd_ias_enroll_req_handler(uint16_t node_id, uint16_t zone_type,
			uint16_t manufacter_code)
{
	struct node *zb_node;
	struct zb_node_info *info;
	uint8_t resp_code = EMBER_ZCL_IAS_ENROLL_RESPONSE_CODE_TOO_MANY_ZONES;
	int ret = 0;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		/* node did not join local network, do nothing */
		log_debug("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	/* Stop write CIE timer after received zone enroll request */
	timer_cancel(app_get_timers(), &(info->poll_timer));

	info->zone_type = zone_type;
	info->manufacter_code = manufacter_code;

	if (info->zone_id == ZB_INVALID_ZONE_ID) {
		ret = appd_get_a_zone_id(&(info->zone_id));
		if (ret >= 0) {
			log_debug("Assigned node 0x%04X zone id %d",
			    node_id, info->zone_id);
			resp_code = EMBER_ZCL_IAS_ENROLL_RESPONSE_CODE_SUCCESS;
		}
	} else {
		log_debug("node 0x%04X zone id %d", node_id, info->zone_id);
		resp_code = EMBER_ZCL_IAS_ENROLL_RESPONSE_CODE_SUCCESS;
	}

	zb_send_enroll_response(node_id, resp_code, info->zone_id);
	log_debug("Sent enroll response code=0x%02X zone_id=%d to node=0x%04X",
	    resp_code, info->zone_id, node_id);

	if (ret < 0) {
		/* Cannot assign zone id */
		log_debug("Cannot assign zone id for node_id=0x%04X", node_id);
		return;
	}

	/* Set enrolled finish */
	appd_ias_enrolled_handler(info);

	return;
}

/*
 * Update ias zone node prop to cloud
 */
void appd_ias_zone_status_change_handler(uint16_t node_id, uint8_t *msg)
{
	struct node *zb_node;
	struct zb_node_info *info;
	struct zone_status_change *change;
	bool alarm;

	ASSERT(msg != NULL);
	change = (struct zone_status_change *)msg;

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		/* node did not join local network, do nothing */
		log_debug("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (info->device_id != ZB_DEVICE_ID_IAS_ZONE) {
		log_debug("Node %s device_id 0x%04X is not IAS node",
		    zb_node->addr, info->device_id);
		return;
	}

	if (info->zone_state == ZB_IAS_ZONE_NOT_WROTE_CIE) {
		log_debug("IAS node %s zone_state 0x%02X"
		    " not start to write CIE",
		    zb_node->addr, info->zone_state);
		return;
	}

	/* Update enroll state because node maybe not send zonde id request */
	appd_ias_enrolled_handler(info);

	if ((info->zone_type == EMBER_ZCL_IAS_ZONE_TYPE_STANDARD_CIE)
	    || (info->zone_type == EMBER_ZCL_IAS_ZONE_TYPE_CONTACT_SWITCH)) {
		alarm = change->status.alarm1_opened;
		log_info("Update sensor status(%d) for node %s",
		    alarm, zb_node->addr);
		appd_send_node_prop(zb_node, ZB_STATUS_PROP_NAME, &alarm);
	} else if (info->zone_type == EMBER_ZCL_IAS_ZONE_TYPE_MOTION_SENSOR) {
		alarm = change->status.alarm2_opened;
		log_info("Update motion sensor motion(%d) for node %s",
		    alarm, zb_node->addr);
		appd_send_node_prop(zb_node, ZB_STATUS_PROP_NAME, &alarm);
	}

	zb_send_notification_response(node_id);

	return;
}

/*
 * Handle motion sensor match descriptor request.
 */
void appd_motion_match_handler(uint16_t node_id, uint8_t seq_no)
{
	/*
	 * Response samsung motion sensor match descriptor request msg struct
	 */
	#pragma pack(1)
	struct zb_zdo_match_resp {
		uint8_t seq_no;
		uint8_t status;
		uint16_t net_addr;
		uint8_t entpoint_cnt;
		uint8_t entpoint[1];
	};
	#pragma pack()

	struct zb_zdo_match_resp match;

	match.seq_no = seq_no;
	match.status = 0;
	match.net_addr = 0x0000;
	match.entpoint_cnt = 1;
	match.entpoint[0] = 1;

	zb_send_match_response(node_id, (uint8_t *)&match, sizeof(match));
}

/*
 * Gateway bind prop handler
 * cmd format 1: bind,source node address,destination node address,cluster_id
 * cmd format 2: unbind,source node address,destination node address,cluster_id
 * format example 1: bind,00158D00006F95F1,00158D00006F9405,0x0006
 * format example 2: unbind,00158D00006F95F1,00158D00006F9405,0x0006
*/
int appd_gw_bind_prop_handler(const char *cmd, char *result, int len)
{
	const char *ptr;
	bool unbind = false;
	char src_addr[ZB_NODE_ADDR_LEN + 1];
	char dst_addr[ZB_NODE_ADDR_LEN + 1];
	uint16_t cluster_id;
	struct zb_node_info *src_info;
	struct zb_node_info *dst_info;
	int ret;

	ASSERT(cmd != NULL);
	ASSERT(result != NULL);

	memset(result, 0, len);
	ptr = cmd;

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}

	if (strncmp(ptr, "bind", 4)) {
		if (strncmp(ptr, "unbind", 6)) {
			log_err("cannot find bind or unbind in command %s",
			    cmd);
			snprintf(result, len,
			    "cannot find bind or unbind in command %s,"
			    " cmd format: bind or unbind,source node address,"
			    "destination node address,cluster_id; format"
			    " example: bind,00158D00006F95F1,"
			    "00158D00006F9405,0x0006", cmd);
			return -1;
		}
		unbind = true;
	}

	/* Skip bind or unbind, */
	if (unbind) {
		ptr += 2;
	}
	ptr += 4;

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}

	if (*ptr != ',') {
		log_err("cannot find first , in command %s", cmd);
		snprintf(result, len,
		    "cannot find first , in command %s,"
		    " cmd format: bind or unbind,source node address,"
		    "destination node address,cluster_id; format"
		    " example: bind,00158D00006F95F1,"
		    "00158D00006F9405,0x0006", cmd);
		return -1;
	}

	ptr++; /* Skip first , */

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}
	strncpy(src_addr, ptr, ZB_NODE_ADDR_LEN);
	src_addr[ZB_NODE_ADDR_LEN] = '\0';

	/* Skip NODE_ADDR_LEN */
	ptr += ZB_NODE_ADDR_LEN;

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}

	if (*ptr != ',') {
		log_err("cannot find second , in command %s, src_addr=%s",
		    cmd, src_addr);
		snprintf(result, len,
		    "cannot find second , in command %s,"
		    " cmd format: bind or unbind,source node address,"
		    "destination node address,cluster_id; format"
		    " example: bind,00158D00006F95F1,"
		    "00158D00006F9405,0x0006", cmd);
		return -1;
	}

	ptr++;  /* Skip second , */

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}

	strncpy(dst_addr, ptr, ZB_NODE_ADDR_LEN);
	dst_addr[ZB_NODE_ADDR_LEN] = '\0';

	/* Skip NODE_ADDR_LEN */
	ptr += ZB_NODE_ADDR_LEN;

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}

	if (*ptr != ',') {
		log_err("cannot find third , in command %s, src_addr=%s,"
		    " dst_addr=%s", cmd, src_addr, dst_addr);
		snprintf(result, len,
		    "cannot find third , in command %s,"
			" cmd format: bind or unbind,source node address,"
			"destination node address,cluster_id; format"
			" example: bind,00158D00006F95F1,"
			"00158D00006F9405,0x0006", cmd);

		return -1;
	}

	ptr++; /* Skip third , */

	/* Skip space */
	while (*ptr == ' ') {
		ptr++;
	}

	cluster_id = (uint16_t)strtoul(ptr, NULL, 16);

	log_debug("%s, src_addr=%s, dst_addr=%s, cluster_id=0x%04X",
	    (unbind ? "unbind" : "bind"), src_addr, dst_addr, cluster_id);

	src_info = appd_get_node_info(src_addr);
	if (!src_info) {
		log_err("no node with addr: %s", src_addr);
		snprintf(result, len - 1,
		    "%s: no node with addr: %s", cmd, src_addr);
		return -1;
	}

	dst_info = appd_get_node_info(dst_addr);
	if (!dst_info) {
		log_err("no node with addr: %s", dst_addr);
		snprintf(result, len - 1,
		    "%s: no node with addr: %s", cmd, dst_addr);
		return -1;
	}

	if (!unbind) {
		ret = zb_send_bind_request(src_info->node_id,
		    src_info->node_eui, cluster_id, dst_info->node_eui);
	} else {
		ret = zb_send_unbind_request(src_info->node_id,
		    src_info->node_eui, cluster_id, dst_info->node_eui);
		}

	return ret;
}

/*
 * Handle gateway prop bind response result.
 */
static void appd_gw_prop_bind_complete_handler(struct node *zb_node,
			uint8_t status, bool unbind)
{
	const char *result = "zb_bind_result";
	const char *cmd = "zb_bind_cmd";
	struct prop *prop_result, *prop_cmd;

	ASSERT(zb_node != NULL);

	prop_result = prop_lookup(result);
	if (!prop_result) {
		log_debug("cannot find prop %s", result);
		return;
	}

	if (!status) {
		snprintf(prop_result->arg, prop_result->len,
		    "%s %s success", zb_node->addr,
		    (unbind ? "unbind" : "bind"));
		prop_cmd = prop_lookup(cmd);
		if (prop_cmd) {
			memset(prop_cmd->arg, 0, prop_cmd->len);
			prop_send(prop_cmd);
		}
	} else {
		snprintf(prop_result->arg, prop_result->len,
		    "%s %s failure", zb_node->addr,
		    (unbind ? "unbind" : "bind"));
	}

	prop_send(prop_result);
}

/*
 * Handle bind response result.
 */
void appd_bind_response_handler(uint16_t node_id,
			uint8_t status, bool unbind)
{
	struct node *zb_node;
	struct zb_node_info *info;

	log_debug("node 0x%04X got bind response status 0x%02X",
	    node_id, status);

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	if (info->device_id == ZB_DEVICE_ID_THERMOSTAT) {
		if (!unbind) {
			appd_thermostat_bound_handler(info, status);
		}
	} else if ((info->device_id == ZB_DEVICE_ID_YIFANG_CONTROLLER)
	    || (info->device_id == ZB_DEVICE_ID_IRIS_CONTROLLER)) {
		appd_gw_prop_bind_complete_handler(zb_node, status, unbind);
	} else {
		log_debug("node 0x%04X device id is 0x%04X,"
		    " no need care about it", node_id, info->device_id);
	}
}

/*
 * Update node decimal prop
 */
void appd_update_decimal_prop(uint16_t node_id, char *name, double value)
{
	struct node *zb_node;

	log_debug("decimal prop name %s, value=%lf from source=0x%04X",
	    name, value, node_id);

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	appd_send_node_prop(zb_node, name, (void *)&value);
	return;
}

/*
 * Update node int prop
 */
void appd_update_int_prop(uint16_t node_id, char *name, int value)
{
	struct node *zb_node;

	log_debug("int prop name %s, value=%d from source=0x%04X",
	    name, value, node_id);

	zb_node = appd_get_node(node_id);
	if (!zb_node) {
		log_err("Cannot find node for node_id=0x%04X", node_id);
		return;
	}

	appd_send_node_prop(zb_node, name, (void *)&value);
	return;
}

/*
 * Update node state
 */
static int appd_update_node_state(struct node *zb_node, void *arg)
{
	struct zb_node_info *info;

	ASSERT(zb_node != NULL);
	if (zb_node->interface != GI_ZIGBEE) {
		return 0;
	}

	info = (struct zb_node_info *)node_state_get(zb_node, STATE_SLOT_NET);
	ASSERT(info != NULL);

	/* Update zone id occupied info */
	if (info->device_id == ZB_DEVICE_ID_IAS_ZONE) {
		if (info->zone_id < ZB_INVALID_ZONE_ID) {
			zb_zone_id_list[info->zone_id] = true;
		}
	}

	appd_update_as_offline_status(info);

	return 0;
}

/*
 * Update all nodes state
 */
void appd_update_all_node_state(void)
{
	int i;
	log_debug("Update all nodes state");
	node_foreach(appd_update_node_state, NULL);
	for (i = 0; i < ARRAY_LEN(zb_zone_id_list); i++) {
		if (zb_zone_id_list[i]) {
			log_debug("zone id %d is occupied", i);
		}
	}
}

