/*
 * Copyright 2014-2018 Ayla Networks, Inc.  All rights reserved0
 */
#ifndef __AYLA_OTA_UPDATE_H__
#define __AYLA_OTA_UPDATE_H__

#include <sys/types.h>

/*
 * OTA header definitions.
 *
 * OTA header format
 *
 * JSON object
 * name/value pairs, one per line, with '\n' line termination.
 * First space separates name from value, value continues to end of line.
 * The header ends with an empty line, followed immediately with
 * binary image contents.
 *
 * ayla_ota_version 1
 * platform AY001MRT <TBD - part number for gateway>
 * image_version AY004SZG0 1.0 2014-06-17 hh:mm:ss build info
 * image_md5 <hex md5>
 * image_len <nnn - pad to 128K>   # needed to check md5 afterwards
 *
 * (optional portion for signed images)
 * image_key <index number of key used (eng, qa, release)>
 * image_signature <base64(encrypted(MD5))> <TBD - review>
 */
#define OTA_HEAD_LINE_LEN 200		/* max line length in OTA header */
#define OTA_HEAD_VERSION 1		/* OTA header version */

/* default retry times when download image fails */
#define OTA_RETRY_TIMES_DEFAULT 10

/*
 * OTA status codes - these match the ones used by BC.
 */
enum patch_state {
	PB_DONE = 0,		/* block has been completely patched */

	/*
	 * Patch not matching software version.
	 */
	PB_ERR_NEW_CRC = 0x01,	/* block would have CRC error after patch */

	/*
	 * Problems with the download.
	 */
	PB_ERR_CONNECT = 0x08,  /* failed to connect to image server */
	PB_ERR_GET = 0x09,	/* service gave error during download */

	/*
	 * Codes indicating an invalid patch.
	 */
	PB_ERR_DECOMP = 0x10,	/* patch file decompression error */
	PB_ERR_OP_LEN = 0x11,	/* segment length extends past end-of-file */
	PB_ERR_FATAL = 0x12,	/* unspecified fatal error in applying patch */
	PB_ERR_OP = 0x13,	/* segment has invalid opcode */
	PB_ERR_STATE = 0x14,	/* patch program in invalid state */
	PB_ERR_CRC = 0x15,	/* block has CRC error before the patch */
	PB_ERR_COPIES = 0x16,	/* more than one block is in copied state */
	PB_ERR_PHEAD = 0x17,	/* patch head version or length error */
	PB_ERR_FILE_CRC = 0x18,	/* patch file has CRC error */

	/*
	 * Possible hardware issues.
	 */
	PB_ERR_ERASE = 0x20,	/* block erase failed */
	PB_ERR_WRITE = 0x21,	/* block write-back failed */
	PB_ERR_SCRATCH_SIZE = 0x22, /* scratch block length too short */
	PB_ERR_DIFF_BLKS = 0x23, /* old and new code are in diff blocks */
	PB_ERR_OLD_BLKS = 0x24, /* old code of patch spans two blocks */
	PB_ERR_NEW_BLKS = 0x25, /* new code of patch spans two blocks */
	PB_ERR_SCR_ERASE = 0x26, /* scratch block erase error */
	PB_ERR_SCR_WRITE = 0x27, /* scratch block write error */
	PB_ERR_PROG = 0x28,	/* error writing progress byte */
	PB_ERR_PROT = 0x29,	/* block to be patched is not state PB_START */

	/*
	 * Module / patcher software problems.
	 */
	PB_ERR_NOFILE = 0x30,	/* patch file not found */
	PB_ERR_HEAD = 0x31,	/* patch file read of head failed */
	PB_ERR_NO_PROG = 0x33,	/* patch file not followed by progress area */
	PB_ERR_INV_PROG = 0x34,	/* invalid progress area */
	PB_ERR_READ = 0x35,	/* patch file read error */
	PB_ERR_DECOMP_INIT = 0x36, /* patch file decompression init error */
	PB_ERR_PREV = 0x37,	/* previous patch attempt failed */
	PB_ERR_OPEN = 0x39,	/* error opening flash device */
	PB_ERR_BOOT = 0x3a,	/* patcher did not boot, bb may be downlevel */

	/*
	 * In-progress state codes.
	 */
	PB_COPIED = 0x3f,	/* old version may have been erased */
	PB_START = 0x7f,	/* block ready to have patch applied */
	PB_NONE = 0xff,		/* block should be left alone */
} PACKED;

struct ota_download_param {
	char *url;		/* url of image for ota */
	size_t total_size;	/* ota download size */
	unsigned retry_times;	/* retry times when ota fails */
	char *checksum;		/* expected checksum, md5 or sha256 */
	char *dsn;		/* device dsn for lan ota */
	char *key;		/* random key for lan ota */
	size_t padding_len;	/* expected padding bytes for lan ota */
	bool lan_connect;	/* lan ota */
};

extern bool debug;

int dl_download_init(void);
void dl_add_header(const char *header);
int dl_download(struct ota_download_param *ota_param);

/*
  * Verify checksum for regular ota
  * In params:
  *     ota_param: dowload param structure for regular ota
  *         member lan_connect in structure need to be false
  * Return value:
  *     0: success
  *     -1: error
  */
int dl_verify(struct ota_download_param *ota_param);

/*
  * Verify checksum for lan ota
  * In params:
  *     ota_param: dowload param structure for lan ota
  *         member lan_connect in structure need to be true
  * Return value:
  *     0: success
  *     -1: error
  */
int dl_lan_verify(struct ota_download_param *ota_param);

/*
  * PUT lan ota status to mobile server
  * In params:
  *     url: server url for lan ota
  *     status: ota status
  * Return value:
  *     0: success
  *     -1: error
  */
int dl_put_lan_ota_status(
	const char *url,
	enum patch_state status);


#endif /* __AYLA_OTA_UPDATE_H__ */
