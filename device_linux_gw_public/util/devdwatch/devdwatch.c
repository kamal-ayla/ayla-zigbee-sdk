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
#include <getopt.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <ayla/utypes.h>
#include <ayla/log.h>

#define DEVD_NAME "devd"

static const struct option options[] = {
	{ .name = "no_devd", .val = 'n'},
	{ .name = "foreground", .val = 'f'},
	{ .name = NULL }
};

static char *cmdname;
static char *devd_argv[10];
static int ds_devd_pid;

static void devdwatch_usage(void)
{
	fprintf(stderr,
	    "usage: %s [--no_devd] [--foreground] -- [flags_for_devd]\n"
	    "       %s [-nf] -- [flags_for_devd]\n",
	    cmdname, cmdname);
	exit(1);
}

static void devdwatch_poll(void)
{
	int wait_status;
	pid_t pid;
	char devd_loc[20];

	for (;;) {
		log_debug("starting devd fork");
		pid = fork();
		if (pid < 0) {
			log_err("fork failed");
			return;
		}
		if (pid == 0) {
			execvp(DEVD_NAME, devd_argv);
			/* perhaps running locally on VM */
			snprintf(devd_loc, sizeof(devd_loc),
			    "./%s", DEVD_NAME);
			devd_argv[0] = devd_loc;
			execvp(devd_loc, devd_argv);
			log_err("unable to start %s", DEVD_NAME);
			goto restart;
		} else {
			ds_devd_pid = pid;
		}
		pid = wait(&wait_status);
		if (pid == -1) {
			log_err("wait error");
			goto restart;
		}
		if (WIFSIGNALED(wait_status) != 0) {
			log_err("devd terminated due to sig %d",
			    WTERMSIG(wait_status));
		} else if (WIFEXITED(wait_status) != 0) {
			log_debug("devd ended normally");
		} else {
			log_err("devd terminated abnormally");
		}
restart:
		sleep(10);
	}
}

static void devdwatch_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	int no_devd = 0;
	int foreground = 0;
	int i;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	optind = 0;
	while ((opt = getopt_long(argc, argv, "fn",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'n':
			no_devd = 1;
			break;
		case 'f':
			foreground = 1;
			break;
		default:
			devdwatch_usage();
			break;
		}
	}
	if (no_devd) {
		exit(1);
	}
	log_init(cmdname, LOG_OPT_FUNC_NAMES | LOG_OPT_DEBUG);
	if (foreground) {
		log_set_options(LOG_OPT_CONSOLE_OUT | LOG_OPT_TIMESTAMPS);
	}

	argc -= optind;
	argv += optind;
	if (argc > ARRAY_LEN(devd_argv) - 3) {
		log_err("too many args to devd");
		exit(1);
	}
	if (!foreground && daemon(0, 0) < 0) {
		log_err("daemon failed: %m");
		exit(4);
	}
	devd_argv[0] = DEVD_NAME;
	log_debug("runnning %s with args:", DEVD_NAME);
	for (i = 0; argv[i]; i++) {
		devd_argv[i + 1] = argv[i];
		log_debug("%s", devd_argv[i + 1]);
	}
	devd_argv[++i] = "-f";	/* always run devd in foreground mode */
	log_debug("%s", devd_argv[i]);
	devd_argv[++i] = NULL;
}

static void ds_usr_exit_handler(int s)
{
	log_debug("Caught signal %d\n", s);
	if (ds_devd_pid) {
		kill(ds_devd_pid, SIGTERM);
	}
	exit(1);
}

int main(int argc, char **argv)
{
	struct sigaction sigUsrHandler;

	memset(&sigUsrHandler, 0, sizeof(sigUsrHandler));
	sigUsrHandler.sa_handler = ds_usr_exit_handler;
	sigemptyset(&sigUsrHandler.sa_mask);
	sigaction(SIGUSR1, &sigUsrHandler, NULL);

	devdwatch_opts(argc, argv);
	devdwatch_poll();

	return 0;
}
