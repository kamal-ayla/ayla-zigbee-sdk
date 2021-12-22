/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <platform/conf.h>


/*
 * Get the value of a uboot environment variable using the fw_printenv
 * utility.
 */
static int platform_conf_read_uboot(const char *name, char *buf, size_t len)
{
	char cmd[100];
	int rc;
	FILE *fp;

	rc = snprintf(cmd, sizeof(cmd), "fw_printenv -n %s 2>/dev/null", name);
	if (rc >= sizeof(cmd)) {
		return -1;
	}
	fp = popen(cmd, "r");
	if (!fp) {
		log_err("popen %s failed %m", cmd);
		return -1;
	}

	memset(buf, 0, len);

	/* unsupported leading characters are escaped by a '1' */
	if (len > 1) {
		rc = fread(buf, 1, 1, fp);
		if (rc == 1) {
			if (buf[0] != '1') {
				++buf;
				--len;
			}
		} else if (rc < 0) {
			goto close_pipe;
		}
	}

	rc = fread(buf, 1, len - 1, fp);
close_pipe:
	if (pclose(fp) != 0) {
		log_err("fw_printenv %s failed",
		    name);
		return -1;
	}
	if (rc < 0) {
		log_err("read of %s failed %m", name);
		return -1;
	}

	/* trim trailing newlines added by fw_printenv */
	if (rc > 0 && buf[rc - 1] == '\n') {
		buf[rc - 1] = '\0';
	}

	return 0;
}

/*
 * Set the value of a uboot environment variable using the fw_setenv
 * utility.
 */
static int platform_conf_write_uboot(const char *name, const char *value)
{
	int rc;
	char *escape;
	char *cmd;

	/*
	 * leading '-' characters are escaped with a '1'
	 * to avoid confusing fw_setenv.  Also escape leading '1's.
	 */
	escape = (value[0] == '-' || value[0] == '1') ? "1" : "";

	rc = asprintf(&cmd, "fw_setenv %s \"%s%s\" 2>/dev/null",
		name, escape, value);
	if (rc == -1) {
		log_err("write of %s failed: "
		    "could not allocate cmd buffer", name);
		return -1;
	}

	rc = system(cmd);
	if (rc) {
		log_err("write of %s failed %m",
		    name);
	}
	free(cmd);

	return rc ? -1 : 0;
}

/*
 * Translate known conf paths to uboot variable names.
 * Returns a statically allocated string, or the original path.
 */
static const char *platform_conf_path_to_uboot(const char *path)
{
	if (!strcmp(path, "id/dsn")) {
		return "id_dsn";
	}
	if (!strcmp(path, "id/rsa_pub_key")) {
		return "id_pubkey";
	}
	return path;
}

int platform_conf_read(const char *path, char *buf, size_t buf_size)
{
	path = platform_conf_path_to_uboot(path);
	return platform_conf_read_uboot(path, buf, buf_size);
}

int platform_conf_write(const char *path, const char *value)
{
	path = platform_conf_path_to_uboot(path);
	return platform_conf_write_uboot(path, value);
}
