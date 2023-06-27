/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>

#include "jansson.h"

#include <ayla/utypes.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/clock.h>
#include <ayla/ayla_interface.h>
#include <ayla/timer.h>
#include <ayla/log.h>
#include <ayla/conf_rom.h>
#include <platform/system.h>

#include "dapi.h"
#include "serv.h"
#include "notify.h"
#include "ds.h"
#include "devd_conf.h"

/* set CONF subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_CONF, __VA_ARGS__)

bool conf_reset;		/* sys/factory flag set */
char *conf_ads_region;		/* client/region option specified */
char *conf_ads_host_override;	/* client/server option specified */

/*
 * Setup oem vars from config
 */
static int devd_conf_oem_set(json_t *obj)
{
	struct device_state *dev = &device;
	const char *oem_model;

	if (!dev->oem) {
		dev->oem = json_get_string_dup(obj, "oem");
		if (!dev->oem) {
			log_err("missing OEM ID");
			return -1;
		}
		log_debug("OEM ID: %s", dev->oem);
	}
	oem_model = json_get_string(obj, "model");
	if (!oem_model) {
		log_err("missing OEM model");
		return -1;
	}
	/* OEM model is configurable at runtime */
	if (!ds_update_oem_model(oem_model)) {
		log_debug("OEM Model: %s", dev->oem_model);
	}
	if (!dev->oem_key) {
		dev->oem_key = json_get_string_dup(obj, "key");
		if (!dev->oem_key) {
			log_err("missing OEM key");
			return -1;
		}
	}
	return 0;
}

/*
 * Setup sys vars from config
 */
static int devd_conf_sys_set(json_t *obj)
{
	struct device_state *dev = &device;
	int timezone;
	unsigned daylight;
	bool set;

	if (!json_get_bool(obj, "setup_mode", &set)) {
		dev->setup_mode = set;
		/* Apply setup mode value to system and update ADS URL */
		platform_apply_setup_mode(dev->setup_mode);
		ds_update_ads_host();
	}
	/*
	 * Check to see if the factory default config has just been loaded.
	 * The "factory" flag is no longer required due to the below check.
	 */
	if (!json_get_bool(obj, "factory", &set) && set) {
		conf_reset = true;
	}
	if (!json_get_int(obj, "timezone", &timezone)) {
		timezone_ayla.mins = -1 * timezone;
		timezone_ayla.valid = 1;
	}
	if (!json_get_bool(obj, "dst_valid", &set)) {
		daylight_ayla.valid = set;
	}
	if (!json_get_bool(obj, "dst_active", &set)) {
		daylight_ayla.active = set;
	}
	if (!json_get_uint(obj, "dst_change", &daylight)) {
		daylight_ayla.change = daylight;
	}
	return 0;
}

static int devd_conf_id_set(json_t *obj)
{
	struct device_state *dev = &device;

	/* If DSN and key are stored in ROM, load them */
	conf_rom_load_id();

	if (!dev->dsn) {
		dev->dsn = json_get_string_dup(obj, "dsn");
		if (!dev->dsn || !strcmp(dev->dsn, "DSN-not-configured")) {
			log_err("missing or invalid DSN");
			return -1;
		}
		log_debug("DSN: %s", dev->dsn);
	}
	if (!dev->pub_key) {
		dev->pub_key = json_get_string_dup(obj, "rsa_pub_key");
		if (!dev->pub_key) {
			log_err("missing rsa_pub_key");
			return -1;
		}
	}
	return 0;
}

/*
 * Setup ds_client from config
 */
static int devd_conf_client_set(json_t *obj)
{
	struct device_state *dev = &device;
	json_t *server_j;
	const char *str;
	bool server_default = false;

	str = json_get_string(obj, "server");
	if (!str) {
		server_j = json_object_get(obj, "server");
		if (server_j) {
			str = json_get_string(server_j, "server");
			json_get_bool(server_j, "default", &server_default);
		}
	}
	if (str && *str) {
		free(conf_ads_host_override);
		conf_ads_host_override = strdup(str);
	}
	dev->ads_host_dev_override = server_default;
	json_get_uint16(obj, "poll_interval", &dev->poll_interval);
	str = json_get_string(obj, "region");
	if (str && *str) {
		free(conf_ads_region);
		conf_ads_region = strdup(str);
	}
	/* Generate new ADS host URL in case it changed */
	dev->setup_mode = 1;
	ds_update_ads_host();
	dev->setup_mode = 0;
	return 0;
}

static int devd_conf_lanip_set(json_t *lanip)
{
	struct device_state *dev = &device;
	const char *str;
	bool enable;
	bool auto_sync = true;			/* Default to enabled */
	u16 keep_alive = CLIENT_LAN_KEEPALIVE;	/* Default to 30 seconds */
	u16 key_id;
	const char *key;

	str = json_get_string(lanip, "status");
	if (!str) {
		log_err("lanip status not set");
		return -1;
	}
	enable = !strcmp(str, "enable");
	key = json_get_string(lanip, "lanip_key");
	if (!key && enable) {
		log_err("lanip key not set");
		return -1;
	}
	if (json_get_uint16(lanip, "lanip_key_id", &key_id) < 0) {
		if (enable) {
			log_err("lanip key_id not set");
			return -1;
		}
		key_id = 0;
	}
	json_get_bool(lanip, "auto_sync", &auto_sync);
	json_get_uint16(lanip, "keep_alive", &keep_alive);

	dev->lan.enable = enable;
	dev->lan.auto_sync = auto_sync;
	dev->lan.keep_alive = keep_alive;
	dev->lan.key_id = key_id;
	snprintf(dev->lan.key, sizeof(dev->lan.key), "%s", key ? key : "");
	return 0;
}

void devd_conf_init(void)
{
	conf_register("id", devd_conf_id_set, NULL);
	conf_register("sys", devd_conf_sys_set, NULL);
	conf_register("oem", devd_conf_oem_set, NULL);
	conf_register("client", devd_conf_client_set, NULL);
	conf_register("lanip", devd_conf_lanip_set, NULL);
}
