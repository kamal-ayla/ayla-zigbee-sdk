/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/types.h>
#include <signal.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <ayla/utypes.h>
#include <ayla/file_event.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/nameval.h>
#include <ayla/timer.h>
#include <ayla/async.h>
#include <ayla/socket.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/hex.h>

#include "../../cond.h"
#include "../../wifi.h"
#include "../../wifi_platform.h"
#include "wifi_platform_ioctl.h"

#define NO_OUTPUT_CMD		" >/dev/null 2>&1"

/* script invoked to configure the platform */
#define WIFI_CONTROL_SCRIPT	"wifi_control.sh"

/* Timeouts for async actions */
#define WIFI_PLATFORM_SCAN_TIMEOUT_MS 10000 /* max scan time */
#define WIFI_PLATFORM_ASSOCIATE_TIMEOUT_MS 12000 /* max associate time */
#define WIFI_PLATFORM_WPS_TIMEOUT_MS 60000 /* max WPS time */

#define WIFI_PLATFORM_RESULT_POLL_MS 200 /* time between polls */

/* field width for parsing scan results */
#define SCAN_FIELD_WIDTH_CHAN 4
#define SCAN_FIELD_WIDTH_SSID 33
#define SCAN_FIELD_WIDTH_BSSID 20
#define SCAN_FIELD_WIDTH_SEC 23
#define SCAN_FIELD_WIDTH_SIGNAL 9
#define SCAN_FIELD_WIDTH_WMODE 8
#define SCAN_FIELD_WIDTH_EXTCH 7
#define SCAN_FIELD_WIDTH_NT 3
#define SCAN_FIELD_WIDTH_WPS 4
#define SCAN_FIELD_WIDTH_DPID 4

#define WIFI_PLATFORM_WIFI_ENABLE_CMD \
	"uci set wireless.@wifi-device[0].disabled=0; uci commit wireless; wifi"

#define WIFI_PLATFORM_WIFI_DISABLE_CMD \
	"uci set wireless.@wifi-device[0].disabled=1; uci commit wireless; wifi"


/* max number of client that is allowed to connect to AP */
#define WIFI_PLATFORM_MAXSTANUM 5

/* Wifi ioctl set param string */
#define WIFI_PLATFORM_STR_VALUE_EMPTY ""
#define WIFI_PLATFORM_STR_VALUE_0 "0"
#define WIFI_PLATFORM_STR_VALUE_1 "1"
#define WIFI_PLATFORM_STR_OPEN "OPEN"
#define WIFI_PLATFORM_STR_NONE "NONE"

/* Wifi ioctl set operations definition */
#define WIFI_PLATFORM_IOCTL_SET_OPS(def) \
	def(ApCliEnable, SET_STA_ENABLE) \
	def(ApCliSsid, SET_STA_SSID) \
	def(ApCliAuthMode, SET_STA_AUTHMODE) \
	def(ApCliEncrypType, SET_STA_ENCRYPTYPE) \
	def(ApCliWPAPSK, SET_STA_WPAPSK) \
	def(ApCliDefaultKeyID, SET_STA_DEFAULTKEYID) \
	def(ApCliKey1, SET_STA_KEY1) \
	def(SiteSurvey, SET_STA_SURVEY) \
	def(Channel, SET_CHANNEL) \
	def(SSID, SET_AP_SSID) \
	def(AuthMode, SET_AP_AUTHMODE) \
	def(EncrypType, SET_AP_ENCRYPTYPE) \
	def(MaxStaNum, SET_AP_MAXSTANUM) \
	def(HideSSID, SET_AP_HIDESSID)

DEF_ENUM(wifi_ioctl_set_ops, WIFI_PLATFORM_IOCTL_SET_OPS);
static DEF_NAME_TABLE(wifi_ioctl_set_op_names, WIFI_PLATFORM_IOCTL_SET_OPS);

/*
 * Operations supported by Wi-Fi control script
 */
#define WIFI_SCRIPT_MODULES(def)		\
	def(station,		STATION)	\
	def(ap,			AP)		\
	def(dhcp-client,	DHCP_CLIENT)	\
	def(dhcp-server,	DHCP_SERVER)

DEF_ENUM(wifi_script_module, WIFI_SCRIPT_MODULES);
static DEF_NAME_TABLE(wifi_script_module_names, WIFI_SCRIPT_MODULES);

#define WIFI_SCRIPT_ACTIONS(def)		\
	def(start,		SCRIPT_START)	\
	def(stop,		SCRIPT_STOP)	\
	def(scan,		SCRIPT_SCAN)	\
	def(connect,		SCRIPT_CONNECT)	\
	def(disconnect,		SCRIPT_DISCONNECT)

DEF_ENUM(wifi_script_action, WIFI_SCRIPT_ACTIONS);
static DEF_NAME_TABLE(wifi_script_action_names, WIFI_SCRIPT_ACTIONS);

/*
 * Wi-Fi platform state
 */
struct wifi_platform_state {
	char *script_directory; /* directory to look for control scripts */

	bool station_enabled; /* Station mode enabled */
	bool ap_enabled; /* AP mode enabled */

	struct async_op scan; /* scan state */
	struct async_op associate; /* join state */
	struct async_op wps_pbc; /* WPS state */
	struct timer scan_poll_timer; /* scan result poll timer */
	struct timer assoc_poll_timer; /* associate result poll timer */
	const struct wifi_state *wifi; /* pointer to Wi-Fi state */
};

static struct wifi_platform_state state;

/*
 * Security string lookup tables
 */
static const struct name_val auth_names[] = {
	{ "NONE", WSEC_NONE },
	{ "WEP", WSEC_WEP },
	{ "WPA1PSK", WSEC_WPA | WSEC_PSK },
	{ "WPA2PSK", WSEC_WPA2 | WSEC_PSK },
	{ "WPAPSK", WSEC_WPA | WSEC_PSK },
	{ "WPA1", WSEC_WPA },
	{ "WPA2", WSEC_WPA2 },
	{ "WPA", WSEC_WPA },
	{NULL, -1}
};

static const struct name_val encryption_names[] = {
	{ "TKIPAES", WSEC_AES },
	{ "TKIP", WSEC_TKIP },
	{ "AES", WSEC_AES },
	{NULL, -1}
};

static const char *wifi_platform_auth_str(enum wifi_sec sec)
{
	const char *name;

	name = lookup_by_val(auth_names, sec &
		(WSEC_SEC_MASK | WSEC_PSK));
	if (!name) {
		log_err("unsupported authentication");
		return "NONE";
	}
	return name;
}

static const char *wifi_platform_crypto_str(enum wifi_sec sec)
{
	if (sec & WSEC_AES) {
		return "AES";
	}
	if (sec & WSEC_TKIP) {
		return "TKIP";
	}
	if (sec & WSEC_WEP) {
		return "WEP";
	}
	if ((sec & WSEC_SEC_MASK) == WSEC_NONE) {
		return "NONE";
	}
	log_err("unsupported encryption: %0X", sec);
	return "NONE";
}

/*
 * Populate an SSID struct from an ASCII string with "0x"
 * escaped hex bytes for unprintable bytes
 */
static int wifi_platform_parse_ssid(const char *input, struct wifi_ssid *ssid)
{
	const char *cp;
	ssize_t len;
	size_t cp_len;

	memset(ssid, 0, sizeof(*ssid));
	cp = input;
	if (*cp == '0' && *(cp + 1) == 'x') {
		len = hex_parse(ssid->val, sizeof(ssid->val), cp + 2, NULL);
		if (len < 0) {
			log_err("parse hex failed: %s", cp);
			return -1;
		}
		ssid->len = len;
		return 0;
	}

	cp_len = strlen(cp);
	if (cp_len > sizeof(ssid->val)) {
		log_err("ssid too long: %s", cp);
		return -1;
	}
	ssid->len = cp_len;
	memcpy(ssid->val, cp, ssid->len);
	return 0;
}

/*
 * Set Wi-Fi state from platform config.
 */
static int wifi_platform_conf_set(json_t *obj)
{
	const char *val;

	val = json_get_string(obj, "script_directory");	/* optional */
	if (!val || !*val) {
		val = "";
		log_debug("no script_directory specified");
	}
	free(state.script_directory);
	state.script_directory = strdup(val);

	return 0;
}

/*
 * Create a JSON object containing Wi-Fi platform state
 */
static json_t *wifi_platform_conf_get(void)
{
	json_t *obj;

	obj = json_object();

	json_object_set_new(obj, "script_directory",
	    json_string(state.script_directory ? state.script_directory : ""));

	/************************************************************
	 * Add handlers here to save any other config in
	 * wifi_platform { }
	 ************************************************************/

	return obj;
}

/*
 * Checks for absence of wifi_platform config and attempts to load
 * platform config from core wifi config group (old structure).
 * After this is performed, platform specific config will be written to the
 * wifi_platform group.
 */
static void wifi_platform_conf_migrate(void)
{
	json_t *conf;

	if (conf_get("wifi_platform")) {
		return;
	}
	conf = conf_get("wifi");
	if (conf) {
		log_debug("loading legacy config file");
		wifi_platform_conf_set(conf);
		conf_save();
	}
}

/*
 * Invoke a script called "wifi_control.sh" to configure the system.
 * This allows system builders to interact with their desired DHCP server
 * and client.  It also provides a hook to easily configure LEDS, invoke
 * scripts, or setup proprietary Wi-Fi module drivers.  The script may choose
 * to ignore some or all of these calls, depending on the implementer's
 * requirements..
 */
static int wifi_platform_run_control_script(enum wifi_script_module module,
	enum wifi_script_action action)
{
	const char *debug_opt = log_debug_enabled() ? "" : NO_OUTPUT_CMD;

	switch (module) {
	case STATION:
		switch (action) {
		case SCRIPT_START:
		case SCRIPT_STOP:
			/* Params: <module> <action> <interface> */
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT " %s %s %s%s",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action],
			    state.wifi->ifname,
			    debug_opt);
		default:
			break;
		}
		break;
	case AP:
		switch (action) {
		case SCRIPT_START:
		case SCRIPT_STOP:
			/*
			 * Params: <module> <action> <interface> <ssid> \
			 *         <channel>
			 */
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT " %s %s %s %s %d%s",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action],
			    state.wifi->ap_ifname,
			    wifi_ssid_to_str(
					&state.wifi->ap_profile.ssid),
			    state.wifi->ap_channel,
			    debug_opt);
		default:
			log_err("%s does not support %s action",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action]);
			return -1;
		}
		break;
	case DHCP_CLIENT:
		switch (action) {
		case SCRIPT_START:
		case SCRIPT_STOP:
			/* Params: <module> <action> <interface> */
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT " %s %s %s%s",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action],
			    state.wifi->ifname,
			    debug_opt);
		default:
			log_err("%s does not support %s action",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action]);
			return -1;
		}
		break;
	case DHCP_SERVER:
		switch (action) {
		case SCRIPT_START:
		case SCRIPT_STOP:
			/* Params: <module> <action> <interface> <ip addr> */
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT " %s %s %s %s%s",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action],
			    state.wifi->ap_ifname,
			    inet_ntoa(state.wifi->ap_ip_addr),
			    debug_opt);
		default:
			log_err("%s does not support %s action",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action]);
			return -1;
		}
		break;
	}
	return -1;
}

/*
 * Lookup the first token in an undelimited list of tokens.
 */
static int wifi_platform_parse_sec_list(const struct name_val *table,
	const char **list, enum wifi_sec *sec)
{
	size_t len;

	/* no more tokens */
	if (!*list || !**list) {
		return -1;
	}
	/* lookup first token in list */
	for (; table->name != NULL; ++table) {
		len = strlen(table->name);
		if (!strncmp(table->name, *list, len)) {
			*list += len;
			*sec = (enum wifi_sec)table->val;
			return 0;
		}
	}
	return -1;
}

/*
 * Parse security token.
 *
 * Format:
 *	NONE
 *	WEP
 *	AUTH/ENCRYPTION where AUTH and ENCRYPTION may be undelimited lists of
 *	multiple tokens
 *
 * Example:
 *	WPA1PSKWPA2PSK/TKIPAES
 */
static int wifi_platform_parse_sec(char *modes, struct wifi_scan_result *scan)
{
	enum wifi_sec sec;
	const char *auth;
	const char *crypto;
	const char *tmpCrypto;
	int count = 0;

	auth = strtok(modes, "/");
	if (auth != NULL) {
		crypto = strtok(NULL, "/");
	}
	while (!wifi_platform_parse_sec_list(auth_names, &auth, &sec)) {
		if (count >= WIFI_SCAN_SEC_CT) {
			log_err("more than %d security modes. excess ignored",
			    WIFI_SCAN_SEC_CT);
			break;
		}
		scan->sec[count] = sec;
		if (crypto != NULL) {
			tmpCrypto = crypto;
			if (!wifi_platform_parse_sec_list(encryption_names,
				&tmpCrypto, &sec)) {
				scan->sec[count] |= sec;
			}
		}
		scan->sec[count] |= WSEC_VALID;
		log_debug("%s: %0X", modes, scan->sec[count]);
		++count;
	}
	if (!count) {
		log_err("failed to parse security string: %s", modes);
	}
	return count;
}

static int wifi_platform_extract_field(const char *field, size_t len,
	char *buf, size_t buf_len)
{
	size_t field_len;
	const char *cp = field;

	/* truncate */
	field_len = strlen(cp);
	if (field_len < len) {
		len = field_len;
	}
	/* trim trailing space */
	while (len && *(cp + len - 1) == ' ') {
		--len;
	}

	/* trim leading whitespace */
	while (len && *cp == ' ') {
		--len;
		++cp;
	}

	if (!len || len >= buf_len) {
		log_err("extract field failed, len %zu, buf len %zu",
			len, buf_len);
		return -1;
	}
	memcpy(buf, cp, len);
	*(buf + len) = '\0';
	return 0;
}

/*
 * Parse a line of scan results.  Results are printed in a very difficult
 * to parse format, so the field widths are being hardcoded.
 *
 * Format:
 *	Ch, SSID, BSSID, Security, Siganl(%), W-Mode, ExtCH, NT, WPS, DPID
 */
static int wifi_platform_parse_scan_line(const char *line,
	struct wifi_scan_result *scan)
{
	const char *cp;
	struct ether_addr *mac;
	char buf[64];
	int rc;

	cp = line;
	/* extract and trim fields */
	rc = wifi_platform_extract_field(cp, SCAN_FIELD_WIDTH_CHAN,
		buf, sizeof(buf));
	if (rc) {
		return -1;
	}
	scan->chan = strtoul(buf, NULL, 10);
	if (!scan->chan) {
		log_err("invalid channel: %s", buf);
		return -1;
	}

	cp += SCAN_FIELD_WIDTH_CHAN;
	rc = wifi_platform_extract_field(cp, SCAN_FIELD_WIDTH_SSID,
		buf, sizeof(buf));
	if (rc) {
		return -1;
	}
	if (wifi_platform_parse_ssid(buf, &scan->ssid) < 0) {
		log_err("invalid SSID: %s", buf);
		return -1;
	}

	cp += SCAN_FIELD_WIDTH_SSID;
	rc = wifi_platform_extract_field(cp, SCAN_FIELD_WIDTH_BSSID,
		buf, sizeof(buf));
	if (rc) {
		return -1;
	}
	mac = ether_aton(buf);
	if (!mac) {
		log_err("invalid BSSID: %s", buf);
		return -1;
	}
	scan->bssid = *mac;

	cp += SCAN_FIELD_WIDTH_BSSID;
	rc = wifi_platform_extract_field(cp, SCAN_FIELD_WIDTH_SEC,
		buf, sizeof(buf));
	if (rc) {
		return -1;
	}
	if (wifi_platform_parse_sec(buf, scan) < 0) {
		log_err("invalid security: %s", buf);
		return -1;
	}

	cp += SCAN_FIELD_WIDTH_SEC;
	rc = wifi_platform_extract_field(cp, SCAN_FIELD_WIDTH_SIGNAL,
		buf, sizeof(buf));
	if (rc) {
		return -1;
	}
	scan->signal = strtol(buf, NULL, 10);
	if (scan->signal >= 0) {
		/* perform rough % to dBm conversion */
		scan->signal = (scan->signal / 2) - 100;
	}
	if (scan->signal > 0 || scan->signal < WIFI_SIGNAL_MIN) {
		scan->signal = scan->signal > 0 ? 0 : WIFI_SIGNAL_MIN;
		log_warn("signal %s limited to %hhd", buf, scan->signal);
	}

	cp += SCAN_FIELD_WIDTH_SIGNAL;
	cp += SCAN_FIELD_WIDTH_WMODE;
	cp += SCAN_FIELD_WIDTH_EXTCH;
	cp += SCAN_FIELD_WIDTH_NT;
	rc = wifi_platform_extract_field(cp, SCAN_FIELD_WIDTH_WPS,
		buf, sizeof(buf));
	if (rc) {
		return -1;
	}
	if (!strcasecmp(buf, "YES")) {
		scan->wps_supported = 1;
	} else if (strcasecmp(buf, "NO")) {
		log_err("invalid WPS: %s", buf);
		return -1;
	}
	return 0;
}

/*
 * Parse scan results
 */
static int wifi_platform_parse_scan_results(char *buf)
{
	struct wifi_scan_result scan;
	char *line;
	int i = 0;
	char *strParse = buf;

	if (*strParse == '\n') {
		strParse = strParse + 1;
	}

	line = strsep(&strParse, "\n");
	if (!line || !line[0]) {
		log_err("no results");
		return -1;
	}

	/* clear old scan results */
	wifi_scan_clear();

	log_debug("line %d: %s", i, line);

	for (;;) {
		line = strsep(&strParse, "\n");
		if (!line || !line[0]) {
			break;
		}

		i++;

		log_debug("line %d: %s", i, line);

		memset(&scan, 0, sizeof(scan));
		/* Parse scan results in each line */
		if (wifi_platform_parse_scan_line(line, &scan) < 0) {
			log_debug("failed to parse scan line");
			continue;
		}

		log_debug("Add scan result %d: ssid:%s, "
			"chan:%u, signal:%d, sec:0x%x",
			i, wifi_ssid_to_str(&scan.ssid), scan.chan,
			scan.signal, scan.sec[0]);

		/* Add a new scan entry */
		if (wifi_scan_add(&scan) != 0) {
			log_err("Add scan results failed");
		}
	}
	return 0;
}

/*
 * Scan complete handler.
 */
static void wifi_platform_scan_complete_handler(int result, void *arg)
{
	void (*callback)(enum wifi_platform_result) = arg;

	/* Cleanup poll timer */
	wifi_timer_clear(&state.scan_poll_timer);

	if (callback) {
		callback((enum wifi_platform_result)result);
	}
}

/*
 * Poll for scan results
 */
static void wifi_platform_scan_poll_timeout(struct timer *timer)
{
	char *scan_buf = NULL;
	int scan_buf_len;
	int rc;

	if (!state.scan.active) {
		log_err("scan not active");
		return;
	}

	/* get scan result */
	rc = wifi_platform_get_scan_result(state.wifi->ifname,
		&scan_buf, &scan_buf_len);
	if (rc) {
		log_err("get scan result failed");
		goto scan_poll_error;
	}

	rc = wifi_platform_parse_scan_results(scan_buf);
	if (rc) {
		log_err("parse scan result failed");
		goto scan_poll_error;
	}

	free(scan_buf);
	wifi_timer_clear(&state.scan_poll_timer);
	log_debug("parse scan result success");
	async_op_finish(&state.scan, PLATFORM_SUCCESS);
	return;

scan_poll_error:
	free(scan_buf);
	wifi_timer_set(&state.scan_poll_timer,
		WIFI_PLATFORM_RESULT_POLL_MS);
	return;
}


/*
 * Associate complete handler.
 */
static void wifi_platform_associate_complete_handler(int result, void *arg)
{
	void (*callback)(enum wifi_platform_result) = arg;

	/* Cleanup poll timer */
	wifi_timer_clear(&state.assoc_poll_timer);

	if (result == PLATFORM_SUCCESS) {
		wifi_platform_run_control_script(DHCP_CLIENT, SCRIPT_START);
	}

	if (callback) {
		callback((enum wifi_platform_result)result);
	}
}

/*
 * Poll for associate results.
 */
static void wifi_platform_assoc_poll_timeout(struct timer *timer)
{
	int connect_status = 0;
	int rc;

	if (!state.associate.active) {
		log_err("associate not active");
		return;
	}

	/* determine association success or failure */
	rc = wifi_platform_get_connstatus(state.wifi->ifname,
		&connect_status);
	if (rc) {
		log_err("failed to get connect status");
		wifi_timer_set(&state.assoc_poll_timer,
			WIFI_PLATFORM_RESULT_POLL_MS);
		return;
	}

	if (connect_status) {
		log_debug("associate success");
		wifi_timer_clear(&state.assoc_poll_timer);
		async_op_finish(&state.associate, PLATFORM_SUCCESS);
	} else {
		log_debug("associating");
		wifi_timer_set(&state.assoc_poll_timer,
			WIFI_PLATFORM_RESULT_POLL_MS);
	}
}

/*
 * Initialize platform interface.  Perform actions needed before configuration
 * is loaded.
 */
void wifi_platform_init(const struct wifi_state *wifi_state)
{
	state.wifi = wifi_state;

	wifi_script_run(NULL, WIFI_PLATFORM_WIFI_ENABLE_CMD);

	conf_register("wifi_platform", wifi_platform_conf_set,
	    wifi_platform_conf_get);

	async_op_init(&state.scan, &cond_state.timers);
	async_op_init(&state.associate, &cond_state.timers);
	async_op_init(&state.wps_pbc, &cond_state.timers);
	async_op_set_timeout_result(&state.scan, PLATFORM_FAILURE);
	async_op_set_timeout_result(&state.associate, PLATFORM_FAILURE);
	async_op_set_timeout_result(&state.wps_pbc, PLATFORM_FAILURE);

	wifi_timer_init(&state.scan_poll_timer,
		wifi_platform_scan_poll_timeout);
	wifi_timer_init(&state.assoc_poll_timer,
		wifi_platform_assoc_poll_timeout);
}

/*
 * Perform startup checks and setup needed after configuration is loaded.
 */
int wifi_platform_start(void)
{
	/* Check "wifi" config for backwards compatibility */
	wifi_platform_conf_migrate();

	/* Check availability of external control script */
	if (wifi_script_run(state.script_directory,
	    WIFI_CONTROL_SCRIPT NO_OUTPUT_CMD) == -1) {
		log_err("fatal: " WIFI_CONTROL_SCRIPT " unavailable");
		return -1;
	}
	log_debug("using " WIFI_CONTROL_SCRIPT);
	return 0;
}

/*
 * Stop Wi-Fi
 */
int wifi_platform_exit(void)
{
	int rc = 0;

	if (state.ap_enabled) {
		rc |= wifi_platform_ap_stop();
	}
	if (state.station_enabled) {
		rc |= wifi_platform_station_stop();
	}

	rc |= wifi_script_run(NULL, WIFI_PLATFORM_WIFI_DISABLE_CMD);
	return rc;
}

/*
 * Enable station mode and prepare to connect to a network.
 */
int wifi_platform_station_start(void)
{
	if (state.station_enabled) {
		log_debug("station already enabled");
		return 0;
	}

	/* Invoke script for platform setup */
	if (wifi_platform_run_control_script(STATION, SCRIPT_START)) {
		return -1;
	}

	/************************************************************
	 * Configure the Wi-Fi driver to enable the client interface.
	 ************************************************************/
	log_info("enable station (%s)", state.wifi->ifname);

	state.station_enabled = true;

	return 0;
}

/*
 * Disable station mode.
 */
int wifi_platform_station_stop(void)
{
	int rc = 0;

	if (!state.station_enabled) {
		log_debug("station not enabled");
		return 0;
	}
	state.station_enabled = false;

	/************************************************************
	 * Configure the Wi-Fi driver to disable the client interface.
	 ************************************************************/
	log_info("disable station (%s)", state.wifi->ifname);

	/* call ioctl interface to stop station */
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_AUTHMODE],
		WIFI_PLATFORM_STR_OPEN,
		strlen(WIFI_PLATFORM_STR_OPEN))) {
		log_err("failed to set station auth mode");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENCRYPTYPE],
		WIFI_PLATFORM_STR_NONE,
		strlen(WIFI_PLATFORM_STR_NONE))) {
		log_err("failed to set station encrypt type");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_SSID],
		WIFI_PLATFORM_STR_VALUE_EMPTY,
		strlen(WIFI_PLATFORM_STR_VALUE_EMPTY))) {
		rc = -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		rc = -1;
	}

	if (wifi_platform_run_control_script(STATION, SCRIPT_STOP)) {
		rc = -1;
	}

	if (wifi_platform_run_control_script(DHCP_CLIENT, SCRIPT_STOP)) {
		rc = -1;
	}
	return rc;
}

/*
 * Return true if station mode is enabled.
 */
bool wifi_platform_station_enabled(void)
{
	return state.station_enabled;
}

/*
 * Enable AP mode, and configure the AP and associated network interface
 * with the specified parameters.
 */
int wifi_platform_ap_start(const struct wifi_profile *prof, int channel,
	const struct in_addr *ip_addr)
{
	char channel_str[16];
	char maxstanum_str[16];

	if (state.ap_enabled) {
		log_debug("AP already enabled");
		return 0;
	}

	/* call ioctl interface to start ap */
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_SSID],
		(const char *)state.wifi->ap_profile.ssid.val,
		state.wifi->ap_profile.ssid.len)) {
		return -1;
	}
	snprintf(channel_str, sizeof(channel_str), "%u",
		state.wifi->ap_channel);
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_CHANNEL], channel_str,
		strlen(channel_str))) {
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_AUTHMODE],
		WIFI_PLATFORM_STR_OPEN,
		strlen(WIFI_PLATFORM_STR_OPEN))) {
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_ENCRYPTYPE],
		WIFI_PLATFORM_STR_NONE,
		strlen(WIFI_PLATFORM_STR_NONE))) {
		return -1;
	}
	snprintf(maxstanum_str, sizeof(maxstanum_str), "%u",
		WIFI_PLATFORM_MAXSTANUM);
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_MAXSTANUM], maxstanum_str,
		strlen(maxstanum_str))) {
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_HIDESSID],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		return -1;
	}

	/************************************************************
	 * Configure the Wi-Fi driver to enable the access point.
	 ************************************************************/
	log_info("enable AP (%s)", state.wifi->ap_ifname);

	if (wifi_platform_run_control_script(DHCP_SERVER, SCRIPT_START) < 0) {
		return -1;
	}
	state.ap_enabled = true;
	return 0;
}

/*
 * Disable AP mode.
 */
int wifi_platform_ap_stop(void)
{
	int rc = 0;

	if (!state.ap_enabled) {
		log_debug("AP not enabled");
		return 0;
	}
	state.ap_enabled = false;

	/************************************************************
	 * Configure the Wi-Fi driver to disable the access point.
	 ************************************************************/
	log_info("disable AP (%s)", state.wifi->ap_ifname);

	/* set max sta num to 0 */
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_MAXSTANUM],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		rc = -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_SSID],
		WIFI_PLATFORM_STR_VALUE_EMPTY,
		strlen(WIFI_PLATFORM_STR_VALUE_EMPTY))) {
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ap_ifname,
		wifi_ioctl_set_op_names[SET_AP_HIDESSID],
		WIFI_PLATFORM_STR_VALUE_1,
		strlen(WIFI_PLATFORM_STR_VALUE_1))) {
		return -1;
	}

	if (wifi_platform_run_control_script(DHCP_SERVER, SCRIPT_STOP)) {
		rc = -1;
	}
	return rc;
}

/*
 * Return true if AP mode is enabled.
 */
bool wifi_platform_ap_enabled(void)
{
	return state.ap_enabled;
}

/*
 * Return the number of stations connected to the AP, or -1 on error;
 */
int wifi_platform_ap_stations_connected(void)
{
	if (!state.ap_enabled) {
		log_warn("AP not enabled");
		return -1;
	}

	/************************************************************
	 * Return the number of stations connected to the AP.
	 ************************************************************/
	log_info("return # of connected stations");

	return -1;
}

/*
 * Start a new scan job.  Return 0 on success and -1 on error.  If a callback
 * is provided and this function returned success, the callback must be invoked
 * to indicate the result of the operation.
 */
int wifi_platform_scan(void (*callback)(enum wifi_platform_result))
{
	if (state.scan.active) {
		log_err("busy: scan in progress");
		return 0;
	}

	/* request a new scan */
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_SURVEY],
		WIFI_PLATFORM_STR_VALUE_EMPTY,
		strlen(WIFI_PLATFORM_STR_VALUE_EMPTY))) {
		log_err("station start scan error");
		return -1;
	}

	/* start to poll scan results */
	wifi_timer_set(&state.scan_poll_timer,
		WIFI_PLATFORM_RESULT_POLL_MS);
	async_op_start(&state.scan,
		wifi_platform_scan_complete_handler,
		callback,
	    WIFI_PLATFORM_SCAN_TIMEOUT_MS);

	return 0;
}

/*
 * Cancel an ongoing scan job.
 */
void wifi_platform_scan_cancel(void)
{
	if (!state.scan.active) {
		log_debug("no scan in progress");
		return;
	}
	async_op_finish(&state.scan, PLATFORM_CANCELED);
}

/*
 * Return true if a scan is in progress.
 */
bool wifi_platform_scanning(void)
{
	return state.scan.active;
}

static int wifi_platform_associate_sec_none(const struct wifi_profile *prof)
{
	char channel_str[16];

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		log_err("failed to set station disable");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_SSID],
		(const char *)prof->ssid.val, prof->ssid.len)) {
		log_err("failed to set station ssid");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_AUTHMODE],
		WIFI_PLATFORM_STR_OPEN,
		strlen(WIFI_PLATFORM_STR_OPEN))) {
		log_err("failed to set station auth mode");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENCRYPTYPE],
		WIFI_PLATFORM_STR_NONE,
		strlen(WIFI_PLATFORM_STR_NONE))) {
		log_err("failed to set station encrypt type");
		return -1;
	}

	snprintf(channel_str, sizeof(channel_str), "%u",
		prof->scan->chan);
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_CHANNEL], channel_str,
		strlen(channel_str))) {
		log_err("failed to set station channel");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_1,
		strlen(WIFI_PLATFORM_STR_VALUE_1))) {
		log_err("failed to set station enable");
		return -1;
	}

	return 0;
}

static int wifi_platform_associate_sec_wep(const struct wifi_profile *prof)
{
	char channel_str[16];
	char key_hex[WIFI_MAX_KEY_LEN * 2 + 1];

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		log_err("failed to set station disable");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_SSID],
		(const char *)prof->ssid.val, prof->ssid.len)) {
		log_err("failed to set station ssid");
		return -1;
	}

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENCRYPTYPE],
		wifi_platform_crypto_str(prof->sec),
		strlen(wifi_platform_crypto_str(prof->sec)))) {
		log_err("failed to set station encrypt type");
		return -1;
	}

	hex_string(key_hex, sizeof(key_hex), prof->key.val,
				prof->key.len, false, 0);

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_AUTHMODE],
		WIFI_PLATFORM_STR_OPEN,
		strlen(WIFI_PLATFORM_STR_OPEN))) {
		log_err("failed to set station auth mode");
		return -1;
	}

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_DEFAULTKEYID],
		WIFI_PLATFORM_STR_VALUE_1,
		strlen(WIFI_PLATFORM_STR_VALUE_1))) {
		log_err("failed to set station key id");
		return -1;
	}

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_KEY1],
		key_hex, strlen(key_hex))) {
		log_err("failed to set station key");
		return -1;
	}

	snprintf(channel_str, sizeof(channel_str), "%u",
		prof->scan->chan);
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_CHANNEL], channel_str,
		strlen(channel_str))) {
		log_err("failed to set station channel");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_1,
		strlen(WIFI_PLATFORM_STR_VALUE_1))) {
		log_err("failed to set station enable");
		return -1;
	}

	return 0;
}

static int wifi_platform_associate_sec_wpa(const struct wifi_profile *prof)
{
	char channel_str[16];
	const char *auth_str;
	const char *crypto_str;

	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		log_err("failed to set station disable");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_SSID],
		(const char *)prof->ssid.val, prof->ssid.len)) {
		log_err("failed to set station ssid");
		return -1;
	}
	auth_str = wifi_platform_auth_str(prof->sec);
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_AUTHMODE],
		auth_str, strlen(auth_str))) {
		log_err("failed to set station auth mode");
		return -1;
	}
	crypto_str = wifi_platform_crypto_str(prof->sec);
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENCRYPTYPE],
		crypto_str, strlen(crypto_str))) {
		log_err("failed to set station encrypt type");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_WPAPSK],
		(const char *)prof->key.val, prof->key.len)) {
		log_err("failed to set station key");
		return -1;
	}
	snprintf(channel_str, sizeof(channel_str), "%u",
		prof->scan->chan);
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_CHANNEL], channel_str,
		strlen(channel_str))) {
		log_err("failed to set station channel");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_1,
		strlen(WIFI_PLATFORM_STR_VALUE_1))) {
		log_err("failed to set station enable");
		return -1;
	}

	return 0;
}


/*
 * Configure a network and attempt to associate with it.
 * Return 0 on success and -1 on error.  If a callback is provided and this
 * function returned success, the callback must be invoked to indicate the
 * result of the operation.
 */
int wifi_platform_associate(const struct wifi_profile *prof,
	void (*callback)(enum wifi_platform_result))
{
	if (state.associate.active) {
		log_warn("busy: already associating");
		return -1;
	}

	if (!prof || !prof->scan) {
		log_err("profile or scan result is NULL");
		return -1;
	}

	if (!(prof->sec & WSEC_VALID)) {
		log_err("security is not valid");
		return -1;
	}

	switch (prof->sec & WSEC_SEC_MASK) {
	case WSEC_WEP:
		if (wifi_platform_associate_sec_wep(prof)) {
			log_err("associate with wep sec AP failed");
			return -1;
		}
	break;

	case WSEC_WPA:
	case WSEC_WPA2:
	case WSEC_WPA | WSEC_WPA2:
		if (wifi_platform_associate_sec_wpa(prof)) {
			log_err("associate with wpa sec AP failed");
			return -1;
		}
	break;

	case WSEC_NONE:
		if (wifi_platform_associate_sec_none(prof)) {
			log_err("associate with none sec AP failed");
			return -1;
		}
	break;

	default:
		log_err("unsupported security");
		return -1;
	}

	/************************************************************
	 * Ask the Wi-Fi driver to associate with the specified network here.
	 *
	 * Invoke async_op_finish() when association is complete
	 * to indicate the result and advance the wifi state machine.
	 * If async_op_finish() is not called before the timeout,
	 * callback will be invoked with a PLATFORM_FAILURE result.
	 *
	 * If using wifi_control.sh to control the DHCP client,
	 * start the DHCP client if association is successful by invoking
	 * wifi_platform_run_control_script(DHCP_CLIENT, SCRIPT_START).
	 ************************************************************/
	log_info("associate with SSID %s  [%s-%s]",
	    wifi_ssid_to_str(&prof->ssid),
	    wifi_platform_auth_str(prof->sec),
	    wifi_platform_crypto_str(prof->sec));

	wifi_timer_set(&state.assoc_poll_timer,
		WIFI_PLATFORM_RESULT_POLL_MS);
	async_op_start(&state.associate,
		wifi_platform_associate_complete_handler,
		callback,
	    WIFI_PLATFORM_ASSOCIATE_TIMEOUT_MS);

	return 0;
}

/*
 * Cancel an ongoing attempt to associate with a network.
 */
void wifi_platform_associate_cancel(void)
{
	if (!state.associate.active) {
		log_debug("not associating");
		return;
	}
	wifi_platform_leave_network();
	async_op_finish(&state.associate, PLATFORM_CANCELED);
}

/*
 * Return true if associating.
 */
bool wifi_platform_associating(void)
{
	return state.associate.active;
}

/*
 * Disable current network
 */
int wifi_platform_leave_network(void)
{
	int rc = 0;

	if (!state.station_enabled) {
		return 0;
	}

	/************************************************************
	 * Ask the Wi-Fi driver to disconnect here.
	 ************************************************************/
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_AUTHMODE],
		WIFI_PLATFORM_STR_OPEN,
		strlen(WIFI_PLATFORM_STR_OPEN))) {
		log_err("failed to set station auth mode");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENCRYPTYPE],
		WIFI_PLATFORM_STR_NONE,
		strlen(WIFI_PLATFORM_STR_NONE))) {
		log_err("failed to set station encrypt type");
		return -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_SSID],
		WIFI_PLATFORM_STR_VALUE_EMPTY,
		strlen(WIFI_PLATFORM_STR_VALUE_EMPTY))) {
		rc = -1;
	}
	if (wifi_platform_ioctl_set_net(state.wifi->ifname,
		wifi_ioctl_set_op_names[SET_STA_ENABLE],
		WIFI_PLATFORM_STR_VALUE_0,
		strlen(WIFI_PLATFORM_STR_VALUE_0))) {
		rc = -1;
	}

	/************************************************************
	 * If DHCP client is running, consider disabling it.
	 ************************************************************/
	return rc;
}


/*
 * Start WPS.  Return 0 on success and -1 on error.  If a callback
 * is provided and this function returned success, the callback must be invoked
 * to indicate the result of the operation.
 */
int wifi_platform_wps_start(void (*callback)(enum wifi_platform_result))
{
	if (!state.station_enabled) {
		log_err("station not enabled");
		return -1;
	}

	log_err("wps has not been supported");
	return -1;
}

/*
 * Cancel WPS.
 */
void wifi_platform_wps_cancel(void)
{
	if (!state.wps_pbc.active) {
		log_debug("WPS inactive");
		return;
	}
	log_err("wps has not been supported");
	return;
}

/*
 * Return true if WPS is active.
 */
bool wifi_platform_wps_started(void)
{
	return state.wps_pbc.active;
}
