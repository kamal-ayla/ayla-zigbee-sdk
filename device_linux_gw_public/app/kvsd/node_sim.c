/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/queue.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/timer.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/gateway_interface.h>

#include <app/app.h>
#include <app/ops.h>
#include <app/props.h>
#include <app/gateway.h>

#include "gateway.h"
#include "node.h"
#include "node_sim.h"


/* Node details */
#define SIM_NODE_OEM_MODEL_PREFIX	"ggdemo_"
#define SIM_NODE_VERSION		"1.0"

/* Single subdevice supported */
#define SIM_NODE_SUBDEVICE		"s1"

/* Template keys */
#define SIM_NODE_TEMPLATE_SIM		"gg_sim"
#define SIM_NODE_TEMPLATE_TSTAT		"gg_tstat"
#define SIM_NODE_TEMPLATE_SENSOR	"gg_sens"

/* Default between simulated node property updates */
#define SIM_NODE_SAMPLE_TIME_DEFAULT_MS	15000

/*
 * Node simulator constants to help generate interesting datapoints.
 */
/* Estimated average monthly day temp in Sunnyvale, CA */
static int sim_temp_high[12] = {
	59, 62, 66, 70, 74, 78, 80, 79, 80, 74, 65, 59
};
/* Estimated average monthly night temp in Sunnyvale, CA */
static int sim_temp_low[12] = {
	39, 41, 43, 45, 49, 52, 55, 55, 53, 48, 42, 38
};
/* Estimated average monthly day humidity in Sunnyvale, CA */
static int sim_humidity_high[12] = {
	90, 88, 89, 84, 81, 82, 85, 86, 83, 83, 88, 89
};
/* Estimated average monthly night humidity in Sunnyvale, CA */
static int sim_humidity_low[12] = {
	56, 52, 50, 44, 42, 45, 49, 53, 44, 41, 46, 48
};
#define SIM_STEP_TIME_MS		15000	/* Simulator update period */
#define SIM_TEMP_JITTER_RANGE		1.0	/* Ambient temp variation */
#define SIM_HUMIDITY_JITTER_RANGE	1.0	/* Ambient humidity variation */
#define SIM_LIGHT_HIGH			85	/* Ambient light upper bound */
#define SIM_LIGHT_LOW			10	/* Ambient light lower bound */
#define SIM_LIGHT_JITTER_RANGE		8.0	/* Ambient light variation */
#define SIM_TSTAT_TEMP_DEADBAND		2.0	/* Thermostat temp deadband */
#define SIM_TEMP_INDOOR_ACTIVE_RATE	240.0	/* Heating/cooling Degrees/hr */
#define SIM_TEMP_INDOOR_PASSIVE_RATE	60.0	/* Degrees/hr */
#define SIM_SETPOINT_DEFAULT		72	/* Thermostat init setpoint */
#define SIM_SETPOINT_VACATION_LOW	55	/* Thermostat vaca low bound */
#define SIM_SETPOINT_VACATION_HIGH	80	/* Thermostat vaca high bound */
#define SIM_BATTERY_CHARGE_RATE		1000	/* Battery [dis]charge %/hr */

/*
 * Simulator-specific state to be associated with a node
 */
struct sim_node_state {
	struct node *node;		/* Pointer to node */
	enum sim_node_type type;	/* Simulated node type */
	unsigned sample_ms;		/* Property update period */
	struct timer sample_timer;	/* Timer for prop updates */
	bool pending_removal;		/* Track simulated node leave event */
};

/*
 * Node simulator state
 */
struct sim_state {
	struct file_event_table *file_events;
	struct timer_head *timers;

	u32 node_index;			/* Index for new node address */

	double ambient_temp;		/* Degrees F */
	double ambient_humidity;	/* % */
	double ambient_light;		/* % */

	struct timer step_timer;	/* Timer for global sim updates */
};

static struct sim_state state;

DEF_NAME_TABLE(sim_node_type_names, SIM_NODE_TYPES);

/*
 * Forward declarations
 */
static int sim_node_prop_battery_enable_set(struct node *node,
	struct node_prop *prop);
static int sim_node_prop_update_tstat(struct node *node,
	struct node_prop *prop);
static void sim_node_sample_timeout(struct timer *timer);
static void sim_node_prop_init_enable(struct node *node,
	struct node_prop *prop);
static void sim_node_prop_init_battery_charge(struct node *node,
	struct node_prop *prop);
static void sim_node_prop_init_temp_setpoint(struct node *node,
	struct node_prop *prop);
static void sim_node_prop_init_local_temp(struct node *node,
	struct node_prop *prop);
static void sim_update_tstat(const struct sim_state *sim,
	struct node *node);

/*****************************************
 * Simulated node template definition
 *****************************************/

struct sim_node_prop_def {
	struct node_prop_def def;
	void (*init)(struct node *, struct node_prop *);
	int (*set_callback)(struct node *, struct node_prop *);
};

static const struct sim_node_prop_def sim_template_sim[] = {
	{ { "enable",		PROP_BOOLEAN,	PROP_TO_DEVICE },
		sim_node_prop_init_enable },
	{ { "battery_enable",	PROP_BOOLEAN,	PROP_TO_DEVICE }, NULL,
	    sim_node_prop_battery_enable_set },
	{ { "battery_charge",	PROP_INTEGER,	PROP_FROM_DEVICE },
		sim_node_prop_init_battery_charge }
};

static const struct sim_node_prop_def sim_template_tstat[] = {
	{ { "temp_setpoint",	PROP_INTEGER,	PROP_TO_DEVICE },
		sim_node_prop_init_temp_setpoint, sim_node_prop_update_tstat },
	{ { "vacation_mode",	PROP_BOOLEAN,	PROP_TO_DEVICE }, NULL,
		sim_node_prop_update_tstat },
	{ { "local_temp",	PROP_DECIMAL,	PROP_FROM_DEVICE },
		sim_node_prop_init_local_temp },
	{ { "heat_on",		PROP_BOOLEAN,	PROP_FROM_DEVICE } },
	{ { "ac_on",		PROP_BOOLEAN,	PROP_FROM_DEVICE } }
};

static const struct sim_node_prop_def sim_template_sensor[] = {
	{ { "temp",		PROP_DECIMAL,	PROP_FROM_DEVICE } },
	{ { "humidity",		PROP_DECIMAL,	PROP_FROM_DEVICE } },
	{ { "light_level",	PROP_DECIMAL,	PROP_FROM_DEVICE } },
};

/*****************************************
 * Simulated node template name to definition mapping
 *****************************************/

struct sim_node_template_entry {
	const char *key;
	const struct sim_node_prop_def * const prop_defs;
	size_t num_props;
};

static const struct sim_node_template_entry sim_node_templates[] = {
	{
		SIM_NODE_TEMPLATE_SIM,
		sim_template_sim, ARRAY_LEN(sim_template_sim)
	},
	{
		SIM_NODE_TEMPLATE_TSTAT,
		sim_template_tstat, ARRAY_LEN(sim_template_tstat)
	},
	{
		SIM_NODE_TEMPLATE_SENSOR,
		sim_template_sensor, ARRAY_LEN(sim_template_sensor)
	}
};

/*****************************************
 * Simulated node and property setup
 *****************************************/

struct sim_node_search_query {
	bool (*match)(const struct sim_node_state *sim_node, const void *);
	const void *arg;
	struct sim_node_state *result;
};

/*
 * Helper function to get the simulated node state from the node's
 * network state slot.
 */
static inline struct sim_node_state *sim_node_state_get(struct node *node)
{
	return (struct sim_node_state *)node_state_get(node, STATE_SLOT_NET);
}

/*
 * Query function intended to be used with node_foreach() to search
 * for a simulated node using the supplied sim_node_search_query.
 */
static int sim_node_search_handler(struct node *node, void *arg)
{
	struct sim_node_search_query *query =
	    (struct sim_node_search_query *)arg;
	struct sim_node_state *sim_node = sim_node_state_get(node);

	ASSERT(query != NULL);
	ASSERT(query->match != NULL);

	if (!sim_node) {
		return 0;
	}
	if (!query->match(sim_node, query->arg)) {
		/* Not a match; keep searching */
		return 0;
	}
	/* Found match, so return > 0 to break foreach loop */
	query->result = sim_node;
	return 1;
}

/*
 * Find the simulator template definition for a particular property.
 * This is useful to restore the simulator state for nodes loaded from config.
 */
static const struct sim_node_prop_def *sim_node_template_lookup(
	const struct node_prop *prop)
{
	const struct sim_node_template_entry *template = sim_node_templates;
	size_t num_templates = ARRAY_LEN(sim_node_templates);
	const struct sim_node_prop_def *prop_def;
	size_t num_props;

	/* Lookup the template by name */
	for (; num_templates; --num_templates, ++template) {
		if (!strcmp(template->key, prop->template->key)) {
			break;
		}
	}
	if (!num_templates) {
		return NULL;
	}
	/* Lookup the property by name */
	prop_def = template->prop_defs;
	num_props = template->num_props;
	for (; num_props; --num_props, ++prop_def) {
		if (!strcmp(prop_def->def.name, prop->name)) {
			return prop_def;
		}
	}
	return NULL;
}

/*
 * Initial setup of a simulated node property.
 */
static int sim_node_prop_init(struct node *node, struct node_prop *prop,
	void *arg)
{
	const struct sim_node_prop_def *prop_def =
	    (const struct sim_node_prop_def *)arg;

	if (!prop_def) {
		/* Property definition not supplied, so try to look it up */
		prop_def = sim_node_template_lookup(prop);
		if (!prop_def) {
			log_warn("%s: property %s is not managed by simulator",
			    node->addr, prop->name);
			return 0;
		}
	}
	/* Assign simulator definition as state for access to callbacks */
	node_prop_state_set(prop, STATE_SLOT_NET, (void *)prop_def, NULL);
	/* Initialize property state */
	if (prop_def->init) {
		prop_def->init(node, prop);
	}
	return 0;
}

/*
 * Associate a node template definition table with a node.  Used by the
 * simulator to setup a node supporting the desired characteristics.
 */
static void sim_node_template_add(struct node *node,
	const char *subdevice, const char *template,
	const struct sim_node_prop_def *table, size_t table_size)
{
	struct node_prop *prop;

	for (; table_size; --table_size, ++table) {
		prop = node_prop_add(node, subdevice, template, &table->def,
		    NULL);
		if (prop) {
			sim_node_prop_init(node, prop, (void *)table);
		}
	}
}

/*
 * Initialize the "enable" property.
 */
static void sim_node_prop_init_enable(struct node *node,
	struct node_prop *prop)
{
	/* Enable simulator by default */
	node_prop_boolean_send(node, prop, true);
}

/*
 * Initialize the "battery_charge" property.
 */
static void sim_node_prop_init_battery_charge(struct node *node,
	struct node_prop *prop)
{
	/* Battery fully charged by default */
	node_prop_integer_send(node, prop, 100);
}

/*
 * Initialize the "temp_setpoint" property.
 */
static void sim_node_prop_init_temp_setpoint(struct node *node,
	struct node_prop *prop)
{
	/* Set default setpoint state (but do not echo it to the cloud) */
	ASSERT(prop->type == PROP_INTEGER);
	*((int *)prop->val) = SIM_SETPOINT_DEFAULT;
}

/*
 * Initialize the "local_temp" property.
 */
static void sim_node_prop_init_local_temp(struct node *node,
	struct node_prop *prop)
{
	/* Set initial local temp to ambient */
	node_prop_decimal_send(node, prop, state.ambient_temp);
}

/*
 * Add properties to a new simulated node.
 */
static void sim_node_populate_props(struct node *node, enum sim_node_type type)
{
	/* All simulated nodes support a sim template */
	sim_node_template_add(node, SIM_NODE_SUBDEVICE, SIM_NODE_TEMPLATE_SIM,
	    sim_template_sim, ARRAY_LEN(sim_template_sim));

	/* Apply node-specific templates */
	switch (type) {
	case SIM_THERMOSTAT:
		sim_node_template_add(node, SIM_NODE_SUBDEVICE,
		    SIM_NODE_TEMPLATE_TSTAT,
		    sim_template_tstat, ARRAY_LEN(sim_template_tstat));
		break;
	case SIM_SENSOR:
		sim_node_template_add(node, SIM_NODE_SUBDEVICE,
		    SIM_NODE_TEMPLATE_SENSOR,
		    sim_template_sensor, ARRAY_LEN(sim_template_sensor));
		break;
	default:
		log_err("unsupported sim_node_type");
		ASSERT_NOTREACHED();
	}
}

/*
 * Callback to handle a new "battery_enable" property value.
 */
static int sim_node_prop_battery_enable_set(struct node *node,
	struct node_prop *prop)
{
	enum gw_power new_power;

	/* Update node with different power status */
	new_power = node_prop_boolean_val(prop) ? GP_BATTERY : GP_MAINS;
	if (new_power == node->power) {
		return 0;
	}
	node->power = new_power;
	log_debug("%s: battery %s", node->addr,
	    node->power == GP_BATTERY ? "ENABLED" : "DISABLED");

	/* Indicate to app that node state was updated */
	node_info_changed(node, NULL);

	/* Set online if offline due to empty battery */
	if (node->power == GP_MAINS && !node->online) {
		log_app("%s: power restored", node->addr);
		node_conn_status_changed(node, true);
	}
	return 0;
}

/*
 * Callback to handle a new thermostat simulator property value that
 * might change its mode.
 */
static int sim_node_prop_update_tstat(struct node *node,
	struct node_prop *prop)
{
	sim_update_tstat(&state, node);
	return 0;
}

/*
 * Cleanup function for sim_node_state.
 */
static void sim_node_state_cleanup(void *arg)
{
	struct sim_node_state *node_state = (struct sim_node_state *)arg;

	timer_cancel(state.timers, &node_state->sample_timer);
	free(node_state);
}

/*
 * Associate node simulator state with a node and schedule it to run.
 */
static struct sim_node_state *sim_node_start(struct node *node,
	enum sim_node_type type, unsigned sample_ms)
{
	struct sim_node_state *node_state;

	node_state = (struct sim_node_state *)calloc(1,
	    sizeof(struct sim_node_state));
	if (!node_state) {
		log_err("malloc failed");
		return NULL;
	}
	node_state->node = node;
	node_state->type = type;
	timer_init(&node_state->sample_timer, sim_node_sample_timeout);
	node_state->sample_ms = sample_ms ? sample_ms :
	    SIM_NODE_SAMPLE_TIME_DEFAULT_MS;
	timer_set(app_get_timers(), &node_state->sample_timer,
	    node_state->sample_ms);
	/* Associate simulator state with node entity */
	node_state_set(node, STATE_SLOT_NET, node_state,
	    sim_node_state_cleanup);
	return node_state;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int sim_node_query_info_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	struct sim_node_state *sim_node = sim_node_state_get(node);

	if (!sim_node) {
		log_err("%s: missing node simulator state", node->addr);
		return -1;
	}
	/*
	 * Query the node's capabilities.  Since this is a simulator, we
	 * already know what the node type is and can look up its information
	 * locally.  When using a real network stack, a gateway might send a
	 * message to the node requesting this info.
	 */
	sim_node_populate_props(node, sim_node->type);
	log_app("%s: configured as %s node", node->addr,
	    sim_node_type_names[sim_node->type]);
	/*
	 * Normally, this callback would be invoked later, when a response
	 * came back from the node, but the node management layer supports
	 * calling it immediately.
	 */
	if (callback) {
		callback(node, NETWORK_SUCCESS);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int sim_node_configure_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	/*
	 * Simulated nodes do not need to be configured, so just update their
	 * online status.  When using a real network, node features such as
	 * push updates for properties might be setup at this time.
	 */
	node_conn_status_changed(node, true);
	if (callback) {
		callback(node, NETWORK_SUCCESS);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int sim_node_prop_set_handler(struct node *node, struct node_prop *prop,
    void (*callback)(struct node *, struct node_prop *,
    enum node_network_result))
{
	const struct sim_node_prop_def *prop_def =
	    (const struct sim_node_prop_def *)node_prop_state_get(
	    prop, STATE_SLOT_NET);

	/*
	 * Invoke optional set callback set when the simulated
	 * node was initialized.
	 */
	if (prop_def && prop_def->set_callback) {
		if (prop_def->set_callback(node, prop) < 0) {
			return -1;
		}
	}
	if (node->online) {
		/* Using simulated nodes, so there is nothing to do here */
		log_info("%s: property %s set to %s", node->addr, prop->name,
		    prop_val_to_str(prop->val, prop->type));
		if (callback) {
			callback(node, prop, NETWORK_SUCCESS);
		}
	} else if (callback) {
		callback(node, prop, NETWORK_OFFLINE);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int sim_node_leave_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	log_app("%s: leaving network", node->addr);
	if (callback) {
		callback(node, NETWORK_SUCCESS);
	}
	/*
	 * Using simulated nodes, so there is nothing to do here.
	 * Simulator state will be cleaned up automatically when the
	 * generic node management state machine deletes the node.
	 */
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer push a downloaded OTA firmware image to a node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int sim_node_ota_handler(struct node *node,
	const char *version, const char *image_path,
	void (*callback)(struct node *, enum node_network_result))
{
	if (node->online) {
		/* Assume OTA load success for simulated nodes */
		log_app("%s: upgraded to version %s", node->addr, version);
		node_info_changed(node, version);
		if (callback) {
			callback(node, NETWORK_SUCCESS);
		}
	} else if (callback) {
		callback(node, NETWORK_OFFLINE);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer when the node is
 * being saved to non-volatile memory.  The JSON object returned is passed
 * to sim_node_load() to restore the simulated node state.
 */
static json_t *sim_node_save(const struct node *node)
{
	const struct sim_node_state *sim_node =
	    sim_node_state_get((struct node *)node);
	json_t *net_state_obj;

	if (!sim_node) {
		log_err("%s: missing node simulator state", node->addr);
		return NULL;
	}
	net_state_obj = json_object();
	json_object_set_new(net_state_obj, "sim_type",
	    json_integer(sim_node->type));
	json_object_set_new(net_state_obj, "sample_ms",
	    json_integer(sim_node->sample_ms));
	return net_state_obj;
}

/*
 * Handler called by the generic node management layer to restore a node
 * loaded from config.  The JSON object output by sim_node_save() is
 * passed in via the net_state_obj parameter.
 */
static int sim_node_loaded(struct node *node, json_t *net_state_obj)
{
	u32 index;
	char *cp;
	enum sim_node_type type;
	unsigned sample_ms;
	unsigned val;

	/* Update index to assign unique addresses for future nodes */
	cp = strrchr(node->addr, '_');
	if (cp) {
		index = strtoul(++cp, NULL, 16);
		if (state.node_index < index) {
			state.node_index = index;
		}
	}
	if (!net_state_obj) {
		log_err("%s: no simulator state", node->addr);
		return -1;
	}
	/* Restore node state stored in the config file by sim_node_save() */
	if (json_get_uint(net_state_obj, "sim_type", &val) < 0 ||
	    val >= SIM_TYPE_COUNT) {
		log_err("invalid sim_type");
		return -1;
	}
	type = val;
	if (json_get_uint(net_state_obj, "sample_ms", &sample_ms) < 0) {
		sample_ms = 0;
	}
	/* Restore simulated node state */
	if (!sim_node_start(node, type, sample_ms)) {
		log_err("%s: failed to initialize simulator", node->addr);
		return -1;
	}
	/*
	 * Call simulator init routine for each property.  Supply NULL
	 * arg so init function looks up template definition.
	 */
	node_prop_foreach(node, sim_node_prop_init, NULL);
	log_app("%s: loaded from config", node->addr);
	/* This is a simulated node, so set it to online */
	node_conn_status_changed(node, true);
	return 0;
}

/*****************************************
 * Simulation data generation functions
 *****************************************/

static double sim_add_varience(double base, double range)
{
	double offset;

	offset = (range * (((double)random() / RAND_MAX) - 0.5));
	return base + offset;
}

static double sim_get_daily_value(int hour, double night, double day)
{
	const int night_hour = 3;	/* Estimated temperature low: 4am */
	const int day_hour = 13;	/* Estimated temperature high: 2pm */
	double range = day - night;

	hour %= 24;	/* Normalize hour: 0-23 */

	if (hour > night_hour && hour <= day_hour) {
		return night + range * ((double)(hour - night_hour) /
		    (double)(day_hour - night_hour));
	} else {
		return day - range * ((double)(hour < day_hour ?
		    hour + 23 - day_hour : hour - day_hour) /
		    (double)(23 - day_hour + night_hour));
	}
}

static double sim_get_monthly_value(int day, double start, double end)
{
	double range = end - start;

	day = ((day - 1) % 31) + 1;	/* Normalize day of month: 1-31 */

	return range * ((double)(day - 1) / 30) + start;
}

static double sim_get_daily_light_value(int hour)
{
	const int dawn = 5;	/* 6am */
	const int noon = 11;	/* 12pm */
	const int dark = 19;	/* 8pm */
	const double range = SIM_LIGHT_HIGH - SIM_LIGHT_LOW;

	hour %= 24;	/* Normalize hour: 0-23 */

	if (hour > dawn && hour <= noon) {
		return SIM_LIGHT_LOW + range * ((double)(hour - dawn) /
		    (double)(noon - dawn));	/* Morning */
	} else if (hour > noon && hour <= dark) {
		return SIM_LIGHT_HIGH - range * ((double)(hour - noon) /
		    (double)(dark - noon));	/* Afternoon */
	} else {
		return SIM_LIGHT_LOW;		/* Night */
	}
}

static void sim_update_ambient(struct sim_state *sim, const struct tm *cal)
{
	sim->ambient_temp = sim_get_daily_value(cal->tm_hour,
	    sim_get_monthly_value(cal->tm_mday,
		sim_temp_low[cal->tm_mon],
		sim_temp_low[cal->tm_mon + 1 % ARRAY_LEN(sim_temp_low)]),
	    sim_get_monthly_value(cal->tm_mday,
		sim_temp_high[cal->tm_mon],
		sim_temp_high[cal->tm_mon + 1 % ARRAY_LEN(sim_temp_high)]));
	sim->ambient_temp = sim_add_varience(sim->ambient_temp,
	    SIM_TEMP_JITTER_RANGE);

	sim->ambient_humidity = sim_get_daily_value(cal->tm_hour,
	    sim_get_monthly_value(cal->tm_mday,
		sim_humidity_high[cal->tm_mon],
		sim_humidity_high[cal->tm_mon + 1 %
		ARRAY_LEN(sim_humidity_high)]),
	    sim_get_monthly_value(cal->tm_mday,
		sim_humidity_low[cal->tm_mon],
		sim_humidity_low[cal->tm_mon + 1 %
		ARRAY_LEN(sim_humidity_low)]));
	sim->ambient_humidity = sim_add_varience(sim->ambient_humidity,
	    SIM_HUMIDITY_JITTER_RANGE);

	sim->ambient_light = sim_get_daily_light_value(cal->tm_hour);
	sim->ambient_light = sim_add_varience(sim->ambient_light,
	    SIM_LIGHT_JITTER_RANGE);

	log_debug("temp=%.2f humidity=%.2f light=%.2f", sim->ambient_temp,
	    sim->ambient_humidity, sim->ambient_light);
}

/*****************************************
 * Send simulated node datapoints
 *****************************************/

/*
 * Get the value of the "enabled" property.
 * All simulated nodes should have this property.
 */
static bool sim_node_is_enabled(const struct sim_state *sim,
	struct node *node)
{
	struct node_prop *enabled;

	enabled = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_SIM, "enable");
	if (!enabled) {
		log_err("missing simulation enable property");
		return false;
	}
	return node_prop_boolean_val(enabled);
}

/*
 * Update the state of properties used by the simulated sensor node.
 */
static void sim_update_outdoor(const struct sim_state *sim,
	struct sim_node_state *sim_node)
{
	struct node *node = sim_node->node;
	struct node_prop *temp, *humidity, *light;

	/* Do nothing if node is offline */
	if (!node->online) {
		return;
	}
	if (!sim_node_is_enabled(sim, node)) {
		return;
	}
	temp = node_prop_lookup(node, NULL,
	    SIM_NODE_TEMPLATE_SENSOR, "temp");
	humidity = node_prop_lookup(node, NULL,
	    SIM_NODE_TEMPLATE_SENSOR, "humidity");
	light = node_prop_lookup(node, NULL,
	    SIM_NODE_TEMPLATE_SENSOR, "light_level");

	if (temp) {
		node_prop_decimal_send(node, temp,
		    sim->ambient_temp);
	}
	if (humidity) {
		node_prop_decimal_send(node, humidity,
		    sim->ambient_humidity);
	}
	if (light) {
		node_prop_decimal_send(node, light,
		    sim->ambient_light);
	}
}

/*
 * Update the state of properties used by the simulated thermostat node for
 * indoor temperature status.
 */
static void sim_update_indoor(const struct sim_state *sim,
	struct sim_node_state *sim_node)
{
	struct node *node = sim_node->node;
	double active_step = SIM_TEMP_INDOOR_ACTIVE_RATE /
	    (3600000.0 / sim_node->sample_ms);
	double passive_step = SIM_TEMP_INDOOR_PASSIVE_RATE /
	    (3600000.0 / sim_node->sample_ms);
	double temp_val;
	bool heating_val;
	bool cooling_val;
	struct node_prop *temp, *heating, *cooling;

	/* Do nothing if node is offline */
	if (!node->online) {
		return;
	}
	/* Lookup thermostat properties */
	temp = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "local_temp");
	heating = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "heat_on");
	cooling = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "ac_on");
	if (!temp || (!heating && !cooling)) {
		/* No indoor temp control */
		if (temp) {
			node_prop_decimal_send(node, temp,
			    sim->ambient_temp);
		}
		return;
	}
	temp_val = node_prop_decimal_val(temp);
	cooling_val = cooling ? node_prop_boolean_val(cooling) : false;
	heating_val = heating ? node_prop_boolean_val(heating) : false;

	/* Update properties */
	if (heating_val) {
		/* Simulate active heating controlled by thermostat */
		log_debug("%s: heating step: %.2f --> %.2f", node->addr,
		    temp_val, temp_val + active_step);
		temp_val += active_step;

	} else if (cooling_val) {
		/* Simulate active cooling controlled by thermostat */
		log_debug("%s: cooling step: %.2f --> %.2f", node->addr,
		    temp_val, temp_val - active_step);
		temp_val -= active_step;
	} else {
		/* Simulate return to ambient temp (simplified linear model) */
		if (temp_val < sim->ambient_temp) {
			log_debug("%s: passive heating: %.2f --> %.2f",
			    node->addr, temp_val, temp_val + passive_step);
			temp_val += passive_step;
		} else if (temp_val > sim->ambient_temp) {
			log_debug("%s: passive cooling: %.2f --> %.2f",
			    node->addr, temp_val, temp_val - passive_step);
			temp_val -= passive_step;
		}
	}
	node_prop_decimal_send(node, temp, temp_val);
}

/*
 * Update the state of properties used by the simulated thermostat node for
 * thermostat mode.
 */
static void sim_update_tstat(const struct sim_state *sim,
	struct node *node)
{
	bool enabled_val;
	int setpoint_val_low;
	int setpoint_val_high;
	double temp_val;
	bool heating_val;
	bool cooling_val;
	bool vacation_val;
	struct node_prop *setpoint, *temp, *vacation, *heating, *cooling;
	enum { OFF, COOLING, HEATING } mode;

	/* Do nothing if node is offline */
	if (!node->online) {
		return;
	}
	enabled_val = sim_node_is_enabled(sim, node);
	/* Lookup thermostat properties */
	setpoint = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "temp_setpoint");
	temp = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "local_temp");
	heating = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "heat_on");
	cooling = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "ac_on");
	if (!setpoint || (!heating && !cooling)) {
		/* No indoor temp control */
		return;
	}
	if (!enabled_val) {
		mode = OFF;
		goto update;
	}
	temp_val = node_prop_decimal_val(temp);
	cooling_val = cooling ? node_prop_boolean_val(cooling) : false;
	heating_val = heating ? node_prop_boolean_val(heating) : false;
	vacation = node_prop_lookup(node, NULL, SIM_NODE_TEMPLATE_TSTAT,
	    "vacation_mode");
	vacation_val = vacation ? node_prop_boolean_val(vacation) : false;
	if (vacation_val) {
		/* Vacation mode applies a fixed setpoint override */
		setpoint_val_low = SIM_SETPOINT_VACATION_LOW;
		setpoint_val_high = SIM_SETPOINT_VACATION_HIGH;
	} else {
		setpoint_val_low = node_prop_integer_val(setpoint);
		setpoint_val_high = node_prop_integer_val(setpoint);
	}
	/* Determine current mode from property values */
	if (heating_val) {
		mode = HEATING;
	} else if (cooling_val) {
		mode = COOLING;
	} else {
		mode = OFF;
	}
	/* Determine next thermostat mode */
	switch (mode) {
	case OFF:
		if (heating && temp_val < setpoint_val_low -
		    SIM_TSTAT_TEMP_DEADBAND) {
			mode = HEATING;
			break;
		}
		if (cooling && temp_val > setpoint_val_high +
		    SIM_TSTAT_TEMP_DEADBAND) {
			mode = COOLING;
			break;
		}
		break;
	case COOLING:
		if (temp_val < setpoint_val_high) {
			mode = OFF;
		}
		break;
	case HEATING:
		if (temp_val > setpoint_val_low) {
			mode = OFF;
		}
		break;
	}
	log_debug("%s: vacation=%hhu setpoint=[%d,%d] mode=%s", node->addr,
	    vacation_val, setpoint_val_low, setpoint_val_high,
	    mode == OFF ? "OFF" : mode == HEATING ? "HEATING" : "COOLING");
update:
	/* Update thermostat status */
	switch (mode) {
	case OFF:
		node_prop_boolean_send(node, heating, false);
		node_prop_boolean_send(node, cooling, false);
		break;

	case COOLING:
		node_prop_boolean_send(node, heating, false);
		node_prop_boolean_send(node, cooling, true);
		break;
		break;
	case HEATING:
		node_prop_boolean_send(node, heating, true);
		node_prop_boolean_send(node, cooling, false);
		break;
	}
}

/*
 * Update the state of properties used by the simulated nodes for
 * battery status.
 */
static void sim_update_battery(const struct sim_state *sim,
	struct sim_node_state *sim_node)
{
	struct node *node = sim_node->node;
	double charge_step = SIM_BATTERY_CHARGE_RATE /
	    (3600000.0 / sim_node->sample_ms);
	int charge_val;
	struct node_prop *charge;

	charge = node_prop_lookup(node, NULL,
	    SIM_NODE_TEMPLATE_SIM, "battery_charge");
	if (!charge) {
		/* No battery support */
		return;
	}
	charge_val = node_prop_integer_val(charge);
	/* Charge or discharge battery based on node state */
	if (node->power == GP_BATTERY) {
		if (charge_val) {
			charge_val -= charge_step;
			if (charge_val <= 0) {
				charge_val = 0;
				log_app("%s: battery depleted, "
				    "going offline", node->addr);
				node_conn_status_changed(node, false);
			}
		}
	} else if (charge_val < 100) {
		charge_val += charge_step;
		if (charge_val > 100) {
			charge_val = 100;
		}
	}
	node_prop_integer_send(node, charge, charge_val);
}


/*****************************************
 * Simulator control functions
 *****************************************/

/*
 * Match function to search for nodes by type.  Ignores nodes that have been
 * flagged for removal from the simulation.
 */
static bool sim_node_search_by_type(const struct sim_node_state *sim_node,
	const void *arg)
{
	enum sim_node_type type;

	ASSERT(arg != NULL);
	type = *(enum sim_node_type *)arg;
	return !sim_node->pending_removal && sim_node->type == type;
}

/*
 * Update ambient conditions for simulation based on system time.
 */
static void sim_update_simulation(struct sim_state *sim)
{
	time_t t = time(NULL);
	struct tm *cal = localtime(&t);

	sim_update_ambient(sim, cal);
}

/*
 * Handler for simulator step timer.  Updates global simulation state.
 */
static void sim_step_timeout(struct timer *timer)
{
	struct sim_state *sim = CONTAINER_OF(struct sim_state, step_timer,
	    timer);

	sim_update_simulation(sim);

	timer_set(app_get_timers(), &state.step_timer, SIM_STEP_TIME_MS);
}

/*
 * Handler for simulated node sample timer.  Updates node state.
 */
static void sim_node_sample_timeout(struct timer *timer)
{
	struct sim_node_state *node_state = CONTAINER_OF(struct sim_node_state,
	    sample_timer, timer);

	log_debug("%s: sampling data", node_state->node->addr);

	/*
	 * Update node state based on simulation state.
	 * Send all outgoing datapoints for this node in a single batch.
	 */
	node_prop_batch_begin(node_state->node);
	sim_update_outdoor(&state, node_state);
	sim_update_indoor(&state, node_state);
	sim_update_tstat(&state, node_state->node);
	sim_update_battery(&state, node_state);
	node_prop_batch_end(node_state->node);

	timer_set(app_get_timers(), &node_state->sample_timer,
	    node_state->sample_ms);
}

/*
 * Initialize the node simulator.
 */
void sim_init(struct timer_head *timers)
{
	struct node_network_callbacks callbacks = {
		.node_query_info = sim_node_query_info_handler,
		.node_configure = sim_node_configure_handler,
		.node_prop_set = sim_node_prop_set_handler,
		.node_factory_reset = NULL,
		.node_leave = sim_node_leave_handler,
		.node_ota_update = sim_node_ota_handler,
		.node_conf_save = sim_node_save,
		.node_conf_loaded = sim_node_loaded
	};
	srandom(time(NULL));	/* Seed random number generator */
	state.timers = timers;
	timer_init(&state.step_timer, sim_step_timeout);
	node_set_network_callbacks(&callbacks);
	sim_update_simulation(&state);	/* Initialize ambient values */
}

/*
 * Start the node simulator.  This begins to periodically update
 * simulated environmental conditions to use for generating node datapoints.
 */
void sim_start(void)
{
	log_app("starting node simulator");
	timer_set(app_get_timers(), &state.step_timer, 0);
}

/*
 * Stop the node simulator.
 */
void sim_stop(void)
{
	log_app("stopping node simulator");
	timer_cancel(app_get_timers(), &state.step_timer);
}

/*
 * Simulate a node joining the network.  Sample_secs is optional and the
 * default value will be used if it is set to 0.
 * Return 0 on success or -1 on failure.
 */
int sim_node_add(enum sim_node_type type, unsigned sample_secs)
{
	struct node *node;
	char addr[GW_NODE_ADDR_SIZE];
	char oem_model[GW_MAX_OEM_MODEL_SIZE];

	/* Assign a unique address */
	++state.node_index;
	snprintf(addr, sizeof(addr), "%s_sim_%02X", sim_node_type_names[type],
	    state.node_index);
	/* Node OEM model.  Helps mobile app present custom UI for nodes. */
	snprintf(oem_model, sizeof(oem_model), SIM_NODE_OEM_MODEL_PREFIX "%s",
	    sim_node_type_names[type]);
	/* Join node */
	node = node_joined(addr, oem_model, GI_ZIGBEE, GP_MAINS, NULL);
	if (!node) {
		return -1;
	}
	/* Initialize simulated node node_state */
	if (!sim_node_start(node, type, sample_secs * 1000)) {
		log_err("%s: failed to initialize simulator", node->addr);
		node_left(node);
		return -1;
	}
	log_app("%s: joined", node->addr);
	return 0;
}

/*
 * Simulate the specified node leaving the network.
 * Return 0 on success or -1 on failure.
 */
int sim_node_remove(enum sim_node_type type)
{
	struct sim_node_search_query query = {
		.match = sim_node_search_by_type,
		.arg = &type,
		.result = NULL };

	/* Search for node to remove by type */
	if (node_foreach(sim_node_search_handler, &query) < 0 ||
	    !query.result) {
		log_warn("no %s node to remove", sim_node_type_names[type]);
		return -1;
	}
	log_app("%s: leaving", query.result->node->addr);
	query.result->pending_removal = true;
	node_left(query.result->node);
	return 0;
}
