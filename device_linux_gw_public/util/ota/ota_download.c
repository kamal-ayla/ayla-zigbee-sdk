/*
 * Copyright 2014-2018 Ayla Networks, Inc.  All rights reserved.
 */
#define _GNU_SOURCE	/* for asprintf() */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <ctype.h>
#include <openssl/md5.h>
#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/timer.h>
#include <ayla/http.h>
#include <ayla/assert.h>
#include <ayla/lan_ota.h>
#include <platform/ota.h>
#include "ota_update.h"

static struct curl_slist *dl_curl_headers;
static char dl_write_err;
static MD5_CTX dl_md5;
static size_t dl_xfer_len;
/* time delay(seconds) to retry download, use dl_delay_index
 * to control delay time increase. If download fails,
 * dl_delay_index will increase, when it increase to max value,
 * it will not increase. If download success,
 * dl_delay_index will reset to 0
 */
static const unsigned int dl_time_delay[] = {0, 10, 30, 60, 90, 120, 150, 180};
static int dl_delay_index;
/* lan ota decrypt params */
static struct lan_ota_dec_param dl_lan_dec_param;

int dl_download_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	return 0;
}

/*
 * Get status code from curl, or return -1 on error.
 */
static long dl_curl_status(CURL *curl)
{
	long status = -1;
	CURLcode ccode;

	ccode = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (ccode != CURLE_OK) {
		log_warn("download get status error %u %s",
		    ccode, curl_easy_strerror(ccode));
		status = -1;
	}
	return status;
}

#ifdef DL_CURL_DEBUG
static int dl_is_binary(void *buf, size_t size)
{
	char *bp;

	for (bp = buf; bp < (char *)buf + size; bp++) {
		if (isprint(*bp) || *bp == '\n' || *bp == '\r') {
			continue;
		}
		return 1;
	}
	return 0;
}
#endif /* DL_CURL_DEBUG */

/* Curl write function for regular ota */
static size_t dl_curl_recv_body(void *buf, size_t size, size_t nmemb, void *arg)
{
	CURL *curl = arg;
	long status;
	ssize_t wlen;

	size *= nmemb;
	status = dl_curl_status(curl);

#ifdef DL_CURL_DEBUG
	if (debug) {
		log_info("recv %zu bytes", size);
		if ((status == HTTP_STATUS_OK
			|| status == HTTP_STATUS_PAR_CONTENT)
			|| dl_is_binary(buf, size)) {
			log_debug_hex("body", buf, size);
		} else {
			fwrite(buf, size, 1, stderr);
		}
	}
#endif /* DL_CURL_DEBUG */

	/* When download using range, server will return status 206 */
	if (!(status == HTTP_STATUS_OK || status == HTTP_STATUS_PAR_CONTENT)
		|| dl_write_err) {
		/* Return value other than size to indicate an error */
		log_err("receive data error: status %ld, dl_write_err %c",
			status, dl_write_err);
		return 0;
	}

	wlen = platform_ota_flash_write(buf, size);
	if (wlen != size) {
		dl_write_err = 1;
		return 0;
	}
	MD5_Update(&dl_md5, buf, wlen);
	dl_xfer_len += wlen;
	return size;
}

/* Curl write function for lan ota */
static size_t dl_lan_curl_recv_body(void *buf, size_t size,
	size_t nmemb, void *arg)
{
	CURL *curl = arg;
	long status;
	ssize_t wlen;
	char *dec_buf;
	size_t dec_len;
	int rc;

	size *= nmemb;
	status = dl_curl_status(curl);

#ifdef DL_CURL_DEBUG
	if (debug) {
		log_info("recv %zu bytes", size);
		if ((status == HTTP_STATUS_OK
			|| status == HTTP_STATUS_PAR_CONTENT)
			|| dl_is_binary(buf, size)) {
			log_debug_hex("body", buf, size);
		} else {
			fwrite(buf, size, 1, stderr);
		}
	}
#endif /* DL_CURL_DEBUG */

	/* When download using range, server will return status 206 */
	if (!(status == HTTP_STATUS_OK || status == HTTP_STATUS_PAR_CONTENT)
		|| dl_write_err) {
		/* Return value other than size to indicate an error */
		log_err("receive data error: status %ld, dl_write_err %c",
			status, dl_write_err);
		return 0;
	}

	dl_xfer_len += size;

	/* decrypt received image data for lan ota */
	rc = lan_ota_img_decrypt_data(&dl_lan_dec_param,
		buf, size, &dec_buf, &dec_len);
	if (rc) {
		log_err("error occurs when decrypting lan ota image data");
		dl_write_err = 1;
		return 0;
	}

	wlen = platform_ota_flash_write(dec_buf, dec_len);
	if (wlen != dec_len) {
		log_err("error occurs when writing lan ota image data, "
			"write len: %zd, decrypt len: %zu",
			wlen, dec_len);
		free(dec_buf);
		dl_write_err = 1;
		return 0;
	}

	free(dec_buf);

	return size;
}

/*
 * Curl write function that ignores any incoming data.
 */
static size_t dl_curl_recv_dump(void *buf, size_t size, size_t nmemb, void *arg)
{
	return size * nmemb;
}

/*
 * Debug info callback from curl.
 * See CURLOPT_DEBUGFUNCTION secton in curl_easy_setopt(3);
 * buf is not NUL-terminated
 */
static int dl_curl_debug(CURL *curl, curl_infotype info,
			char *buf, size_t len, void *arg)
{
	char str[200];

	switch (info) {
	case CURLINFO_TEXT:
	case CURLINFO_HEADER_IN:
	case CURLINFO_HEADER_OUT:
		if (len > sizeof(str) - 1) {
			len = sizeof(str) - 1;
		}
		memcpy(str, buf, len);
		str[len] = '\0';
		if (len > 0 && str[len - 1] == '\n') {
			str[len - 1] = '\0';
		}
		log_debug("%s", str);
		break;
	default:
		break;
	}
	return 0;
}

/*
 * Do HTTP GET of URL.
 * Support resume broken downloads.
 */
static int dl_curl(struct ota_download_param *ota_param,
	unsigned long offset)
{
	CURL *curl;
	CURLcode ccode;
	long status;
	int rc = 0;
	char range_str[64];

	curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	curl_easy_setopt(curl, CURLOPT_URL, ota_param->url);
	curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ayla/ssl/certs/cert.pem");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, dl_curl_headers);
	if (!ota_param->lan_connect) {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
			dl_curl_recv_body);
	} else {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
			dl_lan_curl_recv_body);
	}
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, curl);

	if (offset != 0) {
		log_debug("downloading from offset %lu", offset);
		snprintf(range_str, sizeof(range_str), "%lu-", offset);
		curl_easy_setopt(curl, CURLOPT_RANGE, range_str);
	}

	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 300);

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

	if (debug) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, dl_curl_debug);
	}

	log_info("downloading %s", ota_param->url);

	ccode = curl_easy_perform(curl);
	if (ccode != CURLE_OK) {
		log_warn("download curl error %u %s",
		    ccode, curl_easy_strerror(ccode));
		rc = -1;
		goto out;
	}

	status = dl_curl_status(curl);
	if (status == HTTP_STATUS_OK || status == HTTP_STATUS_PAR_CONTENT) {
		log_info("status %ld", status);
	} else {
		log_err("status %ld", status);
	}
out:
	log_debug("downloading from offset %lu finish, transfer len:%zu",
		offset, dl_xfer_len);
	curl_easy_cleanup(curl);
	return rc;
}

void dl_add_header(const char *header)
{
	dl_curl_headers = curl_slist_append(dl_curl_headers, header);
}

static int dl_md5_get(MD5_CTX *ctx, void *buf, size_t len)
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	size_t off;
	int i;

	if (MD5_Final(digest, ctx) != 1) {
		log_err("md5 final failed");
		return -1;
	}
	for (off = 0, i = 0; i < sizeof(digest); i++) {
		if (off >= len) {
			log_err("buffer too short");
			return -1;
		}
		off += snprintf(buf + off, len - off, "%2.2x", digest[i]);
	}
	return 0;
}

static int dl_readback(struct ota_download_param *ota_param,
	struct lan_ota_dec_param *lan_dec_param)
{
	char buf[8 * 1024];
	size_t total_len;
	ssize_t len;
	size_t off;
	size_t tlen;
	int status;

	dl_xfer_len = 0;
	if (!ota_param->lan_connect) {
		if (MD5_Init(&dl_md5) != 1) {
			log_err("md5 init failed");
			return -1;
		}
		total_len = ota_param->total_size;
	} else {
		if (SHA256_Init(&(lan_dec_param->stx)) != 1) {
			log_err("sha256 init failed");
			return -1;
		}
		total_len = ota_param->total_size - lan_dec_param->padding_len;
	}

	if (platform_ota_flash_read_open()) {
		return -1;
	}

	for (off = 0; off < total_len; off += len) {
		tlen = sizeof(buf);
		if (tlen + off > total_len) {
			tlen = total_len - off;
		}
		len = platform_ota_flash_read(buf, tlen, off);
		if (len < 0) {
			log_err("read error at offset 0x%zx", dl_xfer_len);
			return -1;
		}
		if (!ota_param->lan_connect) {
			MD5_Update(&dl_md5, buf, len);
		} else {
			SHA256_Update(&(lan_dec_param->stx), buf, len);
		}
	}
	status = platform_ota_flash_close();
	if (status) {
		log_err("flash close err %d", status);
		return -1;
	}
	return 0;
}

static int dl_verify_md5(char *md5, char *step)
{
	char md5_read[33];

	if (!md5) {
		log_warn("no md5 provided");
		return -1;
	}
	if (dl_md5_get(&dl_md5, md5_read, sizeof(md5_read))) {
		return -1;
	}
	if (strcasecmp(md5, md5_read)) {
		log_err("md5 mismatch after %s", step);
		log_debug("read     %s", md5_read);
		log_debug("expected %s", md5);
		return -1;
	}
	return 0;
}

static int dl_lan_verify_sha256(
	struct lan_ota_dec_param *lan_dec_param,
	char *sign, char *step)
{
	char sha256_read[SHA256_DIGEST_HEXSTR_LEN + 1];

	if (!sign) {
		log_warn("no sha256 signature provided");
		return -1;
	}
	if (lan_ota_sha256_get(&(lan_dec_param->stx),
		sha256_read, sizeof(sha256_read))) {
		return -1;
	}
	if (strcasecmp(sign, sha256_read)) {
		log_err("sha256 mismatch after %s", step);
		log_info("read     %s", sha256_read);
		log_info("expected %s", sign);
		return -1;
	}
	return 0;
}

/*
  * Verify checksum(md5) for regular ota
  * In params:
  *     ota_param: dowload param structure for regular ota
  *         member lan_connect in structure need to be false
  * Return value:
  *     0: success
  *     -1: error
  */
int dl_verify(struct ota_download_param *ota_param)
{
	ASSERT(ota_param != NULL);
	ASSERT(ota_param->lan_connect == false);

	if (dl_xfer_len != ota_param->total_size) {
		log_err("downloaded %zu expected %zu",
		    dl_xfer_len, ota_param->total_size);
		return -1;
	}
	if (dl_verify_md5(ota_param->checksum, "write")) {
		return -1;
	}
	if (dl_readback(ota_param, NULL)) {
		return -1;
	}
	if (dl_verify_md5(ota_param->checksum, "readback")) {
		return -1;
	}
	log_info("verified readback is correct");
	return 0;
}

/*
  * Verify checksum(sh256) for lan ota
  * In params:
  *     ota_param: dowload param structure for lan ota
  *         member lan_connect in structure need to be true
  * Return value:
  *     0: success
  *     -1: error
  */
int dl_lan_verify(struct ota_download_param *ota_param)
{
	struct lan_ota_dec_param *lan_dec_param = &dl_lan_dec_param;

	ASSERT(ota_param != NULL);
	ASSERT(ota_param->lan_connect != false);

	/* verify signature of lan ota image that has been downloaded */
	if (lan_dec_param->total_decrypted_len != ota_param->total_size) {
		log_err("downloaded %zu, decrypted image len %zu, "
			"expected %zu",
			dl_xfer_len, lan_dec_param->total_decrypted_len,
			ota_param->total_size);
		return -1;
	}
	/* Verify image length matches expected length */
	if (lan_dec_param->padding_len != ota_param->padding_len) {
		log_err("unexpected image size: %zu bytes,"
		    " expected %zu bytes",
		    lan_dec_param->total_size - lan_dec_param->padding_len,
		    ota_param->total_size - ota_param->padding_len);
		return -1;
	}
	if (dl_lan_verify_sha256(lan_dec_param,
		ota_param->checksum, "write")) {
		log_err("verified sha256 write error");
		return -1;
	}
	if (dl_readback(ota_param, lan_dec_param)) {
		log_err("readback error");
		return -1;
	}
	if (dl_lan_verify_sha256(lan_dec_param,
		ota_param->checksum, "readback")) {
		log_err("verified sha256 readback error");
		return -1;
	}
	log_info("verified readback is correct");
	return 0;
}

/* Download ota image. If download fails,
 * it will retry to download from break point,
 * and retry several times which is config by user
 */
static int dl_download_with_retry(struct ota_download_param *ota_param)
{
	int i;
	int rc = 0;
	unsigned int delay_array_size;
	unsigned int delay_time = 0;
	unsigned long offset = 0;

	delay_array_size = ARRAY_LEN(dl_time_delay);

	/* If download fail, retry to download frombreak point */
	for (i = 0; i <= ota_param->retry_times; i++) {
		log_debug("ota download %d times, "
			"offset:%lu, total:%zu",
			i + 1, offset,
			ota_param->total_size);
		rc = dl_curl(ota_param, offset);
		if (dl_write_err) {
			log_err("write error, curl status:%d, "
				"offset:%lu, total:%zu",
				rc, offset,
				ota_param->total_size);
			if (rc == 0) {
				rc = -1;
			}
			break;
		}

		if (rc == 0) {
			dl_delay_index = 0;
			log_debug("download sucess, "
				"offset:%lu, total:%zu",
				offset,
				ota_param->total_size);
			break;
		}

		if (dl_xfer_len > ota_param->total_size) {
			log_err("download len error, "
				"transfer len:%zu, offset:%lu, "
				"total:%zu",
				dl_xfer_len, offset,
				ota_param->total_size);
			break;
		}

		/* resume from offset that download failed */
		offset = dl_xfer_len;

		/* wait several seconds before retrying the op */
		delay_time = dl_time_delay[dl_delay_index];
		log_debug("ota download failed, "
			"wait for %u seconds to retry, "
			"offset:%lu, total:%zu",
			delay_time, offset,
			ota_param->total_size);
		if (delay_time != 0) {
			sleep(delay_time);
		}
		if (dl_delay_index < delay_array_size - 1) {
			dl_delay_index++;
		}
	}

	return rc;
}

static int dl_down_init(struct ota_download_param *ota_param)
{
	if (!ota_param->lan_connect) {
		if (MD5_Init(&dl_md5) != 1) {
			log_err("download: md5 init failed");
			return -1;
		}
		return 0;
	}

	return lan_ota_img_decrypt_init(&dl_lan_dec_param,
	    ota_param->total_size, ota_param->key, ota_param->dsn);
}

static void dl_down_cleanup(struct ota_download_param *ota_param)
{
	if (ota_param->lan_connect) {
		lan_ota_img_decrypt_cleanup(&dl_lan_dec_param);
	}
}

int dl_download(struct ota_download_param *ota_param)
{
	int rc;
	int status;

	if (dl_down_init(ota_param) != 0) {
		log_err("download: init down param failed");
		return -1;
	}
	if (platform_ota_flash_write_open()) {
		log_err("flash write open failed");
		rc = -1;
		goto out;
	}

	rc = dl_download_with_retry(ota_param);
	if (rc) {
		log_err("download failed");
		platform_ota_flash_close();
		goto out;
	}
	status = platform_ota_flash_close();
	if (status) {
		log_err("write to flash cmd exit status 0x%x", status);
		rc = -1;
	}
	dl_down_cleanup(ota_param);
	if (dl_write_err) {
		log_err("write error during download");
		rc = -1;
		goto out;
	}
out:
	return rc;
}

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
	enum patch_state status)
{
	size_t url_len;
	char *cp;
	const char *url_ip;
	size_t param_offset;
	char *url_without_param;
	int ota_status_url_len;
	char *ota_status_url;
	CURL *curl;
	CURLcode ccode;
	long curl_status;
	int rc = 0;

	ASSERT(url != NULL);

	curl = curl_easy_init();
	if (!curl) {
		log_err("curl init failed");
		return -1;
	}

	/* generate url without parameters */
	url_len = strlen(url);
	cp = strstr(url, "://");
	if (cp) {
		url_ip = cp + strlen("://");
	} else {
		url_ip = url;
	}

	cp = strchr(url_ip, '/');
	if (cp) {
		param_offset = cp - url;
	} else {
		param_offset = url_len;
	}

	url_without_param = malloc(param_offset + 1);
	if (!url_without_param) {
		log_err("alloc memory for url without param failed");
		return -1;
	}
	memcpy(url_without_param, url, param_offset);
	*(url_without_param + param_offset) = 0;

	ota_status_url_len = asprintf(&ota_status_url,
	    "%s/ota_status.json?status=%u&err=%u",
	    url_without_param, HTTP_STATUS_OK, status);
	free(url_without_param);
	if (ota_status_url_len == -1) {
		log_err("alloc memory for lan ota status url failed");
		return -1;
	}

	curl_easy_setopt(curl, CURLOPT_URL, ota_status_url);
	curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ayla/ssl/certs/cert.pem");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_curl_recv_dump);

	/* set to PUT */
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);

	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 300);

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

	if (debug) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, dl_curl_debug);
	}

	log_debug("sending lan ota status %u to %s", status, ota_status_url);

	ccode = curl_easy_perform(curl);
	if (ccode != CURLE_OK) {
		log_err("download curl error %u %s",
		    ccode, curl_easy_strerror(ccode));
		rc = -1;
		goto status_out;
	}

	curl_status = dl_curl_status(curl);
	if (curl_status != HTTP_STATUS_OK) {
		log_err("error status %ld", curl_status);
		rc = -1;
	}
status_out:
	curl_easy_cleanup(curl);
	free(ota_status_url);
	return rc;
}
