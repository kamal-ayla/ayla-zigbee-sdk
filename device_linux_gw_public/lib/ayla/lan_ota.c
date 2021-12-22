/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

#include <openssl/sha.h>

#include <ayla/utypes.h>
#include <ayla/crc.h>
#include <ayla/hex.h>
#include <ayla/assert.h>
#include <ayla/json_parser.h>
#include <ayla/base64.h>
#include <ayla/crypto.h>
#include <ayla/log.h>
#include <ayla/lan_ota.h>


#define LAN_OTA_IMAGE_HEADER_SZ 256

/* parse image info for lan ota get from PUT /lanota.json */
/* Json example:
{
	"ota":
	{
		"url": "http://<mobile_server_ip_addr>/bc-0.18.1.patch",
		"ver": "0.18.1",
		"size": 12300,
		"type": "host_mcu",
		"port": 8888,
		"head": "<base64-header-from-first-256B-of-image>"
	}
}
*/
static int lan_ota_parse_img_info(const void *img_info, size_t img_info_len,
	struct lan_ota_img_info *img_info_parsed)
{
	json_error_t json_err;
	json_t *img_info_json;
	json_t *ota_json;
	const char *url;
	const char *ver;
	unsigned size;
	const char *type;
	u16 port;
	const char *header;

	img_info_json = json_loadb(img_info, img_info_len, 0, &json_err);
	if (!img_info_json) {
		log_err("load image json err %s", json_err.text);
		return -1;
	}
	ota_json = json_object_get(img_info_json, "ota");
	if (!ota_json) {
		goto inval_ota_img_json;
	}
	/* parse lan ota image info */
	url = json_get_string(ota_json, "url");
	if (!url) {
		goto inval_ota_img_json;
	}
	ver = json_get_string(ota_json, "ver");
	if (!ver) {
		goto inval_ota_img_json;
	}
	if (json_get_uint(ota_json, "size", &size) < 0) {
		goto inval_ota_img_json;
	}
	type = json_get_string(ota_json, "type");
	if (!type) {
		goto inval_ota_img_json;
	}
	/* type needs to be host_mcu */
	if (strcmp(type, "host_mcu")) {
		log_err("invalid ota type %s", type);
		goto inval_ota_img_json;
	}
	if (json_get_uint16(ota_json, "port", &port) < 0) {
		goto inval_ota_img_json;
	}
	header = json_get_string(ota_json, "head");
	if (!header) {
		log_err("missing base64 encoded img header "
		    "(required in LAN OTA v1 and above)");
		goto inval_ota_img_json;
	}

	log_debug("image info url: %s", url);
	log_debug("image info version: %s", ver);
	log_debug("image info size: %u", size);
	log_debug("image info type: %s", type);
	log_debug("image info port: %hu", port);

	/* save lan ota image info */
	img_info_parsed->url = strdup(url);
	img_info_parsed->version = strdup(ver);
	img_info_parsed->size = size;
	img_info_parsed->type = strdup(type);
	img_info_parsed->port = port;
	img_info_parsed->header = strdup(header);

	json_decref(img_info_json);
	return 0;

inval_ota_img_json:
	log_err("invalid image json format");
	json_decref(img_info_json);
	return -1;
}

static void lan_ota_free_img_info(
	struct lan_ota_img_info *img_info)
{
	free(img_info->url);
	free(img_info->version);
	free(img_info->type);
	free(img_info->header);
}

/* parse json info in image header */
/* Json example:
J = {
	"dsn": "AC000W000441358",
	"ver": "2.4.1",
	"key": "K",
	"sign": "S"
}
*/
static int lan_ota_parse_img_hdr_info(
	const char *img_hdr_info_str,
	struct lan_ota_img_hdr_info *img_hdr_info_parsed)
{
	json_error_t jerr;
	json_t *img_hdr_info_json;
	const char *dsn;
	const char *ver;
	const char *key;
	const char *sign;

	img_hdr_info_json = json_loads(img_hdr_info_str, 0, &jerr);
	if (!img_hdr_info_json) {
		log_err("load image header json err %s", jerr.text);
		return -1;
	}

	/* parse lan ota image info */
	dsn = json_get_string(img_hdr_info_json, "dsn");
	if (!dsn) {
		goto inval_ota_img_hdr_json;
	}
	ver = json_get_string(img_hdr_info_json, "ver");
	if (!ver) {
		goto inval_ota_img_hdr_json;
	}
	key = json_get_string(img_hdr_info_json, "key");
	if (!key) {
		goto inval_ota_img_hdr_json;
	}
	sign = json_get_string(img_hdr_info_json, "sign");
	if (!sign) {
		goto inval_ota_img_hdr_json;
	}

	log_debug("image header dsn: %s", dsn);
	log_debug("image header version: %s", ver);
	log_debug("image header key: %s", key);
	log_debug("image header sign: %s", sign);

	/* save lan ota image info */
	img_hdr_info_parsed->dsn = strdup(dsn);
	img_hdr_info_parsed->version = strdup(ver);
	img_hdr_info_parsed->key = strdup(key);
	img_hdr_info_parsed->sign = strdup(sign);

	json_decref(img_hdr_info_json);
	return 0;

inval_ota_img_hdr_json:
	log_err("invalid image header json format");
	json_decref(img_hdr_info_json);
	return -1;
}

static void lan_ota_free_img_hdr_info(
	struct lan_ota_img_hdr_info *img_hdr_info)
{
	free(img_hdr_info->dsn);
	free(img_hdr_info->version);
	free(img_hdr_info->key);
	free(img_hdr_info->sign);
}

static int lan_ota_verify_img_hdr_info(
	const struct lan_ota_img_info *img_info,
	const struct lan_ota_img_hdr_info *img_hdr_info,
	const char *device_dsn)
{
	/* verify dsn */
	if (strcmp(img_hdr_info->dsn, device_dsn)) {
		/* DSN not consistent */
		log_err("image header dsn %s "
			"is not consistent with dev dsn %s",
			img_hdr_info->dsn, device_dsn);
		return -1;
	}

	/* verify version */
	if (strcmp(img_hdr_info->version, img_info->version)) {
		/* version info not consistent */
		log_err("image header info version %s "
			"is not consistent with image info version %s",
			img_hdr_info->version, img_info->version);
		return -1;
	}

	/* verify key length */
	if (strlen(img_hdr_info->key) != LAN_OTA_KEY_LEN * 2) {
		log_err("the length of key in image header info error: %zu",
			strlen(img_hdr_info->key));
		return -1;
	}

	/* verify sign length */
	if (strlen(img_hdr_info->sign) != SHA256_DIGEST_LENGTH * 2) {
		log_err("the length of sign in image header info error: %zu",
			strlen(img_hdr_info->sign));
		return -1;
	}

	return 0;
}

/*
  * Free the memory of members in ota_info structure
  */
void lan_ota_free_exec_info(
	struct lan_ota_exec_info *ota_info)
{
	ASSERT(ota_info != NULL);

	free(ota_info->ota_type);
	free(ota_info->url);
	free(ota_info->ver);
	free(ota_info->dsn);
	free(ota_info->key);
	free(ota_info->checksum);
}

/* get lan ota exec information */
static int lan_ota_get_exec_info(
	const struct lan_ota_img_info *img_info,
	struct lan_ota_img_hdr_info *img_hdr_info,
	struct lan_ota_exec_info *ota_info)
{
	char *url;
	size_t url_len;
	char *cp;
	char *url_ip;
	size_t port_offset;
	char port_str[16];
	size_t port_len;
	char *url_with_port;

	/* Find the right place in url to insert port */
	url = img_info->url;
	url_len = strlen(url);
	cp = strstr(url, "://");
	if (cp) {
		url_ip = cp + strlen("://");
	} else {
		url_ip = url;
	}

	cp = strchr(url_ip, '/');
	if (cp) {
		port_offset = cp - url;
	} else {
		port_offset = url_len;
	}

	snprintf(port_str, sizeof(port_str), ":%d", img_info->port);
	port_len = strlen(port_str);

	url_with_port = malloc(url_len + port_len + 1);
	if (!url_with_port) {
		log_err("allocate memory for url failed");
		return -1;
	}

	/* insert port info to url */
	memcpy(url_with_port, url, port_offset);
	memcpy(url_with_port + port_offset, port_str, port_len);
	memcpy(url_with_port + port_offset + port_len,
		url + port_offset, url_len - port_offset + 1);

	log_debug("url combine with port: %s", url_with_port);

	ota_info->ota_type = strdup(img_info->type);
	ota_info->url = url_with_port;
	ota_info->ver = strdup(img_info->version);
	ota_info->dsn = strdup(img_hdr_info->dsn);
	ota_info->key = strdup(img_hdr_info->key);
	ota_info->checksum = strdup(img_hdr_info->sign);
	ota_info->size = img_info->size;

	return 0;
}

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
	struct lan_ota_exec_info *ota_info)
{
	struct lan_ota_img_info ota_image_info = { 0 };
	struct lan_ota_img_hdr_info ota_image_hdr_info = { 0 };
	struct crypto_state rsa = { 0 };
	char *img_hdr_str = NULL;
	const char *cp;
	size_t img_hdr_len;
	char decrypted_img_hdr[LAN_OTA_IMAGE_HEADER_SZ];
	const char *img_hdr_crc_str;
	ssize_t decrypted_len;
	size_t img_hdr_json_info_len;
	unsigned short crc16_img_hdr;
	unsigned short crc16_cal;
	int result = -2;
	int rc;

	ASSERT(header != NULL);
	ASSERT(header_len != 0);
	ASSERT(device_dsn != NULL);
	ASSERT(device_pubkey != NULL);

	rc = lan_ota_parse_img_info(header, header_len, &ota_image_info);
	if (rc) {
		log_err("parse lan ota image info error");
		goto header_proc_exit;
	}
	/* Decode base64 encoded image header */
	img_hdr_str = base64_decode(ota_image_info.header,
	    strlen(ota_image_info.header), &img_hdr_len);
	if (!img_hdr_str) {
		log_err("base64 decode of image header failed");
		goto header_proc_exit;
	}
	/* Verify image header is 256B */
	if (img_hdr_len != LAN_OTA_IMAGE_HEADER_SZ) {
		log_err("image header len: %zuB, expected %uB",
		    img_hdr_len, LAN_OTA_IMAGE_HEADER_SZ);
		goto header_proc_exit;
	}
	/* RSA decrypt image header */
	if (crypto_init_rsa(&rsa, RSA_KEY_PUBLIC, device_pubkey) < 0) {
		log_err("RSA decryption init failed");
		goto header_proc_exit;
	}
	decrypted_len = crypto_decrypt(&rsa, img_hdr_str, img_hdr_len,
	    decrypted_img_hdr, sizeof(decrypted_img_hdr));
	crypto_cleanup(&rsa);
	if (decrypted_len <= 0) {
		log_err("image header decryption failed");
		goto header_proc_exit;
	}

	/* Check image header format: <JSON header> || '\0' || CRC16 */
	cp = (char *)memchr(decrypted_img_hdr, '\0', decrypted_len);
	if (cp == NULL) {
		log_err("image header missing delimiter, decrypted len %zd",
		    decrypted_len);
		goto header_proc_exit;
	}
	img_hdr_json_info_len = cp - decrypted_img_hdr;
	if (decrypted_len != img_hdr_json_info_len + 1 + 2) {
		log_err("image header format error, "
		    "decrypted len %zd, json info len %zu",
		    decrypted_len, img_hdr_json_info_len);
		goto header_proc_exit;
	}
	img_hdr_crc_str = cp + 1;

	/* Verify checksum on plaintext of header H = J || 0x00 */
	crc16_img_hdr = ntohs(*(unsigned short *)img_hdr_crc_str);
	crc16_cal = crc16(decrypted_img_hdr, img_hdr_json_info_len + 1,
	    CRC16_INIT);
	if (crc16_cal != crc16_img_hdr) {
		log_err("crc16 mismatch, calculated: 0x%04x, "
		    "image header crc16:0x%04x", crc16_cal, crc16_img_hdr);
		goto header_proc_exit;
	}

	/* Parse and save json info in image header */
	rc = lan_ota_parse_img_hdr_info(decrypted_img_hdr,
		&ota_image_hdr_info);
	if (rc) {
		log_err("parse lan ota image header error");
		goto header_proc_exit;
	}

	/* Verify dsn, version */
	rc = lan_ota_verify_img_hdr_info(&ota_image_info,
		&ota_image_hdr_info, device_dsn);
	if (rc) {
		log_err("verify lan ota image header error");
		goto header_proc_exit;
	}

	log_debug("decrypt image info success");

	if (ota_info) {
		rc = lan_ota_get_exec_info(&ota_image_info,
			&ota_image_hdr_info, ota_info);
		if (rc) {
			log_err("get exec info failed");
			result = -1;
			goto header_proc_exit;
		}
	}
	result = 0;

header_proc_exit:
	free(img_hdr_str);
	lan_ota_free_img_info(&ota_image_info);
	lan_ota_free_img_hdr_info(&ota_image_hdr_info);
	return result;
}

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
	const char *dsn)
{
	ASSERT(lan_dec_param != NULL);
	ASSERT(total_size != 0);
	ASSERT(key != NULL);
	ASSERT(dsn != NULL);

	if (total_size & (AES256_BLK_SIZE - 1)) {
		log_err("decrypt size must be a multiple of %u",
		    AES256_BLK_SIZE);
		return -1;
	}

	lan_dec_param->total_size = total_size;
	lan_dec_param->error_occurred = false;
	lan_dec_param->total_decrypted_len = 0;
	lan_dec_param->padding_len = 0;
	lan_dec_param->aes_remain_len = 0;
	SHA256_Init(&(lan_dec_param->stx));

	/* init lan ota aes decrypt key */
	if (hex_parse(lan_dec_param->img_enc_key,
		sizeof(lan_dec_param->img_enc_key),
		key, NULL) < sizeof(lan_dec_param->img_enc_key)) {
		log_err("key must be %zu bytes",
		    sizeof(lan_dec_param->img_enc_key));
		return -1;
	}

	/* init the AES context using the DSN as the initialization vector */
	if (crypto_init_aes(&lan_dec_param->aes, (const u8 *)dsn,
	    lan_dec_param->img_enc_key,
	    sizeof(lan_dec_param->img_enc_key)) < 0) {
		log_err("AES init failed");
		return -1;
	}
	return 0;
}

/*
 * Free resources associated with lan_ota_dec_param structure.
 * In params:
 *     lan_dec_param: structure of ota decrytion params
 */
void lan_ota_img_decrypt_cleanup(struct lan_ota_dec_param *lan_dec_param)
{
	if (!lan_dec_param) {
		return;
	}
	crypto_cleanup(&lan_dec_param->aes);
}

/* check padding of image, and subtract padding length */
static int lan_ota_dec_check_padding(
	struct lan_ota_dec_param *lan_dec_param,
	const char *decrypted_buf,
	size_t decrypted_len,
	size_t *sha_cal_len)
{
	u8 padding_len;

	*sha_cal_len = decrypted_len;
	/* total decrypted len has not reached the last block */
	if (lan_dec_param->total_decrypted_len < lan_dec_param->total_size) {
		return 0;
	}

	/* total decrypted len exceeds total size, something go wrong */
	if (lan_dec_param->total_decrypted_len > lan_dec_param->total_size) {
		log_err("image size has exceeded, "
			"whole size: %zu, expected size: %zu",
			lan_dec_param->total_decrypted_len,
			lan_dec_param->total_size);
		return -1;
	}

	/* in last block */
	padding_len = (u8)(decrypted_buf[decrypted_len - 1]);
	if (padding_len > AES256_BLK_SIZE) {
		log_err("padding length error: %hhu", padding_len);
		return -1;
	}

	/* if padding length is error, signature check will fail */
	/* PKCS#7 PADDING, the value of padding in last block
		is same as padding length */
	lan_dec_param->padding_len = padding_len;

	log_debug("decrypted image: %zu bytes with %hhu pad bytes",
	    lan_dec_param->total_decrypted_len - padding_len, padding_len);
	*sha_cal_len -= padding_len;
	return 0;
}

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
	size_t *out_dec_len)
{
	size_t total_len;
	size_t decrypt_len;
	size_t remain_len;
	size_t buf_len_consumed;
	size_t sha_cal_len = 0;
	char *decrypt_buf = NULL;

	ASSERT(lan_dec_param != NULL);
	ASSERT(lan_dec_param->total_size != 0);
	ASSERT(buffer != NULL);
	ASSERT(out_dec_buf != NULL);
	ASSERT(out_dec_len != NULL);

	*out_dec_buf = NULL;
	*out_dec_len = 0;

	if (lan_dec_param->error_occurred) {
		return -1;
	}
	if (lan_dec_param->total_decrypted_len + buf_len >
	    lan_dec_param->total_size) {
		log_err("%zu byte decrypt exceeded total length: %zu", buf_len,
		    lan_dec_param->total_size);
		goto dec_lan_ota_img_err;
	}

	/* decrypt size should be multiples of AES256_BLK_SIZE */
	total_len = buf_len + lan_dec_param->aes_remain_len;
	remain_len = total_len & (AES256_BLK_SIZE - 1);
	decrypt_len = total_len - remain_len;

	/* calculate content that can be decrypted
		and remain length of content that
		needs to be decrypted next time */
	if (decrypt_len) {
		log_debug2("decrypt: %zu bytes, %zu remaining",
		    decrypt_len, lan_dec_param->total_size -
		    lan_dec_param->total_decrypted_len - decrypt_len);
		decrypt_buf = malloc(decrypt_len);
		if (!decrypt_buf) {
			log_err("malloc current proc buf error");
			goto dec_lan_ota_img_err;
		}

		/* concatenate remain image content in last get process
			with image content in current get process */
		memcpy(decrypt_buf, lan_dec_param->aes_remain_buf,
			lan_dec_param->aes_remain_len);
		buf_len_consumed = decrypt_len - lan_dec_param->aes_remain_len;
		memcpy(decrypt_buf + lan_dec_param->aes_remain_len,
			buffer, buf_len_consumed);
		/* backup the remaining data for the next processing step */
		memcpy(lan_dec_param->aes_remain_buf, buffer + buf_len_consumed,
			remain_len);
		lan_dec_param->aes_remain_len = remain_len;

		/* use AES key and IV (DSN | 0x00) to decrypt image content */
		if (crypto_decrypt(&lan_dec_param->aes, decrypt_buf,
		    decrypt_len, decrypt_buf, decrypt_len) != decrypt_len) {
			log_err("failed to decrypt %zu bytes", decrypt_len);
			goto dec_lan_ota_img_err;
		}
		lan_dec_param->total_decrypted_len += decrypt_len;

		/* check for valid PKCS#7 padding */
		if (lan_ota_dec_check_padding(lan_dec_param,
		    decrypt_buf, decrypt_len, &sha_cal_len) < 0) {
			log_err("lan ota image check padding error");
			goto dec_lan_ota_img_err;
		}

		/* calculate sha256 of decrypted image content */
		SHA256_Update(&(lan_dec_param->stx), decrypt_buf, sha_cal_len);
	} else if (remain_len > lan_dec_param->aes_remain_len) {
		/* not enough data to decrypt, so just add to overflow buffer */
		memcpy(lan_dec_param->aes_remain_buf +
		    lan_dec_param->aes_remain_len, buffer,
		    remain_len - lan_dec_param->aes_remain_len);
		lan_dec_param->aes_remain_len = remain_len;
	}
	*out_dec_buf = decrypt_buf;
	*out_dec_len = sha_cal_len;
	return 0;

dec_lan_ota_img_err:
	lan_dec_param->error_occurred = true;
	free(decrypt_buf);
	return -1;
}

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
int lan_ota_sha256_get(SHA256_CTX *ctx, void *buf, size_t len)
{
	unsigned char digest[SHA256_DIGEST_LENGTH];
	size_t off;
	int i;

	ASSERT(ctx != NULL);
	ASSERT(buf != NULL);

	if (SHA256_Final(digest, ctx) != 1) {
		log_err("lan ota sha256 final failed");
		return -1;
	}
	for (off = 0, i = 0; i < sizeof(digest); i++) {
		if (off >= len) {
			log_err("lan ota sha256 buffer too short");
			return -1;
		}
		off += snprintf(buf + off, len - off, "%2.2x", digest[i]);
	}
	return 0;
}
