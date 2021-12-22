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
#include <netinet/ether.h>

#include <jansson.h>
#include <dbus/dbus.h>

#include <ayla/utypes.h>
#include <ayla/token_table.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/nameval.h>
#include <ayla/file_event.h>
#include <ayla/timer.h>
#include <ayla/hashmap.h>
#include <ayla/json_parser.h>
#include <ayla/hex.h>
#include <ayla/ayla_interface.h>
#include <ayla/ops.h>
#include <ayla/props.h>
#include <ayla/gateway_interface.h>
#include <ayla/gateway.h>

#include "gateway.h"
#include "node.h"
#include "dbus_utils.h"
#include "dbus_client.h"
#include "bt_utils.h"
#include "bt_gatt.h"
#include "bt_interface.h"

/* Define macro to print verbose debug on D-Bus object management */
/* #define BT_DEBUG_DBUS_OBJS */

/* Fixed subdevice names */
#define BT_SUBDEVICE_DEVICE		"dev"	/* General device info props */

/* Template with mandatory Bluetooth device info for all nodes */
#define BT_TEMPLATE_DEVICE		"generic"

/* Poll period for routine actions */
#define BT_MONITOR_POLL_PERIOD_MS	60000

/* Timeout for Bluetooth pair D-Bus message (longer than default) */
#define BT_DBUS_MSG_TIMEOUT_CONNECT	60000

/* Name of BlueZ service on D-Bus interface */
#define BT_DBUS_SERVICE_BLUEZ		"org.bluez"

/* D-Bus path registered for the gateway Bluetooth agent */
#define BT_DBUS_PATH_AGENT	"/com/aylanetworks/bluetooth/agent1"

#define BT_SCAN_UUID_MAX	20

/*
 * Supported D-Bus interfaces
 */
#define BT_DBUS_INTERFACES(def)						\
	def(,					DBUS_IFACE_UNKNOWN)	\
	def(org.freedesktop.DBus.ObjectManager,	DBUS_IFACE_OBJ_MANAGER) \
	def(org.freedesktop.DBus.Properties,	DBUS_IFACE_PROPS)	\
	def(org.bluez.Adapter1,			DBUS_IFACE_ADAPTER)	\
	def(org.bluez.Device1,			DBUS_IFACE_DEVICE)	\
	def(org.bluez.GattService1,		DBUS_IFACE_GATT_SERVICE) \
	def(org.bluez.GattCharacteristic1,	DBUS_IFACE_GATT_CHAR)	\
	def(org.bluez.AgentManager1,		DBUS_IFACE_AGENT_MANAGER) \
	def(org.bluez.Agent1,			DBUS_IFACE_AGENT)

DEF_ENUM(bt_dbus_interface, BT_DBUS_INTERFACES);
static DEF_NAME_TABLE(bt_dbus_interface_strs, BT_DBUS_INTERFACES);
static DEF_NAMEVAL_TABLE(bt_dbus_interface_table, BT_DBUS_INTERFACES);

/* D-Bus object event types.  These are defined internally for convenience. */
enum bt_dbus_event {
	BT_DBUS_UPDATE,
	BT_DBUS_REMOVE
};

/* GATT characteristic value change notification state */
enum bt_notify_state {
	BT_NOTIFY_UNSUPPORTED,
	BT_NOTIFY_DISABLED,
	BT_NOTIFY_ENABLED
};

/* GATT value read/write state */
enum bt_gatt_val_state {
	BT_GATT_VAL_READY,
	BT_GATT_VAL_WRITE,
	BT_GATT_VAL_READ
};

/*
 * Object type exposed over D-Bus.  The path is a unique string, and each
 * object has one or more interfaces.  Multiple interfaces are analogous to
 * multiple inheritance in object-oriented design.  Generally, only the primary
 * interface is used in this code, but multiple interface support was added to
 * conform to the D-Bus object model.
 */
struct bt_dbus_iface;
struct bt_dbus_obj {
	TAILQ_HEAD(, bt_dbus_iface) iface_list;
	struct dbus_client_msg_handler *prop_changed_handler;
	char path[0];
};

/*
 * D-Bus object interface.  Interface behavior is defined using the included
 * callbacks, and state may be associated with the interface using the data
 * pointer.
 */
struct bt_dbus_iface {
	enum bt_dbus_interface type;
	void (*added)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *);
	void (*removed)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *);
	void (*props_changed)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *, const DBusMessageIter *);
	void (*props_invalidated)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *, const DBusMessageIter *);
	void (*free_data)(void *);
	void *data;
	TAILQ_ENTRY(bt_dbus_iface) entry;
};

/*
 * State to be associated with each Bluetooth Adapter exposed by the BlueZ
 * D-Bus interface.
 */
struct bt_adapter {
	bool initialized;

	bool powered;
	bool discoverable;
	bool pairable;
	bool discovering;
};

/*
 * State to be associated with each Bluetooth Device exposed by the BlueZ
 * D-Bus interface.
 */
struct bt_device {
	struct node *node;
	char *addr;
	char *name;
	char *alias;
	s16 rssi;
	bool paired;
	bool connected;
	bool services_resolved;
	bool legacy_pairing;
	char *uuids[BT_SCAN_UUID_MAX];
	struct bt_dbus_obj *adapter_obj;

	enum {
		BT_PAIRING_UNKNOWN,
		BT_PAIRING_SUPPORTED,
		BT_PAIRING_UNSUPPORTED
	} pairing_support;

	void (*query_complete)(struct node *, enum node_network_result);
	void (*config_complete)(struct node *, enum node_network_result);
	void (*leave_complete)(struct node *, enum node_network_result);
	bool conn_status_syncd;
};

/*
 * State to be associated with each GATT service exposed by the BlueZ
 * D-Bus interface.
 */
struct bt_gatt_service {
	struct bt_uuid uuid;
	struct bt_dbus_obj *device_obj;
	const struct bt_gatt_db_template *template_def;
};

/*
 * State to be associated with each GATT characteristic exposed by the BlueZ
 * D-Bus interface.
 */
struct bt_gatt_char {
	struct bt_uuid uuid;
	struct bt_dbus_obj *gatt_service_obj;
	enum bt_notify_state notify;
	u32 flags;
	const struct bt_gatt_db_prop_list *prop_defs;
	struct bt_gatt_val val;
	enum bt_gatt_val_state state;
	bool pending_prop_add;
	bool pending_write;
	bool pending_read;
};

/*
 * State associated with each Ayla node property that links it to the D-Bus
 * object used to represent it.
 */
struct bt_prop_state {
	struct bt_dbus_obj *obj;
	enum bt_dbus_interface type;
	void *arg;
};

/*
 * Bluetooth interface state
 */
struct bt_state {
	struct file_event_table *file_events;
	struct timer_head *timers;
	struct bt_callbacks callbacks;

	struct timer monitor_timer;

	bool discovery_enabled;
	struct {
		enum {
			BT_CONNECT_READY,
			BT_CONNECT_IN_PROG,
			BT_CONNECT_AUTH_REQ,
			BT_CONNECT_AUTH_DISPLAY
		} status;
		struct bt_dbus_obj *obj;
		void (*complete)(const char *, enum node_network_result,
		    enum bt_dev_op_err_code, void *);
		void *complete_arg;
	} connect;

	struct hashmap dbus_objects;
	char *service_bus_name;
	struct dbus_client_msg_handler *added_handler;
	struct dbus_client_msg_handler *removed_handler;
};

static struct bt_state state;

/* Declare type-specific hashmap functions for D-Bus objects */
HASHMAP_FUNCS_CREATE(bt_dbus_obj, const char, struct bt_dbus_obj)


/*
 * Forward declarations
 */
static int bt_device_pair(struct bt_dbus_obj *obj, struct bt_device *device);
static void bt_gatt_char_props_add(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic);
static int bt_gatt_char_write(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic);
static int bt_gatt_char_read(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic);
static void bt_dbus_iface_update(const char *, const char *,
	const DBusMessageIter *, const DBusMessageIter *, bool);
static void bt_dbus_obj_prop_signal_handler(DBusMessage *, void *);
static void bt_node_connect_complete(const char *addr,
	enum node_network_result result);

/*****************************************
 * Standard node template definitions
 *****************************************/

/*
 * Static template definition for basic Bluetooth device info.  Note: Ayla
 * property names match the BlueZ D-Bus Device object property names.
 */
static const struct node_prop_def const bt_template_device[] = {
	{ "Name",		PROP_STRING,	PROP_FROM_DEVICE },
	{ "Icon",		PROP_STRING,	PROP_FROM_DEVICE },
	{ "Class",		PROP_INTEGER,	PROP_FROM_DEVICE },
	{ "Appearance",		PROP_INTEGER,	PROP_FROM_DEVICE },
	{ "Alias",		PROP_STRING,	PROP_TO_DEVICE }
};


static void (*disconnect_cb)(const char *, enum node_network_result, void *);
static void *disconnect_arg;
static char disconnect_addr[GW_NODE_ADDR_SIZE];


/*****************************************
 * Utility functions
 *****************************************/

/*
 * Associate a node template definition table with a node.  Used by the
 * simulator to setup a node supporting the desired characteristics.
 */
static void bt_template_add(struct node *node,
	const char *subdevice, const char *template, const char *version,
	const struct node_prop_def *table, size_t table_size)
{
	for (; table_size; --table_size, ++table) {
		node_prop_add(node, subdevice, template, table, version);
	}
}

/*
 * Lookup up a D-Bus object's interface state by type.
 */
static struct bt_dbus_iface *bt_dbus_obj_get_iface(
	const struct bt_dbus_obj *obj, enum bt_dbus_interface type)
{
	struct bt_dbus_iface *iface;

	/* DVLX-357: Check NULL point */
	if (!obj) {
		return NULL;
	}

	TAILQ_FOREACH(iface, &obj->iface_list, entry) {
		if (iface->type == type) {
			return iface;
		}
	}
	return NULL;
}

/*
 * Return the Bluetooth Adapter state, if available.
 */
static struct bt_adapter *bt_adapter_get(struct bt_dbus_obj *obj)
{
	struct bt_dbus_iface *iface;

	iface = bt_dbus_obj_get_iface(obj, DBUS_IFACE_ADAPTER);
	if (!iface) {
		return NULL;
	}
	return (struct bt_adapter *)iface->data;
}

/*
 * Return the Bluetooth Device state, if available.
 */
static struct bt_device *bt_device_get(struct bt_dbus_obj *obj)
{
	struct bt_dbus_iface *iface;

	iface = bt_dbus_obj_get_iface(obj, DBUS_IFACE_DEVICE);
	if (!iface) {
		return NULL;
	}
	return (struct bt_device *)iface->data;
}

/*
 * Return the Bluetooth GATT service state, if available.
 */
static struct bt_gatt_service *bt_gatt_service_get(struct bt_dbus_obj *obj)
{
	struct bt_dbus_iface *iface;

	iface = bt_dbus_obj_get_iface(obj, DBUS_IFACE_GATT_SERVICE);
	if (!iface) {
		return NULL;
	}
	return (struct bt_gatt_service *)iface->data;
}

/*
 * Return the Bluetooth GATT characteristic state, if available.
 */
static struct bt_gatt_char *bt_gatt_char_get(struct bt_dbus_obj *obj)
{
	struct bt_dbus_iface *iface;

	iface = bt_dbus_obj_get_iface(obj, DBUS_IFACE_GATT_CHAR);
	if (!iface) {
		return NULL;
	}
	return (struct bt_gatt_char *)iface->data;
}

/*
 * Clear references to an Ayla gateway node from a D-Bus device object
 * when the node is deleted.
 */
static void bt_node_state_cleanup(void *ptr)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)ptr;
	struct bt_device *device;

	if (!obj) {
		return;
	}
	log_debug("obj->path %s", obj->path);
	device = bt_device_get(obj);
	if (device) {
		device->node = NULL;
	}
}

/*
 * Return a reference to the D-Bus device object associated with an Ayla node.
 */
static struct bt_dbus_obj *bt_node_get_obj(struct node *node)
{
	return (struct bt_dbus_obj *)node_state_get(node, STATE_SLOT_NET);
}

/*
 * Add a reference to a D-Bus device object to the associated Ayla node.
 * This facilitates sending messages to the device on node change events.
 */
static void bt_node_set_obj(struct node *node, struct bt_dbus_obj *obj)
{
	log_debug("node addr %s, obj %p",
	    node ? node->addr : "NULL", obj);
	node_state_set(node, STATE_SLOT_NET, obj,
	    obj ? bt_node_state_cleanup : NULL);
}

/*
 * Return a reference to the D-Bus device object and interface associated with
 * an Ayla node property.
 */
static struct bt_prop_state *bt_node_prop_state_get(struct node_prop *prop)
{
	return (struct bt_prop_state *)node_prop_state_get(prop,
	    STATE_SLOT_NET);
}

/*
 * Add or update a reference to the D-Bus device object and interface to the
 * associated Ayla node property. This facilitates sending messages to the
 * object on property sets.
 */
static void bt_node_prop_state_set(struct node_prop *prop,
	struct bt_dbus_obj *obj, enum bt_dbus_interface type, void *arg)
{
	struct bt_prop_state *state;

	if (obj) {
		state = bt_node_prop_state_get(prop);
		if (!state) {
			state = (struct bt_prop_state *)malloc(sizeof(*state));
			if (!state) {
				log_err("malloc failed");
				return;
			}
			node_prop_state_set(prop, STATE_SLOT_NET, state, free);
		}
		state->obj = obj;
		state->type = type;
		state->arg = arg;
	} else {
		node_prop_state_set(prop, STATE_SLOT_NET, NULL, NULL);
	}
}

/*
 * Send a D-Bus property value as an Ayla property.
 */
static int bt_dbus_prop_send(struct node *node, struct node_prop *prop,
	DBusMessageIter *val_iter)
{
	u64 uint_val;
	s64 int_val;
	const char *str_val;
	bool bool_val;
	double double_val;

	switch (prop->type) {
	case PROP_INTEGER:
		switch (dbus_message_iter_get_arg_type(val_iter)) {
		case DBUS_TYPE_INT16:
		case DBUS_TYPE_INT32:
		case DBUS_TYPE_INT64:
			if (dbus_utils_parse_int64(val_iter, &int_val) < 0) {
				goto error;
			}
			break;
		default:
			/* Convert unsigned to integer, if needed */
			if (dbus_utils_parse_uint64(val_iter, &uint_val) < 0) {
				goto error;
			}
			int_val = uint_val;
			break;
		}
		if (int_val < INT32_MIN || int_val > INT32_MAX) {
			log_warn("integer value for %s:%s:%s is out of "
			    "supported 32-bit range", prop->subdevice->key,
			    prop->template->key, prop->name);
			goto error;
		}
		return node_prop_integer_send(node, prop, (int)int_val);
	case PROP_STRING:
		str_val = dbus_utils_parse_string(val_iter);
		if (!str_val) {
			goto error;
		}
		return node_prop_string_send(node, prop, str_val);
	case PROP_BOOLEAN:
		if (dbus_utils_parse_bool(val_iter, &bool_val) < 0) {
			goto error;
		}
		return node_prop_boolean_send(node, prop, bool_val);
	case PROP_DECIMAL:
		if (dbus_utils_parse_double(val_iter, &double_val) < 0) {
			goto error;
		}
		return node_prop_decimal_send(node, prop, double_val);
	default:
		log_err("property type not supported: %s:%s:%s",
		    prop->subdevice->key, prop->template->key, prop->name);
		return -1;
	}
error:
	log_err("failed to parse D-Bus value for %s:%s:%s: %s",
	    prop->subdevice->key, prop->template->key, prop->name,
	    dbus_utils_val_str(val_iter));
	return -1;
}

/*****************************************
 * Bluetooth node monitor routines (runs periodically)
 *****************************************/

/*
 * Perform routine maintenance actions for Bluetooth devices.
 */
static void bt_monitor_device_action(struct bt_dbus_obj *obj,
	struct bt_device *device)
{
	DBusMessage *msg;

	/* Attempt to re-pair or reconnect disconnected nodes */
	if (device->connected || (!device->node && device->pairing_support !=
	    BT_PAIRING_UNSUPPORTED)) {
		return;
	}
	/* Attempt to reconnect disconnected nodes */
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    obj->path, bt_dbus_interface_strs[DBUS_IFACE_DEVICE], "Connect");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}
	if (dbus_client_send(msg) < 0) {
		log_err("D-Bus send failed for %s", device->addr);
	} else {
		log_debug("sent Connect to %s", device->addr);
	}
	dbus_message_unref(msg);
}

/*
 * Perform routine maintenance actions for GATT characteristics.
 */
static void bt_monitor_gatt_char_action(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic)
{
	if (characteristic->pending_prop_add) {
		/*
		 * Attempt to add properties for a known characteristic that
		 * was added before the node that owned it was joined.
		 */
		bt_gatt_char_props_add(obj, characteristic);
	}
	if (characteristic->state == BT_GATT_VAL_READY) {
		/* TODO determine if concurrent read + write is possible */
		if (characteristic->pending_write) {
			/*
			 * Attempt to re-send a characteristic value if it
			 * appears to have failed.
			 */
			bt_gatt_char_write(obj, characteristic);
		} else if (characteristic->pending_read) {
			/*
			 * Attempt to re-request a characteristic value if it
			 * appears to have failed.
			 */
			bt_gatt_char_read(obj, characteristic);
		}
	}
}

static void bt_monitor_timeout(struct timer *timer)
{
	struct bt_dbus_obj *obj;
	struct bt_dbus_iface *iface;
	struct hashmap_iter *iter;

	/* log_debug("running periodic device check..."); */

	/* Perform routine maintenance actions on each tracked object */
	for (iter = hashmap_iter(&state.dbus_objects); iter;
	    iter = hashmap_iter_next(&state.dbus_objects, iter)) {
		obj = bt_dbus_obj_hashmap_iter_get_data(iter);
		if (!obj) {
			continue;
		}
		TAILQ_FOREACH(iface, &obj->iface_list, entry) {
			if (iface->data) {
				switch (iface->type) {
				case DBUS_IFACE_DEVICE:
					bt_monitor_device_action(obj,
					    (struct bt_device *)iface->data);
					break;
				case DBUS_IFACE_GATT_CHAR:
					bt_monitor_gatt_char_action(obj,
					    (struct bt_gatt_char *)iface->data);
					break;
				default:
					break;
				}
			}
		}
	}
	timer_set(state.timers, &state.monitor_timer,
	    BT_MONITOR_POLL_PERIOD_MS);
}

/*
 * Run monitor routine immediately for all objects.  This should be called
 * if there is a state change that potentially affects multiple monitored
 * objects, and it is not ideal to wait for the next monitor timeout.
 */
static void bt_monitor_run(void)
{
	timer_set(state.timers, &state.monitor_timer, 0);
}

/*****************************************
 * Bluetooth node management and prop handling
 *****************************************/

static void bt_query_info_reply_handler(DBusMessage *msg, void *arg,
	const char *err)
{
	struct node *node = (struct node *)arg;
	struct bt_dbus_obj *obj = bt_node_get_obj(node);
	struct bt_device *device;
	DBusMessageIter iter;
	const char *interface = bt_dbus_interface_strs[DBUS_IFACE_DEVICE];
	enum node_network_result result = NETWORK_SUCCESS;

	log_debug("node->addr %s", node ? node->addr : "NULL");

	if (!obj) {
		log_debug("%s: missing D-Bus object", node->addr);
		return;
	}
	device = bt_device_get(obj);
	if (!device) {
		log_err("%s: missing device state", node->addr);
		return;
	}
	if (!msg) {
		result = NETWORK_UNKNOWN;
		goto callback;
	}
	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		log_err("%s: invalid props array", obj->path);
		return;
	}
	/* Enter properties array */
	dbus_message_iter_recurse(&iter, &iter);
	log_debug("obj path %s, interface %s, allow to add",
	    obj->path, interface);
	bt_dbus_iface_update(obj->path, interface, &iter, NULL, true);

	/* Run the monitor task immediately to apply any updates */
	bt_monitor_run();
callback:
	if (device->query_complete) {
		/* Complete node query process */
		if (device->node) {
			device->query_complete(device->node, result);
		}
		device->query_complete = NULL;
	}
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_query_info_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	struct bt_dbus_obj *obj = bt_node_get_obj(node);
	struct bt_device *device;
	DBusMessage *msg;
	int rc;

	log_debug("node->addr %s", node ? node->addr : "NULL");

	if (!obj) {
		log_err("%s: missing D-Bus object", node->addr);
		return -1;
	}
	device = bt_device_get(obj);
	if (!device) {
		log_err("%s: missing device state", node->addr);
		return -1;
	}
	/* Add basic device info template supported by all Bluetooth devices */
	bt_template_add(node, BT_SUBDEVICE_DEVICE, BT_TEMPLATE_DEVICE, NULL,
	    bt_template_device, ARRAY_LEN(bt_template_device));

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_PROPS], "GetAll");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	dbus_message_append_args(msg, DBUS_TYPE_STRING,
	    &bt_dbus_interface_strs[DBUS_IFACE_DEVICE], DBUS_TYPE_INVALID);
	rc = dbus_client_send_with_reply(msg, bt_query_info_reply_handler,
	    node, DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	} else {
		log_info("%s: querying node info", node->addr);
		device->query_complete = callback;
	}
	return rc;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_configure_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	struct bt_dbus_obj *obj = bt_node_get_obj(node);
	struct bt_device *device;
	DBusMessage *msg;
	bool bool_val;
	int rc;

	log_debug("node->addr %s", node ? node->addr : "NULL");

	if (!obj) {
		log_err("%s: missing D-Bus object", node->addr);
		return -1;
	}
	device = bt_device_get(obj);
	if (!device) {
		log_err("%s: missing device state", node->addr);
		return -1;
	}
	/* Set device as trusted */
	bool_val = true;
	msg = bt_utils_create_msg_prop_set(BT_DBUS_SERVICE_BLUEZ,
	    obj->path, bt_dbus_interface_strs[DBUS_IFACE_DEVICE],
	    "Trusted", DBUS_TYPE_BOOLEAN, &bool_val);
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	rc = dbus_client_send(msg);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	} else {
		log_info("%s: configuring node", node->addr);
		if (callback) {
			callback(node, NETWORK_SUCCESS);
		}
	}
	return rc;
}

/*
 * Node property set handler for properties that are mapped to a GATT
 * characteristic.  Note that more than one Ayla property may be mapped to
 * a single characteristic.
 */
static int bt_prop_set_gatt_char(struct bt_prop_state *prop_state,
	struct node *node, struct node_prop *prop,
	void (*callback)(struct node *, struct node_prop *,
	enum node_network_result))
{
	const struct bt_gatt_db_prop *prop_def =
	    (const struct bt_gatt_db_prop *)prop_state->arg;
	struct bt_gatt_char *characteristic;
	int rc;

	if (!node->online) {
		if (callback) {
			callback(node, prop, NETWORK_OFFLINE);
			return 0;
		}
		return -1;
	}
	characteristic = bt_gatt_char_get(prop_state->obj);
	if (!characteristic) {
		log_err("%s: missing GATT characteristic state for "
		    "%s:%s:%s", node->addr, prop->subdevice->key,
		    prop->template->key, prop->name);
		return -1;
	}
	if (!(characteristic->flags & BIT(BT_GATT_WRITE))) {
		log_warn("%s: characteristic %s is not writable",
		    node->addr, bt_uuid_string(&characteristic->uuid));
		return -1;
	}
	if (!prop_def->val_set) {
		log_warn("%s: no val_set handler for characteristic %s",
		    node->addr, bt_uuid_string(&characteristic->uuid));
		return -1;
	}
	rc = prop_def->val_set(node, prop, &characteristic->val);
	if (rc < 0) {
		log_warn("%s: failed to set value for characteristic %s",
		    node->addr, bt_uuid_string(&characteristic->uuid));
		return -1;
	}
	if (rc > 0) {
		log_debug("%s: no value change for characteristic %s",
		    node->addr, bt_uuid_string(&characteristic->uuid));
		return 0;
	}
	/*
	 * TODO: defer write until main loop to combine multiple val_set calls.
	 */
	rc = bt_gatt_char_write(prop_state->obj, characteristic);
	if (!rc) {
		log_info("%s: property %s value %s set GATT characteristic %s",
		    node->addr, prop->name,
		    prop_val_to_str(prop->val, prop->type),
		    bt_uuid_string(&characteristic->uuid));
		if (callback) {
			callback(node, prop, NETWORK_SUCCESS);
		}
	}
	return rc;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_prop_set_handler(struct node *node, struct node_prop *prop,
    void (*callback)(struct node *, struct node_prop *,
    enum node_network_result))
{
	struct bt_prop_state *prop_state = bt_node_prop_state_get(prop);

	DBusMessage *msg;
	int dbus_type;
	int rc;

	ASSERT(node != NULL);
	ASSERT(prop != NULL);

	log_debug("node->addr %s, prop->name %s, value %s",
	    node->addr, prop->name, prop_val_to_str(prop->val, prop->type));

	if (!prop_state) {
		/*
		 * This is not necessarily an error.  Props may be set prior to
		 * receiving the list of managed D-Bus objects.  The prop will
		 * be re-sent after the info is known and the device is online.
		 */
		log_debug("%s: missing prop to D-Bus object mapping for "
		    "%s:%s:%s", node->addr, prop->subdevice->key,
		    prop->template->key, prop->name);
		if (callback) {
			callback(node, prop, NETWORK_OFFLINE);
			return 0;
		}
		return -1;
	}
	/*
	 * Use special set handler for writing GATT characteristics.
	 * Properties linked to other types of D-Bus objects just write the
	 * D-Bus object properties directly.
	 */
	if (prop_state->type == DBUS_IFACE_GATT_CHAR) {
		rc = bt_prop_set_gatt_char(prop_state, node, prop, callback);
		/* Set magic blue bulb mode prop if onoff prop set to 1 */
		if ((!strcmp("MagicBlue", node->oem_model))
		    && (!strcmp("onoff", prop->name))
		    && (1 == *(char *)prop->val)) {
			struct node_prop *prop_mode;
			prop_mode = node_prop_lookup(node,
			    prop->subdevice->key, prop->template->key, "mode");
			if (prop_mode) {
				log_debug("node %s set mode prop", node->addr);
				struct bt_prop_state *mode_state
				    = bt_node_prop_state_get(prop_mode);
				bt_prop_set_gatt_char(mode_state,
				    node, prop_mode, NULL);
			}
		}
		return rc;
	}

	switch (prop->type) {
	case PROP_INTEGER:
		/* TODO support more D-Bus integer types */
		dbus_type = DBUS_TYPE_INT32;
		break;
	case PROP_STRING:
		dbus_type = DBUS_TYPE_STRING;
		break;
	case PROP_BOOLEAN:
		dbus_type = DBUS_TYPE_BOOLEAN;
		break;
	case PROP_DECIMAL:
		dbus_type = DBUS_TYPE_DOUBLE;
		break;
	default:
		log_err("property type not supported: %s:%s:%s",
		    prop->subdevice->key, prop->template->key, prop->name);
		return -1;
	}
	msg = bt_utils_create_msg_prop_set(BT_DBUS_SERVICE_BLUEZ,
	    prop_state->obj->path, bt_dbus_interface_strs[prop_state->type],
	    prop->name, dbus_type, prop->val);
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	rc = dbus_client_send(msg);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	} else {
		log_info("%s: property %s set to %s", node->addr, prop->name,
		    prop_val_to_str(prop->val, prop->type));
		if (callback) {
			callback(node, prop, NETWORK_SUCCESS);
		}
	}
	return rc;
}

static void bt_leave_reply_handler(DBusMessage *msg, void *arg, const char *err)
{
	struct node *node = (struct node *)arg;
	struct bt_dbus_obj *obj = bt_node_get_obj(node);
	struct bt_device *device;

	if (!obj) {
		return;
	}
	log_debug("node addr %s, obj path %s, err %s",
	    node->addr, obj ? obj->path : "", err ? err : "");

	device = bt_device_get(obj);
	if (!device) {
		log_err("%s: missing device state", node->addr);
		return;
	}
	log_debug("device addr %s, complete func %p, device->node %p, node %p",
	    device->addr, device->leave_complete, device->node, node);
	if (device->leave_complete) {
		/* Complete node leave process */
		if (device->node) {
			device->leave_complete(device->node, NETWORK_SUCCESS);
		}
		device->leave_complete = NULL;
	}

	/* Call disconnect callback function */
	if (disconnect_cb) {
		log_debug("device addr %s", device->addr);
		disconnect_cb(device->addr, NETWORK_SUCCESS, disconnect_arg);
		disconnect_cb = NULL;
		disconnect_arg = NULL;
		memset(disconnect_addr, 0, sizeof(disconnect_addr));
	}
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_leave_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	struct bt_dbus_obj *obj = bt_node_get_obj(node);
	struct bt_device *device;
	const char *dev_path;
	DBusMessage *msg;
	int rc;

	log_info("%s: leaving network", node->addr);

	if (!obj) {
		log_err("%s: missing D-Bus object", node->addr);
		return -1;
	}
	device = bt_device_get(obj);
	if (!device) {
		log_err("%s: missing device state", node->addr);
		return -1;
	}
	/* Remove the device */
	if (!device->adapter_obj) {
		log_err("%s: missing adapter object", device->addr);
		return -1;
	}
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    device->adapter_obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_ADAPTER], "RemoveDevice");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	dev_path = obj->path;
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &dev_path,
	    DBUS_TYPE_INVALID);
	rc = dbus_client_send_with_reply(msg, bt_leave_reply_handler,
	    node, DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}
	if (!rc && callback) {
		device->leave_complete = callback;
	}
	return rc;
}

/*****************************************
 * BlueZ Adapter D-Bus interface support
 *****************************************/

/*
 * Bluetooth adapter powered on.
 */
static void bt_adapter_init_done_reply_handler(DBusMessage *reply, void *arg,
	const char *err)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;

	if (err) {
		log_warn("%s: initialization failed: %s", obj->path, err);
		return;
	}
	log_debug("%s: initialized", obj->path);
}

/*
 * Complete Bluetooth adapter power toggle.
 */
static void bt_adapter_init_reply_handler(DBusMessage *reply, void *arg,
	const char *err)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;
	DBusMessage *msg;
	bool bool_val;
	int rc;

	bool_val = true;
	msg = bt_utils_create_msg_prop_set(BT_DBUS_SERVICE_BLUEZ,
	    obj->path, bt_dbus_interface_strs[DBUS_IFACE_ADAPTER],
	    "Powered", DBUS_TYPE_BOOLEAN, &bool_val);
	if (!msg) {
		log_err("message allocation failed");
		return;
	}
	rc = dbus_client_send_with_reply(msg,
	    bt_adapter_init_done_reply_handler, obj, DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}
}

/*
 * Initialize the Bluetooth adapter by toggling power. This was needed to work
 * around some problems experienced with the BlueZ driver, including failed
 * pairing and kernel driver errors.
 */
static int bt_adapter_init(struct bt_dbus_obj *obj)
{
	DBusMessage *msg;
	bool bool_val;
	int rc;

	bool_val = false;
	msg = bt_utils_create_msg_prop_set(BT_DBUS_SERVICE_BLUEZ,
	    obj->path, bt_dbus_interface_strs[DBUS_IFACE_ADAPTER],
	    "Powered", DBUS_TYPE_BOOLEAN, &bool_val);
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	rc = dbus_client_send_with_reply(msg, bt_adapter_init_reply_handler,
	    obj, DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	} else {
		log_debug("%s: initializing adapter", obj->path);
	}
	return rc;
}

static struct bt_adapter *bt_adapter_alloc(void)
{
	struct bt_adapter *adapter;

	adapter = calloc(1, sizeof(*adapter));
	if (!adapter) {
		log_err("malloc failed");
		return NULL;
	}
	return adapter;
}

static void bt_adapter_props_changed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data,
	const DBusMessageIter *props_iter)
{
	struct bt_adapter *adapter = (struct bt_adapter *)data;
	DBusMessageIter iter;
	DBusMessageIter val_iter;
	const char *name;

	ASSERT(type == DBUS_IFACE_ADAPTER);

	for (iter = *props_iter; dbus_message_iter_get_arg_type(&iter) ==
	    DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&iter)) {
		name = dbus_utils_parse_dict(&iter, &val_iter);
		if (!name) {
			continue;
		}
		if (!strcmp(name, "Powered")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &adapter->powered) < 0) {
				goto invalid_val;
			}
		} else if (!strcmp(name, "Discoverable")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &adapter->discoverable) < 0) {
				goto invalid_val;
			}
		} else if (!strcmp(name, "Pairable")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &adapter->pairable) < 0) {
				goto invalid_val;
			}
		} else if (!strcmp(name, "Discovering")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &adapter->discovering) < 0) {
				goto invalid_val;
			}
		}
		continue;
invalid_val:
		log_err("%s: invalid %s value", obj->path, name);
	}
	/* Perform initial Bluetooth adapter setup */
	if (!adapter->initialized) {
		if (!bt_adapter_init(obj)) {
			adapter->initialized = true;
		}
	}
}

/*****************************************
 * BlueZ Device D-Bus interface support
 *****************************************/

/*
 * Get the type for a Bluetooth device.
 */
static char *bt_device_get_type(struct bt_device *device)
{
	struct uuid_model {
		char *uuid;
		char *type;
	};
	struct uuid_model uuids1[] = {
		{ "2899fe00-c277-48a8-91cb-b29ab0f01ac4", "Grillright" },
		{ "0000fe28-0000-1000-8000-00805f9b34fb", "AylaPowered" },
		/* { "28e7b565-0215-46d7-a924-b8e7c48eab9b", "Thermostat" },
		{ "0000180d-0000-1000-8000-00805f9b34fb", "Heart" },
		{ "0000180f-0000-1000-8000-00805f9b34fb", "Battery" } */
	};
	struct uuid_model uuids2[] = {
		{ "0000fff0-0000-1000-8000-00805f9b34fb", "MagicBlue" },
		{ "0000ffe5-0000-1000-8000-00805f9b34fb", "MagicBlue" },
		{ "0000ffe0-0000-1000-8000-00805f9b34fb", "MagicBlue" },
	};
	int i, j, match;

	if (!device) {
		log_warn("missing device state");
		return NULL;
	}
	if (!device->uuids[0]) {
		log_warn("device %s missing service UUIDs info", device->addr);
		return false;
	}

	for (i = 0; ((i < BT_SCAN_UUID_MAX) && device->uuids[i]); i++) {
		for (j = 0; j < (sizeof(uuids1) / sizeof(uuids1[0])); j++) {
			if (!strcmp(uuids1[j].uuid, device->uuids[i])) {
				log_debug("find type %s for device %s",
				    uuids1[j].type, device->addr);
				return uuids1[j].type;
			}
		}
	}

	match = 0;
	for (i = 0; i < (sizeof(uuids2) / sizeof(uuids2[0])); i++) {
		for (j = 0; (j < BT_SCAN_UUID_MAX) && device->uuids[j]; j++) {
			if (!strcmp(uuids2[i].uuid, device->uuids[j])) {
				match++;
				break;
			}
		}
	}

	if (match == (sizeof(uuids2) / sizeof(uuids2[0]))) {
		log_debug("find type %s for device %s",
		    uuids2[0].type, device->addr);
		return uuids2[0].type;
	}

	log_debug("cannot find type for device %s", device->addr);
	return NULL;
}

/*
 * Invoke callback for an update to a Bluetooth device during discovery.
 */
static void bt_device_process_scan_result(struct bt_device *device)
{
	struct bt_scan_result result = {
		.addr = device->addr,
		.name = device->alias ? device->alias : device->name,
		.rssi = device->rssi,
		.type = ""
	};
	char *type;

	if (!result.addr) {
		/* Address must be populated */
		return;
	}
	if (device->legacy_pairing) {
		/* Ignore devices that only support pre-2.1 pairing */
		return;
	}

	type = bt_device_get_type(device);
	if (!type) {
		log_debug("Gateway doesn't support device %s currently",
		    device->addr);
		return;
	} else {
		result.type = type;
	}

	if (state.discovery_enabled && state.callbacks.scan_update) {
		state.callbacks.scan_update(&result);
	}
}

/*
 * Get the OEM model for a Bluetooth device.
 */
static char *bt_device_get_model(struct bt_device *device)
{
	struct uuid_2_model {
		char *uuid;
		char *model;
	};
	struct uuid_2_model uuid_model[] = {
		{ "0000ffe5-0000-1000-8000-00805f9b34fb", "MagicBlue" },
		{ "2899fe00-c277-48a8-91cb-b29ab0f01ac4", "Grillright" },
		{ "0000fe28-0000-1000-8000-00805f9b34fb", "AylaPowered" },
		/* { "28e7b565-0215-46d7-a924-b8e7c48eab9b", "Thermostat" },
		{ "0000180d-0000-1000-8000-00805f9b34fb", "Heart" },
		{ "0000180f-0000-1000-8000-00805f9b34fb", "Battery" } */
	};
	int i, j;

	if (!device) {
		log_warn("missing device state");
		return NULL;
	}
	if (!device->uuids[0]) {
		log_warn("device %s missing service UUIDs info", device->addr);
		return NULL;
	}

	for (i = 0; i < (sizeof(uuid_model) / sizeof(uuid_model[0])); i++) {
		for (j = 0; (j < BT_SCAN_UUID_MAX) && device->uuids[j]; j++) {
			if (!strcmp(uuid_model[i].uuid, device->uuids[j])) {
				log_debug("find model %s for device %s",
				    uuid_model[i].model, device->addr);
				return uuid_model[i].model;
			}
		}
	}

	log_debug("cannot find OEM model for device %s", device->addr);
	return NULL;
}

/*
 * Handle property updates for a Bluetooth device.
 */
static void bt_device_process_update(struct bt_dbus_obj *obj,
	struct bt_device *device)
{
	bool online = device->connected && device->services_resolved;
	log_debug("addr=%s, paired=%d, connected=%d, services_resolved=%d, "
	    "legacy_pairing=%d, pairing_support=%d, conn_status_syncd=%d",
	    device->addr, device->paired, device->connected,
	    device->services_resolved, device->legacy_pairing,
	    device->pairing_support, device->conn_status_syncd);

	if (device->node) {
		log_debug("online=%d, node->addr=%s, node->online=%d",
		    online, device->node->addr, device->node->online);
		if (!device->conn_status_syncd ||
		    online != device->node->online) {
			/* Send online status on first update or on change */
			node_conn_status_changed(device->node, online);
			device->conn_status_syncd = true;
		}
		if (!device->paired &&
		    device->pairing_support == BT_PAIRING_SUPPORTED) {
			node_left(device->node);
			/* Remove node that is no longer paired */
			log_debug("%s: node unpaired, set obj %s as NULL",
			    device->addr, obj->path);
			bt_node_set_obj(device->node, NULL);
		}
	} else if (device->connected && (device->paired ||
	    device->pairing_support == BT_PAIRING_UNSUPPORTED)) {
		char *model = bt_device_get_model(device);
		if (!model) {
			log_debug("%s: node not support now", device->addr);
			return;
		}
		/*
		 * Add connected node that is paired, or pairing was attempted
		 * and failed with an unsupported status.
		 */
		log_debug("%s: node paired", device->addr);
		device->node = node_joined(device->addr,
		    model, GI_BLE, GP_MAINS, NULL);
		if (!device->node) {
			log_err("node join failed: %s", device->addr);
			return;
		}
		bt_node_set_obj(device->node, obj);
		log_debug("added node %s and set obj %s",
		    device->node->addr, obj->path);
	} else {
		/* Update scan list with unpaired devices */
		bt_device_process_scan_result(device);
	}
}

static void bt_device_pair_reply_handler(DBusMessage *reply, void *arg,
	const char *err)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;
	struct bt_device *device = bt_device_get(obj);
	DBusMessage *msg;
	enum node_network_result result = NETWORK_UNSUPPORTED;
	const char *dev_path;
	int rc;

	if (!device) {
		log_warn("missing device state");
		return;
	}
	log_debug("addr=%s, name=%s, error=%s, pairing_support=%d",
	    device->addr, device->name, err ? err : "",
	    device->pairing_support);
	log_debug("paired=%d, connected=%d, services_resolved=%d, "
	    "legacy_pairing=%d, conn_status_syncd=%d",
	    device->paired, device->connected, device->services_resolved,
	    device->legacy_pairing, device->conn_status_syncd);
	if (!err || !strcmp(err, "org.bluez.Error.AlreadyConnected") ||
	    !strcmp(err, "org.bluez.Error.AlreadyExists")) {
		result = NETWORK_SUCCESS;
		device->paired = true;
		if (device->pairing_support == BT_PAIRING_UNKNOWN) {
			/* Pair succeeded */
			device->pairing_support = BT_PAIRING_SUPPORTED;
		}
		if (device->pairing_support == BT_PAIRING_SUPPORTED) {
			log_info("pairing successful: %s", device->addr);
			log_debug("path %s, method_call Connect", obj->path);
			/* Encourage device to remain connected after pairing */
			msg = dbus_message_new_method_call(
			    BT_DBUS_SERVICE_BLUEZ, obj->path,
			    bt_dbus_interface_strs[DBUS_IFACE_DEVICE],
			    "Connect");
			if (msg) {
				dbus_client_send(msg);
				dbus_message_unref(msg);
			}
		} else {
			/* Connect succeeded */
			log_info("connection successful: %s", device->addr);
		}
	} else if (device->pairing_support == BT_PAIRING_UNKNOWN && err &&
	    !strcmp(err, "org.bluez.Error.AuthenticationFailed")) {
		log_debug("pairing unsupported; connect only: %s",
		    device->addr);
		/* Assume device does not support pairing and just connect */
		device->pairing_support = BT_PAIRING_UNSUPPORTED;
		rc = bt_device_pair(obj, device);
		if (!rc) {
			return;
		}
		if (rc > 0) {
			/* Already connected */
			result = NETWORK_SUCCESS;
		}
	}
	if (result != NETWORK_SUCCESS) {
		log_warn("pairing attempt failed: %s", device->addr);
		/* Pair/Connect failed, so remove any cached device info */
		if (!device->adapter_obj) {
			log_err("%s: missing adapter object", device->addr);
			goto done;
		}
		msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
		    device->adapter_obj->path,
		    bt_dbus_interface_strs[DBUS_IFACE_ADAPTER], "RemoveDevice");
		dev_path = obj->path;
		dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &dev_path,
		    DBUS_TYPE_INVALID);
		if (msg) {
			dbus_client_send(msg);
			dbus_message_unref(msg);
		}

		/* DVLX-357: Added to avoid appd crash
		when org.bluez.Error.Failed */
		return;
	}
done:
	log_debug("addr=%s, result=%d", device->addr, result);
	/* A pairing support change may warrant a device update */
	bt_device_process_update(obj, device);
	/* Complete the connect request */
	if (obj == state.connect.obj) {
		bt_node_connect_complete(device->addr, result);
	}
}

/*
 * Pair and connect to a known device.  If the pairing capabilities of the
 * device are unknown, pairing will be attempted first.  If pairing fails in
 * a manner that looks like it might not be supported, fall back to a non-
 * bonded connection.
 */
static int bt_device_pair(struct bt_dbus_obj *obj, struct bt_device *device)
{
	DBusMessage *msg;
	const char *method = NULL;
	int rc;

	switch (device->pairing_support) {
	case BT_PAIRING_UNKNOWN:
		method = "Pair";
		break;
	case BT_PAIRING_SUPPORTED:
		if (device->paired) {
			if (device->connected) {
				/* Nothing to do */
				return 1;
			}
			method = "Connect";
		} else {
			method = "Pair";
		}
		break;
	case BT_PAIRING_UNSUPPORTED:
		method = "Connect";
		break;
	}

	log_debug("path %s, method_call %s", obj->path, method);

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_DEVICE], method);
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	rc = dbus_client_send_with_reply(msg, bt_device_pair_reply_handler,
	    obj, BT_DBUS_MSG_TIMEOUT_CONNECT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}
	return rc;
}

static struct bt_device *bt_device_alloc(void)
{
	struct bt_device *device;

	device = calloc(1, sizeof(*device));
	if (!device) {
		log_err("malloc failed");
		return NULL;
	}
	device->rssi = BT_RSSI_INVALID;
	return device;
}

static void bt_device_free(void *ptr)
{
	struct bt_device *device = (struct bt_device *)ptr;
	int i;

	if (!device) {
		return;
	}
	free(device->addr);
	free(device->name);
	free(device->alias);
	for (i = 0; i < BT_SCAN_UUID_MAX; i++) {
		if (device->uuids[i]) {
			free(device->uuids[i]);
		} else {
			break;
		}
	}
	free(device);
}

static void bt_device_removed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data)
{
	struct bt_device *device;

	ASSERT(type == DBUS_IFACE_DEVICE);

	/* DVLX-357: Check NULL point */
	ASSERT(data != NULL);

	device = (struct bt_device *)data;
	log_debug("obj path %s, addr %s, name %s",
	    obj->path, device->addr, device->name);

	device->paired = false;
	device->connected = false;
	if (device->node) {
		/* Cancel pending network operations other than node leave */
		if (device->query_complete) {
			device->query_complete(device->node, NETWORK_UNKNOWN);
			device->query_complete = NULL;
		}
		if (device->config_complete) {
			device->config_complete(device->node, NETWORK_UNKNOWN);
			device->config_complete = NULL;
		}
	}
	if (obj == state.connect.obj) {
		/* Cancel an ongoing pairing attempt with this device */
		bt_node_connect_cancel(device->addr);
	}
	/* bt_device_process_update(obj, device); */
	if (device->leave_complete) {
		log_debug("complete func %p, device->node %p",
		    device->leave_complete, device->node);
		/* Complete node leave process */
		if (device->node) {
			device->leave_complete(device->node, NETWORK_SUCCESS);
		}
		device->leave_complete = NULL;
	} else if (device->node) {
		node_conn_status_changed(device->node, false);
		node_left(device->node);
		device->conn_status_syncd = true;
		bt_node_set_obj(device->node, NULL);
		log_debug("node %s removed, set node obj %s ptr as NULL",
		    device->node->addr, obj->path);
	}

	/* Call disconnect callback function */
	if (disconnect_cb) {
		log_debug("device addr %s", device->addr);
		if (!strcmp(disconnect_addr, device->addr)) {
			disconnect_cb(device->addr, NETWORK_SUCCESS,
			    disconnect_arg);
			disconnect_cb = NULL;
			disconnect_arg = NULL;
			memset(disconnect_addr, 0, sizeof(disconnect_addr));
		}
	}
}

static void bt_device_props_changed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data,
	const DBusMessageIter *props_iter)
{
	struct bt_device *device = (struct bt_device *)data;
	struct node_prop *prop;
	DBusMessageIter iter;
	DBusMessageIter val_iter;
	const char *name;
	const char *str_val;
	s32 int_val;

	ASSERT(type == DBUS_IFACE_DEVICE);

	/* Must populate address before updating other fields */
	if (!device->addr) {
		for (iter = *props_iter;
		    dbus_message_iter_get_arg_type(&iter) ==
		    DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&iter)) {
			name = dbus_utils_parse_dict(&iter, &val_iter);
			if (name && !strcmp(name, "Address")) {
				str_val = dbus_utils_parse_string(&val_iter);
				if (!str_val) {
					log_err("invalid %s", name);
					return;
				}
				device->addr = strdup(str_val);
				break;
			}
		}
		if (!device->addr) {
			log_warn("cannot update device without address: %s",
			    obj->path);
			return;
		}
	}
	/* Look for existing node for this device */
	if (!device->node) {
		device->node = node_lookup(device->addr);
		if (device->node) {
			bt_node_set_obj(device->node, obj);
		}
	}
	if (device->node) {
		/* Batch any Ayla node property updates */
		node_prop_batch_begin(device->node);
	}
	for (iter = *props_iter; dbus_message_iter_get_arg_type(&iter) ==
	    DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&iter)) {
		name = dbus_utils_parse_dict(&iter, &val_iter);
		if (!name) {
			continue;
		}
		/* Update fixed device info */
		if (!strcmp(name, "Name")) {
			/* Name should not change */
			if (!device->name) {
				str_val = dbus_utils_parse_string(&val_iter);
				if (!str_val) {
					goto invalid_val;
				}
				device->name = strdup(str_val);
			}
		} else if (!strcmp(name, "Alias")) {
			str_val = dbus_utils_parse_string(&val_iter);
			if (!str_val) {
				goto invalid_val;
			}
			free(device->alias);
			device->alias = strdup(str_val);
		} else if (!strcmp(name, "RSSI")) {
			if (dbus_utils_parse_int(&val_iter, &int_val) < 0) {
				goto invalid_val;
			}
			device->rssi = int_val;
		} else if (!strcmp(name, "Paired")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &device->paired) < 0) {
				goto invalid_val;
			}
			if (device->paired) {
				/* Definitely supports pairing */
				device->pairing_support = BT_PAIRING_SUPPORTED;
			}
		} else if (!strcmp(name, "Connected")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &device->connected) < 0) {
				goto invalid_val;
			}
			if (!device->connected) {
				/* Reconnect with device */
				bt_monitor_run();
			}
		} else if (!strcmp(name, "ServicesResolved")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &device->services_resolved) < 0) {
				goto invalid_val;
			}
		} else if (!strcmp(name, "LegacyPairing")) {
			if (dbus_utils_parse_bool(&val_iter,
			    &device->legacy_pairing) < 0) {
				goto invalid_val;
			}
		} else if (!strcmp(name, "Adapter")) {
			/* Link Device to its Adapter object */
			if (!device->adapter_obj) {
				str_val = dbus_utils_parse_string(&val_iter);
				if (!str_val) {
					goto invalid_val;
				}
				device->adapter_obj = bt_dbus_obj_hashmap_get(
				    &state.dbus_objects, str_val);
				if (!device->adapter_obj) {
					log_warn("%s: unknown adapter %s",
					    device->addr, str_val);
				}
			}
		} else if (!strcmp(name, "UUIDs")) {
			/* UUIDs should not change */
			if (!device->uuids[0]) {
				if (dbus_utils_parse_uuid_variant(&val_iter,
				    device->uuids, BT_SCAN_UUID_MAX) < 0) {
					log_debug("%s: parse uuid error",
					    device->addr);
					goto invalid_val;
				}
			}
		}
		if (log_debug_enabled()) {
			str_val = dbus_utils_val_str(&val_iter);
			if (str_val) {
				log_debug("%s: %s: %s", device->addr, name,
				    str_val);
			}
		}
		/* Send any device properties in the template */
		if (device->node) {
			prop = node_prop_lookup(device->node,
			    BT_SUBDEVICE_DEVICE, BT_TEMPLATE_DEVICE, name);
			if (prop) {
				/* Map D-Bus object to the node props */
				if (!bt_node_prop_state_get(prop)) {
					bt_node_prop_state_set(prop, obj, type,
					    NULL);
				}
				if (prop->dir == PROP_FROM_DEVICE) {
					log_debug("%s: sending device props "
					    "%s:%s:%s = %s", device->node->addr,
					    prop->subdevice->key,
					    prop->template->key,
					    prop->name,
					    dbus_utils_val_str(&val_iter));
					bt_dbus_prop_send(device->node, prop,
					    &val_iter);
				}
			}
		}
		continue;
invalid_val:
		log_err("%s: invalid %s value", device->addr, name);
	}
	if (device->node) {
		/* Send the batch */
		node_prop_batch_end(device->node);
	}
	log_debug("device addr %s, obj->path %s", device->addr, obj->path);
	bt_device_process_update(obj, device);
}

/*****************************************
 * BlueZ GattService D-Bus interface support
 *****************************************/

static struct bt_gatt_service *bt_gatt_service_alloc(void)
{
	struct bt_gatt_service *service;

	service = calloc(1, sizeof(*service));
	if (!service) {
		log_err("malloc failed");
		return NULL;
	}
	return service;
}

static void bt_gatt_service_removed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data)
{
	struct bt_gatt_service *service = (struct bt_gatt_service *)data;

	ASSERT(type == DBUS_IFACE_GATT_SERVICE);

	log_debug("%s: GATT service removed",
	    bt_uuid_string(&service->uuid));

	/*
	 * TODO:
	 * MAY need to clear gatt_service_obj references in GATT chars if D-Bus
	 * interfaces are ever removed out of order.  Haven't seen this so far.
	 */
}

static void bt_gatt_service_props_changed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data,
	const DBusMessageIter *props_iter)
{
	struct bt_gatt_service *service = (struct bt_gatt_service *)data;
	DBusMessageIter iter;
	DBusMessageIter val_iter;
	const char *name;
	const char *str_val;

	ASSERT(type == DBUS_IFACE_GATT_SERVICE);

	for (iter = *props_iter; dbus_message_iter_get_arg_type(&iter) ==
	    DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&iter)) {
		name = dbus_utils_parse_dict(&iter, &val_iter);
		if (!name) {
			continue;
		}
		log_debug("%s", name);
		if (!strcmp(name, "UUID")) {
			/* UUID should not change */
			if (bt_uuid_valid(&service->uuid)) {
				continue;
			}
			str_val = dbus_utils_parse_string(&val_iter);
			if (!str_val) {
				goto invalid_val;
			}
			log_debug("UUID %s", str_val);
			if (bt_uuid_parse(&service->uuid, str_val) < 0) {
				goto invalid_val;
			}
			service->template_def = bt_gatt_db_lookup_template(
			    &service->uuid);
		} else if (!strcmp(name, "Device")) {
			/* Device should not change */
			if (service->device_obj) {
				continue;
			}
			str_val = dbus_utils_parse_string(&val_iter);
			if (!str_val) {
				goto invalid_val;
			}
			log_debug("Device %s", str_val);
			service->device_obj = bt_dbus_obj_hashmap_get(
			    &state.dbus_objects, str_val);
			if (!service->device_obj) {
				log_warn("%s: unknown device %s",
				    obj->path, str_val);
			}
		}
		continue;
invalid_val:
		log_err("%s: invalid %s value", obj->path, name);
	}
}

/*****************************************
 * BlueZ GattCharacteristic D-Bus interface support
 *****************************************/

/*
 * Setup a GATT characteristic state by mapping it to Ayla property definitions,
 * and adding the properties to the associated node.
 */
static void bt_gatt_char_props_add(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic)
{
	struct bt_device *device;
	struct bt_gatt_service *service;
	const struct bt_gatt_db_prop *prop_def;
	struct node_prop *prop;
	bool prop_added = false;

	/* Lookup Ayla property definitions for this characteristic */
	if (!characteristic->prop_defs) {
		characteristic->prop_defs = bt_gatt_db_lookup_props(
		    &characteristic->uuid);
		if (!characteristic->prop_defs) {
			/* No supported characteristic for this GATT service */
			return;
		}
	}
	if (!characteristic->gatt_service_obj) {
		log_err("%s: no GATT service object",
		    bt_uuid_string(&characteristic->uuid));
		return;
	}
	service = bt_gatt_service_get(characteristic->gatt_service_obj);
	if (!service) {
		log_err("%s: missing GATT service state",
		    characteristic->gatt_service_obj->path);
		return;
	}
	if (!service->template_def) {
		/* No supported template for this GATT service */
		log_debug("%s: no supported template for GATT service",
		    bt_uuid_string(&service->uuid));
		return;
	}
	if (!service->device_obj) {
		log_err("%s: missing device object",
		    bt_uuid_string(&service->uuid));
		return;
	}
	device = bt_device_get(service->device_obj);
	if (!device) {
		log_err("%s: missing device state", service->device_obj->path);
		return;
	}
	if (!device->node) {
		/* Must defer property add until device is managed */
		characteristic->pending_prop_add = true;
		return;
	}
	characteristic->pending_prop_add = false;
	BT_GATT_DB_PROP_LIST_FOREACH(prop_def, characteristic->prop_defs) {
		prop = node_prop_lookup(device->node, prop_def->subdevice,
		    service->template_def->key, prop_def->def.name);
		if (!prop) {
			prop = node_prop_add(device->node, prop_def->subdevice,
			    service->template_def->key, &prop_def->def,
			    service->template_def->version);
			prop_added = true;
			log_debug("%s: added property %s:%s:%s", device->addr,
			    prop->subdevice->key, prop->template->key,
			    prop->name);
		}
		if (prop) {
			/*
			 * Associate D-Bus object info and property definition
			 * with each added property.
			 */
			bt_node_prop_state_set(prop, obj, DBUS_IFACE_GATT_CHAR,
			    (void *)prop_def);
		}
	}
	if (prop_added) {
		log_debug("%s node_info_changed version NULL", device->addr);
		/* Schedule a node_update to add new properties to the cloud */
		node_info_changed(device->node, NULL);
	}
	/* Fetch characteristic value after props are added and ready */
	if (characteristic->flags & BIT(BT_GATT_READ)) {
		characteristic->pending_read = true;
	}
}

/*
 * Handle a new value for a GATT characteristic.  This calls the send handlers
 * for all associated Ayla properties.  These send handlers are expected to
 * post node property value updates as needed.
 */
static void bt_gatt_char_process_value(struct bt_gatt_char *characteristic,
	void *val, size_t val_len)
{
	struct bt_device *device;
	struct bt_gatt_service *service;
	struct node_prop *prop;
	const struct bt_gatt_db_prop *prop_def;

	/* This is an unmanaged characteristic, so ignore value updates */
	if (!characteristic->prop_defs) {
		log_debug("%s: ignore values for unsupported characteristic",
		    bt_uuid_string(&characteristic->uuid));
		return;
	}
	/* Update the cached value for the characteristic */
	if (bt_gatt_val_set(&characteristic->val, val, val_len) < 0) {
		return;
	}
	if (!characteristic->gatt_service_obj) {
		log_err("%s: no GATT service object",
		    bt_uuid_string(&characteristic->uuid));
		return;
	}
	service = bt_gatt_service_get(characteristic->gatt_service_obj);
	if (!service) {
		log_err("%s: missing GATT service state",
		    characteristic->gatt_service_obj->path);
		return;
	}
	if (!service->device_obj) {
		log_err("%s: missing device object",
		    bt_uuid_string(&service->uuid));
		return;
	}
	device = bt_device_get(service->device_obj);
	if (!device) {
		log_err("%s: missing device state",
		    service->device_obj->path);
		return;
	}
	if (!device->node) {
		log_warn("%s: cannot send props for %s on unmanaged device",
		    device->addr, bt_uuid_string(&service->uuid));
		return;
	}
	/* Call val_send callback for all props associated w/ characteristic */
	node_prop_batch_begin(device->node);
	BT_GATT_DB_PROP_LIST_FOREACH(prop_def, characteristic->prop_defs) {
		if (!prop_def->val_send) {
			continue;
		}
		prop = node_prop_lookup(device->node, prop_def->subdevice,
		    service->template_def->key, prop_def->def.name);
		if (!prop) {
			continue;
		}
		prop_def->val_send(device->node, prop, &characteristic->val);
	}
	node_prop_batch_end(device->node);
}

static void bt_gatt_char_write_reply_handler(DBusMessage *msg, void *arg,
	const char *err)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;
	struct bt_gatt_char *characteristic = bt_gatt_char_get(obj);

	if (!characteristic) {
		log_err("%s: missing GATT characteristic state", obj->path);
		return;
	}
	characteristic->state = BT_GATT_VAL_READY;
	if (!msg) {
		log_warn("%s: GATT characteristic write failed",
		     bt_uuid_string(&characteristic->uuid));
		characteristic->pending_write = true;
		return;
	}
	/* Begin next write */
	if (characteristic->pending_write) {
		bt_gatt_char_write(obj, characteristic);
	}
}

/*
 * Write the current cached value for the specified GATT characteristic.  This
 * is accomplished by invoking the "WriteValue" D-Bus method of the
 * GattCharacteristic object.
 */
static int bt_gatt_char_write(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic)
{
	DBusMessage *msg;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	int rc = -1;

	if (characteristic->state != BT_GATT_VAL_READY) {
		/* BlueZ driver rejects concurrent read/write requests */
		log_debug("%s: deferring write; already in progress",
		    bt_uuid_string(&characteristic->uuid));
		characteristic->pending_write = true;
		return 0;
	}
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_GATT_CHAR], "WriteValue");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	dbus_message_iter_init_append(msg, &iter);
	/* Characteristic data */
	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y",
	    &array_iter) ||
	    !dbus_message_iter_append_fixed_array(&array_iter, DBUS_TYPE_BYTE,\
	    &characteristic->val.data, characteristic->val.len) ||
	    !dbus_message_iter_close_container(&iter, &array_iter)) {
		log_err("failed to populate D-Bus message value");
		goto cleanup;
	}
	/* Empty options array */
	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}",
	    &array_iter) ||
	    !dbus_message_iter_close_container(&iter, &array_iter)) {
		log_err("failed to populate D-Bus message options");
		goto cleanup;
	}
	log_debug_hex(bt_uuid_string(&characteristic->uuid),
	    characteristic->val.data, characteristic->val.len);
	rc = dbus_client_send_with_reply(msg, bt_gatt_char_write_reply_handler,
	    obj, DBUS_TIMEOUT_USE_DEFAULT);
	if (rc < 0) {
		log_err("D-Bus send failed");
		goto cleanup;
	}
	characteristic->pending_write = false;
	characteristic->state = BT_GATT_VAL_WRITE;
cleanup:
	dbus_message_unref(msg);
	return rc;
}

static void bt_gatt_char_read_reply_handler(DBusMessage *reply, void *arg,
	const char *err)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;
	struct bt_gatt_char *characteristic = bt_gatt_char_get(obj);
	DBusMessageIter iter;
	unsigned char *byte_val = NULL;
	size_t byte_val_len = 0;
	int rc;

	if (!characteristic) {
		log_err("%s: missing GATT characteristic state", obj->path);
		return;
	}
	characteristic->state = BT_GATT_VAL_READY;
	if (!reply) {
		log_warn("%s: GATT characteristic read failed",
		     bt_uuid_string(&characteristic->uuid));
		characteristic->pending_read = true;
		return;
	}
	dbus_message_iter_init(reply, &iter);
	/* Characteristic data */
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		log_err("%s: missing value array",
		    bt_uuid_string(&characteristic->uuid));
		return;
	}
	dbus_message_iter_recurse(&iter, &iter);
	dbus_message_iter_get_fixed_array(&iter, &byte_val, &rc);
	if (rc > 0) {
		byte_val_len = rc;
		log_debug_hex(bt_uuid_string(&characteristic->uuid), byte_val,
		    byte_val_len);
		bt_gatt_char_process_value(characteristic, byte_val,
		    byte_val_len);
	}
	/* Begin next read */
	if (characteristic->pending_read) {
		bt_gatt_char_read(obj, characteristic);
	}
}

/*
 * Read the current value for the specified GATT characteristic.  This
 * is accomplished by invoking the "ReadValue" D-Bus method of the
 * GattCharacteristic object.
 */
static int bt_gatt_char_read(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic)
{
	DBusMessage *msg;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	int rc = -1;

	if (characteristic->state != BT_GATT_VAL_READY) {
		/* BlueZ driver rejects concurrent read/write requests */
		log_debug("%s: deferring read; already in progress",
		    bt_uuid_string(&characteristic->uuid));
		characteristic->pending_read = true;
		return 0;
	}
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_GATT_CHAR], "ReadValue");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	dbus_message_iter_init_append(msg, &iter);
	/* Empty options array */
	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}",
	    &array_iter) ||
	    !dbus_message_iter_close_container(&iter, &array_iter)) {
		log_err("failed to populate D-Bus message options");
		goto cleanup;
	}
	rc = dbus_client_send_with_reply(msg, bt_gatt_char_read_reply_handler,
	    obj, DBUS_TIMEOUT_USE_DEFAULT);
	if (rc < 0) {
		log_err("D-Bus send failed");
		goto cleanup;
	}
	characteristic->pending_read = false;
	characteristic->state = BT_GATT_VAL_READ;
cleanup:
	dbus_message_unref(msg);
	return rc;
}

/*
 * Enable or disable GATT characteristic value change notifications.
 */
static int bt_gatt_char_set_notify(struct bt_dbus_obj *obj, bool enable)
{
	DBusMessage *msg;
	int rc;
	const char *method = enable ? "StartNotify" : "StopNotify";

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_GATT_CHAR], method);
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	rc = dbus_client_send(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	} else {
		log_debug("sent %s to %s", method, obj->path);
	}
	dbus_message_unref(msg);
	return rc;
}

/*
 * Handle property updates for a GATT characteristic.
 */
static void bt_gatt_char_process_update(struct bt_dbus_obj *obj,
	struct bt_gatt_char *characteristic)
{
	if (!characteristic->prop_defs) {
		/* Attempt to map characteristic to props and value handlers */
		bt_gatt_char_props_add(obj, characteristic);
	}
	if (characteristic->prop_defs) {
		if (characteristic->notify == BT_NOTIFY_DISABLED &&
		    ((characteristic->flags & BIT(BT_GATT_NOTIFY)) ||
		    (characteristic->flags & BIT(BT_GATT_INDICATE)))) {
			/* Get value updates for managed characteristics */
			bt_gatt_char_set_notify(obj, true);
		}
	} else {
		if (characteristic->notify == BT_NOTIFY_ENABLED) {
			/* Disable value change notifications */
			bt_gatt_char_set_notify(obj, false);
		}
	}
}

static struct bt_gatt_char *bt_gatt_char_alloc(void)
{
	struct bt_gatt_char *characteristic;

	characteristic = calloc(1, sizeof(*characteristic));
	if (!characteristic) {
		log_err("malloc failed");
		return NULL;
	}
	return characteristic;
}

static void bt_gatt_char_free(void *ptr)
{
	struct bt_gatt_char *characteristic = (struct bt_gatt_char *)ptr;

	if (!characteristic) {
		return;
	}
	bt_gatt_val_cleanup(&characteristic->val);
	free(characteristic);
}

static void bt_gatt_char_removed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data)
{
	struct bt_gatt_char *characteristic = (struct bt_gatt_char *)data;

	ASSERT(type == DBUS_IFACE_GATT_CHAR);

	log_debug("%s: GATT characteristic removed",
	    bt_uuid_string(&characteristic->uuid));

	/*
	 * TODO
	 * May want to remove the associated properties and update node info.
	 */
}

static void bt_gatt_char_props_changed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data,
	const DBusMessageIter *props_iter)
{
	struct bt_gatt_char *characteristic = (struct bt_gatt_char *)data;
	DBusMessageIter iter;
	DBusMessageIter val_iter;
	DBusMessageIter array_iter;
	const char *name;
	const char *str_val;
	bool bool_val;
	unsigned char *byte_val = NULL;
	size_t byte_val_len = 0;
	u32 flags;
	int rc;

	ASSERT(type == DBUS_IFACE_GATT_CHAR);

	for (iter = *props_iter; dbus_message_iter_get_arg_type(&iter) ==
	    DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&iter)) {
		name = dbus_utils_parse_dict(&iter, &val_iter);
		if (!name) {
			continue;
		}
		if (!strcmp(name, "Value")) {
			dbus_message_iter_recurse(&val_iter, &array_iter);
			if (dbus_message_iter_get_arg_type(&array_iter) !=
			    DBUS_TYPE_ARRAY) {
				goto invalid_val;
			}
			dbus_message_iter_recurse(&array_iter, &array_iter);
			dbus_message_iter_get_fixed_array(&array_iter,
			    &byte_val, &rc);
			if (rc > 0) {
				byte_val_len = rc;
				log_debug_hex(bt_uuid_string(
				    &characteristic->uuid), byte_val,
				    byte_val_len);
			}
		} else if (!strcmp(name, "UUID")) {
			/* UUID should not change */
			if (bt_uuid_valid(&characteristic->uuid)) {
				continue;
			}
			str_val = dbus_utils_parse_string(&val_iter);
			if (!str_val) {
				goto invalid_val;
			}
			if (bt_uuid_parse(&characteristic->uuid, str_val) < 0) {
				goto invalid_val;
			}
		} else if (!strcmp(name, "Service")) {
			/* Service should not change */
			if (characteristic->gatt_service_obj) {
				continue;
			}
			str_val = dbus_utils_parse_string(&val_iter);
			if (!str_val) {
				goto invalid_val;
			}
			characteristic->gatt_service_obj =
			    bt_dbus_obj_hashmap_get(&state.dbus_objects,
			    str_val);
			if (!characteristic->gatt_service_obj) {
				log_warn("%s: unknown service %s",
				    obj->path, str_val);
			}
		} else if (!strcmp(name, "Notifying")) {
			if (dbus_utils_parse_bool(&val_iter, &bool_val) < 0) {
				goto invalid_val;
			}
			/* XXX may want separate notifying flag */
			characteristic->notify = bool_val ?
			    BT_NOTIFY_ENABLED : BT_NOTIFY_DISABLED;
		}  else if (!strcmp(name, "Flags")) {
			/* Flags should not change */
			if (characteristic->flags) {
				continue;
			}
			dbus_message_iter_recurse(&val_iter, &array_iter);
			if (dbus_message_iter_get_arg_type(&array_iter) !=
			    DBUS_TYPE_ARRAY) {
				goto invalid_val;
			}
			dbus_message_iter_recurse(&array_iter, &array_iter);
			for (; dbus_message_iter_get_arg_type(&array_iter) ==
			    DBUS_TYPE_STRING;
			    dbus_message_iter_next(&array_iter)) {
				str_val = dbus_utils_parse_string(&array_iter);
				if (!str_val) {
					goto invalid_val;
				}
				flags = bt_gatt_flag_parse(str_val);
				if (!flags) {
					log_debug("ignoring unknown GATT "
					    "characteristic flag: %s", str_val);
					continue;
				}
				characteristic->flags |= flags;
			}
		}
		continue;
invalid_val:
		log_err("%s: invalid %s value", obj->path, name);
	}
	/* Update characteristic based on new property values */
	bt_gatt_char_process_update(obj, characteristic);
	if (byte_val) {
		/* Received value in notification, so no need to request read */
		characteristic->pending_read = false;
		/* Send characteristic value */
		bt_gatt_char_process_value(characteristic, byte_val,
		    byte_val_len);
	}
}

/*****************************************
 * BlueZ AgentManager D-Bus interface support
 *****************************************/

/*
 * Handler called by the connection agent when device authentication requires
 * a user-supplied passkey.  Returns -1 on failure.
 */
static int bt_agent_passkey_request(const struct bt_device *device,
	u32 *passkey)
{
	if (state.connect.status == BT_CONNECT_READY) {
		log_warn("unexpected passkey request");
		return -1;
	}
	if (!state.callbacks.passkey_request) {
		log_warn("passkey request not supported");
		return -1;
	}
	return state.callbacks.passkey_request(device->addr, passkey);
}

/*
 * Handler called by the connection agent when device authentication requires
 * the user to visually confirm the supplied passkey.  Returns -1 on failure.
 */
static int bt_agent_passkey_display(const struct bt_device *device,
	u32 passkey)
{
	if (state.connect.status == BT_CONNECT_READY) {
		log_warn("unexpected passkey");
		return -1;
	}
	if (!state.callbacks.passkey_display) {
		/* Passkey display is optional */
		return 0;
	}
	if (state.callbacks.passkey_display(device->addr, passkey) < 0) {
		return -1;
	}
	state.connect.status = BT_CONNECT_AUTH_DISPLAY;
	return 0;
}

/*
 * Handler called by the connection agent the device authentication attempt
 * is complete or has been canceled.  Clears the passkey if it is currently
 * being displayed.
 */
static void bt_agent_cancel(const struct bt_device *device)
{
	if (state.connect.status != BT_CONNECT_AUTH_DISPLAY) {
		return;
	}
	if (!state.callbacks.passkey_display_clear) {
		/* Passkey display is optional */
		return;
	}
	state.callbacks.passkey_display_clear(device->addr);
}

/*
 * Parse the device path field in a message and lookup the device state.
 */
static struct bt_device *bt_agent_get_device_from_msg(DBusMessageIter *iter)
{
	const char *path;
	struct bt_dbus_obj *obj;

	path = dbus_utils_parse_string(iter);
	if (!path) {
		log_err("missing device path");
		return NULL;
	}
	obj = bt_dbus_obj_hashmap_get(&state.dbus_objects, path);
	if (!obj) {
		log_warn("non-existent device: %s", path);
		return NULL;
	}
	return bt_device_get(obj);
}

/*
 * Handle incoming method calls for the registered connection agent.
 * This handles various interactive security operations required to complete
 * pairing with a device.
 */
static void bt_agent_method_handler(DBusMessage *msg, void *arg)
{
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	struct bt_device *device;
	const char *method;
	const char *uuid;
	char *errptr;
	char pin_buf[17];	/* PIN is 1-16 characters */
	const char *pin = pin_buf;
	u32 passkey;

	if (!msg) {
		return;
	}
	method = dbus_message_get_member(msg);
	if (!method) {
		log_warn("missing method");
		return;
	}
	log_debug("incoming method call %s", method);
	dbus_message_iter_init(msg, &iter);
	if (!strcmp(method, "RequestPinCode")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		log_debug("request PIN for %s", device->addr);
		if (bt_agent_passkey_request(device, &passkey) < 0) {
			goto reply;
		}
		/* Convert passkey to PIN for legacy support */
		snprintf(pin_buf, sizeof(pin_buf), "%06u", passkey);
		log_debug("received PIN: %s", pin);
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin,
		    DBUS_TYPE_INVALID);
	} else if (!strcmp(method, "DisplayPinCode")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		dbus_message_iter_next(&iter);
		pin = dbus_utils_parse_string(&iter);
		if (!pin) {
			log_err("missing PIN");
			goto reply;
		}
		/* Convert PIN to passkey for legacy support */
		passkey = strtoul(pin, &errptr, 10);
		if (*errptr != '\0') {
			log_warn("non-numeric PIN not supported: %s", pin);
			goto reply;
		}
		log_debug("display PIN for %s: %s", device->addr, pin);
		if (bt_agent_passkey_display(device, passkey) < 0) {
			goto reply;
		}
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(method, "RequestPasskey")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		log_debug("request passkey for %s", device->addr);
		if (bt_agent_passkey_request(device, &passkey) < 0) {
			goto reply;
		}
		log_debug("received passkey: %06u", passkey);
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey,
		    DBUS_TYPE_INVALID);
	} else if (!strcmp(method, "DisplayPasskey")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		dbus_message_iter_next(&iter);
		if (dbus_utils_parse_uint(&iter, &passkey) < 0) {
			log_err("missing passkey");
			goto reply;
		}
		log_debug("display passkey for %s: %06u", device->addr,
		    passkey);
		if (bt_agent_passkey_display(device, passkey) < 0) {
			goto reply;
		}
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(method, "RequestConfirmation")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		dbus_message_iter_next(&iter);
		if (dbus_utils_parse_uint(&iter, &passkey) < 0) {
			log_err("missing passkey");
			goto reply;
		}
		log_debug("request passkey confirmation for %s: %06u",
		    device->addr, passkey);
		/*
		 * Generally there would be a prompt to confirm the passkey,
		 * but to facilitate remote pairing, it will just be
		 * displayed, and confirmation will be automatic.
		 */
		if (bt_agent_passkey_display(device, passkey) < 0) {
			goto reply;
		}
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(method, "RequestAuthorization")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		log_debug("rejecting incoming pairing attempt from %s",
		    device->addr);
		goto reply;
	} else if (!strcmp(method, "AuthorizeService")) {
		device = bt_agent_get_device_from_msg(&iter);
		if (!device) {
			goto reply;
		}
		dbus_message_iter_next(&iter);
		uuid = dbus_utils_parse_string(&iter);
		if (!uuid) {
			log_err("missing UUID");
			goto reply;
		}
		log_debug("authorizing service for %s: %s", device->addr, uuid);
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(method, "Cancel") || !strcmp(method, "Release")) {
		if (!state.connect.obj) {
			return;
		}
		device = bt_device_get(state.connect.obj);
		if (!device) {
			return;
		}
		log_debug("agent %s call for %s", method, device->addr);
		bt_agent_cancel(device);
		return;
	} else  {
		log_warn("unsupported method: %s", method);
		goto reply;
	}
reply:
	if (!dbus_message_get_no_reply(msg)) {
		if (!reply) {
			reply = dbus_message_new_error(msg,
			    "org.bluez.Error.Rejected", NULL);
		}
		dbus_client_send(reply);
	}
	dbus_message_unref(reply);
}

/*
 * Handle the agent registration reply by setting the
 */
static void bt_agent_manager_register_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;
	const char *path = BT_DBUS_PATH_AGENT;
	DBusMessage *msg;

	if (!reply) {
		log_err("%s: failed to register connection agent", obj->path);
		return;
	}
	log_debug("%s: registered connection agent on %s", obj->path,
	    BT_DBUS_PATH_AGENT);
	/* Set the agent as the default agent */
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_AGENT_MANAGER],
	    "RequestDefaultAgent");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path,
	    DBUS_TYPE_INVALID);
	if (dbus_client_send(msg) < 0) {
		log_err("D-Bus send failed");
	}
	dbus_message_unref(msg);
}

static void bt_agent_manager_added(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data)
{
	DBusMessage *msg;
	const char *path = BT_DBUS_PATH_AGENT;
	/* Default is "KeyboardDisplay" */
	const char *capability = "NoInputNoOutput";
	int rc;

	ASSERT(type == DBUS_IFACE_AGENT_MANAGER);

	/* Register an object path for the connection agent */
	if (dbus_client_obj_path_register(path, DBUS_MSG_TYPE_METHOD_CALL,
	    bt_dbus_interface_strs[DBUS_IFACE_AGENT], NULL,
	    bt_agent_method_handler, NULL) < 0) {
		log_err("failed to register path for connection agent: %s",
		    path);
		return;
	}
	/* Request a connection agent */
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ, obj->path,
	    bt_dbus_interface_strs[DBUS_IFACE_AGENT_MANAGER], "RegisterAgent");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path,
	    DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID);
	rc = dbus_client_send_with_reply(msg,
	    bt_agent_manager_register_reply_handler, obj,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}
}

static void bt_agent_manager_removed(struct bt_dbus_obj *obj,
	enum bt_dbus_interface type, void *data)
{
	dbus_client_obj_path_unregister(BT_DBUS_PATH_AGENT);
}

/*****************************************
 * D-Bus object and interface handlers
 *****************************************/

/*
 * Cleanup resources associated with a D-Bus interface.
 */
static void bt_dbus_iface_free(struct bt_dbus_iface *iface)
{
	if (!iface) {
		return;
	}
	/* Free interface-specific data */
	if (iface->free_data && iface->data) {
		iface->free_data(iface->data);
		iface->data = NULL;
	}
	free(iface);
}

/*
 * Allocate and initialize state for a supported D-Bus interface.  Add new
 * interfaces to the switch statement as needed.
 */
static struct bt_dbus_iface *bt_dbus_iface_alloc(enum bt_dbus_interface type)
{
	struct bt_dbus_iface *iface;
	void (*added)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *) = NULL;
	void (*removed)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *) = NULL;
	void (*props_changed)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *, const DBusMessageIter *) = NULL;
	void (*props_invalidated)(struct bt_dbus_obj *,
	    enum bt_dbus_interface, void *, const DBusMessageIter *) = NULL;
	void (*free_data)(void *) = NULL;
	void *data = NULL;

	/* Allocate interface-specific data */
	switch (type) {
	case DBUS_IFACE_ADAPTER:
		data = bt_adapter_alloc();
		if (!data) {
			return NULL;
		}
		props_changed = bt_adapter_props_changed;
		free_data = free;
		break;
	case DBUS_IFACE_DEVICE:
		data = bt_device_alloc();
		if (!data) {
			return NULL;
		}
		removed = bt_device_removed;
		props_changed = bt_device_props_changed;
		free_data = bt_device_free;
		break;
	case DBUS_IFACE_GATT_SERVICE:
		data = bt_gatt_service_alloc();
		if (!data) {
			return NULL;
		}
		removed = bt_gatt_service_removed;
		props_changed = bt_gatt_service_props_changed;
		free_data = free;
		break;
	case DBUS_IFACE_GATT_CHAR:
		data = bt_gatt_char_alloc();
		if (!data) {
			return NULL;
		}
		removed = bt_gatt_char_removed;
		props_changed = bt_gatt_char_props_changed;
		free_data = bt_gatt_char_free;
		break;
	case DBUS_IFACE_AGENT_MANAGER:
		added = bt_agent_manager_added;
		removed = bt_agent_manager_removed;
		break;
	default:
		/* Interface not handled */
		return NULL;
	}
	iface = (struct bt_dbus_iface *)calloc(1, sizeof(*iface));
	if (!iface) {
		log_err("malloc failed");
		if (free_data && data) {
			free_data(data);
		}
		return NULL;
	}
	iface->type = type;
	iface->added = added;
	iface->removed = removed;
	iface->props_changed = props_changed;
	iface->props_invalidated = props_invalidated;
	iface->free_data = free_data;
	iface->data = data;
	return iface;
}

/*
 * Allocate a new D-Bus object.  Initially, no interfaces are associated with
 * it.
 */
static struct bt_dbus_obj *bt_dbus_obj_alloc(const char *path)
{
	struct bt_dbus_obj *obj;

	obj = (struct bt_dbus_obj *)malloc(sizeof(*obj) + strlen(path) + 1);
	if (!obj) {
		log_err("malloc failed");
		return NULL;
	}
	strcpy(obj->path, path);
	TAILQ_INIT(&obj->iface_list);
	obj->prop_changed_handler = NULL;
	return obj;
}

/*
 * Cleanup resources associated with a D-Bus object, including its interfaces.
 */
static void bt_dbus_obj_free(struct bt_dbus_obj *obj)
{
	struct bt_dbus_iface *iface;

	if (!obj) {
		return;
	}
	if (obj->prop_changed_handler) {
		dbus_client_msg_handler_remove(obj->prop_changed_handler);
	}
	/* Free interface-specific data */
	while ((iface = TAILQ_FIRST(&obj->iface_list)) != NULL) {
		TAILQ_REMOVE(&obj->iface_list, iface, entry);
		bt_dbus_iface_free(iface);
	}
	free(obj);
}

/*
 * Handle a D-Bus interface add or update.  This may create a new object as
 * needed.  Set the allow_add flag to false to prevent adding new interfaces
 * and only update existing ones.
 */
static void bt_dbus_iface_update(const char *path,
	const char *interface, const DBusMessageIter *props_changed,
	const DBusMessageIter *props_invalidated, bool allow_add)
{
	enum bt_dbus_interface type;
	struct bt_dbus_obj *obj;
	struct bt_dbus_iface *iface;
	int rc;
	bool added = false;

	/* Lookup known object types */
	rc = lookup_by_name(bt_dbus_interface_table, interface);
	if (rc < 0 || rc == DBUS_IFACE_UNKNOWN) {
		return;
	}
	type = rc;
	obj = bt_dbus_obj_hashmap_get(&state.dbus_objects, path);
	/* Add a D-Bus object and/or interface as needed */
	if (obj) {
		iface = bt_dbus_obj_get_iface(obj, type);
		if (!iface) {
			if (!allow_add) {
				return;
			}
			iface = bt_dbus_iface_alloc(type);
			if (!iface) {
				return;
			}
			TAILQ_INSERT_TAIL(&obj->iface_list, iface, entry);
			added = true;
			log_debug("object %s added iface type=%d",
			    path, iface->type);
		}
	} else {
		if (!allow_add) {
			return;
		}
		iface = bt_dbus_iface_alloc(type);
		if (!iface) {
			return;
		}
		obj = bt_dbus_obj_alloc(path);
		if (!obj) {
			bt_dbus_iface_free(iface);
			return;
		}
		TAILQ_INSERT_TAIL(&obj->iface_list, iface, entry);
		if (bt_dbus_obj_hashmap_put(&state.dbus_objects, obj->path,
		    obj) != obj) {
			log_err("failed to add obj for %s", path);
			bt_dbus_obj_free(obj);
			return;
		}
		added = true;
		log_debug("added object %s, iface type=%d", path, iface->type);
	}
	if (added) {
		log_debug("added interface %s to %s", interface, path);
		/* Invoke added callback for initializing the interface data */
		if (iface->added) {
			iface->added(obj, type, iface->data);
		}
		/* Subscribe to property changed signals for managed objects */
		if (!obj->prop_changed_handler &&
		    (iface->props_changed || iface->props_invalidated)) {
			obj->prop_changed_handler =
			    dbus_client_signal_handler_add(
			    state.service_bus_name,
			    bt_dbus_interface_strs[DBUS_IFACE_PROPS],
			    "PropertiesChanged", obj->path,
			    bt_dbus_obj_prop_signal_handler, obj);
			if (!obj->prop_changed_handler) {
				log_err("failed to subscribe to "
				    "PropertiesChanged signal for %s",
				    obj->path);
			}
		}
	}
	/* Update properties */
	if (props_changed) {
		log_debug("props changed: %s[%s]", interface, path);
#ifdef BT_DEBUG_DBUS_OBJS
		dbus_utils_msg_print_iter(__func__, props_changed, 3);
#endif
		if (iface->props_changed) {
			iface->props_changed(obj, type, iface->data,
			    props_changed);
		}
	}
	/* Clear property values */
	if (props_invalidated) {
		log_debug("props invalidated: %s[%s]", interface, path);
#ifdef BT_DEBUG_DBUS_OBJS
		dbus_utils_msg_print_iter(__func__, props_invalidated, 3);
#endif
		if (iface->props_invalidated) {
			iface->props_invalidated(obj, type, iface->data,
			    props_invalidated);
		}
	}
}

/*
 * Handle a D-Bus interface removed event.  This will remove the object if
 * needed.
 */
static void bt_dbus_iface_remove(const char *path, const char *interface)
{
	enum bt_dbus_interface type;
	struct bt_dbus_obj *obj;
	struct bt_dbus_iface *iface;
	int rc;
	/*struct node *node = NULL;*/

	obj = bt_dbus_obj_hashmap_get(&state.dbus_objects, path);
	if (!obj) {
		return;
	}
	/* Lookup known object types */
	rc = lookup_by_name(bt_dbus_interface_table, interface);
	if (rc < 0 || rc == DBUS_IFACE_UNKNOWN) {
		return;
	}
	type = rc;
	iface = bt_dbus_obj_get_iface(obj, type);
	if (!iface) {
		return;
	}
	log_debug("removing interface %s from %s", interface, path);
	TAILQ_REMOVE(&obj->iface_list, iface, entry);
	if (iface->removed) {
		iface->removed(obj, iface->type, iface->data);
	}

	/* DVLX-357: Save node point address */
	/*if (type == DBUS_IFACE_DEVICE) {
		struct bt_device *device;
		device = (struct bt_device *)(iface->data);
		if (device) {
			log_debug("device addr %s, device node %p",
			    device->addr, device->node);
			node = device->node;
		}
	}*/

	bt_dbus_iface_free(iface);
	/* Delete object if all interfaces were removed */
	if (TAILQ_EMPTY(&obj->iface_list)) {
		log_debug("removing object %s", path);
		bt_dbus_obj_hashmap_remove(&state.dbus_objects, path);
		bt_dbus_obj_free(obj);

		/* DVLX-357: Updated node obj after removed obj */
		/*if (node && bt_node_get_obj(node)) {
			bt_node_set_obj(node, NULL);
		}*/
	}
}

/*
 * Handle a D-Bus interfaces changed message.
 */
static void bt_dbus_obj_event(const char *path, enum bt_dbus_event event,
	const DBusMessageIter *iface_iter)
{
	DBusMessageIter iter = *iface_iter;
	DBusMessageIter props_iter;
	const char *interface;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		log_err("invalid interface array for %s", path);
		return;
	}
	/* Enter interfaces array */
	dbus_message_iter_recurse(&iter, &iter);
	for (; dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID;
	    dbus_message_iter_next(&iter)) {
		switch (event) {
		case BT_DBUS_UPDATE:
			/* Update events are an array of property dicts */
			interface = dbus_utils_parse_dict(&iter, &props_iter);
			if (!interface) {
				log_err("%s: missing interface", path);
				break;
			}
			if (dbus_message_iter_get_arg_type(&props_iter) !=
			    DBUS_TYPE_ARRAY) {
				log_err("%s: invalid props array", path);
				break;
			}
			/* Enter properties array */
			dbus_message_iter_recurse(&props_iter, &props_iter);
			log_debug("obj path %s, interface %s update,"
			    " allow to add", path, interface);
			bt_dbus_iface_update(path, interface, &props_iter,
			    NULL, true);
			break;
		case BT_DBUS_REMOVE:
			/* Remove events are an array of interface names */
			interface = dbus_utils_parse_string(&iter);
			if (!interface) {
				log_err("%s: missing interface name", path);
				break;
			}
			log_debug("obj path %s, interface %s remove",
			    path, interface);
			bt_dbus_iface_remove(path, interface);
			break;
		}
	}
}

/*
 * Parse a D-Bus object property changed signal message.  This is treated
 * as an interface update.
 */
static void bt_dbus_obj_prop_signal_handler(DBusMessage *msg, void *arg)
{
	struct bt_dbus_obj *obj = (struct bt_dbus_obj *)arg;
	DBusMessageIter iter;
	DBusMessageIter props_iter;
	DBusMessageIter *props_changed = NULL;
	DBusMessageIter *props_invalidated = NULL;
	const char *interface;

	if (!msg) {
		return;
	}
	dbus_message_iter_init(msg, &iter);
	interface = dbus_utils_parse_string(&iter);
	if (!interface) {
		log_err("%s: missing interface name", obj->path);
		return;
	}
	dbus_message_iter_next(&iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		/* Enter props changed array */
		dbus_message_iter_recurse(&iter, &props_iter);
		if (dbus_message_iter_get_arg_type(&props_iter) !=
		    DBUS_TYPE_INVALID) {
			props_changed = &props_iter;
		}
	}
	dbus_message_iter_next(&iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		/* Enter props invalidated array */
		dbus_message_iter_recurse(&iter, &iter);
		if (dbus_message_iter_get_arg_type(&iter) !=
		    DBUS_TYPE_INVALID) {
			props_invalidated = &iter;
		}
	}
	if (!props_changed && !props_invalidated) {
		log_warn("%s: nothing changed", obj->path);
		return;
	}
	log_debug("obj path %s, interface %s",
	    obj->path, interface);
	bt_dbus_iface_update(obj->path, interface,
	    props_changed, props_invalidated, false);
}

/*
 * Parse a D-Bus interface changed signal message.
 */
static void bt_dbus_obj_signal_handler(DBusMessage *msg, void *arg)
{
	DBusMessageIter iter;
	const char *member;
	const char *path;
	enum bt_dbus_event event;

	if (!msg) {
		return;
	}
	member = dbus_message_get_member(msg);
	if (!member) {
		log_err("missing signal member");
		return;
	}
	if (!strcmp(member, "InterfacesAdded")) {
		event = BT_DBUS_UPDATE;
	} else if (!strcmp(member, "InterfacesRemoved")) {
		event = BT_DBUS_REMOVE;
	} else {
		log_err("unsupported signal member: %s", member);
		return;
	}
	dbus_message_iter_init(msg, &iter);
	path = dbus_utils_parse_string(&iter);
	if (!path) {
		log_err("missing object path");
		return;
	}
	log_debug("signal %s, path %s", member, path);
	dbus_message_iter_next(&iter);
	bt_dbus_obj_event(path, event, &iter);
}

/*
 * Unregister for D-Bus interface change signals from the subscribed service.
 */
static void bt_dbus_service_signal_unsubscribe(void)
{
	if (state.added_handler) {
		dbus_client_msg_handler_remove(state.added_handler);
		state.added_handler = NULL;
	}
	if (state.removed_handler) {
		dbus_client_msg_handler_remove(state.removed_handler);
		state.removed_handler = NULL;
	}
}

/*
 * Register for D-Bus interface change signals from the specified service.
 */
static int bt_dbus_service_signal_subscribe(const char *bus_name)
{
	if (state.added_handler || state.removed_handler) {
		bt_dbus_service_signal_unsubscribe();
	}
	state.added_handler = dbus_client_signal_handler_add(
	    bus_name, bt_dbus_interface_strs[DBUS_IFACE_OBJ_MANAGER],
	    "InterfacesAdded", NULL, bt_dbus_obj_signal_handler, NULL);
	if (!state.added_handler) {
		log_err("failed to subscribe to InterfacesAdded signal");
		goto error;
	}
	state.removed_handler = dbus_client_signal_handler_add(
	    bus_name, bt_dbus_interface_strs[DBUS_IFACE_OBJ_MANAGER],
	    "InterfacesRemoved", NULL, bt_dbus_obj_signal_handler, NULL);
	if (!state.removed_handler) {
		log_err("failed to subscribe to InterfacesRemoved signal");
		goto error;
	}
	return 0;
error:
	bt_dbus_service_signal_unsubscribe();
	return -1;
}

/*
 * Handle the response from a managed object query from the Bluetooth service.
 * This initializes the D-Bus object data structures and gets the internal
 * bus name needed for some subsequent operations.
 */
static void bt_dbus_parse_managed_objs(DBusMessage *msg, void *arg,
	const char *err)
{
	DBusMessageIter iter;
	DBusMessageIter path_iter;
	const char *path;
	const char *bus_name;

	log_debug("handling managed objects");
	if (!msg) {
		return;
	}
	/* Subscribe to interface change signals on BlueZ bus */
	bus_name = dbus_message_get_sender(msg);
	if (bus_name) {
		free(state.service_bus_name);
		state.service_bus_name = strdup(bus_name);
		log_debug("set bus name: %s", bus_name);
		bt_dbus_service_signal_subscribe(bus_name);
	} else {
		log_err("no bus name populated");
	}
	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		log_err("invalid managed objects array");
		return;
	}
	/* Enter top-level array */
	dbus_message_iter_recurse(&iter, &iter);
	/* Iterate through path entries */
	for (; dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_DICT_ENTRY;
	    dbus_message_iter_next(&iter)) {
		path = dbus_utils_parse_dict(&iter, &path_iter);
		if (!path) {
			continue;
		}
		bt_dbus_obj_event(path, BT_DBUS_UPDATE, &path_iter);
	}
	/* Run the monitor task immediately to process actions on new objects */
	bt_monitor_run();
}

/*
 * Begin the D-Bus session by requesting information about all registered
 * objects we can interact with.
 */
static int bt_dbus_service_subscribe(const char *service, const char *path)
{
	DBusMessage *msg;
	int rc = 0;

	msg = dbus_message_new_method_call(service, path,
	    bt_dbus_interface_strs[DBUS_IFACE_OBJ_MANAGER],
	    "GetManagedObjects");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	if (dbus_client_send_with_reply(msg, bt_dbus_parse_managed_objs,
	    NULL, DBUS_TIMEOUT_USE_DEFAULT) < 0) {
		log_err("D-Bus send failed");
		rc = -1;
	}
	dbus_message_unref(msg);
	return rc;
}

/*****************************************
 * Bluetooth interface public interface
 *****************************************/

/*
 * Initialize the Bluetooth interface..
 */
int bt_init(struct file_event_table *file_events, struct timer_head *timers,
	const struct bt_callbacks *callbacks)
{
	ASSERT(timers != NULL);
	ASSERT(callbacks != NULL);

	state.file_events = file_events;
	state.timers = timers;
	state.callbacks = *callbacks;
	timer_init(&state.monitor_timer, bt_monitor_timeout);
	/* Initialize D-Bus object data structure */
	hashmap_init(&state.dbus_objects, hashmap_hash_string,
	    hashmap_compare_string, 0);
	/* Load GATT service support */
	bt_gatt_init();
	/* Connect to D-Bus */
	return dbus_client_init(state.file_events, state.timers);
}

/*
 * Free resources associated with the Bluetooth interface.
 */
int bt_cleanup(void)
{
	/* Disconnect from D-Bus */
	dbus_client_cleanup();
	/* Unload the GATT database */
	bt_gatt_cleanup();
	/* Free the D-Bus object data structure */
	hashmap_destroy(&state.dbus_objects);
	return 0;
}

/*
 * Start the Bluetooth interface.
 */
int bt_start(struct bt_callbacks *callbacks)
{
	/* Initialize the Bluetooth driver interface */
	if (bt_init(app_get_file_events(), app_get_timers(),
	    callbacks) < 0) {
		log_err("bt_init returned error");
		return -1;
	}

	log_info("starting Bluetooth interface");

	/* XXX print all received messages */
	/*
	if (log_debug_enabled()) {
		dbus_client_msg_debug_enable(true);
	}
	 */

	/* Query and subscribe to Bluetooth service */
	bt_dbus_service_subscribe(BT_DBUS_SERVICE_BLUEZ, "/");
	/* Start node monitoring routine */
	timer_set(state.timers, &state.monitor_timer,
	    BT_MONITOR_POLL_PERIOD_MS);
	return 0;
}

/*
 * Stop the Bluetooth interface.
 */
int bt_stop(void)
{
	struct hashmap_iter *iter;

	log_info("stopping Bluetooth interface");

	timer_cancel(state.timers, &state.monitor_timer);

	/* Stop discovery when not active */
	bt_discover(false);

	/* Remove D-Bus handlers */
	bt_dbus_service_signal_unsubscribe();

	/* Free all D-Bus objects */
	iter = hashmap_iter(&state.dbus_objects);
	while (iter) {
		bt_dbus_obj_free(bt_dbus_obj_hashmap_iter_get_data(iter));
		iter = hashmap_iter_remove(&state.dbus_objects, iter);
	}
	return 0;
}

/*
 * Enables or disables scanning for discoverable Bluetooth devices.
 */
int bt_discover(bool enable)
{
	DBusMessage *msg;
	struct bt_dbus_obj *obj;
	struct bt_adapter *adapter;
	struct hashmap_iter *iter;
	const char *method = enable ? "StartDiscovery" : "StopDiscovery";
	int rc = -1;

	if (enable == state.discovery_enabled) {
		return 0;
	}
	/* TODO enable discovery filter for BLE only devices ? */

	/* Enable/disable discovery on all adapters */
	for (iter = hashmap_iter(&state.dbus_objects); iter;
	    iter = hashmap_iter_next(&state.dbus_objects, iter)) {
		obj = bt_dbus_obj_hashmap_iter_get_data(iter);
		if (!obj) {
			continue;
		}
		adapter = bt_adapter_get(obj);
		if (!adapter || adapter->discovering == enable) {
			continue;
		}
		msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
		    obj->path, bt_dbus_interface_strs[DBUS_IFACE_ADAPTER],
		    method);
		if (!msg) {
			log_err("message allocation failed");
			return -1;
		}
		if (dbus_client_send(msg) < 0) {
			log_err("D-Bus send failed");
		} else {
			log_debug("sent %s to %s", method, obj->path);
			rc = 0;
		}
		dbus_message_unref(msg);
	}
	if (!rc) {
		log_debug("discovery %s", enable ? "enabled" : "disabled");
		state.discovery_enabled = enable;
	}
	return rc;
}

/*
 * Returns true if discovery is currently enabled.
 */
bool bt_discovery_running(void)
{
	return state.discovery_enabled;
}

static void bt_node_connect_complete(const char *addr,
	enum node_network_result result)
{
	void (*complete)(const char *, enum node_network_result,
	    enum bt_dev_op_err_code, void *);
	void *complete_arg;

	log_debug("result %d, connect.status %d",
	    result, state.connect.status);

	if (state.connect.status == BT_CONNECT_READY) {
		return;
	}
	state.connect.status = BT_CONNECT_READY;
	state.connect.obj = NULL;
	if (state.connect.complete) {
		complete = state.connect.complete;
		complete_arg = state.connect.complete_arg;
		state.connect.complete = NULL;
		state.connect.complete_arg = NULL;
		complete(addr, result, BT_DEV_OP_CONNECT_SUCCESS,
		    complete_arg);
	}
}

/*
 * Begins the pairing process with the specified Bluetooth device.
 */
enum bt_dev_op_err_code bt_node_connect(const char *addr,
	void (*callback)(const char *, enum node_network_result,
	    enum bt_dev_op_err_code, void *),
	void *arg)
{
	struct bt_dbus_obj *obj;
	struct bt_device *device = NULL;
	struct hashmap_iter *iter;
	int rc;

	if (state.connect.status != BT_CONNECT_READY) {
		log_warn("pairing already in progress");
		return BT_DEV_OP_IN_PROGRESS;
	}
	/* Lookup device in object list */
	for (iter = hashmap_iter(&state.dbus_objects); iter;
	    iter = hashmap_iter_next(&state.dbus_objects, iter)) {
		obj = bt_dbus_obj_hashmap_iter_get_data(iter);
		if (!obj) {
			continue;
		}
		device = bt_device_get(obj);
		if (!device) {
			continue;
		}
		if (strcmp(addr, device->addr)) {
			device = NULL;
			continue;
		}
		break;
	}
	if (!device) {
		log_warn("unknown device: %s", addr);
		return BT_DEV_OP_NO_DEVICE;
	}
	rc = bt_device_pair(obj, device);
	if (rc < 0) {
		log_err("D-Bus send failed");
		return BT_DEV_OP_UNKNOWN_ERROR;
	}
	if (rc > 0) {
		log_debug("device already connected: %s", addr);
		if (callback) {
			callback(addr, NETWORK_SUCCESS,
			    BT_DEV_OP_ALREADY_DONE, arg);
		}
		return BT_DEV_OP_ALREADY_DONE;
	}
	log_debug("pairing with device: %s", addr);
	state.connect.obj = obj;
	state.connect.status = BT_CONNECT_IN_PROG;
	state.connect.complete = callback;
	state.connect.complete_arg = arg;
	return BT_DEV_OP_CONNECT_SUCCESS;
}

/*
 * Cancels an ongoing pairing attempt.
 */
void bt_node_connect_cancel(const char *addr)
{
	DBusMessage *msg;
	int rc;

	if (state.connect.status == BT_CONNECT_READY) {
		return;
	}
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    state.connect.obj->path, bt_dbus_interface_strs[DBUS_IFACE_DEVICE],
	    "CancelPairing");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}
	rc = dbus_client_send(msg);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	} else {
		log_debug("pairing attempt canceled");
	}
	bt_node_connect_complete(addr, NETWORK_UNKNOWN);
}

/*
 * Begins the disconnect process with the specified Bluetooth device.
 */
enum bt_dev_op_err_code bt_node_disconnect(const char *addr,
	void (*callback)(const char *, enum node_network_result, void *),
	void *arg)
{
	struct node *node;

	node = node_lookup(addr);
	if (!node) {
		log_debug("no node with addr: %s", addr);
		return BT_DEV_OP_NO_NODE;
	}

	log_debug("node: %s", node->addr);
	if (disconnect_cb) {
		if (!strcmp(disconnect_addr, addr)) {
			log_debug("node addr: %s is disconnecting", addr);
			return BT_DEV_OP_IN_PROGRESS;
		}
	}

	/* Save disconnect callback function and argument */
	disconnect_cb = callback;
	disconnect_arg = arg;
	strcpy(disconnect_addr, node->addr);

	if (bt_leave_handler(node, NULL) < 0) {
		log_debug("bt_leave_handler node: %s error", node->addr);
		disconnect_cb = NULL;
		disconnect_arg = NULL;
		memset(disconnect_addr, 0, sizeof(disconnect_addr));
		return BT_DEV_OP_UNKNOWN_ERROR;
	}
	/*node_remove(node);*/

	return BT_DEV_OP_CONNECT_SUCCESS;
}

/*
 * Check bulb node if got characteristic properties.
 */
bool bt_node_check_bulb_prop(struct node *nd)
{
	if (node_prop_lookup(nd, BT_GATT_SUBDEVICE_DEFAULT,
	    BT_GATT_BULB_TEMPLATE, "onoff")) {
		return true;
	}
	return false;
}

