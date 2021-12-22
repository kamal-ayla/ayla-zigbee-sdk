/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/types.h>
#include <sys/poll.h>
#include <stdio.h>
#include <string.h>

#include <ayla/utypes.h>
#include <ayla/conf_io.h>
#include <ayla/json_parser.h>
#include <ayla/file_event.h>
#include <ayla/nameval.h>
#include <ayla/timer.h>
#include <ayla/uri_code.h>
#include <ayla/base64.h>
#include <ayla/hex.h>
#include <ayla/str_utils.h>
#include <ayla/log.h>

#include "cond.h"
#include "wifi.h"

/* Set CONF subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_CONF, __VA_ARGS__)

/*
 * Wi-Fi security mode string mappings.
 */
const struct name_val wifi_sec_names[] = {
	{"none", WSEC_VALID | WSEC_NONE},
	{"WEP", WSEC_VALID | WSEC_WEP},
	{"WPA_Personal_TKIP", WSEC_VALID | WSEC_WPA | WSEC_PSK | WSEC_TKIP},
	{"WPA_Personal_CCMP", WSEC_VALID | WSEC_WPA | WSEC_PSK | WSEC_CCMP},
	{"WPA_Personal_Mixed",
	    WSEC_VALID | WSEC_WPA | WSEC_PSK | WSEC_TKIP | WSEC_CCMP},
	{"WPA2_Personal_TKIP", WSEC_VALID | WSEC_WPA2 | WSEC_PSK | WSEC_TKIP},
	{"WPA2_Personal_CCMP", WSEC_VALID | WSEC_WPA2 | WSEC_PSK | WSEC_CCMP},
	{"WPA2_Personal_Mixed",
	    WSEC_VALID | WSEC_WPA2 | WSEC_PSK | WSEC_TKIP | WSEC_CCMP},
	{NULL, -1}
};

/*
 * Wi-Fi profile save mode string mappings.
 */
DEF_NAMEVAL_TABLE(wifi_prof_save_mode_table, WIFI_PROF_SAVE_MODES);


/*
 * Read MAC address variable for AP network interface.
 */
static ssize_t wifi_conf_var_read_ap_mac(char *buf, size_t buf_size,
	const char *var, void *arg)
{
	struct wifi_state *wifi = (struct wifi_state *)arg;
	struct net_ifinfo ap_info;
	ssize_t rc;

	if (net_get_ifinfo(wifi->ap_ifname, &ap_info) < 0) {
		log_err("failed to get %s info", wifi->ap_ifname);
		return -1;
	}
	rc = hex_string(buf, buf_size, ap_info.hw_addr.ether_addr_octet,
	    sizeof(ap_info.hw_addr.ether_addr_octet), false, 0);
	if (rc < 0) {
		log_err("failed to expand %s", var);
		return -1;
	}
	return rc;
}

/*
 * Read DSN variable.  This is requested from devd, and may not be available
 * immediately.
 */
static ssize_t wifi_conf_var_read_dsn(char *buf, size_t buf_size,
	const char *var, void *arg)
{
	struct wifi_state *wifi = (struct wifi_state *)arg;
	ssize_t rc;

	if (!wifi->dsn || wifi->dsn[0] == '\0') {
		log_warn("%s not loaded", var);
		return -1;
	}
	rc = strlen(wifi->dsn);
	if (rc > buf_size) {
		log_warn("%s truncated", var);
		rc = buf_size;
	}
	strncpy(buf, wifi->dsn, buf_size);
	return rc;
}

/*
 * Table of variables to expand.
 */
static const struct str_var wifi_conf_var_table[] = {
	STR_VAR_DECL(MAC, wifi_conf_var_read_ap_mac)
	STR_VAR_DECL(DSN, wifi_conf_var_read_dsn)
	STR_VAR_END
};

/*
 * Parse a Wi-Fi profile
 */
static int wifi_conf_prof_import(struct wifi_profile *prof, json_t *obj)
{
	const char *cp;
	int sec_code;
	ssize_t len;

	if (!json_is_object(obj)) {
		log_err("invalid JSON object");
		return -1;
	}
	/* Clear profile before updating */
	memset(prof, 0, sizeof(*prof));

	cp = json_get_string(obj, "ssid");
	if (!cp) {
		log_err("no ssid");
		return -1;
	}
	if (wifi_parse_ssid(cp, &prof->ssid) < 0) {
		return -1;
	}
	cp = json_get_string(obj, "security");
	if (!cp) {
		log_err("no sec type");
		return -1;
	}
	sec_code = lookup_by_name(wifi_sec_names, cp);
	if (sec_code < 0) {
		log_err("invalid security: %s", cp);
		return -1;
	}
	prof->sec = sec_code;

	cp = json_get_string(obj, "key");
	if (cp) {
		if (SEC_MATCH(prof->sec, WSEC_WEP)) {
			len = hex_parse(prof->key.val, sizeof(prof->key.val),
			    cp, NULL);
			if (len < 0) {
				log_err("failed to parse WEP hex key");
				return -1;
			}
			prof->key.len = len;
		} else {
			len = strlen(cp);
			if (len > sizeof(prof->key.val)) {
				log_err("key too long");
				return -1;
			}
			memcpy(prof->key.val, cp, len);
			prof->key.len = len;
		}
	}
	json_get_bool(obj, "hidden", &prof->hidden);

	if (SEC_MATCH(prof->sec, WSEC_NONE) || prof->key.len) {
		if (json_get_bool(obj, "enable", &prof->enable) < 0) {
			prof->enable = true;
		}
	}
	return 0;
}

/*
 * Load Wi-Fi profiles from a JSON array
 */
static int wifi_conf_profs_import(struct wifi_state *wifi, json_t *profs)
{
	unsigned i;
	unsigned j;
	json_t *obj;

	struct wifi_profile *prof;

	if (!json_is_array(profs)) {
		log_debug("invalid JSON array");
		return -1;
	}
	for (i = 0, j = 0; i < json_array_size(profs); i++) {
		if (i >= WIFI_PROF_CT) {
			log_err("too many profiles");
			break;
		}
		obj = json_array_get(profs, i);
		prof = &wifi->profile[j];
		if (wifi_conf_prof_import(prof, obj) < 0) {
			log_err("skipping invalid profile %d", i);
			continue;
		}
		++j;
	}
	/* clear any unused profiles */
	if (j < WIFI_PROF_CT) {
		memset(wifi->profile + j, 0,
		    (WIFI_PROF_CT - j) * sizeof(*prof));
	}
	return 0;
}

/*
 * Create a JSON array of WiFi profiles
 */
static json_t *wifi_conf_prof_export(struct wifi_profile *prof)
{
	json_t *obj;
	json_t *key;
	char hex_key[WIFI_MAX_KEY_LEN * 2 + 1];
	const char *sec_str;

	sec_str = lookup_by_val(wifi_sec_names, prof->sec);
	if (SEC_MATCH(prof->sec, WSEC_WEP)) {
		hex_string(hex_key, sizeof(hex_key), prof->key.val,
		    prof->key.len, true, 0);
		key = json_string(hex_key);
	} else {
		key = json_stringn((char *)prof->key.val, prof->key.len);
	}

	obj = json_object();
	json_object_set_new(obj, "enable", json_boolean(prof->enable));
	json_object_set_new(obj, "ssid",
	    json_string(wifi_ssid_to_str(&prof->ssid)));
	json_object_set_new(obj, "security",
	    json_string(sec_str ? sec_str : "none"));
	json_object_set_new(obj, "key", key);
	if (prof->hidden) {
		json_object_set_new(obj, "hidden", json_true());
	}
	return obj;
}

/*
 * Create a JSON array of WiFi profiles
 */
static json_t *wifi_conf_profs_export(struct wifi_state *wifi)
{
	struct wifi_profile *prof;
	json_t *array;
	json_t *obj;

	array = json_array();

	for (prof = wifi->profile; prof < &wifi->profile[WIFI_PROF_CT];
		prof++) {
		if (!prof->ssid.len) {
			continue;
		}
		obj = wifi_conf_prof_export(prof);
		if (obj) {
			json_array_append_new(array, obj);
		}
	}
	return array;
}

/*
 * Set Wi-Fi state from config
 */
static int wifi_conf_set(json_t *obj)
{
	struct wifi_state *wifi = &wifi_state;
	json_t *item;
	const char *val;
	ssize_t rc;

	item = json_object_get(obj, "enable");
	wifi->enable = !item || json_boolean_value(item);

	val = json_get_string(obj, "interface");
	if (!val || !*val) {
		log_err("interface missing");
		return -1;
	}
	free(wifi->ifname);
	wifi->ifname = strdup(val);

	val = json_get_string(obj, "ap_interface");		/* optional */
	if (!val || !*val) {
		val = wifi->ifname;
		log_debug("no ap_interface specified");
	}
	free(wifi->ap_ifname);
	wifi->ap_ifname = strdup(val);

	item = json_object_get(obj, "simultaneous_ap_sta");	/* optional */
	if (item) {
		wifi->simultaneous_ap_sta = json_boolean_value(item);
	} else {
		/*
		 * If not explicitly configured, allow simultaneous AP and
		 * station mode if the Wi-Fi driver created a dedicated
		 * network interface for AP mode management.
		 */
		wifi->simultaneous_ap_sta =
		    strcmp(wifi->ifname, wifi->ap_ifname) != 0;
	}

	val = json_get_string(obj, "ap_ip_address");		/* optional */
	if (!val || !*val) {
		val = WIFI_AP_IP_ADDR_DEFAULT;
		log_debug("using default AP mode IP address: %s", val);
	}
	if (!inet_aton(val, &wifi->ap_ip_addr)) {
		log_err("invalid AP IP address: %s", val);
		return -1;
	}

	item = json_object_get(obj, "ap_window_at_startup");	/* optional */
	wifi->ap_window_at_startup = !item || json_boolean_value(item);

	item = json_object_get(obj, "ap_window_duration");	/* optional */
	if (!item || !json_is_integer(item)) {
		wifi->ap_window_mins = 0;
	} else {
		rc = json_integer_value(item);
		wifi->ap_window_mins = rc > 0 ? rc : 0;
	}

	item = json_object_get(obj, "ap_window_secure");	/* optional */
	wifi->ap_window_secure = json_boolean_value(item);

	val = json_get_string(obj, "profile_save_mode");	/* optional */
	if (!val || !*val) {
		wifi->prof_save_mode = WIFI_PROF_SAVE_MODE_DEFAULT;
	} else {
		rc = lookup_by_name(wifi_prof_save_mode_table, val);
		if (rc < 0) {
			log_warn("invalid prof_save_mode: %s", val);
			wifi->prof_save_mode = WIFI_PROF_SAVE_MODE_DEFAULT;
		} else {
			wifi->prof_save_mode = rc;
		}
	}

	item = json_object_get(obj, "profile");
	if (!item) {
		log_err("profiles missing");
		return -1;
	}
	if (wifi_conf_profs_import(wifi, item) < 0) {
		return -1;
	}
	item = json_object_get(obj, "ap_profile");
	if (!item) {
		log_err("ap_profile missing");
		return 0;
	}
	if (wifi_conf_prof_import(&wifi->ap_profile, item) < 0) {
		return -1;
	}
	/* If no AP SSID was set, use the default */
	if (!wifi->ap_profile.ssid.len) {
		strncpy((char *)wifi->ap_profile.ssid.val, WIFI_AP_SSID_DEFAULT,
		    sizeof(wifi->ap_profile.ssid.val));
		wifi->ap_profile.ssid.len = strlen(WIFI_AP_SSID_DEFAULT);
	}
	if (json_get_int(item, "channel", &wifi->ap_channel) < 0) {
		log_err("AP channel missing");
		return -1;
	}
	/* Attempt to expand any supported variables found in AP SSID */
	rc = str_expand_vars(wifi_conf_var_table,
	    (char *)wifi->ap_profile.ssid.val,
	    sizeof(wifi->ap_profile.ssid.val),
	    (const char *)wifi->ap_profile.ssid.val, wifi->ap_profile.ssid.len,
	    wifi);
	if (rc < 0) {
		/* Disable AP mode if SSID variable expansion failed */
		log_warn("invalid AP SSID: AP mode temporarily disabled");
		wifi->ap_enable = false;
	} else {
		wifi->ap_profile.ssid.len = rc;
		wifi->ap_enable = true;
	}
	/* Apply change to enable flag immediately */
	if (!wifi->enable) {
		wifi_shutdown();
	} else {
		wifi_step();
	}
	return 0;
}

/*
 * Create a JSON object containing Wi-Fi state
 */
static json_t *wifi_conf_get(void)
{
	struct wifi_state *wifi = &wifi_state;
	json_t *obj;
	json_t *ap_prof_obj;

	obj = json_object();

	json_object_set_new(obj, "enable", json_boolean(wifi->enable));
	/* Do not save "ap_enable".  Always enabled with valid ap_profile */
	json_object_set_new(obj, "interface", json_string(wifi->ifname));
	json_object_set_new(obj, "ap_interface", json_string(wifi->ap_ifname));
	json_object_set_new(obj, "simultaneous_ap_sta",
	    json_boolean(wifi->simultaneous_ap_sta));
	json_object_set_new(obj, "ap_ip_address",
	    json_string(inet_ntoa(wifi->ap_ip_addr)));
	json_object_set_new(obj, "ap_window_at_startup",
	    json_boolean(wifi->ap_window_at_startup));
	json_object_set_new(obj, "ap_window_duration",
	    json_integer(wifi->ap_window_mins));
	json_object_set_new(obj, "ap_window_secure",
	    json_boolean(wifi->ap_window_secure));
	json_object_set_new(obj, "profile_save_mode",
	    json_string(lookup_by_val(wifi_prof_save_mode_table,
	    wifi->prof_save_mode)));
	json_object_set_new(obj, "profile", wifi_conf_profs_export(wifi));
	ap_prof_obj = wifi_conf_prof_export(&wifi->ap_profile);
	json_object_set_new(ap_prof_obj, "channel",
	    json_integer(wifi->ap_channel));
	json_object_set_new(obj, "ap_profile", ap_prof_obj);
	return obj;
}

/*
 * Initialize Wi-Fi config subsystem.
 */
void wifi_conf_init(void)
{
	if (conf_register("wifi", wifi_conf_set, wifi_conf_get) < 0) {
		exit(1);
	}
}
