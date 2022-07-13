/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
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
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/conf_io.h>
#include <ayla/socket.h>
#include <ayla/file_io.h>
#include <ayla/build.h>
#include <ayla/msg_utils.h>

#include "dapi.h"
#include "notify.h"
#include "ds.h"
#include "devd_conf.h"
#include "serv.h"
#include "ops_devd.h"
#include "props_client.h"
#include "props_if.h"
#include "app_if.h"
#include "msg_server.h"
#include "gateway_client.h"

const char version[] = "devd " BUILD_VERSION_LABEL; //original version from ayla David
#define DEVD_CONF_DIR "/config"
#define DEVD_CONF_FILE "devd.conf"
#define APPD_STARTUP_CONF_FILE_LEGACY "appd.conf"
#define DEVD_CONF_FILE_FACTORY_EXT_LEGACY "_factory"
#define DEVD_CONF_FILE_LEGACY "devd"
#define DEVD_CONF_FILE_LEGACY2 "config"
#define DEVD_CONF_FILE_FACTORY_EXT ".conf"

#define APPD_NAME		"appd"

#define DEVD_SOCK_SUBDIR	"devd"
#define DEVD_SOCK_APP_SUBDIR	APPD_NAME

static char *socket_dir = SOCK_DIR_DEFAULT;
char app_sock_path[SOCKET_PATH_STR_LEN];
char devd_sock_path[SOCKET_PATH_STR_LEN];
char devd_msg_sock_path[SOCKET_PATH_STR_LEN];

static char devd_conf_factory[PATH_MAX];
static const char *devd_conf_startup_dir;

struct device_state device;

int debug;

bool foreground;
bool ds_test_mode;

bool conf_loaded;

static bool ds_no_appd;
static int ds_appd_pid;
static bool ds_child_died;
static bool ds_started_appd;

static char *cmdname;
static int ds_wait;

static const struct option options[] = {
	{ .name = "debug", .val = 'd'},
	{ .name = "factory_config", .has_arg = 1, .val = 'c'},
	{ .name = "startup_dir", .has_arg = 1, .val = 's'},
	{ .name = "foreground", .val = 'f'},
	{ .name = "wait", .val = 'w'},
	{ .name = "sockdir", .has_arg = 1, .val = 'o'},
	{ .name = "no_appd", .val = 'n'},
	{ .name = "test", .val = 't'},
	{ .name = NULL }
};

static void usage(void)
{
	fprintf(stderr,
	    "%s\n"
	    "Usage: %s [OPTIONS]\n"
	    "OPTIONS:\n"
	    "  -d, --debug                 Enable debug verbose debug "
	    "messages\n"
	    "  -f, --foreground            Do not daemonize\n"
	    "  -c, --factory_config <file> Factory config file path\n"
	    "  -s, --startup_dir <dir>     Startup config directory\n"
	    "  -w, --wait                  Wait for dhcp_bound event enable "
	    "client\n"
	    "  -o, --sockdir <dir>         Socket directory (e.g. /var/run)\n"
	    "  -n, --no_appd               Do not execute and manage 'appd' "
	    "application service\n"
	    "  -t, --test                  Connect to cloud in test mode\n",
	    version, cmdname);
	exit(EXIT_FAILURE);
}

static int ds_config_legacy_conversion(const char *conf_path,
	const char *startup_dir)
{
	char factory_file[PATH_MAX];
	char startup_file[PATH_MAX];
	char tmp[PATH_MAX];
	const char *dir;
	const char *factory_legacy = DEVD_CONF_FILE_LEGACY
	    DEVD_CONF_FILE_FACTORY_EXT_LEGACY;
	const char *factory_legacy2 = DEVD_CONF_FILE_LEGACY2
	    DEVD_CONF_FILE_FACTORY_EXT_LEGACY;
	const char *startup_legacy = DEVD_CONF_FILE_LEGACY;
	const char *startup_legacy2 = DEVD_CONF_FILE_LEGACY2;
	int rc = 0;

	/* Check if supplied path is a directory or default directory exists */
	if (conf_path && *conf_path) {
		dir = conf_path;
	} else {
		dir = DEVD_CONF_DIR;
	}
	if (!file_is_dir_ayla(dir)) {
		dir = NULL;
	}
	/* Look for a legacy factory config file */
	if (dir) {
		/* Check for legacy default factory file names */
		snprintf(factory_file, sizeof(factory_file), "%s/%s", dir,
		    factory_legacy);
		if (!access(factory_file, R_OK)) {
			goto find_startup;
		}
		snprintf(factory_file, sizeof(factory_file), "%s/%s", dir,
		    factory_legacy2);
		if (!access(factory_file, R_OK)) {
			goto find_startup;
		}
	} else if (conf_path && *conf_path) {
		/* Check for legacy factory file extension */
		snprintf(factory_file, sizeof(factory_file), "%s%s", conf_path,
		    DEVD_CONF_FILE_FACTORY_EXT_LEGACY);
		if (!access(factory_file, R_OK)) {
			goto find_startup;
		}
	}
	factory_file[0] = '\0';
	/* Look for a legacy startup config file */
find_startup:
	if (dir) {
		/* Check for legacy default startup file names */
		snprintf(startup_file, sizeof(startup_file), "%s/%s", dir,
		    startup_legacy);
		if (!access(startup_file, R_OK)) {
			goto convert;
		}
		snprintf(startup_file, sizeof(startup_file), "%s/%s", dir,
		    startup_legacy2);
		if (!access(startup_file, R_OK)) {
			goto convert;
		}
	} else if (conf_path && *conf_path && *factory_file &&
	    !access(conf_path, R_OK)) {
		/*
		 * Check for legacy startup file.  Make sure a legacy factory
		 * file was found, because the non-legacy factory file could
		 * match a legacy startup file in this case.
		 */
		snprintf(startup_file, sizeof(startup_file), "%s", conf_path);
		goto convert;
	}
	startup_file[0] = '\0';
convert:

	if (dir) {
		/* Verify default file names didn't match a directory name */
		if (*factory_file && file_is_dir_ayla(factory_file)) {
			factory_file[0] = '\0';
		}
		if (*startup_file && file_is_dir_ayla(startup_file)) {
			startup_file[0] = '\0';
		}
	}
	/* No legacy files found */
	if (!*factory_file && !*startup_file) {
		return -1;
	}
	/*
	 * Both files found.  Rename with .bak extension as an
	 * intermediate step to prevent potential naming conflicts.
	 */
	if (*factory_file && *startup_file) {
		log_debug("adding .bak extension to legacy config files");
		snprintf(tmp, 5000, "%s.bak", factory_file); //temporary change to ignore the warning
		if (rename(factory_file, tmp) < 0) {
			log_err("rename %s to %s failed: %m", factory_file,
			    tmp);
			rc = -1;
		} else {
			snprintf(factory_file, sizeof(factory_file), "%s", tmp);
		}
		snprintf(tmp, 5000, "%s.bak", startup_file); //temporary change to ignore the warning
		if (rename(startup_file, tmp) < 0) {
			log_err("rename %s to %s failed: %m", startup_file,
			    tmp);
			rc = -1;
		} else {
			snprintf(startup_file, sizeof(startup_file), "%s", tmp);
		}
	} else if (!*factory_file) {
		/* Legacy factory config is missing, so convert the
		 * legacy startup config to a factory config.
		 */
		snprintf(factory_file, sizeof(factory_file), "%s",
		    startup_file);
		startup_file[0] = '\0';
		log_debug("populating factory config from legacy startup "
		    "config");
	}
	/* Migrate factory file to new naming convention */
	if (*factory_file) {
		if (dir) {
			snprintf(tmp, sizeof(tmp), "%s/%s", dir,
			    DEVD_CONF_FILE);
			if (rename(factory_file, tmp) < 0) {
				log_err("rename %s to %s failed: %m",
				    factory_file, tmp);
				rc = -1;
			} else {
				log_info("converted legacy factory config:"
				    " %s to %s", factory_file, tmp);
			}
		} else {
			if (rename(factory_file, conf_path) < 0) {
				log_err("rename %s to %s failed: %m",
				    factory_file, conf_path);
				rc = -1;
			} else {
				log_info("converted legacy factory config:"
				    " %s to %s", factory_file, conf_path);
			}
		}
	}
	/*
	 * Migrate startup file to new naming convention
	 * (and optional startup directory)
	 */
	if (*startup_file) {
		if (dir) {
			snprintf(tmp, sizeof(tmp), "%s/%s.%s",
			    startup_dir ? startup_dir : dir,
			    DEVD_CONF_FILE, CONF_STARTUP_FILE_EXT);
			if (rename(startup_file, tmp) < 0) {
				log_err("rename %s to %s failed: %m",
					startup_file, tmp);
				rc = -1;
			} else {
				log_info("converted legacy startup config:"
				    " %s to %s", startup_file, tmp);
			}
		} else {
			if (startup_dir) {
				snprintf(tmp, sizeof(tmp), "%s/%s.%s",
				    startup_dir, file_get_name(conf_path),
				    CONF_STARTUP_FILE_EXT);
			} else {
				snprintf(tmp, sizeof(tmp), "%s.%s", conf_path,
				    CONF_STARTUP_FILE_EXT);
			}
			if (rename(startup_file, tmp) < 0) {
				log_err("rename %s to %s failed: %m",
					startup_file, tmp);
				rc = -1;
			} else {
				log_info("converted legacy startup config:"
				    " %s to %s", startup_file, tmp);
			}
		}
	}
	return rc;
}

/*
 * Determine location of appd and start it
 */
static void ds_start_appd()
{
	char appd_loc[20];
	char config_dir[PATH_MAX];
	char *argv[12];
	int i = 0;
	int j;

	argv[i++] = APPD_NAME;
	argv[i++] = "-f";
	if (debug) {
		argv[i++] = "-d";
	}
	if (socket_dir) {
		argv[i++] = "-o";
		argv[i++] = socket_dir;
	}
	file_get_dir(devd_conf_factory, config_dir, sizeof(config_dir));
	argv[i++] = "-c";
	argv[i++] = config_dir;
	if (devd_conf_startup_dir) {
		argv[i++] = "-s";
		argv[i++] = (char *)devd_conf_startup_dir;
	}
	argv[i] = NULL;
	ASSERT(i <= ARRAY_LEN(argv));
	if (debug) {
		log_debug("Starting %s using args: ", APPD_NAME);
		for (j = 0; j < i; j++) {
			log_debug("%s", argv[j]);
		}
	}
	execvp(APPD_NAME, argv);

	/* perhaps running locally on VM */
	snprintf(appd_loc, sizeof(appd_loc), "./%s", APPD_NAME);
	log_warn("executing %s failed, trying %s", APPD_NAME, appd_loc);
	argv[0] = appd_loc;
	execvp(appd_loc, argv);
	log_err("unable to start %s", APPD_NAME);
	sleep(2);
	exit(1);
}

/*
 * Terminate appd, if managed by devd.
 */
void ds_kill_appd(void)
{
	if (ds_appd_pid) {
		kill(ds_appd_pid, SIGTERM);
	}
}

/*
 * Fork and start appd
 */
static void ds_fork_and_start_appd_if_needed(struct device_state *dev)
{
	int wait_status;
	pid_t pid;

	if (ds_child_died) {
		ds_child_died = 0;
		pid = waitpid(-1, &wait_status, WNOHANG);
		if (pid != ds_appd_pid) {
			goto run_appd_if_needed;
		}
		ds_started_appd = false;
		if (WIFSIGNALED(wait_status)) {
			log_err("appd terminated due to sig %d",
			    WTERMSIG(wait_status));
		} else if (WIFEXITED(wait_status)) {
			if (WEXITSTATUS(wait_status)) {
				log_debug("appd exited with status: %d",
				    WEXITSTATUS(wait_status));
			} else {
				log_debug("appd exited normally");
			}
		} else {
			log_err("appd terminated abnormally");
		}
	}

run_appd_if_needed:
	if (ds_started_appd) {
		return;
	}
	log_debug("starting appd fork");
	pid = fork();
	if (pid < 0) {
		log_err("fork failed");
		return;
	}
	ds_started_appd = true;
	if (pid == 0) {
		ds_start_appd();
	} else {
		ds_appd_pid = pid;
	}
}

static void ds_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	char *conf_file = NULL;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	optind = 0;
	while ((opt = getopt_long(argc, argv, "c:s:o:dfwnt",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'c':
			conf_file = file_clean_path(optarg);
			break;
		case 's':
			devd_conf_startup_dir = file_clean_path(optarg);
			break;
		case 'f':
			foreground = true;
			break;
		case 'd':
			debug = 1;
			break;
		case 'w':
			ds_wait = true;
			break;
		case 'o':
			socket_dir = file_clean_path(optarg);
			break;
		case 'n':
			ds_no_appd = true;
			break;
		case 't':
			ds_test_mode = true;
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
	log_init(cmdname, LOG_OPT_FUNC_NAMES);
	if (foreground) {
		log_set_options(LOG_OPT_CONSOLE_OUT);
	}
	if (debug) {
		log_set_options(LOG_OPT_TIMESTAMPS | LOG_OPT_DEBUG);
	}
	log_set_subsystem(LOG_SUB_CLIENT);
	if (!foreground && daemon(0, 0) < 0) {
		log_err("daemon failed: %m");
		exit(4);
	}
	/*
	 * Check for and convert legacy config file names, to support
	 * upgrading from older firmware versions.
	 */
	ds_config_legacy_conversion(conf_file, devd_conf_startup_dir);

	/* No factory config specified, so use the default path */
	if (!conf_file) {
		conf_file = DEVD_CONF_DIR "/" DEVD_CONF_FILE;
	}
	/*
	 * The factory config path specified was a directory, so use
	 * the default file name.
	 */
	if (file_is_dir_ayla(conf_file)) {
		snprintf(devd_conf_factory, sizeof(devd_conf_factory),
		    "%s/%s", conf_file, DEVD_CONF_FILE);
	} else {
		snprintf(devd_conf_factory, sizeof(devd_conf_factory), "%s",
		    conf_file);
	}
	/* Generate socket paths */
	snprintf(devd_sock_path, sizeof(devd_sock_path), "%s/%s/%s",
	    socket_dir, DEVD_SOCK_SUBDIR, SOCKET_NAME);
	snprintf(app_sock_path, sizeof(app_sock_path), "%s/%s/%s",
	    socket_dir, DEVD_SOCK_APP_SUBDIR, SOCKET_NAME);
	snprintf(devd_msg_sock_path, sizeof(devd_msg_sock_path),
	    "%s/%s/%s", socket_dir, MSG_APP_NAME_CLIENT, MSG_SOCKET_DEFAULT);
	/* Clear umask to allow socket creation with full privs */
	umask(0);
}

static void ds_run(void)
{
	struct device_state *dev = &device;
	s64 next_timeout_ms;

	for (;;) {
		next_timeout_ms = timer_advance(&dev->timers);
		if (file_event_poll(&dev->file_events, next_timeout_ms) < 0) {
			log_warn("poll error: %m");
		}
		if (!ds_no_appd) {
			ds_fork_and_start_appd_if_needed(dev);
		}
	}
}

static void ds_child_exit_handler(int s)
{
	ds_child_died = 1;
}

static void ds_sig_exit_handler(int s)
{
	log_debug("Caught signal %d", s);
	exit(1);
}

static void ds_sigpipe_handler(int s)
{
	log_warn("Caught SIGPIPE");
}

static void ds_exit_handler(void)
{
	log_debug("inside exit handler");
	ds_kill_appd();
	msg_server_cleanup();
	app_req_delete_all();
	ds_cleanup();
	conf_cleanup();
}

int main(int argc, char **argv)
{
	struct device_state *dev = &device;
	struct sigaction sigHandler;
	log_debug("devd startup...");

	memset(&sigHandler, 0, sizeof(sigHandler));
	sigHandler.sa_handler = ds_sig_exit_handler;
	sigemptyset(&sigHandler.sa_mask);

	sigaction(SIGINT, &sigHandler, NULL);
	sigaction(SIGTERM, &sigHandler, NULL);

	sigHandler.sa_handler = ds_child_exit_handler;
	sigaction(SIGCHLD, &sigHandler, NULL);

	sigHandler.sa_handler = ds_sigpipe_handler;
	sigaction(SIGPIPE, &sigHandler, NULL);

	atexit(ds_exit_handler);

	file_event_init(&dev->file_events);

	ds_opts(argc, argv);

	if (conf_init(devd_conf_factory, devd_conf_startup_dir) < 0) {
		log_err("error initializing devd config: %s",
		    devd_conf_factory);
		exit(EXIT_FAILURE);
	}
	devd_conf_init();



	if (ds_init() < 0) {
		exit(EXIT_FAILURE);
	}

	gateway_init();

	if (conf_load()) {
		exit(EXIT_FAILURE);
	}
	conf_loaded = true;
	if (conf_factory_loaded()) {
		conf_reset = true;
	}

	serv_init();

	if (msg_server_create(dev, devd_msg_sock_path, S_IRWXU | S_IRWXG) < 0) {
		log_err("failed to initialize local messaging server");
		exit(EXIT_FAILURE);
	}

	app_init();

	if (!ds_wait) {
		ds_net_up();
	}
	ds_run();
	return 0;
}
