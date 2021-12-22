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
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/wait.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/un.h>

#include <ayla/utypes.h>
#include <ayla/file_event.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/nameval.h>
#include <ayla/timer.h>
#include <ayla/async.h>
#include <ayla/socket.h>
#include <ayla/log.h>
#include <ayla/hex.h>
#include <ayla/assert.h>

#include <wpa_ctrl.h>	/* wpa_supplicant interface (hostapd/src/common/) */

#include "../../cond.h"
#include "../../wifi.h"
#include "../../wifi_platform.h"


#define NO_OUTPUT_CMD			" >/dev/null 2>&1"

/* Script invoked to configure the platform */
#define WIFI_CONTROL_SCRIPT		"wifi_control.sh"

/* Default control interface directory for Wi-Fi daemons */
#define SUPPLICANT_SOCK_DIR		"/var/run/wpa_supplicant"
#define HOSTAPD_SOCK_DIR		"/var/run/hostapd"

/* PID files to facilitate terminating Wi-Fi daemons */
#define SUPPLICANT_PID_FILE		"/var/run/wpa_supplicant.pid"
#define HOSTAPD_PID_FILE		"/var/run/hostapd.pid"

/* Default config files for Wi-Fi daemons */
#define HOSTAPD_LIVE_CONFIG		"/var/run/hostapd.conf"

/* Timeouts for async actions */
#define WIFI_PLATFORM_SCAN_TIMEOUT_MS		10000	/* max scan time */
#define WIFI_PLATFORM_ASSOCIATE_TIMEOUT_MS	12000	/* max associate time */
#define WIFI_PLATFORM_WPS_TIMEOUT_MS		60000	/* max WPS time */

#define CMD_RETRY_DELAY	500	/* ms, delay before response timeout */
#define CMD_RETRIES	3	/* # of resends if cmd failed */
#define EVENT_TIMEOUT	8000	/* ms, delay between status polls */
#define RESP_BUF_LEN	4096	/* response buffer size on stack */
#define EVENT_BUF_LEN	256	/* event message buffer size */
#define DAEMON_POLL_DELAY 50	/* ms, poll period for daemon exit */
#define DAEMON_TIMEOUT	5000	/* ms, max wait for daemon response */

enum daemon_type {
	WPA_SUPPLICANT,		/* manages Wi-Fi interface in station mode */
	HOSTAPD,		/* manages Wi-Fi interface in AP mode */

	DAEMON_COUNT		/* # of daemon types (MUST be last) */
};

#define DAEMON_TYPES { \
	[WPA_SUPPLICANT] =	"WPA_SUPPLICANT", \
	[HOSTAPD] =		"HOSTAPD", \
}

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
 * Wi-Fi management daemon control channel state
 */
struct ctrl_channel {
	char sock_path[SOCKET_PATH_STR_LEN]; /* daemon's communication socket */
	bool enabled;			/* daemon is enabled */
	struct wpa_ctrl *ctrl;		/* control channel to daemon */
	struct wpa_ctrl *event_ctrl;	/* dedicated event control channel */
	int event_fd;			/* fd of daemon event socket */
	int (*connected_callback)(enum daemon_type, bool); /* conn up/down cb */
};

#define WIFI_WPA_STATES(def)				\
	def(DISCONNECTED,	WPA_DISCONNECTED)	\
	def(INACTIVE,		WPA_INACTIVE)		\
	def(SCANNING,		WPA_SCANNING)		\
	def(ASSOCIATING,	WPA_ASSOCIATING)	\
	def(ASSOCIATED,		WPA_ASSOCIATED)		\
	def(4WAY_HANDSHAKE,	WPA_4WAY_HANDSHAKE)	\
	def(GROUP_HANDSHAKE,	WPA_GROUP_HANDSHAKE)	\
	def(COMPLETED,		WPA_COMPLETED)

DEF_ENUM(wifi_wpa_state, WIFI_WPA_STATES);
static DEF_NAMEVAL_TABLE(wifi_platform_wpa_states, WIFI_WPA_STATES);

/*
 * Wi-Fi platform state
 */
struct wifi_platform_state {
	char *driver;		/* Wi-Fi module driver name (malloc'd) */
	char *ap_driver;	/* AP-specific driver name (malloc'd) */
	char *script_directory;	/* directory to look for control scripts */
	u8 use_hostapd:1;	/* enable AP mode managed by hostapd */

	u8 use_script:1;	/* external wifi_control script available */
	u8 hostapd_available:1;	/* use hostapd to manage AP mode */

	enum wifi_wpa_state  state;
	bool station_enabled;		/* Station mode enabled */
	bool ap_enabled;		/* AP mode enabled */
	struct ctrl_channel channels[DAEMON_COUNT]; /* daemon communication */
	struct async_op scan;		/* scan state */
	struct async_op associate;	/* join state */
	struct async_op wps_pbc;	/* WPS state */
	struct timer event_timer;	/* daemon status poll timer */
	const struct wifi_state *wifi;	/* pointer to Wi-Fi state */
};

/*
 * Handler for asynchronous event message from supplicant
 */
struct wifi_platform_event {
	const char *name;		/* name token */
	void (*handler)(char *, size_t); /* handler callback */
};

static struct wifi_platform_state state;

const char * const wifi_platform_daemon_names[] = DAEMON_TYPES;

/* Set if AP mode was temporarily disabled to scan */
static bool scan_ap_override;

/*
 * Forward declarations
 */
static void wifi_platform_event_scan_results(char *, size_t);
static int wifi_platform_scan_results_recv(char *buf);
static void wifi_platform_event_scan_failed(char *, size_t);
static void wifi_platform_event_connected(char *, size_t);
static void wifi_platform_event_assoc_failed(char *msg, size_t len);
static void wifi_platform_event_wps_success(char *, size_t);
static void wifi_platform_event_wps_timeout(char *, size_t);
static void wifi_platform_event_state_change(char *, size_t);
static void wifi_platform_event_terminating(char *, size_t);
static int wifi_platform_ctrl_channel_connect(struct ctrl_channel *);
static int wifi_platform_ctrl_channel_reset(struct ctrl_channel *);
static int wifi_platform_req(enum daemon_type daemon,
    int (*recv)(char *), const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static int wifi_platform_status_get(void);
static int wifi_platform_configure_and_connect(void);

/*
 * Table of supplicant/hostapd event messages and their handlers
 */
static struct wifi_platform_event wifi_platform_event_table[] = {
	{ "CTRL-EVENT-SCAN-RESULTS",	wifi_platform_event_scan_results },
	{ "CTRL-EVENT-SCAN-FAILED",	wifi_platform_event_scan_failed },
	{ "CTRL-EVENT-CONNECTED",	wifi_platform_event_connected },
	{ "CTRL-EVENT-ASSOC-REJECT",	wifi_platform_event_assoc_failed },
	{ "CTRL-EVENT-AUTH-REJECT",	wifi_platform_event_assoc_failed },
	{ "CTRL-EVENT-SSID-TEMP-DISABLED", wifi_platform_event_assoc_failed },
	{ "WPS-SUCCESS",		wifi_platform_event_wps_success },
	{ "WPS-TIMEOUT",		wifi_platform_event_wps_timeout },
	{ "CTRL-EVENT-STATE-CHANGE",	wifi_platform_event_state_change },
	{ "CTRL-EVENT-TERMINATING",	wifi_platform_event_terminating },
	{ NULL, NULL }
};


/*
 * Set Wi-Fi state from platform config
 */
static int wifi_platform_conf_set(json_t *obj)
{
	json_t *item;
	const char *val;

	item = json_object_get(obj, "use_hostapd");
	state.use_hostapd = json_is_true(item);

	val = json_get_string(obj, "driver");		/* optional */
	if (!val || !*val) {
		val = "";
		log_debug("no driver specified");
	}
	free(state.driver);
	state.driver = strdup(val);

	val = json_get_string(obj, "ap_driver");	/* optional */
	if (!val || !*val) {
		val = state.driver;
		log_debug("no ap_driver specified");
	}
	free(state.ap_driver);
	state.ap_driver = strdup(val);

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

	json_object_set_new(obj, "use_hostapd",
	    json_boolean(state.use_hostapd));
	json_object_set_new(obj, "driver", json_string(state.driver));
	json_object_set_new(obj, "ap_driver", json_string(state.ap_driver));
	json_object_set_new(obj, "script_directory",
	    json_string(state.script_directory ? state.script_directory : ""));
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
 * Scan complete handler to workaround hostapd not performing scan operation.
 * If scan was requested while in AP mode and simultaneous AP and station mode
 * is not supported, we had to switch to station mode.  This clears
 * scan_ap_override at the end of the scan to restore AP mode, then invokes
 * the scan finished callback.
 */
static void wifi_platform_scan_ap_override_handler(int result, void *arg)
{
	scan_ap_override = false;
	wifi_platform_configure_and_connect();
	wifi_platform_async_callback(result, arg);
}

/*
 * Handle scan results event
 */
static void wifi_platform_event_scan_results(char *msg, size_t len)
{
	log_debug("%s", msg);
	/* Ignore results if not requested */
	if (!state.scan.active) {
		return;
	}
	wifi_platform_status_get();
}

/*
 * Handle scan failed event
 */
static void wifi_platform_event_scan_failed(char *msg, size_t len)
{
	log_debug("%s", msg);
	async_op_finish(&state.scan, PLATFORM_FAILURE);
}

/*
 * Handle connected to network event
 */
static void wifi_platform_event_connected(char *msg, size_t len)
{
	log_debug("%s", msg);
	wifi_platform_status_get();
}

/*
 * Handle association failed events
 */
static void wifi_platform_event_assoc_failed(char *msg, size_t len)
{
	log_debug("%s", msg);
	async_op_finish(&state.associate, PLATFORM_FAILURE);
}

/*
 * Handle WPS successful event
 */
static void wifi_platform_event_wps_success(char *msg, size_t len)
{
	log_debug("%s", msg);
	async_op_finish(&state.wps_pbc, PLATFORM_SUCCESS);
}

/*
 * Handle WPS timeout event
 */
static void wifi_platform_event_wps_timeout(char *msg, size_t len)
{
	log_debug("%s", msg);
	async_op_finish(&state.wps_pbc, PLATFORM_FAILURE);
}

/*
 * Handle state change event
 */
static void wifi_platform_event_state_change(char *msg, size_t len)
{
	log_debug("%s", msg);
	wifi_platform_status_get();
}

/*
 * Handle terminating event
 */
static void wifi_platform_event_terminating(char *msg, size_t len)
{
	log_debug("%s", msg);
	wifi_platform_exit();
	wifi_shutdown();
}

/*
 * Handle an event message from the wpa_supplicant.  Parses
 * the message type token and invokes the appropriate handler from
 * the wifi_platform_event_table.
 * Message format is: "<n>MESSAGE_ARGS..." where n is the level.
 */
static void wifi_platform_event_msg_handler(char *msg, size_t len)
{
	struct wifi_platform_event *event;
	char *name;
	char *args;

	if (len <= 3 || msg[0] != '<' || msg[2] != '>') {
		log_err("invalid message format: %s", msg);
		return;
	}
	name = msg + 3;
	args = msg + 3;
	strsep(&args, " ");
	for (event = wifi_platform_event_table; event->name; ++event) {
		if (strcmp(event->name, name)) {
			continue;
		}
		log_debug("received %zu bytes: %s %s", len, name,
		    args ? args : "(no args)");
		if (event->handler) {
			event->handler(args, len + msg - args);
		}
		break;
	}
}

/*
 * Callback for received data on event socket
 */
static void wifi_platform_event_recv(void *arg, int fd)
{
	char buf[EVENT_BUF_LEN];
	size_t reply_len;
	struct ctrl_channel *channel;

	/* channel pointer was set at file event registration */
	channel = (struct ctrl_channel *)arg;
	if (!channel || !channel->enabled || channel->event_fd != fd) {
		log_debug("received event for wrong fd: %d", fd);
		return;
	}
	while (channel->event_ctrl &&
	    wpa_ctrl_pending(channel->event_ctrl) > 0) {
		reply_len = sizeof(buf) - 1;
		if (wpa_ctrl_recv(channel->event_ctrl, buf, &reply_len) < 0) {
			log_err("wpa_ctrl_recv: failed");
			break;
		}
		buf[reply_len] = '\0';
		wifi_platform_event_msg_handler(buf, reply_len);
	}
}

/*
 * Initialize Wi-Fi daemon control channel struct.
 */
static void wifi_platform_ctrl_channel_init(struct ctrl_channel *channel,
    const char *path, const char *ifname,
    int (*callback)(enum daemon_type, bool))
{
	snprintf(channel->sock_path, sizeof(channel->sock_path), "%s/%s",
	    path, ifname);
	channel->enabled = false;
	channel->ctrl = NULL;
	channel->event_ctrl = NULL;
	channel->event_fd = -1;
	channel->connected_callback = callback;
}

/*
 * Disconnect from the wpa_supplicant/hostapd
 */
static int wifi_platform_ctrl_channel_reset(struct ctrl_channel *channel)
{
	struct cond_state *cond = &cond_state;

	if (channel->event_fd > 0) {
		file_event_unreg(&cond->file_events, channel->event_fd,
		    wifi_platform_event_recv, NULL, channel);
		channel->event_fd = -1;
	}
	if (channel->event_ctrl) {
		wpa_ctrl_detach(channel->event_ctrl);
		wpa_ctrl_close(channel->event_ctrl);
		channel->event_ctrl = NULL;
	}
	if (channel->ctrl) {
		wpa_ctrl_close(channel->ctrl);
		channel->ctrl = NULL;
		log_debug("reset complete");
	}
	return 0;
}

/*
 * Connect to daemon socket interface.
 */
static int wifi_platform_ctrl_channel_connect(struct ctrl_channel *channel)
{
	struct cond_state *cond = &cond_state;

	wifi_platform_ctrl_channel_reset(channel);

	channel->ctrl = wpa_ctrl_open(channel->sock_path);
	channel->event_ctrl = wpa_ctrl_open(channel->sock_path);
	if (!channel->ctrl || !channel->event_ctrl) {
		log_err("connect to %s failed", channel->sock_path);
		goto reset;
	}
	if (wpa_ctrl_attach(channel->event_ctrl) < 0) {
		log_err("attach to daemon event channel failed");
		goto reset;
	}
	channel->event_fd = wpa_ctrl_get_fd(channel->event_ctrl);
	/* pass channel as file event arg */
	if (file_event_reg(&cond->file_events, channel->event_fd,
	    wifi_platform_event_recv, NULL, channel) < 0) {
		log_err("registration for events failed");
		goto reset;
	}
	log_debug("connected to %s", channel->sock_path);
	return 0;
reset:
	wifi_platform_ctrl_channel_reset(channel);
	return -1;
}

/*
 * Invoke a script called "wifi_control.sh" to setup the DHCP client/server and
 * the Wi-Fi module for the current mode.  This allows system builders to
 * support their desired DHCP server and client.  It also provides a hook to
 * configure proprietary Wi-Fi module drivers.  Unused features of this script
 * will be more useful for wifi_platform implementations that do not use
 * wpa_supplicant and hostapd.
 */
static int wifi_platform_run_control_script(enum wifi_script_module module,
	enum wifi_script_action action)
{
	const char *security;
	char key_hex[WIFI_MAX_KEY_LEN * 2 + 1];
	const char *debug_opt = log_debug_enabled() ? "" : NO_OUTPUT_CMD;

	/* Control script invocation is optional */
	if (!state.use_script) {
		return 0;
	}

	switch (module) {
	case STATION:
		switch (action) {
		case SCRIPT_START:
		case SCRIPT_STOP:
		case SCRIPT_SCAN:
		case SCRIPT_DISCONNECT:
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
			 *         <channel> <security> <key>
			 */
			if (!state.wifi->curr_profile ||
			    !state.wifi->curr_profile->scan) {
				log_err("no valid profile to connect to");
				return -1;
			}
			security = lookup_by_val(wifi_sec_names,
			    state.wifi->curr_profile->sec);
			hex_string(key_hex, sizeof(key_hex),
			    state.wifi->curr_profile->key.val,
			    state.wifi->curr_profile->key.len, false, 0);
			return wifi_script_run(state.script_directory,
			    WIFI_CONTROL_SCRIPT " %s %s %s %s %d %s %s%s",
			    wifi_script_module_names[module],
			    wifi_script_action_names[action],
			    state.wifi->ifname,
			    wifi_ssid_to_str(&state.wifi->curr_profile->ssid),
			    state.wifi->curr_profile->scan->chan,
			    security ? security : "none",
			    key_hex,
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
 * Generates a basic config file at the path specified.
 * Wi-Fi security is not currently configured.
 * Note that hostapd does not consistently support non-ASCII SSIDs.
 */
static int wifi_platform_daemon_write_config(enum daemon_type daemon)
{
	const char *path;
	FILE *file;
	int rc;

	if (daemon != HOSTAPD) {
		/* no wpa_supplicant config file needed */
		return 0;
	}
#ifdef HOSTAPD_CONFIG_USE_DEFAULT
	return 0;
#endif
	path = HOSTAPD_LIVE_CONFIG;
	file = fopen(path, "w");
	if (!file) {
		log_err("failed opening %s: %m", path);
		return -1;
	}
	rc = fprintf(file,
	    "ctrl_interface=%s\n"
	    "interface=%s\n"
	    "driver=%s\n"
	    "ssid=%s\n"
	    "channel=%d\n"
	    "hw_mode=g\n"
	    "auth_algs=1\n",
	    HOSTAPD_SOCK_DIR,
	    state.wifi->ap_ifname,
	    state.ap_driver,
	    wifi_ssid_to_str(&state.wifi->ap_profile.ssid),
	    state.wifi->ap_channel);
	fclose(file);
	if (rc < 0) {
		log_err("write failed %s: %m", path);
		return -1;
	}
	log_debug("config generated: %s", path);
	return 0;
}

/*
 * Query the system to see if the required daemon is in the PATH
 * and executable. Use the -v (version) option which doesn't actually
 * start the daemon.
 */
static bool wifi_platform_daemon_check_available(enum daemon_type daemon)
{
	int rc;

	switch (daemon) {
	case WPA_SUPPLICANT:
		rc = system("wpa_supplicant -v" NO_OUTPUT_CMD);
		break;
	case HOSTAPD:
		rc = system("hostapd -v" NO_OUTPUT_CMD);
		break;
	default:
		rc = -1;
		log_err("%s not supported", wifi_platform_daemon_names[daemon]);
		break;
	}
	/*
	 * Daemon version commands tend to return non-zero exit status,
	 * so work around this by simply checking that the shell command
	 * not found status (127) is not returned.
	 */
	return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) != 127;
}

/*
 * Attempt to connect the wpa_ctrl socket interface as a test to
 * see if a daemon is started.
 */
static bool wifi_platform_daemon_check_running(enum daemon_type daemon)
{
	struct wpa_ctrl *ctrl = NULL;

	ctrl = wpa_ctrl_open(state.channels[daemon].sock_path);
	if (ctrl) {
		wpa_ctrl_close(ctrl);
		return true;
	}
	return false;
}

/*
 * Start a Wi-fi daemon using a shell command. PID files are enabled
 * so the daemon can be easily terminated.
 */
static int wifi_platform_daemon_start(enum daemon_type daemon)
{
	char cmd[200];
	int rc;

	log_info("starting %s", wifi_platform_daemon_names[daemon]);

	switch (daemon) {
	case WPA_SUPPLICANT:
		snprintf(cmd, sizeof(cmd),
		    "wpa_supplicant -P%s -B -i%s -D%s -C%s%s",
		    SUPPLICANT_PID_FILE, state.wifi->ifname, state.driver,
		    SUPPLICANT_SOCK_DIR,
		    log_debug_enabled() ? " -d" : NO_OUTPUT_CMD);
		break;
	case HOSTAPD:
		snprintf(cmd, sizeof(cmd), "hostapd -P%s -B%s %s%s",
		    HOSTAPD_PID_FILE, log_debug_enabled() ? " -d" : "",
		    HOSTAPD_LIVE_CONFIG,
		    log_debug_enabled() ? "" : NO_OUTPUT_CMD);
		break;
	default:
		log_err("%s not supported", wifi_platform_daemon_names[daemon]);
		return -1;
	}
	log_debug("%s", cmd);
	rc = system(cmd);
	if (rc) {
		log_err("cmd failed: %s", cmd);
		return -1;
	}
	return 0;
}

/*
 * Terminate a daemon using its configured PID file.  This only works
 * if it was started by the wifi_platform_daemon_start() function, which
 * specified the PID file path.
 */
static int wifi_platform_daemon_stop(enum daemon_type daemon)
{
	FILE *file = NULL;
	const char *path = NULL;
	char buf[16];
	size_t size;
	pid_t pid;
	unsigned wait_time;
	int rc = -1;

	log_info("stopping %s", wifi_platform_daemon_names[daemon]);

	switch (daemon) {
	case WPA_SUPPLICANT:
		path = SUPPLICANT_PID_FILE;
		break;
	case HOSTAPD:
		path = HOSTAPD_PID_FILE;
		break;
	default:
		log_err("%s not supported", wifi_platform_daemon_names[daemon]);
		goto error;
	}
	file = fopen(path, "r");
	if (!file) {
		log_err("failed opening pidfile %s: %m", path);
		goto error;
	}
	size = fread(buf, 1, sizeof(buf) - 1, file);
	buf[size] = '\0';
	pid = strtol(buf, NULL, 10);
	if (pid <= 0) {
		log_err("invalid PID: %d", pid);
		goto error;
	}
	if (kill(pid, SIGINT) < 0) {
		log_err("failed to send SIGINT to PID %d: %m", pid);
		goto error;
	}
	log_debug("sent SIGINT to PID %d", pid);
	/* wait for daemon to cleanup and terminate (usually 100-2000 ms) */
	wait_time = 0;
	while (!kill(pid, 0)) {
		if (wait_time >= DAEMON_TIMEOUT) {
			log_warn("timeout waiting for PID %d exit", pid);
			break;
		}
		wait_time += DAEMON_POLL_DELAY;
		usleep(DAEMON_POLL_DELAY * 1000);
	}
	log_debug("PID %d terminated in ~%u ms", pid, wait_time);
	rc = 0;
error:
	if (file) {
		fclose(file);
	}
	return rc;
}

/*
 * Start or stop the appropriate Wi-Fi interface management daemon for
 * the current state.  This function should be called when switching
 * daemons or on daemon error.  Connect/disconnect callbacks are invoked
 * as needed.
 */
static int wifi_platform_daemon_setup(void)
{
	int rc = 0;
	int i;

	/* kill disabled daemons */
	for (i = 0; i < DAEMON_COUNT; ++i) {
		if (state.channels[i].enabled) {
			continue;
		}
		if (!wifi_platform_daemon_check_running(i)) {
			continue;
		}
		if (state.channels[i].connected_callback) {
			log_debug("running %s disconnect callback...",
			    wifi_platform_daemon_names[i]);
			state.channels[i].connected_callback(
			    (enum daemon_type)i, false);
		}
		wifi_platform_ctrl_channel_reset(&state.channels[i]);
		wifi_platform_daemon_stop(i);
	}
	/* start and connect to enabled daemons */
	for (i = 0; i < DAEMON_COUNT; ++i) {
		if (!state.channels[i].enabled) {
			continue;
		}
		if (!wifi_platform_daemon_check_running(i)) {
			wifi_platform_daemon_write_config(i);
			if (wifi_platform_daemon_start(i) < 0) {
				rc = -1;
				continue;
			}
		}
		if (!state.channels[i].ctrl) {
			if (wifi_platform_ctrl_channel_connect(
			    &state.channels[i]) < 0) {
				rc = -1;
				continue;
			}
			if (state.channels[i].connected_callback) {
				log_debug("running %s connect callback...",
				    wifi_platform_daemon_names[i]);
				if (state.channels[i].connected_callback(
				    (enum daemon_type)i, true) < 0) {
					rc = -1;
					continue;
				}
			}
		}
	}
	return rc;
}

/*
 * Start appropriate daemon and connect to it.
 */
static int wifi_platform_configure_and_connect(void)
{
	if (state.station_enabled && state.ap_enabled &&
	    !state.wifi->simultaneous_ap_sta) {
		log_err("configuration failed: AP-STA not supported");
		return -1;
	}
	/* Update administrative enabled flags for each control channel */
	state.channels[WPA_SUPPLICANT].enabled =
	    state.station_enabled || scan_ap_override;
	state.channels[HOSTAPD].enabled =
	    state.hostapd_available && state.ap_enabled && !scan_ap_override;

	/* Apply configuration Wi-fi management daemons */
	return wifi_platform_daemon_setup();
}

/*
 * Send a command to the selected Wi-Fi management daemon.
 */
static int wifi_platform_req(enum daemon_type daemon,
    int (*recv_cb)(char *), const char *fmt, ...)
{
	size_t len;
	size_t reply_len;
	va_list args;
	char *cmd = NULL;
	char buf[RESP_BUF_LEN];
	struct ctrl_channel *channel;
	int rc;
	size_t tries = 0;

	channel = &state.channels[daemon];
	if (!channel->enabled) {
		log_err("%s channel not enabled",
		    wifi_platform_daemon_names[daemon]);
		goto error;
	}
	va_start(args, fmt);
	len = vasprintf(&cmd, fmt, args);
	va_end(args);
	if (len < 0) {
		log_err("malloc failed");
		goto error;
	}

retry:
	if (tries > CMD_RETRIES) {
		log_err("no more command retries");
		goto error;
	}
	++tries;

	/* attempt to reconnect if connection lost */
	if (!channel->ctrl) {
		if (wifi_platform_ctrl_channel_connect(channel) < 0) {
			usleep(CMD_RETRY_DELAY * 1000);
			goto retry;
		}
	}

	log_debug("sending: %s", cmd);

	/* send message to supplicant/hostapd */
	reply_len = sizeof(buf) - 1;
	rc = wpa_ctrl_request(channel->ctrl, cmd, len, buf, &reply_len, NULL);
	if (rc < 0) {
		if (rc == -2) {
			log_err("wpa_ctrl_request: timed out");
			goto retry;
		}
		log_err("wpa_ctrl_request: failed");
		wifi_platform_ctrl_channel_reset(channel);
		usleep(CMD_RETRY_DELAY * 1000);
		goto retry;
	}

	buf[reply_len] = '\0';
	if (reply_len) {
		log_debug("msg: %s", buf);
	} else {
		log_warn("empty reply message");
	}
	/* retry if busy response */
	if (!strncmp(buf, "FAIL-BUSY", 9)) {
		log_warn("wpa_ctrl_request: failed with busy response");
		usleep(CMD_RETRY_DELAY * 1000);
		goto retry;
	}

	free(cmd);

	if (!recv_cb) {
		return 0;
	}
	return recv_cb(buf);

error:
	free(cmd);
	return -1;
}

/*
 * Parse response string and return the line.
 * Returns NULL if the command failed, or there is no more data.
 */
static char *wifi_platform_parse_resp_line(char **buf)
{
	char *line;

	line = strsep(buf, "\n");
	if (!line || !line[0]) {
		return NULL;
	}
	if (!strcmp(line, "FAIL")) {
		log_debug("failure response");
		return NULL;
	}
	return line;
}

/*
 * returns 0 if response is "OK" and -1 otherwise
 */
static int wifi_platform_ok_resp(char *buf)
{
	char *line;

	line = wifi_platform_parse_resp_line(&buf);
	if (line && !strcmp(line, "OK")) {
		return  0;
	}
	return -1;
}

/*
 * Parse security token
 */
static int wifi_platform_parse_sec(char *modes, struct wifi_scan_result *scan)
{
	enum wifi_sec *parse = scan->sec;
	enum wifi_sec sec;
	char *token;
	char *tag;
	int count = 0;

	if (*modes != '[') {
		*parse = WSEC_NONE | WSEC_VALID;
		return 1;
	}
	for (;;) {
		sec = 0;
		if (*modes++ != '[') {
			break;
		}
		token = strsep(&modes, "]");
		if (!token || !token[0]) {
			break;
		}
		if (!strcmp(token, "ESS")) {
			scan->ess_supported = 1;
			continue;
		}
		if (!strcmp(token, "WPS")) {
			scan->wps_supported = 1;
			continue;
		}
		tag = strsep(&token, "-+");
		if (!tag || !tag[0]) {
			break;
		}
		if (!strcmp(tag, "WPA")) {
			sec = WSEC_WPA;
		} else if (!strcmp(tag, "WPA2")) {
			sec = WSEC_WPA2;
		} else if (!strcmp(tag, "WEP")) {
			sec = WSEC_WEP;
		} else if (!strcmp(tag, "NONE")) {
			sec = WSEC_NONE;
		} else {
			log_warn("unknown proto %s", tag);
			continue;
		}
		for (;;) {
			tag = strsep(&token, "-+");
			if (!tag || !tag[0]) {
				break;
			}
			if (!strcmp(tag, "PSK")) {
				sec |= WSEC_PSK;
			} else if (!strcmp(tag, "TKIP")) {
				sec |= WSEC_TKIP;
			} else if (!strcmp(tag, "CCMP")) {
				sec |= WSEC_CCMP;
			} else {
				log_warn("unknown tag %s", tag);
				continue;
			}
		}
		if (count >= WIFI_SCAN_SEC_CT) {
			log_err("more than %d security modes. excess ignored",
			    WIFI_SCAN_SEC_CT);
			continue;	/* handle the first N security types */
		}
		count++;
		*parse++ = sec | WSEC_VALID;
	}
	return count;
}

/*
 * Parse supplicant scan results
 */
static int wifi_platform_scan_results_recv(char *buf)
{
	struct wifi_scan_result scan;
	char *line;
	char *bssid;
	char *freq;
	char *signal;
	char *modes;
	char *ssid;
	struct ether_addr *mac;
	long val;
	char *errptr;

	line = wifi_platform_parse_resp_line(&buf);
	if (!line) {
		log_debug("no results");
		return -1;
	}

	/* clear old scan results */
	wifi_scan_clear();

	for (;;) {
		line = wifi_platform_parse_resp_line(&buf);
		if (!line) {
			break;
		}
		bssid = strsep(&line, "\t");
		freq = strsep(&line, "\t");
		signal = strsep(&line, "\t");
		modes = strsep(&line, "\t");
		ssid = strsep(&line, "\t");

		if (!bssid || !freq || !signal || !modes || !ssid) {
			log_err("missing field: \"%s\"", buf);
			continue;
		}
		memset(&scan, 0, sizeof(scan));
		mac = ether_aton(bssid);
		if (!mac) {
			log_err("bad BSSID %s", bssid);
			continue;
		}
		scan.bssid = *mac;

		val = strtol(freq, &errptr, 10);
		if (*errptr != '\0') {
			log_err("bad freq %s", freq);
			continue;
		}
		val = wifi_freq_chan(val);
		if (val < 0) {
			log_err("no chan at %s", freq);
			val = 0;
			continue;
		}
		scan.chan = val;

		val = strtol(signal, &errptr, 10);
		if (*errptr != '\0') {
			log_err("bad signal %s", signal);
			continue;
		}
		if (val >= 0) {
			/* perform rough % to dBm conversion */
			val = (val / 2) - 100;
		}
		if (val > 0 || val < WIFI_SIGNAL_MIN) {
			val = val > 0 ? 0 : WIFI_SIGNAL_MIN;
			log_warn("signal %s limited to %ld", signal, val);
		}
		scan.signal = val;

		if (ssid[0] == '\0') {
			log_debug("ignoring hidden network: BSSID %s", bssid);
			continue;
		}
		if (wifi_parse_ssid(ssid, &scan.ssid) < 0) {
			log_err("SSID invalid");
			continue;
		}
		scan.type = BT_INFRASTRUCTURE;	/* Assume result is AP */
		wifi_platform_parse_sec(modes, &scan);

		wifi_scan_add(&scan);
	}
	return 0;
}

/*
 * Parse supplicant status table
 */
static int wifi_platform_status_parse(char *buf)
{
	char *line;
	char *token;
	struct ether_addr *bssid = NULL;
	struct wifi_ssid ssid;
	enum wifi_wpa_state wpa_state = -1;
	int rc;

	for (;;) {
		line = wifi_platform_parse_resp_line(&buf);
		if (!line) {
			break;
		}
		token = strsep(&line, "=");
		if (!token) {
			log_err("unparsed line: %s", line);
			continue;
		}
		if (!strcmp(token, "wpa_state")) {
			rc = lookup_by_name(wifi_platform_wpa_states, line);
			if (rc < 0) {
				log_err("unknown wpa state %s", token);
			}
			wpa_state = rc;
			state.state = wpa_state;
		} else if (!strcmp(token, "ssid")) {
			if (line) {
				wifi_parse_ssid(line, &ssid);
			}
		} else if (!strcmp(token, "bssid")) {
			if (line) {
				bssid = ether_aton(token);
			}
		}
	}
	if (state.state == WPA_COMPLETED && ssid.len &&
	    state.wifi->curr_profile) {
		if (!wifi_ssid_match(&ssid, &state.wifi->curr_profile->ssid)) {
			/*
			 * XXX Sometimes suppliment shows association
			 * complete status when the network was
			 * reconfigured from a good network to a
			 * bad network, because it appears to cache
			 * the old network config and reconnect.
			 */
			state.state = WPA_ASSOCIATING;
		}
	}
	if (state.state == WPA_COMPLETED && bssid && state.wifi->curr_hist) {
		/* Update history entry with BSSID */
		wifi_hist_update(state.wifi->curr_hist->error, NULL, bssid);
	}
	return wpa_state < 0 ? -1 :  0;
}

/*
 * Handle a status update while scanning.
 * Return 0 if successful, 1 to continue polling for status, or -1
 * if an error occurred.
 */
static int wifi_platform_scan_status(void)
{
	int rc;

	if (state.state == WPA_SCANNING) {
		return 1;
	}
	rc = wifi_platform_req(WPA_SUPPLICANT, wifi_platform_scan_results_recv,
	    "SCAN_RESULTS");
	async_op_finish(&state.scan, !rc ? PLATFORM_SUCCESS : PLATFORM_FAILURE);
	return rc;
}

/*
 * Handle supplicant status when joining a network.
 * Return 0 if successful, 1 to continue polling for status, or -1
 * if an error occurred.
 */
static int wifi_platform_join_status(void)
{
	switch (state.state) {
	case WPA_DISCONNECTED:
	case WPA_INACTIVE:
	case WPA_SCANNING:
	case WPA_ASSOCIATING:
	case WPA_ASSOCIATED:
	case WPA_4WAY_HANDSHAKE:
	case WPA_GROUP_HANDSHAKE:
		return 1;
		break;
	case WPA_COMPLETED:
		async_op_finish(&state.associate, PLATFORM_SUCCESS);
		wifi_platform_run_control_script(DHCP_CLIENT, SCRIPT_START);
		break;
	}
	return 0;
}

/*
 * Parse supplicant status and perform any actions
 * required for the new state
 */
static int wifi_platform_status_recv(char *buf)
{
	bool status_poll = false;

	if (wifi_platform_status_parse(buf) < 0) {
		return -1;
	}
	if (state.scan.active) {
		if (wifi_platform_scan_status() > 0) {
			status_poll = true;
		}
	}
	if (state.associate.active) {
		if (wifi_platform_join_status() > 0) {
			status_poll = true;
		}
	}
	if (status_poll) {
		wifi_timer_set(&state.event_timer, EVENT_TIMEOUT);
	}
	return 0;
}

/*
 * Request supplicant status
 */
static int wifi_platform_status_get(void)
{
	return wifi_platform_req(WPA_SUPPLICANT, wifi_platform_status_recv,
	    "STATUS-VERBOSE");
}

/*
 * Configure daemons and application state on daemon connection
 * disconnection.
 */
static int wifi_platform_connected_callback(enum daemon_type daemon,
	bool connected)
{
	switch (daemon) {
	case WPA_SUPPLICANT:
		if (connected) {
			/* Remove all networks to reset state */
			if (wifi_platform_req(daemon, NULL,
				"REMOVE_NETWORK all") < 0) {
				return -1;
			}
		} else {
			/* Event timer is only used for wpa_supplicant */
			wifi_timer_clear(&state.event_timer);
			/* Leave the current network, if connected */
			wifi_platform_leave_network();
		}
		/* Cancel any pending operations relating to wpa_supplicant */
		async_op_finish(&state.scan, PLATFORM_CANCELED);
		async_op_finish(&state.associate, PLATFORM_CANCELED);
		async_op_finish(&state.wps_pbc, PLATFORM_CANCELED);
		break;
	case HOSTAPD:
		/*
		 * No connection actions for hostapd.  Config is done
		 * via the live config file.
		 */
		break;
	default:
		break;
	}
	return 0;
}

/*
 * Poll status
 */
static void wifi_platform_event_timeout(struct timer *timer)
{
	log_debug("polling status");
	if (wifi_platform_status_get() < 0) {
		/* retry later if command failed */
		wifi_timer_set(&state.event_timer, EVENT_TIMEOUT);
		log_debug("retrying status in %d ms", EVENT_TIMEOUT);
	}
}

/*
 * Handle response from "SCAN" command.
 */
static int wifi_platform_scan_recv(char *buf)
{
	if (wifi_platform_ok_resp(buf) < 0) {
		log_err("bad response");
		return -1;
	}
	wifi_timer_set(&state.event_timer, EVENT_TIMEOUT);
	return 0;
}

/*
 * Handle response from hostapd station query.
 */
static int wifi_platform_parse_bssid_resp(char *buf)
{
	char *line;

	line = strsep(&buf, "\n");
	if (!line || !line[0] || !strcmp(line, "FAIL") || !ether_aton(line)) {
		return -1;
	}
	return 0;
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

	wifi_timer_init(&state.event_timer, wifi_platform_event_timeout);

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
	int rc;

	/* Check "wifi" config for backwards compatibility */
	wifi_platform_conf_migrate();

	/* Init daemon control interface channels */
	wifi_platform_ctrl_channel_init(&state.channels[WPA_SUPPLICANT],
	    SUPPLICANT_SOCK_DIR, state.wifi->ifname,
	    wifi_platform_connected_callback);
	wifi_platform_ctrl_channel_init(&state.channels[HOSTAPD],
	    HOSTAPD_SOCK_DIR, state.wifi->ap_ifname,
	    wifi_platform_connected_callback);

	/* Check availability of Wi-Fi management daemons */
	if (!wifi_platform_daemon_check_available(WPA_SUPPLICANT)) {
		log_err("fatal: wpa_supplicant not installed");
		return -1;
	}
	log_debug("using wpa_supplicant");
	if (state.use_hostapd) {
		if (!wifi_platform_daemon_check_available(HOSTAPD)) {
			log_warn("hostapd not installed: AP mode disabled");
		} else {
			state.hostapd_available = true;
			log_debug("using hostapd");
		}
	}
	/* Check availability of external control script */
	rc = wifi_script_run(state.script_directory,
	    WIFI_CONTROL_SCRIPT NO_OUTPUT_CMD);
	if (rc == -1) {
		log_warn(WIFI_CONTROL_SCRIPT " not available");
	} else {
		log_debug("using " WIFI_CONTROL_SCRIPT);
		state.use_script = true;
	}
	if (!state.use_script && !state.hostapd_available) {
		log_warn("AP mode is disabled");
	} else if (state.wifi->simultaneous_ap_sta) {
		log_debug("simultaneous AP and Station mode enabled");
	}
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
	/* Invoke script for Wi-Fi module setup */
	if (wifi_platform_run_control_script(STATION, SCRIPT_START)) {
		return -1;
	}
	/* Start and configure wpa_supplicant */
	state.station_enabled = true;
	if (wifi_platform_configure_and_connect() < 0) {
		state.station_enabled = false;
		return -1;
	}
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
	/* Stop wpa_supplicant */
	state.station_enabled = false;
	rc = wifi_platform_configure_and_connect();
	/* Invoke script for Wi-Fi module setup */
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
	/* Invoke script for Wi-Fi module setup */
	if (wifi_platform_run_control_script(AP, SCRIPT_START) < 0) {
		return -1;
	}
	/* Start and configure hostapd */
	state.ap_enabled = true;
	if (wifi_platform_configure_and_connect() < 0) {
		state.ap_enabled = false;
		return -1;
	}
	if (wifi_platform_run_control_script(DHCP_SERVER, SCRIPT_START) < 0) {
		return -1;
	}
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
	/* Stop hostapd */
	state.ap_enabled = false;
	rc = wifi_platform_configure_and_connect();
	/* Invoke script for Wi-Fi module setup */
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
	if (wifi_platform_req(HOSTAPD, wifi_platform_parse_bssid_resp,
	    "STA-FIRST") < 0) {
		return 0;
	}
	return 1;	/* Don't bother to count, just return 1 */
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
	/*
	 * For some reason, scanning is not supported by hostapd.
	 * Set scanning state and reconfigure daemons to swap to
	 * wpa_supplicant for the scan.
	 */
	if (!state.wifi->simultaneous_ap_sta && state.ap_enabled &&
	    wifi_platform_ap_stations_connected() > 0) {
		log_warn("deferring scan while stations connected to AP");
		return 0;
	}
	if (!state.station_enabled) {
		log_debug("enabling station mode for scan");
		scan_ap_override = true;
		wifi_platform_configure_and_connect();
	}
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_scan_recv,
	    "SCAN") < 0) {
		if (scan_ap_override) {
			scan_ap_override = false;
			wifi_platform_configure_and_connect();
		}
		return -1;
	}
	if (scan_ap_override) {
		async_op_start(&state.scan,
		    wifi_platform_scan_ap_override_handler, callback,
		    WIFI_PLATFORM_SCAN_TIMEOUT_MS);
	} else {
		async_op_start(&state.scan, wifi_platform_async_callback,
		    callback, WIFI_PLATFORM_SCAN_TIMEOUT_MS);
	}
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
	/*
	 * There is no way to cancel a requested scan with
	 * the wpa_supplicant.
	 */
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
	char ssid_hex[WIFI_SSID_LEN * 2 + 1];
	char key_hex[WIFI_MAX_KEY_LEN * 2 + 1];
	char *cp;

	if (state.associate.active) {
		log_warn("busy: already associating");
		return -1;
	}

	/* Remove all networks to reset state */
	if (wifi_platform_req(WPA_SUPPLICANT, NULL,
		"REMOVE_NETWORK all") < 0) {
		return -1;
	}
	/* add unconfigured network ID 0 */
	if (wifi_platform_req(WPA_SUPPLICANT, NULL,
		"ADD_NETWORK") < 0) {
		return -1;
	}

	hex_string(ssid_hex, sizeof(ssid_hex),
	    prof->ssid.val, prof->ssid.len, false, 0);

	/* set SSID */
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "SET_NETWORK 0 ssid %s", ssid_hex) < 0) {
		return -1;
	}

	/* set preferred BSSID */
	if (prof->scan) {
		if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
		    "SET_NETWORK 0 bssid %s",
		    net_ether_to_str(&prof->scan->bssid)) < 0) {
			return -1;
		}
	}

	/* if no security, so skip security setup */
	if (SEC_MATCH(WSEC_NONE, prof->sec)) {
		if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
			"SET_NETWORK 0 key_mgmt NONE")) {
			return -1;
		}
		goto select_network;
	}

	/* set key management protocol */
	cp = (prof->sec & WSEC_PSK) ? "WPA-PSK" : "NONE";
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
		"SET_NETWORK 0 key_mgmt %s", cp)) {
		return -1;
	}

	/*
	 * The wpa_supplicant should automatically scan/query an AP
	 * and select the best security options connecting. This process
	 * appears to be more robust than manually configuring security.
	 */
#ifdef CONFIGURE_SEC
	/* set security protocol */
	switch (prof->sec & WSEC_SEC_MASK) {
	case WSEC_WEP:
		cp = "WEP";
		break;
	case WSEC_WPA:
		cp = "WPA";
		break;
	case WSEC_WPA2:
		cp = "WPA2";
		break;
	default:
		log_err("unsupported security");
		cp = "NONE";
	}
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "SET_NETWORK 0 proto %s", cp)) {
		return -1;
	}

	/* set pairwise */
	if ((prof->sec & (WSEC_CCMP | WSEC_TKIP)) == (WSEC_CCMP | WSEC_TKIP)) {
		cp = "CCMP TKIP";
	} else if (prof->sec & WSEC_CCMP) {
		cp = "CCMP";
	} else if (prof->sec & WSEC_TKIP) {
		cp = "TKIP";
	} else {
		cp = "NONE";
	}
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "SET_NETWORK 0 pairwise %s", cp)) {
		return -1;
	}

	/* set group cipher */
	if ((prof->sec & (WSEC_CCMP | WSEC_TKIP)) == (WSEC_CCMP | WSEC_TKIP)) {
		cp = "CCMP TKIP";
	} else if (prof->sec & WSEC_CCMP) {
		cp = "CCMP";
	} else if (prof->sec & WSEC_TKIP) {
		cp = "TKIP";
	} else if (prof->sec & WSEC_WEP104) {
		cp = "WEP-104";
	} else if (prof->sec & WSEC_WEP40) {
		cp = "WEP-40";
	} else {
		goto send_key;
	}
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "SET_NETWORK 0 group %s", cp)) {
		return -1;
	}

send_key:
#endif
	if (prof->sec & WSEC_PSK) {
		/* Send pre-shared key */
		if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
		    "SET_NETWORK 0 psk \"%s\"", prof->key.val)) {
			return -1;
		}
	} else if (SEC_MATCH(prof->sec, WSEC_WEP)) {
		hex_string(key_hex, sizeof(key_hex), prof->key.val,
		    prof->key.len, false, 0);
		/* Send WEP key 0 */
		if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
		    "SET_NETWORK 0 wep_key0 %s", key_hex)) {
			return -1;
		}
		if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
		    "SET_NETWORK 0 wep_tx_keyidx 0")) {
			return -1;
		}
	}

select_network:
	/* enable network */
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "SELECT_NETWORK 0")) {
		return -1;
	}
	log_debug("selection complete");
	async_op_start(&state.associate, wifi_platform_async_callback, callback,
	    WIFI_PLATFORM_ASSOCIATE_TIMEOUT_MS);
	/* set timer waiting for join event */
	wifi_timer_set(&state.event_timer, EVENT_TIMEOUT);
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
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "REMOVE_NETWORK all") < 0) {
		return -1;
	}
	/* Disable DHCP client */
	if (state.state == WPA_COMPLETED) {
		wifi_platform_run_control_script(DHCP_CLIENT, SCRIPT_STOP);
	}
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
	if (wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp,
	    "WPS_PBC") < 0) {
		return -1;
	}
	async_op_start(&state.wps_pbc, wifi_platform_async_callback, callback,
	    WIFI_PLATFORM_WPS_TIMEOUT_MS);
	/* waiting for WPS status event */
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
	wifi_platform_req(WPA_SUPPLICANT, wifi_platform_ok_resp, "WPS_CANCEL");
	async_op_finish(&state.wps_pbc, PLATFORM_CANCELED);
}

/*
 * Return true if WPS is active.
 */
bool wifi_platform_wps_started(void)
{
	return state.wps_pbc.active;
}
