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
#include <arpa/inet.h>

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

#include "dbus_utils.h"
#include "dbus_client.h"
#include "gatt_service.h"

#include "../wifi.h"
#include "../wifi_platform.h"

/* Name of BlueZ service on D-Bus interface */
#define BT_DBUS_SERVICE_BLUEZ		"org.bluez"

#define BT_GATT_MGR_IFACE               "org.bluez.GattManager1"
#define BT_GATT_SERV_IFACE              "org.bluez.GattService1"
#define BT_GATT_CHR_IFACE               "org.bluez.GattCharacteristic1"
#define BT_GATT_DESP_IFAC               "org.bluez.GattDescriptor1"
#define BT_GATT_AGT_MGR_IFACE           "org.bluez.AgentManager1"
#define BT_GATT_AGT_IFACE               "org.bluez.Agent1"
#define BT_GATT_APT_IFACE               "org.bluez.Adapter1"
#define BT_GATT_PROP_IFACE              "org.freedesktop.DBus.Properties"
#define BT_GATT_PROF_IFACE              "org.bluez.GattProfile1"
#define BT_GATT_SERV_IFACE              "org.bluez.GattService1"
#define BT_GATT_CHRC_IFACE              "org.bluez.GattCharacteristic1"
#define BT_GATT_ADV_MGR_IFACE           "org.bluez.LEAdvertisingManager1"
#define BT_GATT_OBJ_MGR_IFACE           "org.freedesktop.DBus.ObjectManager"
#define BT_GATT_DEV_IFACE               "org.bluez.Device1"
#define BT_DBUS_ADV_INTERFACE	        "org.bluez.LEAdvertisement1"

#define BT_GATT_AYLA_PATH_PREF          "/ayla"
#define BT_GATT_AGENT_PATH              BT_GATT_AYLA_PATH_PREF"/agent1"
#define BT_DBUS_ADV_OBJ_PATH	        BT_GATT_AYLA_PATH_PREF"/advertisement1"
#define BT_GATT_APP_PATH                "/"
#define BT_GATT_APT_PATH                "/org/bluez/hci0"
#define BT_GATT_OBJ_MGR_PATH            "/"

#define BT_GATT_OBJ_PATH                BT_GATT_AYLA_PATH_PREF"/app"
#define BT_GATT_AYLA_OBJ_PATH           BT_GATT_OBJ_PATH"/serv_ayla"
#define BT_GATT_AYLA_DSN_OBJ_PATH       BT_GATT_AYLA_OBJ_PATH"/dsn"
#define BT_GATT_AYLA_DUID_OBJ_PATH      BT_GATT_AYLA_OBJ_PATH"/duid"

#define BT_GATT_CONF_OBJ_PATH           BT_GATT_OBJ_PATH"/serv_conf"
#define BT_GATT_CONF_CONN_OBJ_PATH      BT_GATT_CONF_OBJ_PATH"/connect"
#define BT_GATT_CONF_STAT_OBJ_PATH      BT_GATT_CONF_OBJ_PATH"/state"
#define BT_GATT_CONF_SCAN_OBJ_PATH      BT_GATT_CONF_OBJ_PATH"/scan"
#define BT_GATT_CONF_RESULT_OBJ_PATH    BT_GATT_CONF_OBJ_PATH"/result"

#define BT_GATT_CONN_OBJ_PATH           BT_GATT_OBJ_PATH"/serv_conn"
#define BT_GATT_CONN_SETUP_OBJ_PATH     BT_GATT_CONN_OBJ_PATH"/setup"

#define BT_GATT_AYLA_SERVICE_UUID       "0000FE28-0000-1000-8000-00805F9B34FB"
#define BT_GATT_AYLA_DSN_CHRC_UUID      "00000001-FE28-435B-991A-F1B21BB9BCD0"
#define BT_GATT_AYLA_DUID_CHRC_UUID     "00000002-FE28-435B-991A-F1B21BB9BCD0"
#define BT_GATT_CONF_SERVICE_UUID       "1CF0FE66-3ECF-4D6E-A9FC-E287AB124B96"
#define BT_GATT_CONF_CONN_CHRC_UUID     "1F80AF6A-2B71-4E35-94E5-00F854D8F16F"
#define BT_GATT_CONF_STAT_CHRC_UUID     "1F80AF6C-2B71-4E35-94E5-00F854D8F16F"
#define BT_GATT_CONF_SCAN_CHRC_UUID     "1F80AF6D-2B71-4E35-94E5-00F854D8F16F"
#define BT_GATT_CONF_RESULT_CHRC_UUID   "1F80AF6E-2B71-4E35-94E5-00F854D8F16F"
#define BT_GATT_CONN_SERVICE_UUID       "FCE3EC41-59B6-4873-AE36-FAB25BD59ADC"
#define BT_GATT_CONN_SETUP_CHRC_UUID    "7E9869ED-4DB3-4520-88EA-1C21EF1BA834"

/* GATT Characteristic Properties Bitfield values */
#define BT_GATT_CHRC_PROP_BROADCAST			0x01
#define BT_GATT_CHRC_PROP_READ				0x02
#define BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP		0x04
#define BT_GATT_CHRC_PROP_WRITE				0x08
#define BT_GATT_CHRC_PROP_NOTIFY			0x10
#define BT_GATT_CHRC_PROP_INDICATE			0x20
#define BT_GATT_CHRC_PROP_AUTH				0x40
#define BT_GATT_CHRC_PROP_EXT_PROP			0x80

#define BT_GATT_CHRC_R          BT_GATT_CHRC_PROP_READ
#define BT_GATT_CHRC_W          BT_GATT_CHRC_PROP_WRITE
#define BT_GATT_CHRC_N          BT_GATT_CHRC_PROP_NOTIFY
#define BT_GATT_CHRC_RW         (BT_GATT_CHRC_R | BT_GATT_CHRC_W)
#define BT_GATT_CHRC_RN         (BT_GATT_CHRC_R | BT_GATT_CHRC_N)
#define BT_GATT_CHRC_WN         (BT_GATT_CHRC_W | BT_GATT_CHRC_N)
#define BT_GATT_CHRC_RWN        (BT_GATT_CHRC_RW | BT_GATT_CHRC_N)


#define BT_AYLA_DSN_LEN         15
#define BT_ADDR_LEN             17
#define BT_PATH_LEN             64

#define BT_UPDATE_ADV_DELAY_MS	1000
#define BT_DEFAULT_DELAY_MS	1000
#define BT_WAIT_DELAY_MS	10000

#define BT_LOG_BUF_LEN          128


struct ad_data {
	uint8_t data[25];
	uint8_t len;
};

struct service_data {
	char *uuid;
	struct ad_data data;
};

struct manufacturer_data {
	uint16_t id;
	struct ad_data data;
};

struct data {
	uint8_t type;
	struct ad_data data;
};

static char *service_uuids[] = {
	"1CF0FE66-3ECF-4D6E-A9FC-E287AB124B96",
	NULL
};

static struct adv_data {
	bool registered;
	char *type;
	char *local_name;
	uint16_t local_appearance;
	uint16_t duration;
	uint16_t timeout;
	uint16_t discoverable_to;
	char **uuids;
	size_t uuids_len;
	struct service_data service;
	struct manufacturer_data manufacturer;
	struct data data;
	bool discoverable;
	bool tx_power;
	bool name;
	bool appearance;
} advmt = {
	.type = "peripheral",
	.uuids = service_uuids,
	.uuids_len = 1,
	.discoverable = false
};

static bool bt_adv_enable_flag;

enum bt_state_en {
	BT_INIT          = 0,
	BT_POWER_ON      = 1,
	BT_REG_AGENT     = 2,
	BT_REG_DEF_AGENT = 3,
	BT_GET_LOC_ADDR  = 4,
	BT_SIG_SUBSCRIBE = 5,
	BT_REQ_MGR_OBJ   = 6,
	BT_WAITING       = 7,
	BT_REG_APP_PATH  = 8,
	BT_REG_APP       = 9,
	BT_REG_ADV_PATH  = 10,
	BT_REG_ADV       = 11,
	BT_READY         = 12
};

/*
 * Bluetooth interface state
 */
struct bt_state_st {
	struct file_event_table *file_events;
	struct timer_head *timers;
	struct dbus_client_msg_handler *added_handler;
	struct dbus_client_msg_handler *removed_handler;
	enum bt_state_en state;
	char *bus_name;
};

static struct bt_state_st bt_state;

/*
 * dbus device structure
 */
struct bt_dbus_device {
	STAILQ_ENTRY(bt_dbus_device) link;
	struct dbus_client_msg_handler *prop_changed_handler;
	char *path;
};

static STAILQ_HEAD(, bt_dbus_device) deviceq;


struct connect_st {
	uint8_t ssid[WIFI_SSID_LEN];
	uint8_t ssid_len;
	uint8_t bssid[ETH_ALEN];
	uint8_t key[WIFI_MAX_KEY_LEN];
	uint8_t key_len;
	/* 0 OPEN, 1 WEP, 2 WPA, 3 WPA2-personal */
	uint8_t security;
} PACKED;

enum state_en {
	/* Current Wi-Fi state:
	0x0: N/A
	0x1: Disabled
	0x2: Connecting to Wi-Fi
	0x3: Connecting to Network (Obtaining IP address)
	0x4: Connecting to the Cloud
	0x5: Up/Connected
	*/
	STATE_NONE = 0,
	STATE_DISABLE = 1,
	STATE_CONNECTING_WIFI = 2,
	STATE_CONNECTING_NET = 3,
	STATE_CONNECTING_CLOUD = 4,
	STATE_NET_UP = 5,
};

struct status_st {
	uint8_t ssid[WIFI_SSID_LEN];
	uint8_t ssid_len;
	uint8_t error;
	/* Current Wi-Fi state:
	0x0: N/A
	0x1: Disabled
	0x2: Connecting to Wi-Fi
	0x3: Connecting to Network (Obtaining IP address)
	0x4: Connecting to the Cloud
	0x5: Up/Connected
	*/
	uint8_t state;
} PACKED;

struct scan_result_st {
	uint8_t index;
	uint8_t ssid[WIFI_SSID_LEN];
	uint8_t ssid_len;
	uint8_t bssid[ETH_ALEN];
	int16_t rssi;
	/* 0 OPEN, 1 WEP, 2 WPA, 3 WPA2-personal */
	uint8_t security;
} PACKED;

static uint8_t bt_gatt_dsn[BT_AYLA_DSN_LEN + 1];
static uint8_t bt_gatt_duid[BT_ADDR_LEN + 1];
static struct connect_st bt_gatt_conn;
static struct status_st bt_gatt_conn_stat;
static bool bt_gatt_conn_stat_notify;
static uint8_t bt_gatt_scan;
static struct scan_result_st bt_gatt_scan_resu;
static bool bt_gatt_scan_resu_notify;
static uint8_t bt_gatt_setup_token[WIFI_SETUP_TOKEN_LEN + 1];

static uint8_t bt_adv_name[BT_ADDR_LEN + 1];

static struct timer bt_adv_timer;


/* Timer for state change handling */
static struct timer bt_step_timer;



/*
 * Init device queue.
 */
void bt_device_queue_init(void)
{
	STAILQ_INIT(&deviceq);
}

/*
 *  Clean up device queue.
 */
void bt_device_queue_cleanup(void)
{
	struct bt_dbus_device *device;

	if (STAILQ_EMPTY(&deviceq)) {
		return;
	}

	STAILQ_FOREACH(device, &deviceq, link) {
		STAILQ_REMOVE(&deviceq, device, bt_dbus_device, link);
		if (device->prop_changed_handler) {
			dbus_client_msg_handler_remove(
			    device->prop_changed_handler);
		}
		free(device->path);
		free(device);
	}
}

void bt_ad_add_string_item(DBusMessageIter *array,
		const char *item, const char *value)
{
	DBusMessageIter dict, vari;
	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &item);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "s", &vari);
	dbus_message_iter_append_basic(&vari, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

void bt_ad_add_bool_item(DBusMessageIter *array,
		const char *item, const bool value)
{
	DBusMessageIter dict, vari;
	dbus_bool_t bvalue = value;
	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &item);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "b", &vari);
	dbus_message_iter_append_basic(&vari, DBUS_TYPE_BOOLEAN, &bvalue);
	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

void bt_ad_add_uuids_item(DBusMessageIter *array,
		const char *item, char **uuids, size_t uuids_len)
{
	DBusMessageIter dict, vari, sub_array;
	size_t i;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &item);
	dbus_message_iter_open_container(&dict,
	    DBUS_TYPE_VARIANT, "as", &vari);

	dbus_message_iter_open_container(&vari,
	    DBUS_TYPE_ARRAY, "as", &sub_array);
	for (i = 0; i < uuids_len; i++) {
		dbus_message_iter_append_basic(&sub_array,
		    DBUS_TYPE_STRING, &uuids[i]);
	}
	dbus_message_iter_close_container(&vari, &sub_array);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

/*
 * Handle incoming method calls for the advertisement.
 */
static void bt_ad_method_handler(DBusMessage *msg, void *arg)
{
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	const char *method;
	const char *path;

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
	if (!strcmp(method, "GetAll")) {
		DBusMessageIter args, array;

		path = dbus_utils_parse_string(&iter);
		if (!path) {
			log_err("missing UUID");
			goto reply;
		}
		log_debug("GetAll path %s", path);
		reply = dbus_message_new_method_return(msg);

		dbus_message_iter_init_append(reply, &args);
		dbus_message_iter_open_container(&args,
		    DBUS_TYPE_ARRAY, "{sv}", &array);

		bt_ad_add_string_item(&array, "Type", advmt.type);
		bt_ad_add_string_item(&array, "LocalName", advmt.local_name);
		bt_ad_add_uuids_item(&array,
		    "ServiceUUIDs", advmt.uuids, advmt.uuids_len);
		bt_ad_add_bool_item(&array,
		    "Discoverable", advmt.discoverable);

		dbus_message_iter_close_container(&args, &array);
	} else if (!strcmp(method, "Release")) {
		dbus_client_new_path_unregister(BT_DBUS_ADV_OBJ_PATH);
		return;
	} else {
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
	if (reply) {
		dbus_message_unref(reply);
	}
}

static void bt_ad_add_string_property_item(DBusMessageIter *array,
		char *name, char *value)
{
	DBusMessageIter dict, vari;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &name);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
	    DBUS_TYPE_STRING_AS_STRING, &vari);
	dbus_message_iter_append_basic(&vari, DBUS_TYPE_STRING, &value);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_strings_property_item(DBusMessageIter *array,
		char *name, char *value)
{
	DBusMessageIter dict, vari, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
	    DBUS_TYPE_ARRAY_AS_STRING
	    DBUS_TYPE_STRING_AS_STRING, &vari);

	dbus_message_iter_open_container(&vari,
	    DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &sub_array);
	dbus_message_iter_append_basic(&sub_array, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&vari, &sub_array);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_path_property_item(DBusMessageIter *array,
		char *name, char *value)
{
	DBusMessageIter dict, vari;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &name);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
	    DBUS_TYPE_OBJECT_PATH_AS_STRING, &vari);
	dbus_message_iter_append_basic(&vari,
	    DBUS_TYPE_OBJECT_PATH, &value);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_bool_property_item(DBusMessageIter *array,
		char *name, bool value)
{
	DBusMessageIter dict, vari;
	dbus_bool_t bvalue = value;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &name);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
	    DBUS_TYPE_BOOLEAN_AS_STRING, &vari);
	dbus_message_iter_append_basic(&vari, DBUS_TYPE_BOOLEAN, &bvalue);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_bytes_property_item(DBusMessageIter *array,
		char *name, uint8_t *value, uint16_t len)
{
	DBusMessageIter dict, vari, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
	    DBUS_TYPE_ARRAY_AS_STRING
	    DBUS_TYPE_BYTE_AS_STRING, &vari);

	dbus_message_iter_open_container(&vari,
	    DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &sub_array);
	dbus_message_iter_append_fixed_array(&sub_array,
	    DBUS_TYPE_BYTE, &value, len);
	dbus_message_iter_close_container(&vari, &sub_array);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_flag_property_item(DBusMessageIter *array,
		char *name, uint8_t flag)
{
	struct chrc_prop_data {
		uint8_t prop;
		char *str;
	};
	struct chrc_prop_data chrc_props[] = {
		/* Default Properties */
		{ BT_GATT_CHRC_PROP_BROADCAST, "broadcast" },
		{ BT_GATT_CHRC_PROP_READ, "read" },
		{ BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
		    "write-without-response" },
		{ BT_GATT_CHRC_PROP_WRITE, "write" },
		{ BT_GATT_CHRC_PROP_NOTIFY, "notify" },
		{ BT_GATT_CHRC_PROP_INDICATE, "indicate" },
		{ BT_GATT_CHRC_PROP_AUTH, "authenticated-signed-writes" },
		{ BT_GATT_CHRC_PROP_EXT_PROP, "extended-properties" }
	};
	DBusMessageIter dict, vari, sub_array;
	int i;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
	    DBUS_TYPE_ARRAY_AS_STRING
	    DBUS_TYPE_STRING_AS_STRING, &vari);

	dbus_message_iter_open_container(&vari,
	    DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &sub_array);

	for (i = 0; i < ARRAY_LEN(chrc_props); i++) {
		if (flag & chrc_props[i].prop) {
			dbus_message_iter_append_basic(&sub_array,
			    DBUS_TYPE_STRING, &chrc_props[i].str);
		}
	}

	dbus_message_iter_close_container(&vari, &sub_array);

	dbus_message_iter_close_container(&dict, &vari);
	dbus_message_iter_close_container(array, &dict);
}


static void bt_ad_add_interface_no_prop(
	DBusMessageIter *array, char *pinterface)
{
	DBusMessageIter dict, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_STRING, &pinterface);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);
	dbus_message_iter_close_container(&dict, &sub_array);

	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_root_interface(DBusMessageIter *array,
	char *pInterface, char *uuids)
{
	DBusMessageIter dict, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_STRING, &pInterface);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);

	bt_ad_add_strings_property_item(&sub_array, "UUIDs", uuids);

	dbus_message_iter_close_container(&dict, &sub_array);
	dbus_message_iter_close_container(array, &dict);
}



void bt_ad_add_root_obj_path(DBusMessageIter *array)
{
	DBusMessageIter dict, sub_array;
	char *path = BT_GATT_OBJ_PATH;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_ARRAY_AS_STRING
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);

	bt_ad_add_interface_no_prop(&sub_array,
	    "org.freedesktop.DBus.Introspectable");
	bt_ad_add_interface_no_prop(&sub_array,
	    "org.freedesktop.DBus.Properties");
	bt_ad_add_root_interface(&sub_array,
	    "org.bluez.GattProfile1", "Ayla");

	dbus_message_iter_close_container(&dict, &sub_array);

	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_service_interface(DBusMessageIter *array,
	char *pInterface, char *uuid, bool primary)
{
	DBusMessageIter dict, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_STRING, &pInterface);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);

	bt_ad_add_string_property_item(&sub_array, "UUID", uuid);
	bt_ad_add_bool_property_item(&sub_array, "Primary", primary);

	dbus_message_iter_close_container(&dict, &sub_array);
	dbus_message_iter_close_container(array, &dict);
}

void bt_ad_add_service_obj_path(DBusMessageIter *array,
	char *path, char *uuid, bool primary)
{
	DBusMessageIter dict, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_ARRAY_AS_STRING
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);

	bt_ad_add_interface_no_prop(&sub_array,
	    "org.freedesktop.DBus.Introspectable");
	bt_ad_add_interface_no_prop(&sub_array,
	    "org.freedesktop.DBus.Properties");
	bt_ad_add_service_interface(&sub_array,
	    "org.bluez.GattService1", uuid, primary);

	dbus_message_iter_close_container(&dict, &sub_array);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_ad_add_chrc_interface(DBusMessageIter *array,
	char *pInterface, char *uuid, char *spath,
	uint8_t *value, uint16_t len, uint8_t flag)
{
	DBusMessageIter dict, sub_array;
	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_STRING, &pInterface);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);

	bt_ad_add_string_property_item(&sub_array, "UUID", uuid);
	bt_ad_add_path_property_item(&sub_array, "Service", spath);
	bt_ad_add_bool_property_item(&sub_array, "Notifying", false);
	bt_ad_add_bool_property_item(&sub_array, "NotifyAcquired", false);
	bt_ad_add_bytes_property_item(&sub_array, "Value", value, len);
	bt_ad_add_flag_property_item(&sub_array, "Flags", flag);

	dbus_message_iter_close_container(&dict, &sub_array);
	dbus_message_iter_close_container(array, &dict);
}

void bt_ad_add_chrc_obj_path(DBusMessageIter *array,
	char *path, char *uuid, char *spath,
	uint8_t *value, uint16_t len, uint8_t flag)
{
	DBusMessageIter dict, sub_array;

	dbus_message_iter_open_container(array,
	    DBUS_TYPE_DICT_ENTRY, NULL, &dict);
	dbus_message_iter_append_basic(&dict,
	    DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_ARRAY_AS_STRING
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&sub_array);

	bt_ad_add_interface_no_prop(&sub_array,
	    "org.freedesktop.DBus.Introspectable");
	bt_ad_add_interface_no_prop(&sub_array,
	    "org.freedesktop.DBus.Properties");

	bt_ad_add_chrc_interface(&sub_array,
	    "org.bluez.GattCharacteristic1", uuid, spath, value, len, flag);

	dbus_message_iter_close_container(&dict, &sub_array);
	dbus_message_iter_close_container(array, &dict);
}

static void bt_gatt_send_prop_change_signal(const char *path,
		uint8_t *value, int len)
{
	DBusMessage *signal = NULL;
	DBusMessageIter iter, array;
	char *ptr = BT_GATT_CHRC_IFACE;
	int ret;

	log_debug("Sending PropertiesChanged signal on %s", path);

	signal = dbus_message_new_signal(path,
	    BT_GATT_PROP_IFACE, "PropertiesChanged");
	if (signal == NULL) {
		log_err("Unable to allocate new msg for %s"
		    ".PropertiesChanged signal", path);
		return;
	}

	dbus_message_iter_init_append(signal, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,	&ptr);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		&array);

	bt_ad_add_bytes_property_item(&array, "Value", value, len);

	dbus_message_iter_close_container(&iter, &array);

	ret = dbus_client_send(signal);
	if (ret < 0) {
		log_err("dbus_client_send PropertiesChanged signal on %s"
		    "failed", path);
		return;
	}
	dbus_message_unref(signal);
}

static struct wifi_scan_result scan_result[WIFI_SCAN_CT];
static int scan_result_count;
static int scan_result_get;

void bt_start_scan(void)
{
	memset(&scan_result, 0, sizeof(scan_result));
	scan_result_count = 0;
	scan_result_get = 0;
	log_debug("Cleared scan results");
	wifi_scan();
}

static void bt_set_scan_result(int get_index, struct wifi_scan_result *results)
{
	const struct wifi_scan_result *scan;
	enum wifi_sec sec;
	char logbuf[BT_LOG_BUF_LEN];

	if (!results) {
		log_err("input parameter results error");
		return;
	}
	scan = &results[get_index];

	memset(&bt_gatt_scan_resu, 0, sizeof(bt_gatt_scan_resu));

	bt_gatt_scan_resu.index = get_index;

	memcpy(bt_gatt_scan_resu.ssid, scan->ssid.val,
	    scan->ssid.len);
	bt_gatt_scan_resu.ssid_len = scan->ssid.len;

	memcpy(bt_gatt_scan_resu.bssid, &scan->bssid.ether_addr_octet,
	    ETH_ALEN);
	bt_gatt_scan_resu.rssi = scan->signal;

	sec = wifi_scan_get_best_security(scan, NULL);
	switch (sec & WSEC_SEC_MASK) {
	case WSEC_WEP:
		bt_gatt_scan_resu.security = 1;
		break;
	case WSEC_WPA:
		bt_gatt_scan_resu.security = 2;
		break;
	case WSEC_WPA2:
		bt_gatt_scan_resu.security = 3;
		break;
	default:
		bt_gatt_scan_resu.security = 0;
		break;
	}

	memset(&logbuf, 0, sizeof(logbuf));
	sprintf(logbuf, "index %d, ssid ",
	    bt_gatt_scan_resu.index);
	strncpy(logbuf + strlen(logbuf),
	    (char *)bt_gatt_scan_resu.ssid, bt_gatt_scan_resu.ssid_len);
	sprintf(logbuf + strlen(logbuf), ", rssi %d, security %d",
	    bt_gatt_scan_resu.rssi, bt_gatt_scan_resu.security);
	log_debug("set scan result %s", logbuf);
}

/*
 * Update scan result GATT chrc value.
 */
void bt_update_scan_results(void)
{
	if (!scan_result_get) {
		if (!scan_result_count) {
			scan_result_count = wifi_get_scan_results(
			    scan_result, WIFI_SCAN_CT);
			log_debug("got scan result %d", scan_result_count);
		}
		if (scan_result_count) {
			bt_set_scan_result(scan_result_get, scan_result);
			scan_result_get++;
		}
	} else if (scan_result_get < scan_result_count) {
		bt_set_scan_result(scan_result_get, scan_result);
		scan_result_get++;
	}
}

/*
 * Send scan results.
 */
void bt_send_scan_results(void)
{
	struct wifi_scan_result results[WIFI_SCAN_CT];
	int result_count = 0;
	int i;

	memset(&results, 0, sizeof(results));
	result_count = wifi_get_scan_results(results, WIFI_SCAN_CT);
	log_debug("got scan result count %d", result_count);
	for (i = 0; i < result_count; i++) {
		bt_set_scan_result(i, results);
		/* rssi converts as network byte order */
		bt_gatt_scan_resu.rssi = htons(bt_gatt_scan_resu.rssi);
		bt_gatt_send_prop_change_signal(
		    BT_GATT_CONF_RESULT_OBJ_PATH,
		    (uint8_t *)&bt_gatt_scan_resu,
		    sizeof(bt_gatt_scan_resu));
		/* rssi converts back to host byte order */
		bt_gatt_scan_resu.rssi = ntohs(bt_gatt_scan_resu.rssi);
	}

	/* Send a empty record to tell Mobile App send finished */
	memset(&bt_gatt_scan_resu, 0, sizeof(bt_gatt_scan_resu));
	bt_gatt_send_prop_change_signal(
	    BT_GATT_CONF_RESULT_OBJ_PATH,
	    (uint8_t *)&bt_gatt_scan_resu,
	    sizeof(bt_gatt_scan_resu));
}

void bt_scan_complete(void)
{
	log_debug("bt_gatt_scan_resu_notify %d, bt_adv_enable_flag %d",
	    bt_gatt_scan_resu_notify, bt_adv_enable_flag);
	if (bt_gatt_scan_resu_notify && bt_adv_enable_flag) {
		bt_send_scan_results();
	}
}

static void bt_update_connect_state(void)
{
	enum state_en state;

	if (!wifi_state.curr_hist) {
		log_err("no current history entry");
		return;
	}

	memset(&bt_gatt_conn_stat, 0, sizeof(bt_gatt_conn_stat));

	memcpy(bt_gatt_conn_stat.ssid, wifi_state.curr_hist->ssid.val,
	    WIFI_SSID_LEN);
	bt_gatt_conn_stat.ssid_len = wifi_state.curr_hist->ssid.len;
	bt_gatt_conn_stat.error = wifi_state.curr_hist->error;

	switch (wifi_state.state) {
	case WS_DISABLED:
		state = STATE_DISABLE;
		break;
	case WS_SELECT:
		state = STATE_NONE;
		break;
	case WS_IDLE:
		state = STATE_NONE;
		break;
	case WS_JOIN:
		state = STATE_CONNECTING_WIFI;
		break;
	case WS_DHCP:
		state = STATE_CONNECTING_NET;
		break;
	case WS_WAIT_CLIENT:
		state = STATE_CONNECTING_CLOUD;
		break;
	case WS_UP:
		state = STATE_NET_UP;
		break;
	case WS_ERR:
		state = STATE_NONE;
		break;
	default:
		state = STATE_NONE;
		break;
	}

	bt_gatt_conn_stat.state = state;

	log_debug("ssid %s, ssid_len %d, error %d, state %d",
	    bt_gatt_conn_stat.ssid, bt_gatt_conn_stat.ssid_len,
	    bt_gatt_conn_stat.error, bt_gatt_conn_stat.state);
}

static void bt_clear_conn_status(void)
{
	memset(&bt_gatt_conn_stat, 0, sizeof(bt_gatt_conn_stat));
	log_debug("Cleared connect status");
}

/*
 * Handle WiFi connect state change event
 */
void bt_connect_state_change(void)
{
	bt_update_connect_state();

	log_debug("bt_gatt_conn_stat_notify %d", bt_gatt_conn_stat_notify);
	if (bt_gatt_conn_stat_notify) {
		log_debug("path %s value ssid %s, error %u, state %u",
		    BT_GATT_CONF_STAT_OBJ_PATH,
		    bt_gatt_conn_stat.ssid,
		    bt_gatt_conn_stat.error,
		    bt_gatt_conn_stat.state);
		bt_gatt_send_prop_change_signal(
		    BT_GATT_CONF_STAT_OBJ_PATH,
		    (uint8_t *)&bt_gatt_conn_stat,
		    sizeof(bt_gatt_conn_stat));
	}
}

/*
 * Connect to an AP
 */
static int bt_connect_wifi(struct connect_st *conn)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_scan_result *scan;
	struct wifi_profile *prof;
	enum wifi_sec sec;
	struct wifi_ssid ssid = { 0 };
	struct ether_addr bssid = { { 0 } };
	struct wifi_key key = { 0 };
	bool test_connect = false;
	bool hidden = false;
	ssize_t rc;
	enum wifi_error wifi_err = WIFI_ERR_NONE;

	if (!conn) {
		log_err("input arameter conn is NULL");
		return -1;
	}

	bt_clear_conn_status();

	log_debug("Connecting to an AP %s", conn->ssid);

	memcpy(bssid.ether_addr_octet, conn->bssid, ETH_ALEN);
	memcpy(ssid.val, conn->ssid, WIFI_SSID_LEN);
	ssid.len = conn->ssid_len;
	memcpy(key.val, conn->key, WIFI_MAX_KEY_LEN);
	key.len = conn->key_len;

	/* BSSID is the most specific, so check that first */
	if (!EMPTY_HWADDR(bssid)) {
		scan = wifi_scan_lookup_bssid(wifi, &bssid);
	} else if (ssid.len) {
		scan = wifi_scan_lookup_ssid(wifi, &ssid);
	} else {
		log_err("missing SSID/BSSID");
		return -1;
	}
	if (!scan) {
		log_err("station not found in scan: %s", ssid.len ?
		    wifi_ssid_to_str(&ssid) : net_ether_to_str(&bssid));
		wifi_err = WIFI_ERR_NOT_FOUND;
		goto failed;
	}

	/* Validate key */
	sec = wifi_scan_get_best_security(scan, NULL);
	if (SEC_MATCH(sec, WSEC_NONE)) {
		/* No security available, so make sure the key is empty */
		if (key.len) {
			log_warn("no security available for selected network: "
			    "ignoring key");
			key.len = 0;
		}
	} else {
		if (!key.len) {
			log_err("missing key");
			wifi_err = WIFI_ERR_INV_KEY;
			goto failed;
		}
		/* Convert WEP key hex string to bytes */
		if (SEC_MATCH(sec, WSEC_WEP)) {
			rc = hex_parse_n(key.val, sizeof(key.val),
			    (const char *)key.val, key.len, NULL);
			if (rc < 0) {
				log_warn("failed to parse WEP hex key");
				wifi_err = WIFI_ERR_INV_KEY;
				goto failed;
			}
			key.len = rc;
		}
		/* Re-check security, this time validating the key */
		sec = wifi_scan_get_best_security(scan, &key);
		if (SEC_MATCH(sec, WSEC_NONE)) {
			log_err("key not valid for available security modes");
			wifi_err = WIFI_ERR_INV_KEY;
			goto failed;
		}
	}

	if (test_connect) {
		log_debug("%s connect test passed", wifi_ssid_to_str(&ssid));
		return 0;
	}

	/* Find or add profile */
	prof = wifi_prof_add(&scan->ssid, sec, &key, &wifi_err);
	if (!prof) {
		goto failed;
	}
	prof->scan = scan;
	prof->hidden = hidden;
	log_debug("SSID='%s' BSSID=%s key_len=%u sec=%04x",
	    wifi_ssid_to_str(&ssid), net_ether_to_str(&prof->scan->bssid),
	    key.len, (unsigned)sec);

failed:
	if (wifi_err != WIFI_ERR_NONE) {
		if (scan) {
			wifi_hist_new(&scan->ssid, &scan->bssid, wifi_err,
			    true);
		} else {
			wifi_hist_new(&ssid, NULL, wifi_err, true);
		}
		return -1;
	}

	/* Set preferred profile and associate with it */
	wifi_connect(prof);
	return 0;
}

/*
 * Handle incoming method calls for GATT.
 */
static void bt_gatt_method_handler(DBusMessage *msg, void *arg)
{
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	const char *method;
	const char *path = (const char *)arg;
	const char *dsn;

	if (!msg) {
		log_warn("missing msg");
		return;
	}
	method = dbus_message_get_member(msg);
	if (!method) {
		log_warn("missing method");
		return;
	}
	log_debug("incoming method call %s on path %s from %s",
	    method, path, dbus_message_get_sender(msg));

	dbus_message_iter_init(msg, &iter);

	if (!strcmp(method, "ReadValue")) {
		DBusMessageIter array;
		uint8_t *ptr;

		reply = dbus_message_new_method_return(msg);

		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter,
		    DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &array);

		if (!strcmp(path, BT_GATT_AYLA_DSN_OBJ_PATH)) {
			memset(bt_gatt_dsn, 0, sizeof(bt_gatt_dsn));
			dsn = wifi_get_dsn();
			if (dsn) {
				strncpy((char *)bt_gatt_dsn, dsn,
				    BT_AYLA_DSN_LEN);
			}
			log_debug("path %s value dsn %s",
			    BT_GATT_AYLA_DSN_CHRC_UUID, bt_gatt_dsn);
			ptr = bt_gatt_dsn;
			dbus_message_iter_append_fixed_array(&array,
			    DBUS_TYPE_BYTE,
			    &ptr,
			    BT_AYLA_DSN_LEN);
		} else if (!strcmp(path, BT_GATT_AYLA_DUID_OBJ_PATH)) {
			log_debug("path %s value duid %s",
			    BT_GATT_AYLA_DUID_CHRC_UUID, bt_gatt_duid);
			ptr = bt_gatt_duid;
			dbus_message_iter_append_fixed_array(&array,
			    DBUS_TYPE_BYTE,
			    &ptr,
			    BT_ADDR_LEN);
		} else if (!strcmp(path, BT_GATT_CONF_STAT_OBJ_PATH)) {
			log_debug("path %s value ssid %s, error %u, state %u",
			    BT_GATT_CONF_STAT_CHRC_UUID,
			    bt_gatt_conn_stat.ssid,
			    bt_gatt_conn_stat.error,
			    bt_gatt_conn_stat.state);
			ptr = (uint8_t *)&bt_gatt_conn_stat;
			dbus_message_iter_append_fixed_array(&array,
			    DBUS_TYPE_BYTE,
			    &ptr,
			    sizeof(bt_gatt_conn_stat));
		} else if (!strcmp(path, BT_GATT_CONF_RESULT_OBJ_PATH)) {
			log_debug("read scan results");
			memset(&bt_gatt_scan_resu, 0,
			    sizeof(bt_gatt_scan_resu));
			bt_update_scan_results();
			/* rssi converts as network byte order */
			bt_gatt_scan_resu.rssi = htons(bt_gatt_scan_resu.rssi);
			ptr = (uint8_t *)&bt_gatt_scan_resu;
			dbus_message_iter_append_fixed_array(&array,
			    DBUS_TYPE_BYTE,
			    &ptr,
			    sizeof(bt_gatt_scan_resu));
			/* rssi converts back to host byte order */
			bt_gatt_scan_resu.rssi = ntohs(bt_gatt_scan_resu.rssi);
		} else {
			log_warn("unsupported path: %s", path);
		}
		dbus_message_iter_close_container(&iter, &array);
	} else if (!strcmp(method, "WriteValue")) {
		DBusMessageIter args, array;
		int value_len;
		uint8_t *value;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
			log_err("WriteValue path %s arg error", path);
			goto reply;
		}

		dbus_message_iter_recurse(&iter, &array);
		dbus_message_iter_get_fixed_array(&array, &value, &value_len);

		if (!strcmp(path, BT_GATT_CONF_CONN_OBJ_PATH)) {
			if (value_len > sizeof(bt_gatt_conn)) {
				log_err("WriteValue path %s arg value"
				    " length %d error", path, value_len);
				goto reply;
			}
			memset(&bt_gatt_conn, 0,
			    sizeof(bt_gatt_conn));
			memcpy(&bt_gatt_conn, value, sizeof(bt_gatt_conn));
			log_debug("connect ssid %s, security %d\n",
			    bt_gatt_conn.ssid, bt_gatt_conn.security);
			bt_connect_wifi(&bt_gatt_conn);
		} else if (!strcmp(path, BT_GATT_CONF_SCAN_OBJ_PATH)) {
			if (value_len > sizeof(bt_gatt_scan)) {
				log_err("WriteValue path %s arg value"
				    " length %d error", path, value_len);
				goto reply;
			}
			bt_gatt_scan = value[0];
			if (bt_gatt_scan == '1') {
				log_debug("start wifi scan");
				bt_start_scan();
			}
		} else if (!strcmp(path, BT_GATT_CONN_SETUP_OBJ_PATH)) {
			if (value_len > WIFI_SETUP_TOKEN_LEN) {
				log_err("WriteValue path %s arg value"
				    " length %d error", path, value_len);
				goto reply;
			}
			memset(bt_gatt_setup_token, 0,
			    sizeof(bt_gatt_setup_token));
			memcpy(bt_gatt_setup_token, value, value_len);
			wifi_set_setup_token((char *)bt_gatt_setup_token);
		} else {
			log_warn("unsupported path: %s", path);
		}

		reply = dbus_message_new_method_return(msg);

		dbus_message_iter_init_append(reply, &args);
	} else if (!strcmp(method, "StartNotify")) {
		log_debug("StartNotify method on %s", path);
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &iter);

		if (!strcmp(path, BT_GATT_CONF_RESULT_OBJ_PATH)) {
			bt_gatt_scan_resu_notify = true;
		} else if (!strcmp(path, BT_GATT_CONF_STAT_OBJ_PATH)) {
			bt_gatt_conn_stat_notify = true;
		}
	} else if (!strcmp(method, "StopNotify")) {
		log_debug("StopNotify method on %s", path);
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &iter);

		if (!strcmp(path, BT_GATT_CONF_RESULT_OBJ_PATH)) {
			bt_gatt_scan_resu_notify = false;
		} else if (!strcmp(path, BT_GATT_CONF_STAT_OBJ_PATH)) {
			bt_gatt_conn_stat_notify = false;
		}
	} else {
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
	if (reply) {
		dbus_message_unref(reply);
	}
}

/*
 * Handle incoming method calls for app.
 */
static void bt_app_method_handler(DBusMessage *msg, void *arg)
{
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	const char *method;

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
	if (!strcmp(method, "GetManagedObjects")) {
		DBusMessageIter args, array;

		log_debug("Reply GetManagedObjects method call");
		reply = dbus_message_new_method_return(msg);

		dbus_message_iter_init_append(reply, &args);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY,
		    DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		    DBUS_TYPE_OBJECT_PATH_AS_STRING
		    DBUS_TYPE_ARRAY_AS_STRING
		    DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		    DBUS_TYPE_STRING_AS_STRING
		    DBUS_TYPE_ARRAY_AS_STRING
		    DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		    DBUS_TYPE_STRING_AS_STRING
		    DBUS_TYPE_VARIANT_AS_STRING
		    DBUS_DICT_ENTRY_END_CHAR_AS_STRING
		    DBUS_DICT_ENTRY_END_CHAR_AS_STRING
		    DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
		    &array);

		bt_ad_add_root_obj_path(&array);

		bt_ad_add_service_obj_path(&array,
		    BT_GATT_AYLA_OBJ_PATH, BT_GATT_AYLA_SERVICE_UUID, true);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_AYLA_DSN_OBJ_PATH,
		    BT_GATT_AYLA_DSN_CHRC_UUID, BT_GATT_AYLA_OBJ_PATH,
		    bt_gatt_dsn, BT_AYLA_DSN_LEN,
		    BT_GATT_CHRC_R);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_AYLA_DUID_OBJ_PATH,
		    BT_GATT_AYLA_DUID_CHRC_UUID, BT_GATT_AYLA_OBJ_PATH,
		    bt_gatt_duid, BT_ADDR_LEN,
		    BT_GATT_CHRC_R);

		bt_ad_add_service_obj_path(&array,
		    BT_GATT_CONF_OBJ_PATH, BT_GATT_CONF_SERVICE_UUID, true);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_CONF_CONN_OBJ_PATH,
		    BT_GATT_CONF_CONN_CHRC_UUID, BT_GATT_CONF_OBJ_PATH,
		    (uint8_t *)&bt_gatt_conn, sizeof(bt_gatt_conn),
		    BT_GATT_CHRC_W);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_CONF_STAT_OBJ_PATH,
		    BT_GATT_CONF_STAT_CHRC_UUID, BT_GATT_CONF_OBJ_PATH,
		    (uint8_t *)&bt_gatt_conn_stat, sizeof(bt_gatt_conn_stat),
		    BT_GATT_CHRC_RN);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_CONF_SCAN_OBJ_PATH,
		    BT_GATT_CONF_SCAN_CHRC_UUID, BT_GATT_CONF_OBJ_PATH,
		    &bt_gatt_scan, sizeof(bt_gatt_scan),
		    BT_GATT_CHRC_W);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_CONF_RESULT_OBJ_PATH,
		    BT_GATT_CONF_RESULT_CHRC_UUID, BT_GATT_CONF_OBJ_PATH,
		    (uint8_t *)&bt_gatt_scan_resu, sizeof(bt_gatt_scan_resu),
		    BT_GATT_CHRC_RN);

		bt_ad_add_service_obj_path(&array,
		    BT_GATT_CONN_OBJ_PATH, BT_GATT_CONN_SERVICE_UUID, true);
		bt_ad_add_chrc_obj_path(&array, BT_GATT_CONN_SETUP_OBJ_PATH,
		    BT_GATT_CONN_SETUP_CHRC_UUID, BT_GATT_CONN_OBJ_PATH,
		    bt_gatt_setup_token, WIFI_SETUP_TOKEN_LEN,
		    BT_GATT_CHRC_W);

		dbus_message_iter_close_container(&args, &array);
	}  else {
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
	if (reply) {
		dbus_message_unref(reply);
	}
}

/*
 * Register GATT(APP, WiFi setup service and chrc) object path
 */
static int bt_register_gatt_obj_path(void)
{
	bool fail = false;
	int i;
	char *paths[] = {
		BT_GATT_OBJ_PATH,
		BT_GATT_AYLA_OBJ_PATH,
		BT_GATT_AYLA_DSN_OBJ_PATH,
		BT_GATT_AYLA_DUID_OBJ_PATH,
		BT_GATT_CONF_OBJ_PATH,
		BT_GATT_CONF_CONN_OBJ_PATH,
		BT_GATT_CONF_STAT_OBJ_PATH,
		BT_GATT_CONF_SCAN_OBJ_PATH,
		BT_GATT_CONF_RESULT_OBJ_PATH,
		BT_GATT_CONN_OBJ_PATH,
		BT_GATT_CONN_SETUP_OBJ_PATH
	};
	char *interfaces[] = {
		BT_GATT_PROF_IFACE,
		BT_GATT_SERV_IFACE,
		BT_GATT_CHRC_IFACE,
		BT_GATT_CHRC_IFACE,
		BT_GATT_SERV_IFACE,
		BT_GATT_CHRC_IFACE,
		BT_GATT_CHRC_IFACE,
		BT_GATT_CHRC_IFACE,
		BT_GATT_CHRC_IFACE,
		BT_GATT_SERV_IFACE,
		BT_GATT_CHRC_IFACE
	};

	log_debug("Register APP object path");
	if (dbus_client_new_path_register(BT_GATT_APP_PATH,
	    BT_GATT_OBJ_MGR_IFACE, bt_app_method_handler, NULL) < 0) {
		log_err("failed to register path %s", BT_GATT_APP_PATH);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return -1;
	}

	log_debug("Register service and chrc object path");
	for (i = 0; i < ARRAY_LEN(paths); i++) {
		if (dbus_client_new_path_register(paths[i], interfaces[i],
		    bt_gatt_method_handler, paths[i]) < 0) {
			log_err("failed to register path %s", paths[i]);
			fail = true;
			break;
		}
	}

	if (fail) {
		while (i > 0) {
			i--;
			dbus_client_new_path_unregister(paths[i]);
		}
		dbus_client_new_path_unregister(BT_GATT_APP_PATH);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return -1;
	}

	log_debug("register app object path finish");
	bt_state.state = BT_REG_APP;
	timer_set(bt_state.timers, &bt_step_timer, 0);
	return 0;
}

static void bt_unregister_gatt_obj_path(void)
{
	char *paths[] = {
		BT_GATT_OBJ_PATH,
		BT_GATT_AYLA_OBJ_PATH,
		BT_GATT_AYLA_DSN_OBJ_PATH,
		BT_GATT_AYLA_DUID_OBJ_PATH,
		BT_GATT_CONF_OBJ_PATH,
		BT_GATT_CONF_STAT_OBJ_PATH,
		BT_GATT_CONF_CONN_OBJ_PATH,
		BT_GATT_CONF_SCAN_OBJ_PATH,
		BT_GATT_CONF_RESULT_OBJ_PATH,
		BT_GATT_CONN_OBJ_PATH,
		BT_GATT_CONN_SETUP_OBJ_PATH
	};
	int i;

	log_debug("unregister service and chrc obejct path");
	for (i = 0; i < ARRAY_LEN(paths); i++) {
		dbus_client_new_path_unregister(paths[i]);
	}

	log_debug("unregister app obejct path");
	dbus_client_new_path_unregister(BT_GATT_APP_PATH);
}

/*
 * Handle the register application reply
 */
static void bt_reg_app_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	if (!reply) {
		log_err("failed to register application, err %s", err);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return;
	}
	log_debug("register application completed");
	bt_state.state = BT_REG_ADV_PATH;
	timer_set(bt_state.timers, &bt_step_timer, 0);
}

/*
 * Register application
 */
static int bt_reg_app(void)
{
	DBusMessage *msg;
	DBusMessageIter args, array;
	char *ptr;
	int rc;

	log_debug("method_call RegisterApplication");

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez/hci0", "org.bluez.GattManager1",
	    "RegisterApplication");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}

	dbus_message_iter_init_append(msg, &args);

	ptr = BT_GATT_APP_PATH;
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ptr);

	dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY,
	    "{sv}", &array);

	dbus_message_iter_close_container(&args, &array);

	rc = dbus_client_send_with_reply(msg,
	    bt_reg_app_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		bt_unregister_gatt_obj_path();
		return -1;
	}

	return 0;
}

/*
 * Handle the unregister application reply
 */
static void bt_unreg_app_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	if (!reply) {
		log_err("failed to remove service and chrc, err %s", err);
		return;
	}
	log_debug("remove service and chrc completed");
}

/*
 * Unregister application
 */
static void bt_unreg_app(void)
{
	DBusMessage *msg;
	DBusMessageIter args;
	char *ptr;
	int rc;

	log_debug("method_call UnregisterApplication");

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez/hci0", "org.bluez.GattManager1",
	    "UnregisterApplication");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}

	dbus_message_iter_init_append(msg, &args);
	ptr = BT_GATT_APP_PATH;
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ptr);

	rc = dbus_client_send_with_reply(msg,
	    bt_unreg_app_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}
}

/*
 * Register advertisement object path
 */
static int bt_reg_adv_obj_path(void)
{
	log_debug("register advertisement object path");
	/* Register an object path for the advertisement */
	if (dbus_client_new_path_register(BT_DBUS_ADV_OBJ_PATH,
	    NULL, bt_ad_method_handler, NULL) < 0) {
		log_err("failed to register path for advertisement: %s",
		    BT_DBUS_ADV_OBJ_PATH);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return -1;
	}

	log_debug("register app object path finish");
	bt_state.state = BT_REG_ADV;
	timer_set(bt_state.timers, &bt_step_timer, 0);
	return 0;
}

/*
 * Unregister advertisement object path
 */
static void bt_unreg_adv_obj_path(void)
{
	log_debug("Unregister advertisement object path");
	/* Unregister advertisement object path */
	dbus_client_new_path_unregister(BT_DBUS_ADV_OBJ_PATH);
}

/*
 * Handle the register advertisement reply
 */
static void bt_reg_adv_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	if (!reply) {
		log_err("failed to register advertisement, err %s", err);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return;
	}

	log_debug("register advertisement completed");
	bt_state.state = BT_READY;
	timer_set(bt_state.timers, &bt_step_timer, 0);
}

/*
 * Register advertisement
 */
static int bt_reg_adv()
{
	DBusMessage *msg;
	DBusMessageIter args, array;
	char *ptr;
	int rc;

	log_debug("method_call RegisterAdvertisement");

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez/hci0", "org.bluez.LEAdvertisingManager1",
	    "RegisterAdvertisement");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}

	dbus_message_iter_init_append(msg, &args);

	ptr = BT_DBUS_ADV_OBJ_PATH;
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH,
	    &ptr);

	dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY,
	    "{sv}", &array);

	dbus_message_iter_close_container(&args, &array);

	rc = dbus_client_send_with_reply(msg,
	    bt_reg_adv_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
		return -1;
	}
	return 0;
}

/*
 * Handle the unregister advertisement reply
 */
static void bt_unreg_adv_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	if (!reply) {
		log_err("failed to unregister advertisement, err %s", err);
		return;
	}
	log_debug("unregister advertisement completed");
}

/*
 * Unregister advertisement
 */
static void bt_unreg_adv()
{
	DBusMessage *msg;
	DBusMessageIter args;
	char *ptr;
	int rc;

	log_debug("method_call UnregisterAdvertisement");

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez/hci0", "org.bluez.LEAdvertisingManager1",
	    "UnregisterAdvertisement");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}

	dbus_message_iter_init_append(msg, &args);
	ptr = BT_DBUS_ADV_OBJ_PATH;
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ptr);

	rc = dbus_client_send_with_reply(msg,
	    bt_unreg_adv_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}
}

/*
 * hci adv setup enabled or disabled.
 */
void hci_adv_enable(bool enable)
{
	int rc = 0;

	if (enable) {
		log_debug("enabling adv");
		rc = system("hciconfig hci0 leadv > /dev/null 2>&1");
		if (rc == -1) {
			log_err("hciconfig hci0 leadv failed");
		}
		bt_adv_enable_flag = true;

		if (bt_state.state == BT_WAITING) {
			bt_state.state = BT_REG_APP_PATH;
			timer_set(bt_state.timers, &bt_step_timer, 0);
		}
	} else {
		log_debug("disabling adv");
		rc = system("hciconfig hci0 noleadv > /dev/null 2>&1");
		if (rc == -1) {
			log_err("hciconfig hci0 noleadv failed");
		}
		bt_adv_enable_flag = false;
	}
}

/*
 * Update advertisement state
 */
void bt_update_adv(void)
{
	log_debug("bt_adv_enable_flag %d", bt_adv_enable_flag);
	hci_adv_enable(bt_adv_enable_flag);
}

/*
 * Delayed update adv state
 */
static void bt_update_adv_timeout(struct timer *timer)
{
	log_debug("bt_adv_enable_flag %d", bt_adv_enable_flag);
	if (!bt_adv_enable_flag) {
		if (bt_state.state == BT_READY) {
			bt_unregister_gatt_obj_path();
			bt_unreg_app();
			bt_unreg_adv_obj_path();
			bt_unreg_adv();
			bt_state.state = BT_WAITING;
		}
	}
	bt_update_adv();
}

static void bt_adapter_properties_changed(
		const char *iface, DBusMessageIter *iter)
{
	DBusMessageIter dict, entry, value;
	const char *name;
	const char *search[2] = {"Powered", "Discovering"};
	dbus_bool_t found = false, status;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
		log_err("iter not DBUS_TYPE_ARRAY");
		return;
	}

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry)
		    != DBUS_TYPE_STRING) {
			log_err("name iter not DBUS_TYPE_STRING");
			break;
		}

		dbus_message_iter_get_basic(&entry, &name);
		if (name) {
			log_debug("property name %s", name);
			if (!strcmp(name, search[0])
				|| !strcmp(name, search[1])) {
				found = true;
				break;
			}
		}

		dbus_message_iter_next(&dict);
	}

	if (found) {
		dbus_message_iter_next(&entry);
		if (dbus_message_iter_get_arg_type(&entry)
		    != DBUS_TYPE_VARIANT) {
			log_err("property iter not DBUS_TYPE_VARIANT");
			return;
		}

		dbus_message_iter_recurse(&entry, &value);

		if (dbus_message_iter_get_arg_type(&value)
		    != DBUS_TYPE_BOOLEAN) {
			log_err("value iter not DBUS_TYPE_BOOLEAN");
			return;
		}

		dbus_message_iter_get_basic(&value, &status);
		log_debug("property value: %d", status);

		bt_update_adv();
	}

	return;
}

static void bt_device_properties_changed(
		const char *path, const char *iface, DBusMessageIter *iter)
{
	DBusMessageIter dict, entry, value;
	const char *name;
	const char *search = "Connected";
	dbus_bool_t found = false, status;

	if (!path || !iface || !iter) {
		log_err("input parameter error");
		return;
	}

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
		log_err("iter not DBUS_TYPE_ARRAY");
		return;
	}

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry)
		    != DBUS_TYPE_STRING) {
			log_err("name iter not DBUS_TYPE_STRING");
			break;
		}

		dbus_message_iter_get_basic(&entry, &name);
		if (name) {
			if (!strcmp(name, search)) {
				log_debug("found property name %s", name);
				found = true;
				break;
			}
		}

		dbus_message_iter_next(&dict);
	}

	if (found) {
		dbus_message_iter_next(&entry);
		if (dbus_message_iter_get_arg_type(&entry)
		    != DBUS_TYPE_VARIANT) {
			log_err("property iter not DBUS_TYPE_VARIANT");
			return;
		}

		dbus_message_iter_recurse(&entry, &value);

		if (dbus_message_iter_get_arg_type(&value)
		    != DBUS_TYPE_BOOLEAN) {
			log_err("value iter not DBUS_TYPE_BOOLEAN");
			return;
		}

		dbus_message_iter_get_basic(&value, &status);
		log_debug("property value: %d", status);

		bt_update_adv();
	}

	return;
}

static void bt_properties_changed(
		DBusMessage *msg, void *arg)
{
	const char *path = (const char *)arg;
	const char *iface;
	DBusMessageIter iter;

	if (dbus_message_iter_init(msg, &iter) == false) {
		log_err("dbus_message_iter_init failed");
		return;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
		log_err("interface iter not DBUS_TYPE_STRING");
		return;
	}

	dbus_message_iter_get_basic(&iter, &iface);
	if (!iface) {
		log_err("missing interface content");
		return;
	}
	log_debug("path %s, interface %s", path, iface);

	dbus_message_iter_next(&iter);

	if (!strcmp(iface, BT_GATT_DEV_IFACE)) {
		bt_device_properties_changed(path, iface, &iter);
	} else if (!strcmp(iface, BT_GATT_APT_IFACE)) {
		bt_adapter_properties_changed(iface, &iter);
	} else {
		log_err("unsupported signal interface: %s", iface);
	}

	return;
}

/*
 * Register for device properties change signal.
 */
static int bt_device_signal_subscribe(const char *bus_name, const char *path)
{
	struct bt_dbus_device *device;

	log_debug("bus_name %s, path %s", bus_name, path);

	STAILQ_FOREACH(device, &deviceq, link) {
		if (!strcmp(device->path, path)) {
			log_debug("path %s already subscribed", path);
			return 0;
		}
	}

	device = calloc(1, sizeof(struct bt_dbus_device));
	if (!device) {
		log_err("malloc bt_dbus_device memory failed");
		return -1;
	}

	device->prop_changed_handler =
	    dbus_client_signal_handler_add(
	    bus_name, BT_GATT_PROP_IFACE,
	    "PropertiesChanged", path,
	    bt_properties_changed, (void *)path);

	if (!device->prop_changed_handler) {
		log_err("failed to subscribe to PropertiesChanged signal "
		    "for %s", path);
		free(device);
		return -1;
	}

	device->path = strdup(path);

	STAILQ_INSERT_TAIL(&deviceq, device, link);

	return 0;
}

/*
 * Cleanup resources including its interfaces.
 */
static void bt_device_signal_unsubscribe(const char *path)
{
	struct bt_dbus_device *device, *find = NULL;

	log_debug("path %s", path);

	STAILQ_FOREACH(device, &deviceq, link) {
		if (!strcmp(device->path, path)) {
			find = device;
			log_debug("found device->path %s", device->path);
			break;
		}
	}

	if (!find) {
		log_debug("Did not find device %s", path);
		return;
	}

	if (find->prop_changed_handler) {
		dbus_client_msg_handler_remove(find->prop_changed_handler);
	}

	STAILQ_REMOVE(&deviceq, find, bt_dbus_device, link);

	free(find->path);
	free(find);

	timer_set(bt_state.timers, &bt_adv_timer, BT_UPDATE_ADV_DELAY_MS);
}

static void bt_obj_mgr_add_signal_handler(const char *bus_name,
		DBusMessageIter *iter, const char *path)
{
	DBusMessageIter dict, entry;
	const char *iface;
	int rc;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
		log_err("invalid interface array for %s", path);
		return;
	}

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
		if (dbus_message_iter_get_arg_type(&dict)
		    != DBUS_TYPE_DICT_ENTRY) {
			continue;
		}
		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry)
		    != DBUS_TYPE_STRING) {
			log_err("invalid dict type for %s", path);
			break;
		}

		dbus_message_iter_get_basic(&entry, &iface);
		log_debug("obj path %s, interface %s", path, iface);
		if (!strcmp(iface, BT_GATT_DEV_IFACE)
		    || !strcmp(iface, BT_GATT_APT_IFACE)) {
			rc = bt_device_signal_subscribe(
			    bus_name, path);
			if (rc < 0) {
				log_err("bt_device_signal_subscribe path %s "
				    "error", path);
			}
		}

		dbus_message_iter_next(&entry);
		dbus_message_iter_next(&dict);
	}

	return;
}

static void bt_obj_mgr_remove_signal_handler(const char *bus_name,
		DBusMessageIter *iter, const char *path)
{
	DBusMessageIter entry;
	const char *iface;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
		log_err("invalid interface array for %s", path);
		return;
	}

	dbus_message_iter_recurse(iter, &entry);

	while (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_INVALID) {
		if (dbus_message_iter_get_arg_type(&entry)
		    != DBUS_TYPE_STRING) {
			continue;
		}
		dbus_message_iter_get_basic(&entry, &iface);
		log_debug("obj path %s, interface %s removed", path, iface);
		if (!strcmp(iface, BT_GATT_DEV_IFACE)
		    || !strcmp(iface, BT_GATT_APT_IFACE)) {
			bt_device_signal_unsubscribe(path);
		}
		dbus_message_iter_next(&entry);
	}

	return;
}

static void bt_obj_mgr_signal_handler(
		DBusMessage *msg, void *arg)
{
	DBusMessageIter iter;
	const char *member, *path;
	const char *bus_name = dbus_message_get_sender(msg);

	member = dbus_message_get_member(msg);
	if (!member) {
		log_err("missing signal member");
		return;
	}

	if (dbus_message_iter_init(msg, &iter) == false) {
		log_err("msg init error");
		return;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
		log_err("missing object path");
		return;
	}

	dbus_message_iter_get_basic(&iter, &path);
	if (!path) {
		log_err("get object path error");
		return;
	}

	log_debug("bus_name %s, signal member %s, path %s",
	    bus_name, member, path);

	dbus_message_iter_next(&iter);

	if (!strcmp(member, "InterfacesAdded")) {
		bt_obj_mgr_add_signal_handler(bus_name, &iter, path);
	} else if (!strcmp(member, "InterfacesRemoved")) {
		bt_obj_mgr_remove_signal_handler(bus_name, &iter, path);
	} else {
		log_err("unsupported signal member: %s", member);
	}

	return;
}

/*
 * Unregister object manager interface change signals.
 */
static void bt_obj_mgr_signal_unsubscribe(void)
{
	if (bt_state.added_handler) {
		dbus_client_msg_handler_remove(bt_state.added_handler);
		bt_state.added_handler = NULL;
	}
	if (bt_state.removed_handler) {
		dbus_client_msg_handler_remove(bt_state.removed_handler);
		bt_state.removed_handler = NULL;
	}
}

/*
 * Register object manager interface change signals.
 */
static int bt_obj_mgr_signal_subscribe(const char *bus_name)
{
	log_debug("bus_name %s", bus_name ? bus_name : "NULL");

	if (bt_state.added_handler || bt_state.removed_handler) {
		bt_obj_mgr_signal_unsubscribe();
	}
	bt_state.added_handler = dbus_client_signal_handler_add(
	    bus_name, BT_GATT_OBJ_MGR_IFACE,
	    "InterfacesAdded", BT_GATT_OBJ_MGR_PATH,
	    bt_obj_mgr_signal_handler, NULL);
	if (!bt_state.added_handler) {
		log_err("failed to subscribe to InterfacesAdded signal");
		goto error;
	}

	bt_state.removed_handler = dbus_client_signal_handler_add(
	    bus_name, BT_GATT_OBJ_MGR_IFACE,
	    "InterfacesRemoved", BT_GATT_OBJ_MGR_PATH,
	    bt_obj_mgr_signal_handler, NULL);
	if (!bt_state.removed_handler) {
		log_err("failed to subscribe to InterfacesRemoved signal");
		goto error;
	}

	log_debug("signal subscribe on bus_name %s finished", bus_name);
	bt_state.state = BT_REQ_MGR_OBJ;
	timer_set(bt_state.timers, &bt_step_timer, 0);

	return 0;
error:
	bt_obj_mgr_signal_unsubscribe();
	return -1;
}

/*
 * Handle a D-Bus interfaces changed message.
 */
static void bt_parse_obj_info(const char *bus_name, const char *path,
	const DBusMessageIter *iface_iter)
{
	DBusMessageIter iter = *iface_iter;
	DBusMessageIter props_iter;
	const char *iface;
	int rc;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		log_err("invalid interface array for %s", path);
		return;
	}
	/* Enter interfaces array */
	dbus_message_iter_recurse(&iter, &iter);
	for (; dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID;
	    dbus_message_iter_next(&iter)) {
		iface = dbus_utils_parse_dict(&iter, &props_iter);
		if (!iface) {
			log_err("%s: missing interface", path);
			break;
		}
		log_debug("obj path %s, interface %s", path, iface);
		if (!strcmp(iface, BT_GATT_DEV_IFACE)
		    || !strcmp(iface, BT_GATT_APT_IFACE)) {
			rc = bt_device_signal_subscribe(bus_name, path);
			if (rc < 0) {
				log_err("bt_device_signal_subscribe path %s "
				    "error", path);
			}
		}
	}
}

/*
 * Handle the response from a managed object query from the Bluetooth service.
 * This initializes the D-Bus object data structures and gets the internal
 * bus name needed for some subsequent operations.
 */
static void bt_parse_managed_objs(DBusMessage *msg, void *arg,
	const char *err)
{
	DBusMessageIter iter;
	DBusMessageIter path_iter;
	const char *path;
	const char *bus_name;

	log_debug("handling managed objects");

	if (!msg) {
		log_err("msg NULL");
		return;
	}

	/* Subscribe to interface change signals on BlueZ bus */
	bus_name = dbus_message_get_sender(msg);
	if (!bus_name) {
		log_err("no bus name populated");
		return;
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
			log_warn("no object path populated");
			continue;
		}
		bt_parse_obj_info(bus_name, path, &path_iter);
	}

	log_debug("request managed object finished");
	bt_state.state = BT_WAITING;
	timer_set(bt_state.timers, &bt_step_timer, 0);
}

/*
 * Begin the D-Bus session by requesting information about all registered
 * objects we can interact with.
 */
static int bt_req_mgr_obj(void)
{
	DBusMessage *msg;
	int rc = 0;

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    BT_GATT_OBJ_MGR_PATH, BT_GATT_OBJ_MGR_IFACE, "GetManagedObjects");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	if (dbus_client_send_with_reply(msg, bt_parse_managed_objs,
	    NULL, DBUS_TIMEOUT_USE_DEFAULT) < 0) {
		log_err("D-Bus send failed");
		rc = -1;
	}
	dbus_message_unref(msg);

	return rc;
}

/*
 * Handle the default agent registration reply
 */
static void bt_register_default_agent_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	const char *bus_name;

	if (!reply) {
		log_err("failed %s", err);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return;
	}
	log_debug("registered default agent");
	bt_state.state = BT_GET_LOC_ADDR;
	timer_set(bt_state.timers, &bt_step_timer, 0);

	if (!bt_state.bus_name) {
		bus_name = dbus_message_get_sender(reply);
		if (bus_name) {
			bt_state.bus_name = strdup(bus_name);
		} else {
			log_err("no bus name populated");
		}
	}
}

static int bt_register_default_agent(void)
{
	const char *path = BT_GATT_AGENT_PATH;
	DBusMessage *msg;
	int rc;

	/* Set the agent as the default agent */
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez", BT_GATT_AGT_MGR_IFACE, "RequestDefaultAgent");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path,
	    DBUS_TYPE_INVALID);

	rc = dbus_client_send_with_reply(msg,
	    bt_register_default_agent_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
		return -1;
	}

	return 0;
}

/*
 * Handle the agent registration reply by setting the
 */
static void bt_register_agent_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	if (!reply) {
		log_err("register connection agent on %s failed",
		    BT_GATT_AGENT_PATH);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return;
	}
	log_debug("registered connection agent on %s",
	    BT_GATT_AGENT_PATH);
	bt_state.state = BT_REG_DEF_AGENT;
	timer_set(bt_state.timers, &bt_step_timer, 0);
}

static int bt_register_agent(void)
{
	DBusMessage *msg;
	const char *path = BT_GATT_AGENT_PATH;
	const char *capability = "NoInputNoOutput";
	int rc;

	/* Register an object path for the agent */
	if (dbus_client_new_path_register(BT_GATT_AGENT_PATH,
	    NULL, bt_ad_method_handler, NULL) < 0) {
		log_err("failed to register path for agent: %s",
		    BT_GATT_AGENT_PATH);
		return -1;
	}

	/* Request a connection agent */
	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez", BT_GATT_AGT_MGR_IFACE, "RegisterAgent");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path,
	    DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID);

	rc = dbus_client_send_with_reply(msg,
	    bt_register_agent_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
		return -1;
	}

	return 0;
}

/*
 * Handle the remove advertise manager reply
 */
static void bt_agent_remove_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	if (!reply) {
		log_err("failed to remove agent, err %s", err);
		return;
	}
	log_debug("removed agent");
}

static void bt_agent_remove()
{
	DBusMessage *msg;
	DBusMessageIter args;
	char *ptr;
	int rc;

	log_debug("method_call UnregisterAgent");

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    "/org/bluez", BT_GATT_AGT_MGR_IFACE, "UnregisterAgent");
	if (!msg) {
		log_err("message allocation failed");
		return;
	}

	dbus_message_iter_init_append(msg, &args);
	ptr = BT_GATT_AGENT_PATH;
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &ptr);

	rc = dbus_client_send_with_reply(msg,
	    bt_agent_remove_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
	}

	dbus_client_new_path_unregister(BT_GATT_AGENT_PATH);
}

/*
 * Bluetooth adapter powered on.
 */
static void bt_adapter_power_done_reply_handler(DBusMessage *reply, void *arg,
	const char *err)
{
	const char *bus_name;

	if (!reply) {
		log_warn("adapter %s power on failed: %s",
		    BT_GATT_APT_PATH, err);
		timer_set(bt_state.timers, &bt_step_timer,
		    BT_DEFAULT_DELAY_MS);
		return;
	}

	bus_name = dbus_message_get_sender(reply);
	if (bus_name) {
		bt_state.bus_name = strdup(bus_name);
	} else {
		log_err("no bus name populated");
	}

	log_debug("adapter %s powered on", BT_GATT_APT_PATH);
	bt_state.state = BT_REG_AGENT;
	timer_set(bt_state.timers, &bt_step_timer, 0);
}

/*
 * Power Bluetooth adapter.
 */
static int bt_adapter_power_on(void)
{
	DBusMessage *msg;
	bool onoff = true;
	int rc;

	msg = bt_utils_create_msg_prop_set(BT_DBUS_SERVICE_BLUEZ,
	    BT_GATT_APT_PATH, BT_GATT_APT_IFACE,
	    "Powered", DBUS_TYPE_BOOLEAN, &onoff);
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}
	rc = dbus_client_send_with_reply(msg,
	    bt_adapter_power_done_reply_handler,
	    NULL, DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("dbus_client_send_with_reply failed");
	} else {
		log_debug("power on adapter %s", BT_GATT_APT_PATH);
	}

	return rc;
}

/*
 * Set advertisement name
 */
static void bt_set_adv_name(const uint8_t *bt_addr)
{
	bt_adv_name[0]  = 'A';
	bt_adv_name[1]  = 'y';
	bt_adv_name[2]  = 'l';
	bt_adv_name[3]  = 'a';
	bt_adv_name[4]  = '-';
	bt_adv_name[5]  = bt_addr[0];
	bt_adv_name[6]  = bt_addr[1];
	bt_adv_name[7]  = bt_addr[3];
	bt_adv_name[8]  = bt_addr[4];
	bt_adv_name[9]  = bt_addr[6];
	bt_adv_name[10] = bt_addr[7];
	bt_adv_name[11] = bt_addr[9];
	bt_adv_name[12] = bt_addr[10];
	bt_adv_name[13] = bt_addr[12];
	bt_adv_name[14] = bt_addr[13];
	bt_adv_name[15] = bt_addr[15];
	bt_adv_name[16] = bt_addr[16];
	bt_adv_name[17] = '\0';

	advmt.local_name = (char *)bt_adv_name;
}

/*
 * Handle the get local device address reply
 */
static void bt_get_local_addr_reply_handler(DBusMessage *reply,
	void *arg, const char *err)
{
	DBusMessageIter iter, vari;
	const char *addr;

	if (!reply) {
		log_err("failed to get_local_addr, err %s", err);
		goto error;
	}

	if (dbus_message_iter_init(reply, &iter) == false) {
		log_err("dbus_message_iter_init failed");
		goto error;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
		log_err("property iter not DBUS_TYPE_VARIANT");
		goto error;
	}

	dbus_message_iter_recurse(&iter, &vari);

	if (dbus_message_iter_get_arg_type(&vari) != DBUS_TYPE_STRING) {
		log_err("value iter not DBUS_TYPE_STRING");
		goto error;
	}

	dbus_message_iter_get_basic(&vari, &addr);
	if (!addr) {
		log_err("no addr value");
		goto error;
	}

	memset(bt_gatt_duid, 0, sizeof(bt_gatt_duid));
	strncpy((char *)bt_gatt_duid, addr, BT_ADDR_LEN);
	log_debug("set addr value: %s", (char *)bt_gatt_duid);

	bt_set_adv_name(bt_gatt_duid);

	bt_state.state = BT_SIG_SUBSCRIBE;
	timer_set(bt_state.timers, &bt_step_timer, 0);
	return;

error:
	timer_set(bt_state.timers, &bt_step_timer,
	    BT_DEFAULT_DELAY_MS);
	return;
}

/*
 * Get local bluetooth device address.
 */
static int bt_get_local_addr(void)
{
	DBusMessage *msg;
	DBusMessageIter iter;
	char *iface, *name;
	int rc;

	log_debug("method_call Get on %s", BT_GATT_APT_PATH);

	msg = dbus_message_new_method_call(BT_DBUS_SERVICE_BLUEZ,
	    BT_GATT_APT_PATH, BT_GATT_PROP_IFACE, "Get");
	if (!msg) {
		log_err("message allocation failed");
		return -1;
	}

	dbus_message_iter_init_append(msg, &iter);

	iface = BT_GATT_APT_IFACE;
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
	name = "Address";
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

	rc = dbus_client_send_with_reply(msg,
	    bt_get_local_addr_reply_handler, NULL,
	    DBUS_TIMEOUT_USE_DEFAULT);
	dbus_message_unref(msg);
	if (rc < 0) {
		log_err("D-Bus send failed");
		return -1;
	}
	return 0;
}

/*
 * Handle a bt state change
 */
static void bt_step_timeout(struct timer *timer)
{
	log_debug("state %d", bt_state.state);
	switch (bt_state.state) {
	case BT_INIT:
		bt_state.state = BT_POWER_ON;
		timer_set(bt_state.timers, timer, BT_DEFAULT_DELAY_MS);
		break;
	case BT_POWER_ON:
		if (bt_adapter_power_on() < 0) {
			log_err("bt_adapter_power_on failed");
			break;
		}
		break;
	case BT_REG_AGENT:
		if (bt_register_agent() < 0) {
			log_err("bt_register_agent failed");
			break;
		}
		break;
	case BT_REG_DEF_AGENT:
		if (bt_register_default_agent() < 0) {
			log_err("bt_register_agent failed");
			break;
		}
		break;
	case BT_GET_LOC_ADDR:
		if (bt_get_local_addr() < 0) {
			log_err("bt_get_local_addr failed");
			break;
		}
		break;
	case BT_SIG_SUBSCRIBE:
		if (bt_obj_mgr_signal_subscribe(bt_state.bus_name) < 0) {
			log_err("bt_obj_mgr_signal_subscribe failed");
			break;
		}
		break;
	case BT_REQ_MGR_OBJ:
		if (bt_req_mgr_obj() < 0) {
			log_err("bt_req_mgr_obj failed");
			break;
		}
		break;
	case BT_WAITING:
		if (bt_adv_enable_flag) {
			bt_state.state = BT_REG_APP_PATH;
			timer_set(bt_state.timers, &bt_step_timer, 0);
		}
		break;
	case BT_REG_APP_PATH:
		if (bt_register_gatt_obj_path() < 0) {
			log_err("bt_register_gatt_obj_path failed");
			break;
		}
		break;
	case BT_REG_APP:
		if (bt_reg_app() < 0) {
			log_err("bt_get_local_addr failed");
			break;
		}
		break;
	case BT_REG_ADV_PATH:
		if (bt_reg_adv_obj_path() < 0) {
			log_err("bt_reg_adv_obj_path failed");
			break;
		}
		break;
	case BT_REG_ADV:
		if (bt_reg_adv() < 0) {
			log_err("bt_reg_adv failed");
			break;
		}
		break;
	case BT_READY:
		timer_cancel(bt_state.timers, timer);
		break;
	}
}

/*
 * Initialize the GATT service.
 */
int gatt_init(struct file_event_table *file_events, struct timer_head *timers)
{
	int rc;

	ASSERT(file_events != NULL);
	ASSERT(timers != NULL);

	bt_state.file_events = file_events;
	bt_state.timers = timers;

	timer_init(&bt_adv_timer, bt_update_adv_timeout);
	timer_init(&bt_step_timer, bt_step_timeout);

	wifi_reg_scan_complete_cb(bt_scan_complete);
	wifi_reg_connect_state_change_cb(bt_connect_state_change);
	wifi_reg_ap_mode_change_cb(hci_adv_enable);

	/* Connect to D-Bus */
	rc = dbus_client_init(bt_state.file_events, bt_state.timers);
	if (rc < 0) {
		log_err("dbus_client_init failed");
		return -1;
	}

	bt_device_queue_init();

	bt_step_timeout(&bt_step_timer);

	return 0;
}

/*
 * Free resources.
 */
int gatt_cleanup(void)
{
	/* Unsubscribe object manager signal */
	bt_obj_mgr_signal_unsubscribe();

	bt_device_queue_cleanup();

	bt_agent_remove();

	/* Disconnect from D-Bus */
	dbus_client_cleanup();

	/* Disable Advertising */
	hci_adv_enable(false);
	return 0;
}

