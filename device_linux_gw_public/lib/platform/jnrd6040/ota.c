/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <platform/ota.h>

#define OTA_MTD_PART "alt_firmware"

#ifdef MTD_DEBUG	/* test settings */
#define OTA_MTD_PROC "/tmp/mtd"
static const char ota_cmd[] = "cat >/tmp/otadl";
#else			/* production settings */
#define OTA_MTD_PROC "/proc/mtd"
static const char ota_cmd[] = "mtd -e " OTA_MTD_PART " write - " OTA_MTD_PART;
#endif

static FILE *ota_pipe;
static char ota_dev[100];	/* device name of MTD partition */
static size_t ota_size;
static size_t ota_write_len;	/* bytes written */
static size_t ota_erase_size;
static size_t ota_read_off;	/* next read offset */
static int ota_read_fd = -1;


/*
 * Get device name for given partition using /proc/mtd.
 * Fill in size,  erase_size, if provided.
 */
static int ota_flash_dev_get(const char *part, char *buf, size_t len,
				size_t *size, size_t *erase_size)
{
	FILE *fp;
	char line[100];
	char *lp;
	char *cp;
	char *name;
	int rc = -1;
	unsigned long val;

	buf[0] = '\0';
	fp = fopen(OTA_MTD_PROC, "r");
	if (!fp) {
		log_err("open of " OTA_MTD_PROC " failed %m");
		return -1;
	}

	for (;;) {
		lp = fgets(line, sizeof(line), fp);
		if (!lp) {
			if (feof(fp)) {
				break;
			}
			log_err("read err on " OTA_MTD_PROC " %m");
			break;
		}

		/*
		 * Example line:
		 * mtd5: 03600000 00020000 "alt_firmware"
		 */
		name = strchr(lp, '"');	/* first quote */
		if (!name) {
			continue;
		}
		name++;			/* advance past quote */
		cp = strchr(name, '"');	/* second quote */
		if (!cp) {
			continue;
		}
		*cp++ = '\0';		/* terminate name */
		if (strcmp(name, part)) {
			continue;	/* not a match */
		}
		name = lp;
		cp = strchr(name, ':');
		if (!cp) {
			continue;
		}
		*cp = '\0';
#ifdef MTD_DEBUG
		snprintf(buf, len, "%s", name);		/* for testing */
#else
		snprintf(buf, len, "/dev/%s", name);
#endif /* MTD_DEBUG */

		val = strtoul(cp + 1, &cp, 16);
		if (*cp != ' ') {
			log_err("invalid size in " OTA_MTD_PROC);
			break;
		}
		if (size) {
			*size = val;
		}
		val = strtoul(cp + 1, &cp, 16);
		if (*cp != ' ') {
			log_err("invalid erase_size in " OTA_MTD_PROC);
			break;
		}
		if (erase_size) {
			*erase_size = val;
		}
		rc = 0;
		break;
	}
	fclose(fp);
	return rc;
}

static int ota_get_part(void)
{
	if (ota_dev[0]) {
		return 0;
	}

	if (ota_flash_dev_get(OTA_MTD_PART, ota_dev, sizeof(ota_dev),
	    &ota_size, &ota_erase_size)) {
		log_err("read_open: couldn't determine MTD partition");
		return -1;
	}
	log_debug("MTD dev %s size %x erase_size %x",
	    ota_dev, ota_size, ota_erase_size);
	return 0;
}

static int ota_flash_block_is_bad(int fd, size_t off)
{
	int rc;
	loff_t offset = off;

	rc = ioctl(fd, MEMGETBADBLOCK, &offset);
	if (rc < 0) {
		log_err("ioctl for bad block failed - %m");
	}
	return rc;
}

/*
 * Pad out last write to the pipe to the erase size with 0xff.
 */
static int ota_pipe_pad(FILE *fp, size_t off, size_t block)
{
	char buf[1024];
	size_t pad;
	ssize_t tlen;
	size_t rc;

	pad = (off & (block - 1));
	if (!block || !pad) {
		return 0;
	}

	memset(buf, 0xff, sizeof(buf));

	for (pad = block - pad; pad; pad -= tlen) {
		tlen = pad;
		if (tlen > sizeof(buf)) {
			tlen = sizeof(buf);
		}
		rc = fwrite(buf, 1, tlen, fp);
		if (!rc) {
			return -1;
		}
		tlen = rc;
	}
	return 0;
}

/*
 * Setup where a new OTA image needs to be stored and open any
 * streams if needed.
 */
int platform_ota_flash_write_open(void)
{
	if (ota_get_part()) {
		return -1;
	}
	ota_pipe = popen(ota_cmd, "w");
	if (!ota_pipe) {
		log_err("popen for mtd failed %m");
		return -1;
	}
	ota_write_len = 0;
	return 0;
}

/*
 * Write a chunk of the OTA image to flash. Return the number of bytes
 * successfully written.
 */
ssize_t platform_ota_flash_write(void *buf, size_t len)
{
	ssize_t rc;

	rc = fwrite(buf, 1, len, ota_pipe);
	if (rc > 0) {
		ota_write_len += rc;
	}
	return rc;
}

/*
 * Writing of the OTA has finished. Cleanup/close any streams if
 * needed.
 */
int platform_ota_flash_close(void)
{
	int status;
	int err = 0;

	if (ota_read_fd >= 0) {
		close(ota_read_fd);
		ota_read_fd = -1;
	}
	if (!ota_pipe) {
		return 0;		/* ignore if never opened */
	}

	if (ota_pipe_pad(ota_pipe, ota_write_len, ota_erase_size)) {
		log_debug("ota pipe pad failed %m");
		err = 1 << 16;
	}

	status = pclose(ota_pipe);
	ota_pipe = NULL;
	if (status == -1) {
		log_debug("ota pipe pclose failed %m");
		return status;
	}
	return status | err;
}

/*
 * Setup reading back of the downloaded OTA image for verification.
 */
int platform_ota_flash_read_open(void)
{
	if (ota_get_part()) {
		return -1;
	}
	ota_read_fd = open(ota_dev, O_RDONLY, 0);
	if (ota_read_fd < 0) {
		log_err("read_open of %s failed %m", ota_dev);
		return -1;
	}
	ota_read_off = 0;
	return 0;
}

/*
 * Used to read back the downloaded OTA image for verification.
 * Copy the recently stored image into buf with max length of len. 'off' is the
 * offset onto the image. Return the number of bytes copied.
 */
ssize_t platform_ota_flash_read(void *buf, size_t len, size_t off)
{
	off_t new_off;
	ssize_t len_read;
	int rc;

	if (ota_read_fd < 0) {
		return 0;
	}

	/*
	 * If starting a block, see if it should be skipped.
	 */
	if (ota_erase_size && (ota_read_off & (ota_erase_size - 1)) == 0) {
		rc = ota_flash_block_is_bad(ota_read_fd, ota_read_off);
		if (rc < 0) {
			return rc;
		}
		if (rc) {
			ota_read_off += ota_erase_size;
			new_off = lseek(ota_read_fd, ota_read_off, SEEK_SET);
			if (new_off == (off_t)-1) {
				log_err("lseek to %zx failed - %m",
				    ota_read_off);
				return new_off;
			}
		}
	}

	len_read = read(ota_read_fd, buf, len);
	if (len_read > 0) {
		ota_read_off += len_read;
	}
	return len_read;
}

/*
 * Apply the OTA after it's been downloaded and verified.
 */
int platform_ota_apply(void)
{
	if (system("ota_switch")) {
		log_err("ota_switch failed");
		return -1;
	}
	return 0;
}
