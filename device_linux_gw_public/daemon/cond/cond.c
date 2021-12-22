/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
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
#include <getopt.h>

#include <sys/stat.h>
#include <sys/signal.h>

/*SOCKET*/
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stddef.h>
#include <errno.h>
#include <poll.h>

#include <time.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/crc.h>
#include <ayla/conf_io.h>
#include <ayla/file_event.h>
#include <ayla/file_io.h>
#include <ayla/timer.h>
#include <ayla/socket.h>
#include <ayla/amsg.h>
#include <ayla/msg_utils.h>
#include <ayla/log.h>
#include <ayla/build.h>

#include "cond.h"
#include "wifi.h"

#ifdef SUPPORT_BLE_WIFI_SETUP
#include "ble/gatt_service.h"
#endif

char version[] = "cond " BUILD_VERSION_LABEL;

bool debug;
bool foreground;
bool use_net_events;
char msg_sock_path[SOCKET_PATH_STR_LEN];
char devd_msg_sock_path[SOCKET_PATH_STR_LEN];
static const char *config_file = COND_CONF_FILE;
static const char *config_startup_dir;

struct cond_state cond_state;

static const struct option options[] = {
	{ .name = "debug", .val = 'd' },
	{ .name = "foreground", .val = 'f' },
	{ .name = "wait", .val = 'w'},
	{ .name = "config", .val = 'c', .has_arg = 1 },
	{ .name = "startup_dir", .val = 's', .has_arg = 1 },
	{ .name = "sockdir", .val = 'o', .has_arg = 1 },
	{ .name = NULL }
};

static char *cmdname;

static void usage(void)
{
	fprintf(stderr,
	    "%s\n"
	    "Usage: %s [OPTIONS]\n"
	    "OPTIONS:\n"
	    "  -d, --debug             Enable debug verbose debug messages\n"
	    "  -f, --foreground        Do not daemonize\n"
	    "  -w, --wait              Wait for dhcp_bound events\n"
	    "  -c, --config <file>     Config file path\n"
	    "  -s, --startup_dir <dir> Startup config directory\n"
	    "  -o, --sockdir <dir>     Socket directory\n",
	    version, cmdname);
}

static void ds_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	char *socket_dir = MSG_SOCKET_DIR_DEFAULT;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	optind = 0;
	while ((opt = getopt_long(argc, argv, "dfwc:s:o:",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'd':
			debug = true;
			break;
		case 'f':
			foreground = true;
			break;
		case 'w':
			use_net_events = true;
			break;
		case 'c':
			config_file = optarg;
			break;
		case 's':
			config_startup_dir = optarg;
			break;
		case 'o':
			socket_dir = optarg;
			break;
		default:
			usage();
			exit(EXIT_FAILURE);
			break;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unused arguments\n", cmdname);
		usage();
		exit(EXIT_FAILURE);
	}

	/* Create message socket paths */
	snprintf(msg_sock_path, sizeof(msg_sock_path), "%s/%s/%s",
	    socket_dir, MSG_APP_NAME_WIFI, MSG_SOCKET_DEFAULT);
	snprintf(devd_msg_sock_path, sizeof(devd_msg_sock_path), "%s/%s/%s",
	    socket_dir, MSG_APP_NAME_CLIENT, MSG_SOCKET_DEFAULT);
	/* clear umask to allow socket creation with full privs */
	umask(0);
}

static void cond_init(void)
{
	log_init(cmdname, LOG_OPT_FUNC_NAMES);
	if (foreground) {
		log_set_options(LOG_OPT_CONSOLE_OUT);
	}
	if (debug) {
		log_set_options(LOG_OPT_DEBUG | LOG_OPT_TIMESTAMPS);
	}
	log_set_subsystem(LOG_SUB_WIFI);

	if (conf_init(config_file, config_startup_dir) < 0) {
		exit(EXIT_FAILURE);
	}
}

static void cond_daemonize(void)
{
	log_debug("daemonizing...");
	if (daemon(0, 0) < 0) {
		log_err("daemon failed: %m");
		exit(EXIT_FAILURE);
	}
}

static void cond_signal_handler(int signal)
{
	log_debug("caught signal: %d", signal);
	exit(signal);
}

static void cond_exit_handler(void)
{
	wifi_exit();
	conf_cleanup();
	#ifdef SUPPORT_BLE_WIFI_SETUP
	gatt_cleanup();
	#endif
}

static void cond_poll(void)
{
	struct cond_state *cond = &cond_state;

	for (;;) {
		wifi_poll();
		if (file_event_poll(&cond->file_events,
		    timer_advance(&cond->timers)) < 0) {
			log_err("poll err %m");
			break;
		}
	}
}

int main(int argc, char **argv)
{
	struct cond_state *cond = &cond_state;

	file_event_init(&cond->file_events);
	ds_opts(argc, argv);
	cond_init();
	wifi_conf_init();
	wifi_init();
	wifi_interface_init();
	atexit(cond_exit_handler);
	if (conf_load() < 0) {
		exit(EXIT_FAILURE);
	}
	#ifdef SUPPORT_BLE_WIFI_SETUP
	if (gatt_init(&cond->file_events, &cond->timers) < 0) {
		log_err("gatt_init failed");
		exit(EXIT_FAILURE);
	}
	#endif
	if (wifi_start() < 0) {
		exit(EXIT_FAILURE);
	}
	signal(SIGINT, cond_signal_handler);
	signal(SIGTERM, cond_signal_handler);
	if (!foreground) {
		cond_daemonize();
	}
	cond_poll();
	return 0;
}
