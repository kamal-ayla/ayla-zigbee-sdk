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

#include "../../cond.h"
#include "../../wifi.h"
#include "../../wifi_platform.h"

#define NO_OUTPUT_CMD		" >/dev/null 2>&1"

/* script invoked to configure the platform */
#define WIFI_CONTROL_SCRIPT	"wifi_control.sh"

/* Timeouts for async actions */
#define WIFI_PLATFORM_SCAN_TIMEOUT_MS		10000	/* max scan time */
#define WIFI_PLATFORM_ASSOCIATE_TIMEOUT_MS	12000	/* max associate time */
#define WIFI_PLATFORM_WPS_TIMEOUT_MS		60000	/* max WPS time */

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
	char *script_directory;	/* directory to look for control scripts */

	bool station_enabled;		/* Station mode enabled */
	bool ap_enabled;		/* AP mode enabled */

	struct async_op scan;		/* scan state */
	struct async_op associate;	/* join state */
	struct async_op wps_pbc;	/* WPS state */

	const struct wifi_state *wifi;	/* pointer to Wi-Fi state */
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
	{ "WPA1", WSEC_WPA },
	{ "WPA2", WSEC_WPA2 },
	{NULL, -1}
};

/*
static const struct name_val encryption_names[] = {
	{ "TKIP", WSEC_TKIP },
	{ "AES", WSEC_AES },
	{ "CCMP", WSEC_CCMP },
	{NULL, -1}
};
*/


static const char *wifi_platform_auth_str(enum wifi_sec sec)
{
	const char *name;

	name = lookup_by_val(auth_names, sec & (WSEC_SEC_MASK | WSEC_PSK));
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
	if ((sec & WSEC_SEC_MASK) == WSEC_NONE) {
		return "NONE";
	}
	log_err("unsupported encryption: %0X", sec);
	return "NONE";
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

	/************************************************************
	 * Add handlers here to load any other config in
	 * wifi_platform { }
	 ************************************************************/

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
 * Async operation complete callback.  Simply invokes the supplied callback
 * with wifi_platform_result argument.
 */
static void wifi_platform_async_callback(int result, void *arg)
{
	void (*callback)(enum wifi_platform_result) = arg;

	if (callback) {
		callback((enum wifi_platform_result)result);
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
		case SCRIPT_DISCONNECT:
		case SCRIPT_SCAN:
			/* Params: <module> <action> <interface> */
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT " %s %s %s%s",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action],
			    state.wifi->ifname,
			    debug_opt);
		case SCRIPT_CONNECT:
			/*
			 * Params: <module> <action> <interface> <ssid> \
			 *         <channel> <auth> <encryption> <key>
			 */
			if (!state.wifi->curr_profile ||
			    !state.wifi->curr_profile->scan) {
				log_err("no valid profile to connect to");
				return -1;
			}
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT
			    " connect %s %s %d %s %s \"%s\"%s",
			    state.wifi->ifname,
			    wifi_ssid_to_str(&state.wifi->curr_profile->ssid),
			    state.wifi->curr_profile->scan->chan,
			    wifi_platform_auth_str(
			    state.wifi->curr_profile->sec),
			    wifi_platform_crypto_str(
			    state.wifi->curr_profile->sec),
			    (const char *)state.wifi->curr_profile->key.val,
			    debug_opt);
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
			    wifi_ssid_to_str(&state.wifi->ap_profile.ssid),
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
 * Initialize platform interface.  Perform actions needed before configuration
 * is loaded.
 */
void wifi_platform_init(const struct wifi_state *wifi_state)
{
	state.wifi = wifi_state;

	conf_register("wifi_platform", wifi_platform_conf_set,
	    wifi_platform_conf_get);

	async_op_init(&state.scan, &cond_state.timers);
	async_op_init(&state.associate, &cond_state.timers);
	async_op_init(&state.wps_pbc, &cond_state.timers);
	async_op_set_timeout_result(&state.scan, PLATFORM_FAILURE);
	async_op_set_timeout_result(&state.associate, PLATFORM_FAILURE);
	async_op_set_timeout_result(&state.wps_pbc, PLATFORM_FAILURE);
}

/*
 * Perform startup checks and setup needed after configuration is loaded.
 */
int wifi_platform_start(void)
{
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
	log_info("TODO: enable station (%s)", state.wifi->ifname);

	state.station_enabled = true;

	return 0;
}

/*
 * Disable station mode.
 */
int wifi_platform_station_stop(void)
{
	int rc;

	if (!state.station_enabled) {
		log_debug("station not enabled");
		return 0;
	}
	state.station_enabled = false;

	/************************************************************
	 * Configure the Wi-Fi driver to disable the client interface.
	 ************************************************************/
	log_info("TODO: disable station (%s)", state.wifi->ifname);

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
	if (state.ap_enabled) {
		log_debug("AP already enabled");
		return 0;
	}
	/* Invoke script for platform setup */
	if (wifi_platform_run_control_script(AP, SCRIPT_START) < 0) {
		return -1;
	}

	/************************************************************
	 * Configure the Wi-Fi driver to enable the access point.
	 ************************************************************/
	log_info("TODO: enable AP (%s)", state.wifi->ap_ifname);

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
	int rc;

	if (!state.ap_enabled) {
		log_debug("AP not enabled");
		return 0;
	}
	state.ap_enabled = false;

	/************************************************************
	 * Configure the Wi-Fi driver to disable the access point.
	 ************************************************************/
	log_info("TODO: disable AP (%s)", state.wifi->ap_ifname);

	if (wifi_platform_run_control_script(AP, SCRIPT_STOP)) {
		rc = -1;
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
	log_info("TODO: return # of connected stations");

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
	/* Invoke script for platform setup */
	if (wifi_platform_run_control_script(STATION, SCRIPT_SCAN) < 0) {
		return -1;
	}

	/************************************************************
	 * Ask the Wi-Fi driver to start a new scan here.
	 *
	 * When the scan is complete, perform the following steps:
	 * 1. Call wifi_scan_clear() to clear the cached scan results.
	 * 2. Read the scan results from the Wi-Fi driver, and call
	 *    wifi_scan_add() to load each result.
	 * 3. Invoke async_op_finish() to indicate the scan job
	 *     result and advance the wifi state machine.
	 *
	 * If async_op_finish() is not called before the timeout,
	 * callback will be invoked with a PLATFORM_FAILURE result.
	 ************************************************************/
	log_info("TODO: start scan");

	async_op_start(&state.scan, wifi_platform_async_callback, callback,
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
	/* Invoke script for platform setup */
	if (wifi_platform_run_control_script(STATION, SCRIPT_CONNECT) < 0) {
		log_err("failed to connect to network");
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
	log_info("TODO: associate with SSID %s  [%s-%s]",
	    wifi_ssid_to_str(&prof->ssid),
	    wifi_platform_auth_str(prof->sec),
	    wifi_platform_crypto_str(prof->sec));

	log_debug("selection complete");
	async_op_start(&state.associate, wifi_platform_async_callback, callback,
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
	if (!state.station_enabled) {
		return 0;
	}

	/************************************************************
	 * Ask the Wi-Fi driver to disconnect here.
	 ************************************************************/

	/* Invoke script for platform setup */
	if (wifi_platform_run_control_script(STATION, SCRIPT_DISCONNECT)) {
		return -1;
	}

	/************************************************************
	 * If DHCP client is running, consider disabling it.
	 ************************************************************/
	return 0;
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
	if (state.wps_pbc.active) {
		log_warn("busy: WPS PBC already requested");
		return 0;
	}

	/************************************************************
	 * Ask the Wi-Fi driver to start a WPS scan here.
	 *
	 * Invoke async_op_finish() when complete
	 * to indicate the result and advance the wifi state machine.
	 * If async_op_finish() is not called before the timeout,
	 * callback will be invoked with a PLATFORM_FAILURE result.
	 ************************************************************/
	log_info("TODO: start WPS scan");

	async_op_start(&state.wps_pbc, wifi_platform_async_callback, callback,
	    WIFI_PLATFORM_WPS_TIMEOUT_MS);
	return 0;
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
	log_err("TODO");
	async_op_finish(&state.wps_pbc, PLATFORM_CANCELED);
}

/*
 * Return true if WPS is active.
 */
bool wifi_platform_wps_started(void)
{
	return state.wps_pbc.active;
}
