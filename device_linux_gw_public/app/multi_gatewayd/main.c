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
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/msg_utils.h>

#include "gateway.h"

#define APPD_CONF_FILE_DEFAULT		"/config/appd.conf"

static char *cmdname;
static bool debug;
static bool foreground;
static const char *conf_factory_file = APPD_CONF_FILE_DEFAULT;
static const char *conf_startup_dir;
static const char *socket_dir = MSG_SOCKET_DIR_DEFAULT;

static const struct option options[] = {
	{ .name = "debug", .val = 'd'},
	{ .name = "foreground", .val = 'f'},
	{ .name = "sockdir", .has_arg = 1, .val = 'o'},
	{ .name = "factory_config", .has_arg = 1, .val = 'c'},
	{ .name = "startup_dir", .has_arg = 1, .val = 's'},
	{ .name = NULL }
};

/*
 * Shows usage information for running appd
 */
static void usage(void)
{
	fprintf(stderr, "Usage: %s\n", cmdname);
	fprintf(stderr, "  Options:\n");
	fprintf(stderr, "    -c --factory_config <file>		"
	    "Specify factory config file\n");
	fprintf(stderr, "    -s --startup_dir <dir>		"
	    "Specify startup config directory\n");
	fprintf(stderr, "    -d --debug				"
	    "Run in debug mode\n");
	fprintf(stderr, "    -f --foreground			"
	    "Don't detach daemon process, run in foreground\n");
	fprintf(stderr, "    -o --sockdir	<dir>		"
	    "Specify socket directory\n");
	exit(EXIT_FAILURE);
}

/*
 * Parse the command line options passed into appd
 */
static void parse_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}
	optind = 0;
	while ((opt = getopt_long(argc, argv, "?dfo:c:s:",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'f':
			foreground = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'o':
			socket_dir = optarg;
			break;
		case 'c':
			conf_factory_file = optarg;
			break;
		case 's':
			conf_startup_dir = optarg;
			break;
		case '?':
		default:
			usage();
			break;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unused arguments\n", cmdname);
		usage();
	}
}

/*
 * Call application library's graceful exit function if a signal is received.
 */
static void signal_handler(int signal)
{
	app_exit(signal, false);
}

/*
 * Main function
 */
int main(int argc, char **argv)
{
	/* Parse command line options */
	parse_opts(argc, argv);

	/* Setup the application library */
	app_init(cmdname, appd_version, appd_init, appd_start);
	app_set_debug(debug);
	app_set_exit_func(appd_exit);
	app_set_poll_func(appd_poll, 1000);
	app_set_factory_reset_func(appd_factory_reset);
	app_set_conn_event_func(appd_connectivity_event);
	app_set_registration_event_func(appd_registration_event);
	if (app_set_conf_file(conf_factory_file, conf_startup_dir) < 0) {
		exit(EXIT_FAILURE);
	}
	if (socket_dir) {
		if (app_set_socket_directory(socket_dir) < 0) {
			exit(EXIT_FAILURE);
		}
	}

	/* Configure signal handlers for graceful termination */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Execute the application (only returns on unrecoverable error) */
	return app_run(foreground);
}
