/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/uri_code.h>
#include <ayla/socket.h>
#include <ayla/server.h>
#include <ayla/cmd.h>
#include <ayla/uri_code.h>
#include <ayla/nameval.h>
#include <ayla/log.h>
#include <ayla/build.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>

/* Subdirectory of devd's local web server */
#define DEVD_SERVER_SUBDIR		"devd"

/* Macros to generate common socket paths */
#define DEVD_SERVER_SOCK	\
	acli_get_sock_path(DEVD_SERVER_SUBDIR, SOCKET_NAME)
#define DEVD_MSG_SOCK	\
	acli_get_sock_path(MSG_APP_NAME_CLIENT, MSG_SOCKET_DEFAULT)
#define COND_MSG_SOCK	\
	acli_get_sock_path(MSG_APP_NAME_WIFI, MSG_SOCKET_DEFAULT)

DEF_NAMEVAL_TABLE(server_method_table, SERVER_METHODS);

const char *version = "acli " BUILD_VERSION_LABEL;

static const char *cmdname;
static const char *socket_dir = SOCK_DIR_DEFAULT;

/* Forward declarations */
static const struct cmd_info acli_event_cmds[];
static const struct cmd_info acli_control_cmds[];
static const struct cmd_info acli_config_cmds[];
static const struct cmd_info acli_wifi_cmds[];
static const struct cmd_info acli_cmds[];


static const char *acli_get_sock_path(const char *app_name,
	const char *sock_name)
{
	static char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s/%s",
	    socket_dir, app_name, sock_name);
	return path;
}

static int acli_invalid(int argc, char **argv)
{
	fprintf(stderr, "%s: invalid command\n", cmdname);
	return 2;
}

static int acli_invalid_args(int argc, char **argv)
{
	fprintf(stderr, "%s: invalid arguments\n", cmdname);
	return 2;
}

static int acli_subcmd(const struct cmd_info *cmds, int argc, char **argv)
{
	if (argc <= 0) {
		return acli_invalid_args(argc, argv);
	}
	return cmd_handle_argv(cmds, argv[0], argc - 1, argv + 1);
}

static int acli_print_help(const struct cmd_info *cmds, const char *banner)
{
	if (banner) {
		printf("%s:\n", banner);
	}
	for (; cmds->name; ++cmds) {
		if (cmds->help) {
			printf("  %-15s %s\n", cmds->name, cmds->help);
		}
	}
	return 0;
}

/*
 * Add args to a URL using proper HTTP formatting.
 */
static int acli_make_url_with_args(int argc, char **argv, const char *url,
    char *buf, size_t size)
{
	char *cp;
	char *endp = buf + size;
	ssize_t len;

	len = strnlen(url, size);
	strncpy(buf, url, len);
	buf += len;
	if (argc < 1) {
		*buf = '\0';
		return 0;
	}
	*buf++ = '?';

	for (; argc > 0; --argc, ++argv) {
		cp = strchr(*argv, '=');
		if (!cp) {
			return -1;
		}
		len = cp - *argv + 1;
		if (len >= endp - buf) {
			return -1;
		}
		strncpy(buf, *argv, len);
		buf += len;
		len = uri_encode(buf, endp - buf, *argv + len,
		    strlen(*argv + len), uri_arg_allowed_map);
		if (len < 0) {
			return -1;
		}
		buf += len;
		if (argc > 1) {
			*buf++ = '&';
		} else {
			*buf++ = '\0';
		}
	}
	return 0;
}

/*
 * Send an HTTP request to a local HTTP server.
 */
static int acli_local_req(enum server_method method, const char *path,
    const char *url, const char *payload)
{
	int status;
	char *reply;

	log_debug("%s local: %s %s", server_method_str[method], path, url);

	status = serv_local_client_req(method, url, path, payload, &reply);
	fprintf(stderr, "HTTP status: %d\n", status);
	if (reply) {
		printf("%s\n", reply);
		free(reply);
	}
	return status;
}

/*
 * Handle the response to a message sent by acli_msg_send().
 */
static void acli_msg_resp_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, const struct amsg_msg_info *info, void *arg)
{
	size_t i;
	char c;
	bool ascii = true;
	bool terminated = false;

	if (err != AMSG_ERR_NONE || !info->payload_size) {
		return;
	}
	/* Check if response is printable */
	for (i = 0; i < info->payload_size; ++i) {
		c = *((char *)info->payload + i);
		if (c == '\0') {
			terminated = true;
			break;
		}
		if (!isascii(c)) {
			ascii = false;
			break;
		}
	}
	log_debug("response: [%hhu:%hhu] %zu bytes",
	    info->interface, info->type, info->payload_size);
	if (ascii) {
		if (terminated) {
			fprintf(stdout, "%s\n", (const char *)info->payload);
		} else {
			fwrite(info->payload, 1, info->payload_size, stdout);
			fwrite("\n", 1, 2, stdout);
		}
	}
}

/*
 * Send a message to the message server bound to the specified path and block
 * until the response is received.
 */
static int acli_msg_send(const char *path, uint8_t interface, uint8_t type,
	const void *payload, size_t payload_size)
{
	struct amsg_client client;
	enum amsg_err err;

	if (access(path, F_OK) < 0) {
		fprintf(stderr, "error: socket not available %s\n", path);
		return -1;
	}
	if (amsg_client_init(&client, NULL, NULL) < 0) {
		log_err("amsg_client_init() failed");
		return -1;
	}
	if (amsg_client_connect(&client, path) < 0) {
		return -1;
	}
	msg_send_app_info(&client.endpoint, MSG_APP_NAME_CLI);
	log_debug("send: %s [%hhu:%hhu] %zu bytes",
	    path, interface, type, payload_size);
	err = amsg_send_sync(&client.endpoint, interface, type,
	    payload, payload_size, acli_msg_resp_handler, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		fprintf(stderr, "error: %s\n", amsg_err_string(err));
	}
	amsg_client_cleanup(&client);
	return err == AMSG_ERR_NONE ? 0 : -1;
}

static int acli_event_dhcp(enum msg_system_dhcp_event event,
	int argc, char **argv)
{
	struct msg_system_dhcp msg = { .event = event };
	const char *path;
	int rc;

	if (argc >= 1) {
		if (!strcmp("help", argv[0])) {
			fprintf(stderr, "usage: %s dhcp_<event type> "
			    "[interface] [app name]\n", cmdname);
			return 2;
		}
		snprintf(msg.interface, sizeof(msg.interface), "%s", argv[0]);
	}
	if (argc >= 2) {
		/* Send to specified app */
		path = acli_get_sock_path(argv[1], MSG_SOCKET_DEFAULT);
		return acli_msg_send(path,
		    MSG_INTERFACE_SYSTEM, MSG_SYSTEM_DHCP_EVENT,
		    &msg, sizeof(msg));
	}
	/* Otherwise, send to cond and devd */
	rc = acli_msg_send(COND_MSG_SOCK,
	    MSG_INTERFACE_SYSTEM, MSG_SYSTEM_DHCP_EVENT, &msg, sizeof(msg));
	rc &= acli_msg_send(DEVD_MSG_SOCK,
	    MSG_INTERFACE_SYSTEM, MSG_SYSTEM_DHCP_EVENT, &msg, sizeof(msg));
	return rc;
}

/*
 * Send a DHCP up event.
 * Usage: event dhcp_bound [<network interface> [destination daemon]
 */
static int acli_event_dhcp_up(int argc, char **argv)
{
	acli_event_dhcp(MSG_SYSTEM_DHCP_BOUND, argc, argv);
	return 0;
}

/*
 * Send a DHCP down event.
 * Usage: event dhcp_deconfig|dhcp_leasefail \
 *           [<network interface> [destination daemon]
 */
static int acli_event_dhcp_down(int argc, char **argv)
{
	acli_event_dhcp(MSG_SYSTEM_DHCP_UNBOUND,
	    argc, argv);
	return 0;
}

/*
 * Send a DHCP refresh event.
 * Usage: event dhcp_refresh [<network interface> [destination daemon]
 */
static int acli_event_dhcp_refresh(int argc, char **argv)
{
	acli_event_dhcp(MSG_SYSTEM_DHCP_REFRESH,
	    argc, argv);
	return 0;
}

static int acli_event_help(int argc, char **argv)
{
	return acli_print_help(acli_event_cmds, "event commands");
}

static const struct cmd_info acli_event_cmds[] = {
	CMD_INIT("dhcp_bound",	acli_event_dhcp_up,	"DHCP up event"),
	CMD_INIT("dhcp_deconfig", acli_event_dhcp_down,	"DHCP down event"),
	CMD_INIT("dhcp_leasefail", acli_event_dhcp_down, "DHCP down event"),
	CMD_INIT("dhcp_refresh", acli_event_dhcp_refresh, "DHCP renew event"),
	CMD_INIT("help",	acli_event_help, "show event command list"),
	CMD_END_DEFAULT(acli_invalid_args)
};

/*
 * Start a push-button user registration window.
 * Usage: control reg_start
 */
static int acli_control_reg_start(int argc, char **argv)
{
	return acli_msg_send(DEVD_MSG_SOCK,
	    MSG_INTERFACE_CLIENT, MSG_CLIENT_USERREG_WINDOW_START, NULL, 0);
}

/*
 * Update the device's setup token.
 * Usage: control setup_token <new token>
 */
static int acli_control_setup_token_update(int argc, char **argv)
{
	struct msg_client_setup_info msg = { { 0 } };

	if (argc != 1) {
		fprintf(stderr, "usage: %s setup_token <new_token>\n", cmdname);
		return 2;
	}
	snprintf(msg.setup_token, sizeof(msg.setup_token), "%s", argv[0]);
	return acli_msg_send(DEVD_MSG_SOCK,
	    MSG_INTERFACE_CLIENT, MSG_CLIENT_SETUP_INFO, &msg, sizeof(msg));
}

/*
 * Update the device's location.
 * Usage: control location <new location>
 */
static int acli_control_location_update(int argc, char **argv)
{
	struct msg_client_setup_info msg = { { 0 } };

	if (argc != 1) {
		fprintf(stderr, "usage: %s location <new_location>\n", cmdname);
		return 2;
	}
	snprintf(msg.location, sizeof(msg.location), "%s", argv[0]);
	return acli_msg_send(DEVD_MSG_SOCK,
	    MSG_INTERFACE_CLIENT, MSG_CLIENT_SETUP_INFO, &msg, sizeof(msg));
}

static int acli_control_help(int argc, char **argv)
{
	return acli_print_help(acli_control_cmds, "control commands");
}

static const struct cmd_info acli_control_cmds[] = {
	CMD_INIT("reg_start",	acli_control_reg_start,
	    "open push-button registration window"),
	CMD_INIT("setup_token",	acli_control_setup_token_update,
	    "update device setup token"),
	CMD_INIT("location",	acli_control_location_update,
	    "update device location"),
	CMD_INIT("help",	acli_control_help, "show control command list"),
	CMD_END_DEFAULT(acli_invalid_args)
};

/*
 * Config paths look like: <app name>[/<item path>]
 */
static void acli_split_config_path(char *path, char **app_name, char **item)
{
	char *cp;

	*app_name = path;
	cp = strchr(path, '/');
	if (cp) {
		*cp = '\0';
		*item = cp + 1;
	} else {
		*item = "";
	}
}

/*
 * Request a configuration value.
 * Usage: config get <daemon>/<config path>
 */
static int acli_config_get(int argc, char **argv)
{
	const char *path;
	char *app;
	char *item;
	char *msg;
	int rc;

	if (argc != 1) {
		fprintf(stderr, "usage: %s get <app name>/<config path>\n",
		    cmdname);
		return 2;
	}
	acli_split_config_path(argv[0], &app, &item);
	path = acli_get_sock_path(app, MSG_SOCKET_DEFAULT);
	/* Standard config JSON format */
	rc = asprintf(&msg, "{\"name\":\"%s\"}", item);
	if (rc < 0) {
		log_err("malloc failed");
		return 1;
	}
	rc = acli_msg_send(path, MSG_INTERFACE_CONFIG, MSG_CONFIG_VALUE_REQ,
	    msg, rc);
	free(msg);
	return rc;
}

/*
 * Update a configuration value.
 * Usage: config set <daemon>/<config path> <value>
 */
static int acli_config_set(int argc, char **argv)
{
	const char *path;
	const char *value;
	char *app;
	char *item;
	char *msg;
	int rc;

	if (argc != 2) {
		fprintf(stderr, "usage: %s set "
		    "<app name>/<config path> <value>\n", cmdname);
		return 2;
	}
	acli_split_config_path(argv[0], &app, &item);
	path = acli_get_sock_path(app, MSG_SOCKET_DEFAULT);
	value = argv[1];
	/* Standard config JSON format */
	rc = asprintf(&msg, "{\"name\":\"%s\",\"val\":%s}", item, value);
	if (rc < 0) {
		log_err("malloc failed");
		return 1;
	}
	rc = acli_msg_send(path, MSG_INTERFACE_CONFIG, MSG_CONFIG_VALUE_SET,
	    msg, rc);
	free(msg);
	return rc;
}

/*
 * Remove a configuration value.
 * Usage: config delete <daemon>/<config path>
 */
static int acli_config_delete(int argc, char **argv)
{
	const char *path;
	char *app;
	char *item;
	char *msg;
	int rc;

	if (argc != 1) {
		fprintf(stderr, "usage: %s delete <app name>/<config path>\n",
		    cmdname);
		return 2;
	}
	acli_split_config_path(argv[0], &app, &item);
	path = acli_get_sock_path(app, MSG_SOCKET_DEFAULT);
	/* Standard config JSON format */
	rc = asprintf(&msg, "{\"name\":\"%s\"}", item);
	if (rc < 0) {
		log_err("malloc failed");
		return 1;
	}
	rc = acli_msg_send(path, MSG_INTERFACE_CONFIG, MSG_CONFIG_VALUE_DELETE,
	    msg, rc);
	free(msg);
	return rc;
}

/*
 * Request a factory reset.
 * Usage: config reset <daemon>
 */
static int acli_config_reset(int argc, char **argv)
{
	const char *path;

	if (argc != 1) {
		fprintf(stderr, "usage: %s reset <app name>\n", cmdname);
		return 2;
	}
	path = acli_get_sock_path(argv[0], MSG_SOCKET_DEFAULT);

	return acli_msg_send(path,
	    MSG_INTERFACE_CONFIG, MSG_CONFIG_FACTORY_RESET, NULL, 0);
}

static int acli_config_help(int argc, char **argv)
{
	return acli_print_help(acli_config_cmds, "config commands");
}

static const struct cmd_info acli_config_cmds[] = {
	CMD_INIT("get",		acli_config_get,	"print value"),
	CMD_INIT("set",		acli_config_set,	"set new value"),
	CMD_INIT("delete",	acli_config_delete,	"remove object"),
	CMD_INIT("reset",	acli_config_reset, "restore factory defaults"),
	CMD_INIT("help",	acli_config_help, "show config command list"),
	CMD_END_DEFAULT(acli_invalid_args)
};

/*
 * Open a Wi-Fi AP mode window.
 * Usage: wifi ap_start
 */
static int acli_wifi_ap_start(int argc, char **argv)
{
	return acli_msg_send(COND_MSG_SOCK,
	    MSG_INTERFACE_WIFI, MSG_WIFI_AP_WINDOW_OPEN, NULL, 0);
}

/*
 * Perform a Wi-Fi scan and return the results.
 * Usage: wifi scan
 */
static int acli_wifi_scan(int argc, char **argv)
{
	if (acli_msg_send(COND_MSG_SOCK,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SCAN_START, NULL, 0) < 0) {
		return 1;
	}
	return acli_msg_send(COND_MSG_SOCK,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SCAN_RESULTS_REQ, NULL, 0);
}

/*
 * Attempt to connect to a Wi-Fi access point.
 * Usage: wifi connect <SSID> [key]
 */
static int acli_wifi_connect(int argc, char **argv)
{
	const char *ssid;
	const char *key;
	char payload[512];
	size_t len;
	ssize_t rc;

	if (argc < 1 || argc > 2) {
		fprintf(stderr, "usage: %s connect <SSID> [key]\n",
		    cmdname);
		return 2;
	}
	ssid = argv[0];
	key = argc == 2 ? argv[1] : NULL;
	/* Construct JSON payload */
	len = snprintf(payload, sizeof(payload), "{\"ssid\":\"");
	rc = uri_encode(payload + len, sizeof(payload) - len, ssid,
	    strlen(ssid), uri_printable_ascii_map);
	if (rc < 0) {
		log_err("cannot encode SSID");
		return 1;
	}
	len += rc;
	if (key) {
		len += snprintf(payload + len, sizeof(payload) - len,
		    "\",\"key\":\"");
		rc = uri_encode(payload + len, sizeof(payload) - len, key,
		    strlen(key), uri_printable_ascii_map);
		len += rc;
	}
	len += snprintf(payload + len, sizeof(payload) - len, "\"}");
	log_debug("payload: %s", payload);
	return acli_msg_send(COND_MSG_SOCK,
	    MSG_INTERFACE_WIFI, MSG_WIFI_CONNECT, payload, len);
}

/*
 * Request Wi-Fi status.
 * Usage: wifi status
 */
static int acli_wifi_status(int argc, char **argv)
{
	return acli_msg_send(COND_MSG_SOCK, MSG_INTERFACE_WIFI,
	    MSG_WIFI_STATUS_REQ, NULL, 0);
}

/*
 * Get human-readable Wi-Fi info printout.
 * Usage: wifi print
 */
static int acli_wifi_print(int argc, char **argv)
{
	const char *msg = "print";

	return acli_msg_send(COND_MSG_SOCK, MSG_INTERFACE_CLI, MSG_CLI_INPUT,
	    msg, strlen(msg));
}

static int acli_wifi_help(int argc, char **argv)
{
	return acli_print_help(acli_wifi_cmds, "wifi commands");
}

static const struct cmd_info acli_wifi_cmds[] = {
	CMD_INIT("ap_start",	acli_wifi_ap_start,	"open AP window"),
	CMD_INIT("scan",	acli_wifi_scan,		"scan for networks"),
	CMD_INIT("connect",	acli_wifi_connect,	"connect to a network"),
	CMD_INIT("status",	acli_wifi_status,	"get Wi-Fi status"),
	CMD_INIT("print",	acli_wifi_print,	"print Wi-Fi info"),
	CMD_INIT("help",	acli_wifi_help,		"wifi command list"),
	CMD_END_DEFAULT(acli_invalid_args)
};

/*
 * For testing, allow any of the subcommands to be the first argument.
 * This eliminates the need to make all the symlinks.
 */
static int acli_main(int argc, char **argv)
{
	if (argc < 1) {
		return acli_invalid(argc, argv);
	}
	cmdname = argv[0];
	return cmd_handle_argv(acli_cmds, cmdname, argc - 1, argv + 1);
}

/*
 * Send a local HTTP request.
 * Usage: http <method> <URI> [arguments: name1=value1 name2=value2 ...]
 */
static int acli_http(int argc, char **argv)
{
	enum server_method method;
	char url[1024];
	const char *path = DEVD_SERVER_SOCK;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <method> <URI> [args...]\n",
		    cmdname);
		return 2;
	}
	rc = lookup_by_name(server_method_table, argv[0]);
	if (rc < 0) {
		fprintf(stderr, "unsupported method: %s\n", argv[0]);
		return 2;
	}
	method = rc;
	if (argc > 2) {
		if (acli_make_url_with_args(argc - 2, argv + 2, argv[1], url,
		    sizeof(url)) < 0) {
			fprintf(stderr, "invalid URL argument(s)\n");
			return 1;
		}
		return acli_local_req(method, path, url, NULL) < 0 ? 1 : 0;
	}
	return acli_local_req(method, path, argv[1], NULL) < 0 ? 1 : 0;
}

static int acli_event(int argc, char **argv)
{
	return acli_subcmd(acli_event_cmds, argc, argv);
}

static int acli_control(int argc, char **argv)
{
	return acli_subcmd(acli_control_cmds, argc, argv);
}
static int acli_config(int argc, char **argv)
{
	return acli_subcmd(acli_config_cmds, argc, argv);
}

static int acli_wifi(int argc, char **argv)
{
	return acli_subcmd(acli_wifi_cmds, argc, argv);
}

static int acli_help(int argc, char **argv)
{
	printf("usage: %s [options] <commands> [args..]\n"
	       "options:\n"
	       "  -d              Enable debug\n"
	       "  -o <dir>        Socket directory\n",
	    cmdname);
	return acli_print_help(acli_cmds, "commands");
}

static int acli_set_socket(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: -o <sockdir> <command> [args...]\n");
		return 2;
	}
	socket_dir = argv[0];

	/* Execute the command */
	return acli_main(argc - 1, argv + 1);
}

static int acli_set_debug(int argc, char **argv)
{
	log_set_options(LOG_OPT_DEBUG);
	return acli_main(argc, argv);
}

static const struct cmd_info acli_cmds[] = {
	CMD_INIT("acli",	acli_main,	NULL),
	CMD_INIT("http",	acli_http,	"send HTTP request to server"),
	CMD_INIT("event",	acli_event,	"send event notification"),
	CMD_INIT("control",	acli_control,	"send a command"),
	CMD_INIT("config",	acli_config,	"access configuration"),
	CMD_INIT("wifi",	acli_wifi,	"interact with Wi-Fi"),
	CMD_INIT("help",	acli_help,	"show command list"),
	CMD_INIT("-o",		acli_set_socket, NULL),
	CMD_INIT("-d",		acli_set_debug,	NULL),
	CMD_END_DEFAULT(acli_invalid)
};

int main(int argc, char **argv)
{
	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	log_init(cmdname, LOG_OPT_FUNC_NAMES | LOG_OPT_CONSOLE_OUT);
	log_set_subsystem(LOG_SUB_CLI);

	return cmd_handle_argv(acli_cmds, cmdname, argc - 1, argv + 1);
}
