/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <platform/ota.h>

#define PLATFORM_OTA_FILE_PATH	"/tmp/ayla_ota.img"

static int ota_fd = -1;

/*
 * For OTA: Setup where a new OTA image needs to be stored and open any
 * streams if needed.
 */
int platform_ota_flash_write_open(void)
{
	ota_fd = open(PLATFORM_OTA_FILE_PATH, O_WRONLY | O_CREAT,
	    S_IRUSR | S_IWUSR);
	if (ota_fd < 0) {
		log_err("file cannot be opened for writing - %m");
		return -1;
	}
	log_debug("opened file %s for writing", PLATFORM_OTA_FILE_PATH);
	return 0;
}

/*
 * For OTA: Write a chunk of the OTA image to flash. Return the number of bytes
 * successfully written.
 */
ssize_t platform_ota_flash_write(void *buf, size_t len)
{
	ssize_t ret;

	if (ota_fd < 0) {
		log_err("invalid fd.");
		return -1;
	}
	ret = write(ota_fd, buf, len);
	if (ret < 0) {
		log_err("file cannot be written to - %m");
		return -1;
	}
	return ret;
}

/*
 * For OTA: Writing of the OTA has finished. Cleanup/close any streams if
 * needed.
 */
int platform_ota_flash_close(void)
{
	int ret = close(ota_fd);

	if (ret < 0) {
		log_err("file cannot be closed - %m");
	}
	return ret;
}

/*
 * For OTA: Setup reading back of the downloaded OTA image for verification.
 */
int platform_ota_flash_read_open(void)
{
	ota_fd = open(PLATFORM_OTA_FILE_PATH, O_RDONLY,
	    S_IRUSR | S_IWUSR);
	if (ota_fd < 0) {
		log_err("file cannot be opened for reading - %m");
		return -1;
	}
	return 0;
}

/*
 * For OTA: Used to read back the downloaded OTA image for verification.
 * Copy the recently stored image into buf with max length of len. 'off' is the
 * offset onto the image. Return the number of bytes copied.
 */
ssize_t platform_ota_flash_read(void *buf, size_t len, size_t off)
{
	ssize_t ret;

	if (ota_fd < 0) {
		log_err("invalid fd.");
		return -1;
	}
	ret = read(ota_fd, buf, len);
	if (ret < 0) {
		log_err("file cannot be read - %m");
		return -1;
	}
	return ret;
}

/*
 * For OTA: Apply the OTA after it's been downloaded and verified.
 */
int platform_ota_apply(void)
{
	if (system("apply_ota.sh " PLATFORM_OTA_FILE_PATH)) {
		log_err("failed to apply OTA image %s", PLATFORM_OTA_FILE_PATH);
		return -1;
	}
	return 0;
}
