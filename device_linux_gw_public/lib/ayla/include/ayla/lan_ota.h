/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_LAN_OTA_H__
#define __AYLA_LAN_OTA_H__

#include <jansson.h>
#include <openssl/sha.h>
#include <ayla/crypto.h>

#define AES256_BLK_SIZE	16	/* Block size for AES 256 */
#define LAN_OTA_KEY_LEN	32	/* LAN Key length */
#define LAN_OTA_IV_SIZE	16	/* CBC IV Seed */

/* 256 bits, 32 bytes */
#define SHA256_DIGEST_LENGTH 32
#define SHA256_DIGEST_HEXSTR_LEN (SHA256_DIGEST_LENGTH * 2)

/* lan ota image info */
struct lan_ota_img_info {
	char *url; /* url in mobile server to get lan ota image */
	char *version; /* version of lan ota image */
	size_t size; /* clear text size of lan ota image */
	char *type; /* type of lan ota image: module/host mcu */
	int port; /* port in mobile server to get lan ota image */
	char *header; /* base64 encoded, 256B encrypted image header */
};

/* lan ota image header info */
struct lan_ota_img_hdr_info {
	char *dsn; /* dsn of device that need to do lan ota */
	char *version; /* version of lan ota image */
	char *key; /* the key to decrypt image content using AES256 */
	char *sign; /* the signature of image plain text */
};

/* lan ota execute info */
struct lan_ota_exec_info {
	char *ota_type; /* ota  type */
	char *url; /* ota  url */
	char *ver; /* ota  image version */
	char *dsn; /* device dsn for lan ota */
	char *key; /* random key for lan ota */
	char *checksum; /* ota  checksum */
	size_t size; /* ota  image size */
};

struct lan_ota_dec_param {
	/* AES CBC decrypt context */
	struct crypto_state aes;
	/* image AES 256 key */
	unsigned char img_enc_key[LAN_OTA_KEY_LEN];
	/* body content for next time AES decryption */
	unsigned char aes_remain_buf[AES256_BLK_SIZE];
	/* indicate whether error occurs during decryption */
	bool error_occurred;
	size_t total_size; /* ota image size */
	/* total length of image that has been decrypted */
	size_t total_decrypted_len;
	/* padding length of image */
	size_t padding_len;
	/* the length of body content for next time AES decryption */
	size_t aes_remain_len;
	/* signature of recieved body */
	SHA256_CTX stx;
};

/*
  * Process lan ota image header
  * In params:
  *     header: pointer to image header
  *     header_len: length of image header
  *     device_dsn: DSN of device which lan ota is deployed to
  *     device_pubkey: public key of device
  * Out params:
  *     ota_info: structure contains lan ota execution info,
  *          the memory of structure members needs to be freed
  *		by the calling function,
  *          pass NULL if ota info is not needed
  * Return value:
  *     0: success
  *     -1: internal error
  *     -2: bad image header
  */
int lan_ota_header_proc(const void *header, size_t header_len,
	const char *device_dsn, const char *device_pubkey,
	struct lan_ota_exec_info *ota_info);

/*
  * Free the memory of members in ota_info structure
  */
void lan_ota_free_exec_info(
	struct lan_ota_exec_info *ota_info);

/*
 * Init lan ota decrypt data, called before lan ota image decryption
 * In params:
 *     lan_dec_param: structure of ota decrytion params
 *     total_size: size of lan ota image payload
 *     key: key for lan ota image payload decryption
 *     dsn: device serial number
 */
int lan_ota_img_decrypt_init(
	struct lan_ota_dec_param *lan_dec_param,
	unsigned long total_size,
	const char *key,
	const char *dsn);

/*
 * Free resources associated with lan_ota_dec_param structure.
 * In params:
 *     lan_dec_param: structure of ota decrytion params
 */
void lan_ota_img_decrypt_cleanup(struct lan_ota_dec_param *lan_dec_param);

/*
  * Decrypt part/all of lan ota image payload that have been received
  * In params:
  *     lan_dec_param: structure of ota decrytion params
  *     buffer: image payload which is received
  *               from mobile server in current curl fetch
  *     buf_len: image payload length in current curl fetch
  * Out params:
  *     out_dec_buf: decrypted image payload,
  *                        needs to be freed by the calling function
  *     out_dec_len: decrypted image length
  * Return value:
  *     0: success
  *     -1: error
  */
int lan_ota_img_decrypt_data(
	struct lan_ota_dec_param *lan_dec_param,
	const char *buffer,
	size_t buf_len,
	char **out_dec_buf,
	size_t *out_dec_len);

/*
  * Get lan ota image's signature
  * In params:
  *     ctx: SHA256 CTX structure
  *     len: buffer length
  * Out params:
  *     buf: hex string representation of sha256 digest
  * Return value:
  *     0: success
  *     -1: error
  */
int lan_ota_sha256_get(SHA256_CTX *ctx, void *buf, size_t len);

#endif /* __AYLA_LAN_OTA_H__ */
