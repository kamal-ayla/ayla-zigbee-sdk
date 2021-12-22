/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <jansson.h>
#include <curl/curl.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/build.h>
#include <ayla/buffer.h>
#include <ayla/time_utils.h>
#include <ayla/str_utils.h>
#include <ayla/nameval.h>
#include <ayla/base64.h>
#include <ayla/crypto.h>
#include <ayla/http.h>
#include <ayla/http_client.h>

#include "ds.h"
#include "dapi.h"
#include "ds_client.h"

#define DS_CLIENT_REQ_MAX_TIMEOUT	1200	/* Absolute max time (20 min) */

#define DS_CLIENT_ADS_UNAUTH_HOLDOFF	60	/* Holdoff after unauth fail */
#define DS_CLIENT_ADS_OVERLOAD_HOLDOFF	60	/* Holdoff after overload */

#define DS_CLIENT_MAX_AUTH_ERRORS	3	/* Retries after 401 response */

#define DS_CLIENT_AUTH_MAX_LEN		400	/* Ayla auth key buf size */
#define DS_CLIENT_CONTENT_TYPE_MAX_LEN	128	/* HTTP hdr 127 bytes + '\0' */

static DEF_NAME_TABLE(ds_client_proto_names, DS_CLIENT_PROTOS);
static DEF_NAMEVAL_TABLE(http_content_type_table, HTTP_CONTENT_TYPES);


/*
 * Load a variable for variable expansion with the specified value.
 */
static ssize_t ds_client_load_var(char *buf, size_t buf_size, const char *var,
	const char *val)
{
	size_t len;

	if (!val || val[0] == '\0') {
		log_err("$%s is not set", var);
		return -1;
	}
	len = snprintf(buf, buf_size, "%s", val);
	if (len >= buf_size) {
		return -1;
	}
	return len;
}

/*
 * Load variable: $DEV_KEY
 */
static ssize_t ds_client_load_dev_key(char *buf, size_t buf_size,
	const char *var, void *arg)
{
	return ds_client_load_var(buf, buf_size, var, device.key);
}

/*
 * Load variable: $DSN
 */
static ssize_t ds_client_load_dsn(char *buf, size_t buf_size,
	const char *var, void *arg)
{
	return ds_client_load_var(buf, buf_size, var, device.dsn);
}

/*
 * Load variable: $ADS_HOST
 */
static ssize_t ds_client_load_ads_host(char *buf, size_t buf_size,
	const char *var, void *arg)
{
	return ds_client_load_var(buf, buf_size, var, device.ads_host);
}

/*
 * Load a user-defined variable.
 */
static ssize_t ds_client_load_user_defined_var(char *buf, size_t buf_size,
	const char *var, void *arg)
{
	const struct ds_client_req_info *info =
	    (const struct ds_client_req_info *)arg;
	unsigned var_num = strtoul(var, NULL, 10);

	/* Custom variables start at $1 and cannot exceed url_vars size */
	ASSERT(var_num != 0 && var_num <= DS_URL_VAR_COUNT);

	return ds_client_load_var(buf, buf_size, var,
	    info->url_vars[var_num - 1]);
}

/*
 * Table of variables that may be used in a client request link.
 */
const struct str_var ds_client_url_var_table[] = {
	STR_VAR_DECL(DEV_KEY, ds_client_load_dev_key)
	STR_VAR_DECL(DSN, ds_client_load_dsn)
	STR_VAR_DECL(ADS_HOST, ds_client_load_ads_host)
	STR_VAR_DECL(1, ds_client_load_user_defined_var)
	STR_VAR_DECL(2, ds_client_load_user_defined_var)
	STR_VAR_DECL(3, ds_client_load_user_defined_var)
	STR_VAR_DECL(4, ds_client_load_user_defined_var)
	STR_VAR_DECL(5, ds_client_load_user_defined_var)
	STR_VAR_END
};

/*
 * Parse Content-Type header field.  Extracts the first semi-colon delimited
 * field and attempts to match it with known content types.
 */
static enum http_content_type ds_client_parse_content_type(
	const char *content_type)
{
	char buf[DS_CLIENT_CONTENT_TYPE_MAX_LEN];
	const char *str;
	char *cp;
	int rc;

	/* Parse only the first field of the Content-Type header */
	cp = strchr(content_type, ';');
	if (cp) {
		memcpy(buf, content_type, cp - content_type);
		buf[cp - content_type] = '\0';
		str = buf;
	} else {
		str = content_type;
	}
	rc = lookup_by_name(http_content_type_table, str);
	if (rc < 0) {
		return HTTP_CONTENT_UNKNOWN;
	}
	return rc;
}

/*
 * Parse the Ayla-specific server time field that is returned with
 * certain requests.
 */
static void ds_client_parse_server_time(int argc, char **argv, void *arg)
{
	struct ds_client *client = (struct ds_client *)arg;
	char *errptr;
	unsigned long time;

	if (argc < 1) {
		return;
	}
	if (!client->req.ayla_auth) {
		/* Only apply server time for Ayla authenticated requests */
		return;
	}
	time = strtoul(argv[0], &errptr, 10);
	if (*errptr != '\0') {
		log_warn("invalid server time format: %s", argv[0]);
		return;
	}
	log_debug("received Ayla server-time: %lu", time);
	ds_clock_set((time_t)time, CS_SERVER);
}

/*
 * Parse the Ayla-specific authentication token field assigned to this device.
 */
static void ds_client_parse_auth_token(int argc, char **argv, void *arg)
{
	struct device_state *dev = &device;

	if (argc < 1 || !strlen(argv[0])) {
		return;
	}
	log_debug("received Ayla auth token: %d bytes", strlen(argv[0]));
	snprintf(dev->auth_header, sizeof(dev->auth_header), "%s %s",
	    ADS_AUTH_VERS, argv[0]);
}

/*
 * Custom header parsers for ADS requests.
 */
const struct http_tag ds_client_auth_header_tags[] = {
	{ ADS_TIME_FIELD,		ds_client_parse_server_time },
	{ ADS_TEMP_AUTH_FIELD,		ds_client_parse_auth_token },
	{ NULL }
};

/*
 * Return an allocated string with a the client authentication key for the
 * specified request.
 */
static char *ds_client_auth_gen(struct device_state *dev,
	enum http_method method, const char *uri)
{
	time_t t;
	char buf[DS_CLIENT_AUTH_MAX_LEN];
	char rsa_buf[512];	/* Space for encrypt with up to 4096B key */
	size_t len;
	ssize_t rsa_len;
	char *output;
	struct crypto_state rsa = { 0 };

	t = time(NULL);
	if (t < CLOCK_START) {
		t = CLOCK_START;
	}
	len = snprintf(buf, sizeof(buf), "%lu %s /%s\n", (unsigned long)t,
	    http_method_names[method], uri);
	if (len >= sizeof(buf)) {
		log_err("%zu byte input buffer is too small", sizeof(buf));
		return NULL;
	}
	/* Encrypt the data using RSA */
	if (crypto_init_rsa(&rsa, RSA_KEY_PUBLIC, dev->pub_key) < 0) {
		return NULL;
	}
	rsa_len = crypto_encrypt(&rsa, buf, len, rsa_buf, sizeof(rsa_buf));
	crypto_cleanup(&rsa);
	if (rsa_len <= 0) {
		log_err("client auth key encryption failed");
		return NULL;
	}
	/* Encode the OEM key in base64 */
	output = base64_encode(rsa_buf, rsa_len, NULL);
	if (!output) {
		log_err("base64 encoding failed");
		return NULL;
	}
	return output;
}

/*
 * Add the Ayla-specific authentication header required to make a request to
 * ADS.
 */
static int ds_client_add_auth_header(struct ds_client *client,
	enum http_method method, const char *url)
{
	struct device_state *dev = &device;
	char buf[DS_CLIENT_LINK_MAX_LEN];
	char *auth_header;
	char *param;
	int i;
	int rc;

	ASSERT(!ds_client_busy(client));

	/* Add authentication token received from ADS */
	if (dev->auth_header[0] != '\0') {
		if (http_client_add_header(client->context,
		    ADS_TEMP_AUTH_FIELD, "%s", dev->auth_header) < 0) {
			return -1;
		}
		return 0;
	}
	/* No auth token available, so generate an auth key for this request */
	snprintf(buf, sizeof(buf), "%s", url);
	/* Extract base URI to use for client authentication key */
	param = strchr(buf, '?');
	if (param) {
		*param = '\0';
	}
	url = buf;
	/* find 3rd occurence of '/' */
	for (i = 0; i < 3; i++) {
		url = strchr(url, '/');
		if (!url) {
			log_err("invalid url: %s", url);
			return -1;
		}
		url++;
	}
	auth_header = ds_client_auth_gen(dev, method, url);
	if (!auth_header) {
		return -1;
	}
	rc = http_client_add_header(client->context, ADS_CLIENT_AUTH_FIELD,
	    "%s %s", ADS_AUTH_VERS, auth_header);
	free(auth_header);
	return rc;
}

/*
 * Update device stats when an ADS request has completed successfully.
 */
static void ds_client_ok_status_handler(void)
{
	struct device_state *dev = &device;

	dev->req_auth_errors = 0;
	dev->conn_mtime = time_mtime_ms();
	dev->conn_time = time(NULL);
}

/*
 * Handle an error from ADS indicating an overload condition.  This
 * might occur if the service is flooded with requests and is rate-limiting
 * or rejecting all new requests.  The best course of action is to immediately
 * halt requests, and attempt to reconnect after a reasonable delay.
 */
static void ds_client_overload_status_handler(void)
{
	log_warn("cloud reported overload condition");
	ds_cloud_failure(DS_CLIENT_ADS_OVERLOAD_HOLDOFF);
}

/*
 * Handle a 401 status.  Return 0 to retry the request, or -1 if the maximum
 * number of failures has been reached.
 */
static int ds_client_unauth_status_handler(void)
{
	struct device_state *dev = &device;

	dev->auth_header[0] = '\0';
	dev->req_auth_errors++;
	if (dev->req_auth_errors >= DS_CLIENT_MAX_AUTH_ERRORS) {
		/* Delay next reconnect attempt for 60s */
		ds_cloud_failure(DS_CLIENT_ADS_UNAUTH_HOLDOFF);
		return -1;
	}
	/* Retry immediately */
	return 0;
}

/*
 * HTTP client read function.  The correct read data source is selected
 * based on the client's request data setup.
 */
static ssize_t ds_client_read(void *dest, size_t size, size_t offset,
	void *arg)
{
	struct ds_client *client = (struct ds_client *)arg;
	struct ds_client_data *data;

	ASSERT(dest != NULL);

	if (!size) {
		return 0;
	}
	data = &client->req.req_data;
	switch (data->type) {
	case DS_DATA_NONE:
		size = 0;
		break;
	case DS_DATA_BUF:
		if (!data->buf.ptr) {
			return -1;
		}
		if (offset >= data->buf.len) {
			return 0;
		}
		if (offset + size > data->buf.len) {
			size = data->buf.len - offset;
		}
		memcpy(dest, (u8 *)data->buf.ptr + offset, size);
		break;
	case DS_DATA_QBUF:
		if (!data->qbuf) {
			return -1;
		}
		size = queue_buf_copyout(data->qbuf, dest, size, offset);
		break;
	case DS_DATA_FILE:
		if (!data->file) {
			return -1;
		}
		if (fseek(data->file, offset, SEEK_SET) < 0) {
			log_err("file seek failed: %m");
			return -1;
		}
		size = fread(dest, 1, size, data->file);
		break;
	}
	return size;
}

/*
 * HTTP client write function.  The correct write data destination is selected
 * based on the client's response data setup.
 */
static ssize_t ds_client_write(const void *src, size_t size,
	size_t offset, void *arg)
{
	struct ds_client *client = (struct ds_client *)arg;
	struct ds_client_data *data;

	ASSERT(client != NULL);
	ASSERT(src != NULL);

	if (!size) {
		return 0;
	}
	data = &client->req.resp_data;
	switch (data->type) {
	case DS_DATA_NONE:
		break;
	case DS_DATA_BUF:
		if (!data->buf.ptr) {
			return -1;
		}
		if (offset > data->buf.len) {
			log_err("sparse buffer writes are not supported");
			return -1;
		}
		if (offset + size > data->buf.capacity) {
			log_err("%zu byte write exceeds %zu byte buffer size",
			    size, data->buf.capacity);
			return -1;
		}
		memcpy((u8 *)data->buf.ptr + offset, src, size);
		data->buf.len = offset + size;
		break;
	case DS_DATA_QBUF:
		if (!data->qbuf) {
			return -1;
		}
		/* Queue buffer requires sequential writes */
		ASSERT(offset == queue_buf_len(data->qbuf));
		if (queue_buf_put(data->qbuf, src, size) < 0) {
			return -1;
		}
		break;
	case DS_DATA_FILE:
		if (!data->file) {
			return -1;
		}
		if (fseek(data->file, offset, SEEK_SET) < 0) {
			log_err("file seek failed: %m");
			return -1;
		}
		if (fwrite(src, 1, size, data->file) != size) {
			log_err("%zu byte file write failed: %m", size);
		}
		break;
	}
	return size;
}

/*
 * HTTP client request complete callback.  This logs request information and
 * updates the device state as needed for ADS requests.
 */
static void ds_client_send_done(enum http_client_err err,
	const struct http_client_req_info *info, void *arg)
{
	struct ds_client *client = (struct ds_client *)arg;
	struct ds_client_req *req = &client->req;
	struct device_state *dev = &device;
	void (*complete)(enum http_client_err,
	    const struct http_client_req_info *, const struct ds_client_data *,
	    void *);
	void *complete_arg;
	struct ds_client_data resp_data = { .type = DS_DATA_NONE };

	/* Log request info and perform generic actions */
	switch (err) {
	case HTTP_CLIENT_ERR_NONE:
		if (HTTP_STATUS_IS_SUCCESS(info->http_status)) {
			log_debug("%s complete, HTTP status %u, time %ums, %s",
			    http_method_names[req->method],
			    info->http_status, info->time_ms, req->link);
			/* Attempt to parse response content type */
			if (info->content_type) {
				req->resp_data.content =
				    ds_client_parse_content_type(
				    info->content_type);
			}
		} else {
			log_warn("%s complete, HTTP status %u, time %ums, %s",
			    http_method_names[req->method],
			    info->http_status, info->time_ms, req->link);
		}
		break;
	case HTTP_CLIENT_ERR_TIMEOUT:
	case HTTP_CLIENT_ERR_CANCELED:
		log_warn("%s %s, time %ums, %s",
		    http_method_names[req->method],
		    http_client_err_string(err), info->time_ms, req->link);
		break;
	default:
		/* FAILED */
		log_warn("%s %s, error %s, %s",
		    http_method_names[req->method],
		    http_client_err_string(err),
		    curl_easy_strerror((CURLcode)info->curl_error), req->link);

		/* Set the update local time flag */
		if ((CURLcode)info->curl_error == CURLE_SSL_CACERT) {
			dev->update_time = 1;
			log_debug("dev->update_time %d", dev->update_time);
		}
		break;
	}
	if (info->sent_bytes || info->received_bytes) {
		log_debug("uploaded %zuB @ %uB/s, downloaded %zuB @ %uB/s",
		    info->sent_bytes, info->upload_speed_bps / 8,
		    info->received_bytes, info->download_speed_bps / 8);
	}
	/* Perform additional management on Ayla-specific requests */
	if (req->ayla_cloud) {
		switch (err) {
		case HTTP_CLIENT_ERR_NONE:
			/* Update local IP address */
			if (!info->local_ip ||
			    !inet_aton(info->local_ip, &dev->lan_ip)) {
				dev->lan_ip.s_addr = 0;
			}
			/* Handle specific HTTP statuses */
			switch (info->http_status) {
			case HTTP_STATUS_UNAUTH:
				if (!req->ayla_auth) {
					break;
				}
				/* Attempt to recover from Ayla auth err */
				if (ds_client_unauth_status_handler() < 0) {
					/* Out of retries */
					log_warn("max auth errors reached");
					break;
				}
				/* Re-send request */
				log_debug("resend request with new auth token");
				ds_client_data_cleanup(&req->resp_data);
				ds_client_send(client, req);
				return;
			case HTTP_STATUS_TOO_MANY_REQUESTS:
			case HTTP_STATUS_UNAVAIL:
			case HTTP_STATUS_GATEWAY_TIMEOUT:
				ds_client_overload_status_handler();
				break;
			default:	/* Handle all other statuses */
				if (HTTP_STATUS_IS_SUCCESS(info->http_status)) {
					/* Update Ayla connection details */
					ds_client_ok_status_handler();
				}
				break;
			}
			break;
		case HTTP_CLIENT_ERR_TIMEOUT:
		case HTTP_CLIENT_ERR_FAILED:
			/* Handle connectivity failure */
			ds_cloud_failure(0);
			break;
		case HTTP_CLIENT_ERR_CANCELED:
			break;
		}
	}
	/* Backup and reset request, in case callback starts a new send */
	complete = req->complete;
	complete_arg = req->complete_arg;
	if (err == HTTP_CLIENT_ERR_NONE) {
		resp_data = req->resp_data;
		req->resp_data.type = DS_DATA_NONE;
		/* Ensure all file data is written before the callback */
		if (resp_data.type == DS_DATA_FILE && resp_data.file) {
			fflush(resp_data.file);
		}
	}
	ds_client_req_reset(req);

	/* Invoke request complete callback and cleanup response data */
	if (complete) {
		complete(err, info, &resp_data, complete_arg);
	}
	ds_client_data_cleanup(&resp_data);
}

/*
 * Return the size in bytes of the client data.
 */
size_t ds_client_data_size(const struct ds_client_data *data)
{
	struct stat st;

	ASSERT(data != NULL);

	switch (data->type) {
	case DS_DATA_NONE:
		break;
	case DS_DATA_BUF:
		return data->buf.len;
	case DS_DATA_QBUF:
		if (!data->qbuf) {
			break;
		}
		return queue_buf_len(data->qbuf);
	case DS_DATA_FILE:
		if (!data->file) {
			break;
		}
		if (fstat(fileno(data->file), &st) < 0) {
			break;
		}
		return st.st_size;
	}
	return 0;
}

/*
 * Cleanup the client data.  The client data structure is merely a handle to
 * the actual data buffer, which is stored somewhere else in memory.  If the
 * delete_on_cleanup flag is set, this function will free the data.  Otherwise,
 * it will just clear the handle to the data, and leave the original data
 * intact.
 */
void ds_client_data_cleanup(struct ds_client_data *data)
{
	ASSERT(data != NULL);

	switch (data->type) {
	case DS_DATA_NONE:
		break;
	case DS_DATA_BUF:
		if (data->delete_on_cleanup) {
			data->buf.len = 0;
			if (data->buf.mallocd) {
				data->buf.mallocd = false;
				free(data->buf.ptr);
				data->buf.ptr = NULL;
				data->buf.capacity = 0;
			}
		}
		break;
	case DS_DATA_QBUF:
		if (data->qbuf && data->delete_on_cleanup) {
			queue_buf_reset(data->qbuf);
		}
		break;
	case DS_DATA_FILE:
		if (data->file) {
			if (data->delete_on_cleanup) {
				/* XXX empty file remains in file system */
				if (ftruncate(fileno(data->file), 0) < 0) {
					log_warn("failed to clear file");
				}
			}
			fclose(data->file);
			data->file = NULL;
		}
		break;
	}
}

/*
 * Initialize an empty client data structure.
 */
void ds_client_data_init(struct ds_client_data *data)
{
	ASSERT(data != NULL);
	memset(data, 0, sizeof(*data));
}

/*
 * Setup a client data structure to point to a fixed size buffer.  The
 * buffer will NOT automatically be deleted on cleanup.
 */
void ds_client_data_init_buf(struct ds_client_data *data, void *buf,
	size_t buf_size, size_t len, bool mallocd)
{
	ASSERT(data != NULL);
	ASSERT(len <= buf_size);

	ds_client_data_init(data);
	data->type = DS_DATA_BUF;
	data->buf.ptr = buf;
	data->buf.len = len;
	data->buf.capacity = buf_size;
	data->buf.mallocd = mallocd;
}

/*
 * Setup a client data structure to point to a queue buffer.  The buffer will
 * NOT automatically be reset on cleanup.
 */
void ds_client_data_init_qbuf(struct ds_client_data *data,
	struct queue_buf *qbuf)
{
	ASSERT(data != NULL);
	ASSERT(qbuf != NULL);

	ds_client_data_init(data);
	data->type = DS_DATA_QBUF;
	data->qbuf = qbuf;
}

/*
 * Setup a client data structure to point to a file.  The file will NOT
 * automatically be deleted on cleanup.  Returns the FILE pointer on success,
 * or NULL on failure.
 */
FILE *ds_client_data_init_file(struct ds_client_data *data,
	const char *path, const char *mode)
{
	FILE *fp;

	ASSERT(data != NULL);
	ASSERT(path != NULL);
	ASSERT(mode != NULL);

	ds_client_data_init(data);
	fp = fopen(path, mode);
	if (!fp) {
		log_err("failed to open %s: %m", path);
		return NULL;
	}
	data->type = DS_DATA_FILE;
	data->file = fp;
	data->content = HTTP_CONTENT_BINARY;
	return fp;
}

/*
 * Setup a client data structure to point to a fixed size buffer containing
 * JSON encoded data.  The buffer WILL automatically be freed on cleanup.
 */
char *ds_client_data_init_json(struct ds_client_data *data,
	const json_t *obj)
{
	char *buf;
	size_t buf_len;

	ASSERT(data != NULL);

	if (!obj) {
		log_err("NULL JSON object");
		return NULL;
	}
	buf = json_dumps(obj, JSON_COMPACT);
	if (!buf) {
		log_err("malloc failed");
		return NULL;
	}
	log_debug("buf %s", buf);
	buf_len = strlen(buf);
	ds_client_data_init_buf(data, buf, buf_len + 1, buf_len, true);
	data->content = HTTP_CONTENT_JSON;
	data->delete_on_cleanup = true;
	return buf;
}

/*
 * Parse the client data and return a JSON object.  Returns NULL on failure.
 */
json_t *ds_client_data_parse_json(const struct ds_client_data *data)
{
	json_error_t error;
	json_t *obj = NULL;

	ASSERT(data != NULL);

	switch (data->type) {
	case DS_DATA_NONE:
		break;
	case DS_DATA_BUF:
		if (!data->buf.ptr || !data->buf.len) {
			break;
		}
		obj = json_loadb(data->buf.ptr, data->buf.len, 0, &error);
		if (!obj) {
			log_err("JSON parse error at line %d: %s",
			    error.line, error.text);
		}
		break;
	case DS_DATA_QBUF:
		if (!data->qbuf || !queue_buf_len(data->qbuf)) {
			break;
		}
		obj = queue_buf_parse_json(data->qbuf, 0);
		break;
	case DS_DATA_FILE:
		if (!data->file) {
			break;
		}
		obj = json_loadf(data->file, 0, &error);
		if (!obj) {
			log_err("JSON parse error at line %d: %s",
			    error.line, error.text);
		}
		break;
	}
	return obj;
}

/*
 * Initialize a client request based on the setup in the request info structure.
 * The request structure should be zeroed out before calling this function.
 */
int ds_client_req_init(struct ds_client_req *req,
	const struct ds_client_req_info *info)
{
	ssize_t len;

	ASSERT(req != NULL);
	ASSERT(info != NULL);

	/* Raw URL and hostname are mutually exclusive options */
	ASSERT((info->raw_url != NULL) ^ (info->host != NULL));

	req->method = info->method;
	/* Construct URL */
	if (info->raw_url) {
		/* Sender passed in full URL */
		if (info->uri_args) {
			len = snprintf(req->link, sizeof(req->link), "%s?%s",
			    info->raw_url, info->uri_args);
		} else {
			len = snprintf(req->link, sizeof(req->link), "%s",
			    info->raw_url);
		}
	} else if (info->uri) {
		/* Sender passed in protocol, hostname, URI, etc */
		if (info->uri_args) {
			len = snprintf(req->link, sizeof(req->link),
			    "%s://%s/%s?%s",
			    ds_client_proto_names[info->proto], info->host,
			    info->uri, info->uri_args);
		} else {
			len = snprintf(req->link, sizeof(req->link),
			    "%s://%s/%s", ds_client_proto_names[info->proto],
			    info->host, info->uri);
		}
	} else {
		len = snprintf(req->link, sizeof(req->link), "%s://%s",
		    ds_client_proto_names[info->proto], info->host);
	}
	if (len >= sizeof(req->link)) {
		log_err("link %zu byte max: %s", sizeof(req->link) - 1,
		    req->link);
		return -1;
	}
	/* Expand URL variables */
	len = str_expand_vars(ds_client_url_var_table, req->link,
	    sizeof(req->link), req->link, len, (void *)info);
	if (len < 0 || len >= sizeof(req->link)) {
		log_err("URL variable expansion failed");
		return -1;
	}
	/* Manage cloud state machine if this is an Ayla cloud request */
	req->ayla_cloud = !info->non_ayla;
	/* Use Ayla device authentication on secure requests */
	if (req->ayla_cloud && info->proto == DS_PROTO_HTTPS) {
		req->ayla_auth = true;
	}
	/* Setup request and response data handling */
	req->req_data = info->req_data;
	if (info->resp_file_path) {
		if (!ds_client_data_init_file(&req->resp_data,
		    info->resp_file_path, "w")) {
			return -1;
		}
	} else {
		req->resp_data.content = info->resp_content;
	}
	/* Set a stall timeout */
	req->timeout_secs = info->timeout_secs;
	return 0;
}

/*
 * Cleanup and reset a client request.  This is called automatically
 * after a request has completed.
 */
void ds_client_req_reset(struct ds_client_req *req)
{
	ASSERT(req != NULL);

	ds_client_data_cleanup(&req->req_data);
	ds_client_data_cleanup(&req->resp_data);
	memset(req, 0, sizeof(*req));
}

/*
 * Initialize a client.  This a configures and extends an HTTP client
 * library context.
 */
int ds_client_init(struct http_client *http_client, struct ds_client *client,
	size_t resp_buf_init_size, const char *debug_label)
{
	int rc;

	ASSERT(http_client != NULL);
	ASSERT(client != NULL);

	client->context = http_client_context_add(http_client);
	if (!client->context) {
		return -1;
	}
	http_client_context_set_data_funcs(client->context,
	    ds_client_read, ds_client_write, client);
	http_client_context_set_timeout(client->context,
	    DS_CLIENT_REQ_MAX_TIMEOUT);
#ifndef BUILD_RELEASE
	/* Enable verbose HTTP client debug on non-release builds */
	if (log_debug_enabled()) {
		/* Enable curl debug text */
		http_client_context_set_debug(client->context,
		    HTTP_CLIENT_DEBUG_INFO, debug_label);
	}
#endif
	/* Initialize response buffer */
	if (resp_buf_init_size) {
		rc = queue_buf_init(&client->resp_buf, QBUF_OPT_PRE_ALLOC,
		    resp_buf_init_size);
	} else {
		rc = queue_buf_init(&client->resp_buf, 0, 0);
	}
	return rc;
}

/*
 * Reset a client and free all resources.
 */
void ds_client_cleanup(struct ds_client *client)
{
	if (!client) {
		return;
	}
	ds_client_reset(client);
	queue_buf_destroy(&client->resp_buf);
	http_client_context_remove(client->context);
	client->context = NULL;
}

/*
 * Returns true if a request is in progress.
 */
bool ds_client_busy(struct ds_client *client)
{
	ASSERT(client != NULL);

	return http_client_busy(client->context);
}

/*
 * Send a client request.  Node: currently, the client allocates memory for a
 * single request, so req must point to the client's own request state.
 */
int ds_client_send(struct ds_client *client, struct ds_client_req *req)
{
	size_t send_size = -1;

	ASSERT(client != NULL);
	ASSERT(req == &client->req);

	if (http_client_busy(client->context)) {
		log_err("request already pending");
		return -1;
	}
	if (req->link[0] == '\0') {
		log_err("request not initialized");
		return -1;
	}
	/* Customize request for Ayla Device Service */
	if (req->ayla_cloud && req->ayla_auth) {
		if (ds_client_add_auth_header(client, req->method,
		    req->link) < 0) {
			goto error;
		}
		if (http_client_set_header_parsers(client->context,
		    ds_client_auth_header_tags) < 0) {
			goto error;
		}
	}
	/* Setup request based on method */
	switch (req->method) {
	case HTTP_GET:
		/* Set Accept header */
		if (req->resp_data.content != HTTP_CONTENT_UNKNOWN) {
			http_client_add_header(client->context, "Accept", "%s",
			    http_content_type_names[req->resp_data.content]);
		}
		break;
	case HTTP_PUT:
	case HTTP_POST:
		send_size = ds_client_data_size(&req->req_data);
		/* Set Content-type header */
		if (send_size > 0 &&
		    req->req_data.content != HTTP_CONTENT_UNKNOWN) {
			http_client_add_header(client->context, "Content-Type",
			    "%s",
			    http_content_type_names[req->req_data.content]);
		}
		break;
	default:
		break;
	}
	/*
	 * Default to client's response buffer, if none selected.  This is
	 * necessary for all methods, because some non-GET requests return data.
	 */
	if (client->req.resp_data.type == DS_DATA_NONE) {
		ds_client_data_init_qbuf(&client->req.resp_data,
		    &client->resp_buf);
		client->req.resp_data.delete_on_cleanup = true;
	}

	if (http_client_send(client->context, req->method, req->link, send_size,
	    ds_client_send_done, client, req->timeout_secs) < 0) {
		goto error;
	}
	return 0;
error:
	http_client_cancel(client->context);
	return -1;
}

/*
 * Cancel a pending request (if there is one), and reset the client's request
 * state.
 */
void ds_client_reset(struct ds_client *client)
{
	ASSERT(client != NULL);

	if (client->context) {
		http_client_cancel(client->context);
	}
	ds_client_req_reset(&client->req);
}
