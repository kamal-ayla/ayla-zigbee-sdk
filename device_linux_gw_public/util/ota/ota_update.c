/*
 * Copyright 2014-2018 Ayla Networks, Inc.  All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/types.h>

#include <ayla/utypes.h>
#include <ayla/timer.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/log.h>
#include <ayla/build.h>
#include <ayla/lan_ota.h>
#include <platform/system.h>
#include <platform/ota.h>

#include "ota_update.h"

const char version[] = "ota_update " BUILD_VERSION_LABEL;

bool debug;
static bool foreground;
static bool ota_reboot;		/* reboot if operation successful */
static bool ota_apply;
static char *ota_status_socket;
static char *cmdname;
static struct ota_download_param ota;
static const struct option options[] = {
	{ .val = 'd', .name = "debug"},
	{ .val = 'f', .name = "foreground"},
	{ .val = 'a', .name = "apply"},
	{ .val = 'r', .name = "reboot"},
	{ .val = 'H', .name = "head", .has_arg = 1},
	{ .val = 'u', .name = "url", .has_arg = 1},
	{ .val = 'l', .name = "len", .has_arg = 1},
	{ .val = 'c', .name = "checksum", .has_arg = 1},
	{ .val = 's', .name = "socket", .has_arg = 1},
	{ .val = 't', .name = "times-retry", .has_arg = 1},
	{ .val = 'L', .name = "lan-connect"},
	{ .val = 'D', .name = "dsn", .has_arg = 1},
	{ .val = 'k', .name = "key", .has_arg = 1},
	{ .name = NULL }
};

static void usage(void)
{
	fprintf(stderr,
	    "%s\n"
	    "Usage: %s [OPTIONS]\n"
	    "OPTIONS:\n"
	    "  -d, --debug               Enable debug verbose debug messages\n"
	    "  -f, --foreground          Do not daemonize\n"
	    "  -a, --apply               Apply update after download\n"
	    "  -r, --reboot              Reboot when done\n"
	    "  -H, --head <header line>  Add a custom header line to request\n"
	    "  -u, --url <URL>           URL to request OTA update data from\n"
	    "  -l, --len <image size>    Expected size of OTA image in bytes\n"
	    "  -c, --checksum <hash>     OTA image checksum\n"
	    "  -s, --socket <path>       Socket to send OTA status msg to\n"
	    "  -t, --times-retry <N>     Number of download retries\n"
	    "  -L, --lan-connect         Enable LAN OTA mode\n"
	    "  -D, --dsn <device S/N>    DSN of LAN OTA image\n"
	    "  -k, --key <crypto key>    Encryption key of LAN OTA image\n",
	    version, cmdname);
	exit(1);
}

static void ota_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	char *errptr;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	optind = 0;
	while ((opt = getopt_long(argc, argv, "adfH:l:c:rs:u:t:LD:k:",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'a':
			ota_apply = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'f':
			foreground = true;
			break;
		case 'h':
			usage();
			break;
		case 'H':
			dl_add_header(optarg);
			break;
		case 'l':
			ota.total_size = strtoul(optarg, &errptr, 10);
			if (*errptr != '\0') {
				fprintf(stderr, "%s: invalid len %s",
				    cmdname, optarg);
				usage();
			}
			break;
		case 'c':
			ota.checksum = optarg;
			break;
		case 'r':
			ota_reboot = true;
			break;
		case 's':
			ota_status_socket = optarg;
			break;
		case 'u':
			ota.url = optarg;
			break;
		case 't':
			ota.retry_times = strtoul(optarg, &errptr, 10);
			if (*errptr != '\0') {
				fprintf(stderr, "%s: invalid retry times %s",
				    cmdname, optarg);
				usage();
			}
			break;
		case 'L':
			/* lan ota enabled */
			ota.lan_connect = true;
			break;
		case 'D':
			/* device dsn */
			ota.dsn = optarg;
			break;
		case 'k':
			/* random key for lan ota */
			ota.key = optarg;
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
	if (!ota.url || !ota.total_size) {
		fprintf(stderr, "%s: url and len are required parameters\n",
		    cmdname);
		usage();
	}
	if (!ota.retry_times) {
		ota.retry_times = OTA_RETRY_TIMES_DEFAULT;
	}
	if (ota.lan_connect) {
		if (!ota.dsn || !ota.key || !ota.checksum) {
			fprintf(stderr,
				"%s: dsn/key/checksum parameter need "
				"to be specified in lan ota\n", cmdname);
			usage();
		}
		/*
		 * LAN OTA images are AES256 encrypted with PKCS7 padding.
		 * Calculate the total download size including padding.
		 */
		ota.padding_len = AES256_BLK_SIZE -
		    (ota.total_size & (AES256_BLK_SIZE - 1));
		ota.total_size += ota.padding_len;

		if (strlen(ota.key) != LAN_OTA_KEY_LEN * 2) {
			fprintf(stderr,
				"%s: length of lan ota key string should "
				"be %d\n", cmdname, LAN_OTA_KEY_LEN * 2);
			usage();
		}
		if (strlen(ota.checksum) != SHA256_DIGEST_HEXSTR_LEN) {
			fprintf(stderr,
				"%s: length of lan ota checksum string should "
				"be %d\n", cmdname, SHA256_DIGEST_HEXSTR_LEN);
			usage();
		}
	}
}

static void ota_daemon(void)
{
	if (!foreground && daemon(0, 0) < 0) {
		log_err("daemon failed: %m");
	}
}

/*
 * Send OTA status to server, but only the first time this is called.
 */
static void ota_status(enum patch_state status)
{
	struct amsg_client client;
	struct msg_ota_status msg;
	enum amsg_err err;

	if (ota.lan_connect) {
		if (dl_put_lan_ota_status(ota.url, status) < 0) {
			log_err("error sending LAN OTA status %d to %s",
			    status, ota.url);
		}
	}
	if (ota_status_socket) {
		if (amsg_client_init(&client, NULL, NULL) < 0 ||
		    amsg_client_connect(&client, ota_status_socket) < 0) {
			log_err("failed to connect to %s", ota_status_socket);
			return;
		}
		msg.status = status;
		err = amsg_send_sync(&client.endpoint,
		    MSG_INTERFACE_OTA, MSG_OTA_STATUS,
		    &msg, sizeof(msg), NULL, NULL, 0);
		if (err != AMSG_ERR_NONE) {
			log_err("error sending status: %s",
			    amsg_err_string(err));
		} else {
			log_debug("sent status %d to %s",
			    status, ota_status_socket);
		}
		amsg_client_cleanup(&client);
	}
}

int main(int argc, char **argv)
{
   int rc;

   rc = dl_download_init();

   ota_opts(argc, argv);

   log_init(cmdname, LOG_OPT_FUNC_NAMES);
   if (foreground) {
      log_set_options(LOG_OPT_CONSOLE_OUT);
   }
   if (debug) {
      log_set_options(LOG_OPT_DEBUG | LOG_OPT_TIMESTAMPS);
   }
   log_set_subsystem(LOG_SUB_OTA);
   ota_daemon();
   if (rc) {
      log_err("download init failed");
      ota_status(PB_ERR_BOOT);
      exit(1);
   }
   rc = dl_download(&ota);
   if (rc) {
      log_err("download failed rc %d", rc);
      ota_status(PB_ERR_GET);	/* XXX or connect err */
      exit(1);
   }
   if (!ota.lan_connect) {
      rc = dl_verify(&ota);
   } else {
      rc = dl_lan_verify(&ota);
   }
   if (rc) {
      log_err("verify failed rc %d", rc);
      ota_status(PB_ERR_NEW_CRC);
      exit(1);
   }
   if (ota_apply) {
      rc = platform_ota_apply();
      if (rc) {
         log_err("apply failed rc %d", rc);
         ota_status(PB_ERR_FATAL);
            exit(1);
      }
   }
   ota_status(PB_DONE);
   if (ota_reboot) {
      log_info("done");
   }
   return 0;
}
