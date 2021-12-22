/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/nameval.h>
#include <ayla/http.h>
#include <ayla/time_utils.h>
#include <ayla/json_parser.h>
#include <ayla/base64.h>
#include <ayla/crypto.h>
#include <ayla/ayla_interface.h>
#include <ayla/log.h>
#include <ayla/json_interface.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <curl/curl.h>
#include <jansson.h>

#include "dapi.h"
#include "notify.h"
#include "ds.h"
#include "devd_conf.h"
#include "serv.h"
#include "ops_devd.h"
#include "props_client.h"
#include "props_if.h"
#include "gateway_if.h"

#define CLIENT_LAN_CRYPT_DEBUG	0	/* change to 1 or debug for more logs */

/*
 * LAN (local applications) definitions.
 */

#define CLIENT_LAN_RETRY_LIMIT	2	/* tries before giving up on loc cli */
#define CLIENT_LAN_RETRY_WAIT	1000	/* loc cli retry wait, milliseconds */
#define CLIENT_LAN_CONN_WAIT	5000	/* loc cli connect wait, millisec */
#define CLIENT_LAN_JSON		30	/* max # of tokens in lan response */
#define CLIENT_LANIP_JSON	14	/* max # of tokens in lanip response */
#define CLIENT_LAN_EXCH_VER	1	/* LAN exchange version # */
#define CLIENT_LAN_PROTO_NUM	1	/* proto # of LAN encryption */
#define CLIENT_LAN_SIGN_SIZE	32	/* sign size of LAN packet exchanges */
#define CLIENT_LAN_REFRESH_LIM	10	/* # of refresh before mod does GET */

/* begin string of rsa public key */
#define CLIENT_LAN_RSA_PUBKEY_BEGIN "-----BEGIN RSA PUBLIC KEY-----\n"
/* end string of rsa public key */
#define CLIENT_LAN_RSA_PUBKEY_END "\n-----END RSA PUBLIC KEY-----\n"
/* character numbers of per line */
#define CLIENT_LAN_RSA_NUM_PER_LINE 64

static void client_lan_step(struct timer *timer);

static void client_lan_free_pubkey(struct client_lan_reg *lan)
{
	if (lan->pubkey) {
		free(lan->pubkey);
		lan->pubkey = NULL;
	}
}

static void client_lan_remove(struct device_state *dev,
			struct client_lan_reg *lan)
{
	log_debug("removing lan->id %u", lan->id);
	timer_cancel(&dev->timers, &lan->step_timer);
	timer_cancel(&dev->timers, &lan->timer);
	lan->uri[0] = '\0';
	lan->pending = 0;
	lan->rsa_ke = 0;
	crypto_cleanup(&lan->mod_enc);
	crypto_cleanup(&lan->app_enc);
	client_lan_free_pubkey(lan);
	ds_dest_avail_set(device.dests_avail & ~BIT(lan->id));
}

/*
 * Convert key to rsa key format.
 * The returned string should be freed by caller
 */
static char *client_lan_convert_rsa_key(const char *key)
{
	char *rsa_key;
	int keylen;
	int rsa_key_len;
	int headerlen;
	int taillen;
	int cr_num;
	int roundlen;
	int i;

	keylen = strlen(key);
	headerlen = strlen(CLIENT_LAN_RSA_PUBKEY_BEGIN);
	taillen = strlen(CLIENT_LAN_RSA_PUBKEY_END);
	cr_num = keylen / CLIENT_LAN_RSA_NUM_PER_LINE;

	rsa_key_len = keylen + headerlen +
		taillen + cr_num;
	rsa_key = malloc(rsa_key_len + 1);
	if (!rsa_key) {
		return NULL;
	}
	memcpy(rsa_key, CLIENT_LAN_RSA_PUBKEY_BEGIN, headerlen);

	roundlen = 0;
	for (i = 0; i < cr_num; i++) {
		memcpy(rsa_key + headerlen + roundlen,
			key + (i * CLIENT_LAN_RSA_NUM_PER_LINE),
			CLIENT_LAN_RSA_NUM_PER_LINE);
		memcpy(rsa_key + headerlen + roundlen +
			CLIENT_LAN_RSA_NUM_PER_LINE,
			"\n", 1);
		roundlen += CLIENT_LAN_RSA_NUM_PER_LINE + 1;
	}
	if ((keylen % CLIENT_LAN_RSA_NUM_PER_LINE) != 0) {
		memcpy(rsa_key + headerlen + roundlen,
			key + (i * CLIENT_LAN_RSA_NUM_PER_LINE),
			keylen % CLIENT_LAN_RSA_NUM_PER_LINE);
	}
	memcpy(rsa_key + headerlen + keylen + cr_num,
		CLIENT_LAN_RSA_PUBKEY_END,
		taillen);
	*(rsa_key + rsa_key_len) = 0;
	return rsa_key;
}

/*
 * Parse the LAN registration request.
 * lan: pointer to the result.
 * Returns 0 on success, or appropriate status otherwise.
 */
static int client_json_lan_parse(struct server_req *req,
	struct client_lan_reg *lan, int *notify)
{
	struct device_state *dev = &device;
	json_t *obj;
	const char *ip;
	const char *key;
	char *rsa_key;

	memset(lan, 0, sizeof(*lan));
	obj = json_object_get(req->body_json, "local_reg");
	if (!obj) {
		log_warn("no local_reg");
		ds_json_dump(__func__, req->body_json);
		return HTTP_STATUS_BAD_REQUEST;
	}
	if (json_get_string_copy(obj, "uri", lan->uri, sizeof(lan->uri)) < 0) {
		log_warn("invalid uri");
		ds_json_dump(__func__, req->body_json);
		return HTTP_STATUS_BAD_REQUEST;
	}
	ip = json_get_string(obj, "ip");
	if (!ip || !inet_aton(ip, &lan->host_addr)) {
		log_warn("no ip");
		ds_json_dump(__func__, req->body_json);
		return HTTP_STATUS_BAD_REQUEST;
	}
	if (json_get_uint16(obj, "port", &lan->host_port)) {
		log_warn("invalid port");
		ds_json_dump(__func__, req->body_json);
		return HTTP_STATUS_BAD_REQUEST;
	}
	key = json_get_string(obj, "key");
	if (key && dev->wifi_ap_enabled) {
		rsa_key = client_lan_convert_rsa_key(key);
		if (!rsa_key) {
			log_warn("failed to convert rsa key");
			return HTTP_STATUS_UNAVAIL;
		}
		log_debug("key: %s", key);
		log_debug("rsa key: %s", rsa_key);
		lan->rsa_ke = 1;
		/* save rsa pub key */
		lan->pubkey = rsa_key;
	}

	*notify = 1;		/* defaut to 1 */
	json_get_int(obj, "notify", notify);

	lan->mtime = 0;
	return 0;
}

/*
 * Timeout LAN registration entries.
 */
static void client_lan_timeout(struct timer *timer)
{
	struct device_state *dev = &device;
	struct client_lan_reg *lan;

	lan = CONTAINER_OF(struct client_lan_reg, timer, timer);
	if (lan->uri[0] == '\0') {
		return;
	}

	log_debug("expire, lan #%u: age %llums", lan->id,
	    (long long unsigned)time_mtime_ms() - lan->mtime);
	client_lan_remove(dev, lan);
}

static struct client_lan_reg *client_lan_lookup(struct client_lan_reg *lan,
						u8 *id)
{
	struct client_lan_reg *best;
	struct client_lan_reg *reg;

	*id = 0;
	best = NULL;
	for (reg = client_lan_reg; reg < &client_lan_reg[CLIENT_LAN_REGS];
	    reg++) {
		if (reg->uri[0] == '\0' && !best) {
			best = reg;
		}
		if (reg->host_addr.s_addr == lan->host_addr.s_addr &&
		    reg->host_port == lan->host_port &&
		    !strncmp(reg->uri, lan->uri, sizeof(lan->uri))) {
			best = reg;
			break;
		}
	}
	if (best) {
		*id = (best - client_lan_reg) + 1;
	}
	return best;
}

static void random_fill(void *buf, size_t len)
{
	while (((long)buf % sizeof(u32)) && len) {
		*(u8 *)buf = random();
		buf++;
		len--;
	}
	while (len > sizeof(u32)) {
		*(u32 *)buf = random();
		buf += sizeof(u32);
		len -= sizeof(u32);
	}
	while (len) {
		*(u8 *)buf = random();
		buf++;
		len--;
	}
}

static size_t client_lan_recv_resp(void *ptr, size_t size, size_t nmemb,
		void *stream)
{
	struct client_lan_reg *lan = stream;
	u32 len;

	size *= nmemb;

	len = lan->recved_len;
	if (len + size >= sizeof(lan->buf)) {
		lan->buf[0] = '\0';
		return 0;
	}
	memcpy(lan->buf + len, ptr, size);
	lan->recved_len += size;
	lan->buf[lan->recved_len] = '\0';	/* XXX debug */
	if (CLIENT_LAN_CRYPT_DEBUG) {
		log_debug("body '%s'", lan->buf + len);
	}

	return size;
}

static void client_lan_reset_timer(struct device_state *dev,
				struct client_lan_reg *lan)
{
	u32 keep_alive = dev->lan.keep_alive;

	lan->mtime = time_mtime_ms();
	if (!keep_alive) {
		/* Keep alive time not configured, so use default */
		keep_alive = CLIENT_LAN_KEEPALIVE;
	}
	timer_reset(&dev->timers, &lan->timer, client_lan_timeout,
	    keep_alive * 1000);
}

/*
 * Debug info callback from curl.
 * See CURLOPT_DEBUGFUNCTION secton in curl_easy_setopt(3);
 * buf is not NUL-terminated
 */
static int client_lan_curl_debug(CURL *curl, curl_infotype info,
			char *buf, size_t len, void *arg)
{
	char str[200];

	switch (info) {
	case CURLINFO_TEXT:
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
 * Returns 0 on a success, 1 on a NOT_FOUND error, and -1 for all other errors
 */
static int client_lan_send(struct device_state *dev, struct client_lan_reg *lan,
	const char *reg_url, const char *url, enum http_method method,
	const char *body, const char *op)
{
	CURL *curl;
	CURLcode ccode;
	char link[DS_CLIENT_LINK_MAX_LEN];
	struct curl_slist *header;
	char err_msg[CURL_ERROR_SIZE];
	int rc = 0;

	if (!lan || lan->uri[0] == '\0') {
		return -1;
	}
	lan->status = HTTP_STATUS_NOT_FOUND;
	client_lan_reset_timer(dev, lan);
	curl = curl_easy_init();

	snprintf(link, sizeof(link), "http://%s:%u%s%s%s%s",
	    inet_ntoa(lan->host_addr), lan->host_port,
	    (reg_url[0] != '\0' && reg_url[0] != '/') ? "/" : "",
	    reg_url,
	    (url[0] != '\0' && url[0] != '/') ? "/" : "",
	    url);
	if (debug) {
		log_debug("t %llu link \"%s\"",
		    (long long unsigned)time_mtime_ms(), link);
	}

	header = curl_slist_append(NULL, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, link);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, client_lan_recv_resp);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, lan);

	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 3);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err_msg);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);

	if (body) {
		if (method == HTTP_PUT) {
			log_err("PUT not supported");
			rc = -1;
			goto out;
		} else {
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
		}
	}
	if (debug) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION,
		    client_lan_curl_debug);
	}
	lan->recved_len = 0;

	/*
	 * Send request.
	 */
	ccode = curl_easy_perform(curl);

	if (ccode != CURLE_OK) {
not_ok:
		log_warn("curl error %u %s",
		    ccode, curl_easy_strerror(ccode));
		rc = -1;
		goto out;
	}

	ccode = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &lan->status);
	if (ccode != CURLE_OK) {
		goto not_ok;
	}

	switch (lan->status) {
	case HTTP_STATUS_OK:
	case HTTP_STATUS_ACCEPTED:
	case HTTP_STATUS_NO_CONTENT:
	case HTTP_STATUS_PAR_CONTENT:
		log_debug("url %s status %lu", url, lan->status);
		break;
	case HTTP_STATUS_NOT_FOUND:
	default:
		log_warn("url %s status %lu", url, lan->status);
		if (lan->status == HTTP_STATUS_NOT_FOUND) {
			rc = 1;
			break;
		}
		rc = -1;
		break;
	}
	client_lan_reset_timer(dev, lan);
out:
	curl_slist_free_all(header);
	curl_easy_cleanup(curl);

	return rc;
}

/*
 * make JSON object containing seq_no and data.
 */
static char *client_lan_encaps_cleartext(struct client_lan_reg *lan,
					json_t *body)
{
	json_t *obj;
	char *out;

	/*
	 * make JSON object containing seq_no and data.
	 */
	obj = json_object();
	if (!obj) {
		return NULL;
	}
	json_object_set_new(obj, "seq_no", json_integer(lan->send_seq_no));
	lan->send_seq_no++;		/* TBD: increment only on success */
	json_object_set(obj, "data", body);

	/*
	 * Turn JSON object into string.
	 */
	out = json_dumps(obj, JSON_COMPACT);
	json_decref(obj);
	return out;
}

/*
 * Base-64 encode data and insert in JSON object.
 * TBD: XXX move this to json_parser in the library.
 */
static int json_obj_set_base64(json_t *obj, const char *tag,
				const void *buf, size_t len)
{
	char *data;
	json_t *item;

	data = base64_encode(buf, len, NULL);
	if (!data) {
		log_err("base64 failed");
		return -1;
	}
	item = json_string(data);
	free(data);
	if (!item) {
		log_err("json_string failed");
		return -1;
	}
	return json_object_set_new(obj, tag, item);
}

/*
 * Build the JSON object containing data and signature.
 */
static char *client_lan_encaps_crypto(void *enc, size_t enc_len,
					void *sig, size_t sig_len)
{
	json_t *obj;
	char *out;

	obj = json_object();
	if (!obj) {
		log_debug("json_obj failed");
		return NULL;
	}

	if (json_obj_set_base64(obj, "enc", enc, enc_len)) {
		json_decref(obj);
		return NULL;
	}
	if (json_obj_set_base64(obj, "sign", sig, sig_len)) {
		json_decref(obj);
		return NULL;
	}

	/*
	 * Return the JSON object in string form.
	 */
	out = json_dumps(obj, JSON_COMPACT);
	if (!out) {
		log_debug("final json dumps failed");
	}
	json_decref(obj);
	return out;
}

/*
 * Encapsulate the body of a response to a client's revere-REST command.
 * Return a string containing the JSON enc and sign objects.
 * Returns NULL on failure due to memory allocation problems.
 */
static char *client_lan_encaps(struct device_state *dev,
				struct client_lan_reg *lan, json_t *body)
{
	size_t len;
	size_t pad;
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_len;
	char *out = NULL;
	char *buf;

	out = client_lan_encaps_cleartext(lan, body);
	if (!out) {
		return NULL;
	}
	log_debug("body: '%s'", out);

	/*
	 * Compute signature.
	 */
	len = strlen(out);
	if (HMAC(EVP_sha256(), lan->mod_sign_key, sizeof(lan->mod_sign_key),
	    (unsigned char *)out, len, md, &md_len) == NULL) {
		log_err("HMAC failed");
		free(out);
		return NULL;
	}

	/*
	 * Allocate encryption buffer for the object and copy it.
	 */
	pad = -len & (CLIENT_LAN_IV_SIZE - 1);
	buf = (char *)realloc(out, len + pad);
	if (!buf) {
		log_err("malloc failed len %zu pad %zu", len, pad);
		goto error;
	}
	out = buf;
	memset(out + len, 0, pad);
	len += pad;

	/*
	 * Encrypt buffer in place.
	 */
	if (crypto_encrypt(&lan->mod_enc, out, len, out, len) != len) {
		log_err("failed to encrypt %zu bytes", len);
		goto error;
	}

	/*
	 * Encapsulate the cryptotext and signature as base64 strings in JSON.
	 */
	buf = client_lan_encaps_crypto(out, len, md, md_len);
	free(out);
	return buf;
error:
	free(out);
	return NULL;
}

/*
 * Send to LAN client the response to a reverse-REST command.
 */
void client_lan_cmd_resp(struct server_req *req, int status)
{
	struct device_state *dev = &device;
	struct serv_rev_req *rev_req = (struct serv_rev_req *)req->arg;
	struct client_lan_reg *lan;
	char link[DS_CLIENT_LINK_MAX_LEN];
	int rc;
	json_t *body;
	char *out;

	ASSERT(rev_req && rev_req->lan);
	lan = rev_req->lan;

	snprintf(link, sizeof(link), "%s?cmd_id=%d&status=%d",
	    rev_req->resp_uri, rev_req->cmd_id, status);

	if (req->reply_is_json) {
		body = queue_buf_parse_json(&req->reply, 0);
	} else {
		body = json_object();	/* Empty body */
	}
	out = client_lan_encaps(dev, rev_req->lan, body);
	json_decref(body);
	if (!out) {
		log_err("encapsulation failed");
		goto failure;
	}
	rc = client_lan_send(dev, lan, "", link, HTTP_POST, out, __func__);
	free(out);
	if (rc) {
		goto failure;
	}
	if (lan->pending) {
		timer_reset(&dev->timers, &lan->step_timer, client_lan_step, 0);
	}
	return;
failure:
	client_lan_remove(dev, lan);
}

/*
 * This function can be used to POST any JSON to a LAN client given a URL
 * If err_type is provided, it'll be set to the appropriate error type if
 * an error occurs.
 */
int client_lan_post(struct device_state *dev, struct client_lan_reg *lan,
			json_t *body, const char *url, const char **err_type)
{
	char *out;
	int rc = -1;

	if (!lan->uri[0]) {
		goto failure;
	}
	out = client_lan_encaps(dev, lan, body);
	if (!out) {
		log_err("encaps failed");
		goto failure;
	}

	rc = client_lan_send(dev, lan, lan->uri, url, HTTP_POST, out, __func__);
	free(out);

	if (rc) {
failure:
		log_debug("failure sending to lan %u, rc = %d", lan->id, rc);
		if (rc == 1) {
			/* mobile app doesn't care about the msg */
			/* don't kill the session */
			if (err_type) {
				*err_type = JINT_ERR_UNKWN_PROP;
			}
			return 0;
		}
		if (err_type) {
			*err_type = JINT_ERR_CONN_ERR;
		}
		client_lan_remove(dev, lan);
		return -1;
	}
	return 0;
}

static int client_lan_key_gen(struct device_state *dev,
	struct client_lan_reg *lan, u8 *secret, size_t sec_len,
	u8 *dest, size_t seed_size,
	char *root, char *suffix)
{
	char seed[seed_size];
	int seed_len;
	unsigned int dest_len;

	seed_len = snprintf(seed, sizeof(seed) - 1, "%s%s", root, suffix);
	seed[seed_len] = '\0';

	/*
	 * key = PRF(secret, seed) = SHA256(secret, A(1) + seed).
	 *	A(0) = seed, A(i) = SHA256(secret, A(i - 1))
	 * so, key = SHA256(secret, SHA256(secret, seed) + seed);
	 * See RFC 2104.
	 */
	#if OPENSSL_VERSION_NUMBER < 0x10100000L
	/* OpenSSL 1.0.2 and below (old code) */
	{
		HMAC_CTX ctx;
		HMAC_CTX_init(&ctx);
		HMAC_Init(&ctx, secret, sec_len, EVP_sha256());
		HMAC_Update(&ctx, (u8 *)seed, seed_len);
		HMAC_Final(&ctx, dest, &dest_len);
		ASSERT(dest_len == CLIENT_LAN_KEY_LEN);

		HMAC_Init(&ctx, NULL, 0, NULL);
		HMAC_Update(&ctx, dest, dest_len);
		HMAC_Update(&ctx, (u8 *)seed, seed_len);
		HMAC_Final(&ctx, dest, &dest_len);
		HMAC_CTX_cleanup(&ctx);
		ASSERT(dest_len == CLIENT_LAN_KEY_LEN);
	}
	#else
	/* OpenSSL 1.1.0 and above (new code) */
	{
		HMAC_CTX *ctx;
		ctx = HMAC_CTX_new();
		if (ctx == NULL) {
			log_err("app malloc failed");
			return -1;
		}
		HMAC_Init_ex(ctx, secret, sec_len, EVP_sha256(), NULL);
		HMAC_Update(ctx, (u8 *)seed, seed_len);
		HMAC_Final(ctx, dest, &dest_len);
		ASSERT(dest_len == CLIENT_LAN_KEY_LEN);

		HMAC_Init_ex(ctx, NULL, 0, NULL, NULL);
		HMAC_Update(ctx, dest, dest_len);
		HMAC_Update(ctx, (u8 *)seed, seed_len);
		HMAC_Final(ctx, dest, &dest_len);
		HMAC_CTX_free(ctx);
		ASSERT(dest_len == CLIENT_LAN_KEY_LEN);
	}
	#endif

	return 0;
}

/*
 * client_lan_key_parse requires 64-bit integers to correctly do key exchange.
 */
ASSERT_COMPILE(json_int, sizeof(json_int_t) >= sizeof(s64));

static int client_lan_gen_keys(struct device_state *dev,
    struct client_lan_reg *lan, u8 *secret, size_t sec_len,
    const char *random_two, json_int_t *time_two)
{
	size_t seed_size = 4 * CLIENT_LAN_RAND_SIZE + 2;
	char mod_root[seed_size];
	char app_root[seed_size];
	int len;

	log_debug("generate key");

	/* Run the KDF Functions to generate the keys */
	len = snprintf(mod_root, sizeof(mod_root), "%s%s%" JSON_INTEGER_FORMAT
	    "%llu", random_two, lan->random_one, *time_two,
	    (long long unsigned)lan->time_one);
	if (len < 0 || len >= sizeof(mod_root)) {
		log_warn("mod root buffer overflow");
		return -1;
	}
	mod_root[len] = '\0';
	len = snprintf(app_root, sizeof(app_root), "%s%s%lld%lld",
	    lan->random_one, random_two, lan->time_one, *time_two);
	if (len < 0 || len >= sizeof(mod_root)) {
		log_warn("app root buffer overflow");
		return -1;
	}
	app_root[len] = '\0';

	/* Generate random signing keys */
	if (client_lan_key_gen(dev, lan, secret, sec_len,
	    lan->mod_sign_key, seed_size, mod_root, "0")) {
		return -1;
	}
	if (client_lan_key_gen(dev, lan, secret, sec_len,
	    lan->app_sign_key, seed_size, app_root, "0")) {
		return -1;
	}
	/* Generate random encryption keys */
	if (client_lan_key_gen(dev, lan, secret, sec_len,
	    lan->mod_enc_key, seed_size, mod_root, "1")) {
		return -1;
	}
	if (client_lan_key_gen(dev, lan, secret, sec_len,
	    lan->app_enc_key, seed_size, app_root, "1")) {
		return -1;
	}
	/* Generate random encryption IVs and initialize AES contexts */
	if (client_lan_key_gen(dev, lan, secret, sec_len,
	    (u8 *)lan->buf, seed_size, mod_root, "2")) {
		return -1;
	}
	crypto_cleanup(&lan->mod_enc);
	if (crypto_init_aes(&lan->mod_enc, (u8 *)lan->buf,
	    lan->mod_enc_key, CLIENT_LAN_KEY_LEN)) {
		return -1;
	}
	if (client_lan_key_gen(dev, lan, secret, sec_len,
	    (u8 *)lan->buf, seed_size, app_root, "2")) {
		return -1;
	}
	crypto_cleanup(&lan->app_enc);
	if (crypto_init_aes(&lan->app_enc, (u8 *)lan->buf,
	    lan->app_enc_key, CLIENT_LAN_KEY_LEN)) {
		return -1;
	}
	lan->valid_key = 1;
	return 0;
}

/*
 * Parse the JSON key exchange response from lan client.
 */
static int client_lan_key_parse(struct device_state *dev,
			    struct client_lan_reg *lan)
{
	json_t *root;
	json_t *obj;
	json_error_t jerr;
	json_int_t time_two;
	const char *random_two;
	int rc;

	/*
	 * Handle response.
	 * This should be asynchronous, eventually. TBD XXX.
	 */
	if (!lan->recved_len) {
		log_warn("no response");
		return -1;
	}
	root = json_loadb(lan->buf, lan->recved_len, 0, &jerr);
	if (!root) {
		log_warn("parse error: %s", jerr.text);
		log_warn("buf '%s'", lan->buf);
		return -1;
	}
	random_two = json_get_string(root, "random_2");
	obj = json_object_get(root, "time_2");
	if (!random_two || !json_is_integer(obj)) {
		log_warn("missing response fields");
		json_decref(root);
		return -1;
	}
	time_two = json_integer_value(obj);

	if (!lan->rsa_ke) {
		rc = client_lan_gen_keys(dev, lan,
		    (u8 *)dev->lan.key,
		    sizeof(dev->lan.key), random_two, &time_two);
	} else {
		rc = client_lan_gen_keys(dev, lan,
		    (u8 *)dev->lan.lanip_random_key,
		    sizeof(dev->lan.lanip_random_key), random_two,
			&time_two);
	}
	json_decref(root);

	if (rc) {
		log_warn("key gen error - bad key");
		client_lan_remove(dev, lan);
		return 0; /* an error, but don't retry */
	}
	/*
	 * Key exchange is done, we should not need public key anymore.
	 */
	client_lan_free_pubkey(lan);
	return 0;
}

static int client_lan_key_exch(struct device_state *dev,
				struct client_lan_reg *lan)
{
	u8 random_data[(CLIENT_LAN_RAND_SIZE * 6) / 8];
	char *random_one;
	char *out = NULL;
	json_t *root = NULL;
	json_t *obj;
	char rsa_buf[256];
	ssize_t rsa_len;
	struct crypto_state rsa = { 0 };

	int rc = -1;

	random_fill((char *)random_data, sizeof(random_data));
	random_one = base64_encode((char *)random_data, sizeof(random_data),
	    NULL);
	if (!random_one) {
		goto failure;
	}

	strncpy(lan->random_one, random_one, sizeof(lan->random_one));
	free(random_one);

	/*
	 * Generate positive 64-bit time-related number.
	 * The number must be less than 10 to the 15th, or about 49 bits,
	 * so that the final seed we create is no more than 15 bytes.
	 */
	lan->time_one = time_mtime_ms() & 0x01FFFFFFFFFFFFull;

	root = json_object();
	if (!root) {
		goto failure;
	}
	obj = json_object();
	if (!obj) {
		goto failure;
	}
	json_object_set_new(root, "key_exchange", obj);
	json_object_set_new(obj, "ver", json_integer(CLIENT_LAN_EXCH_VER));
	json_object_set_new(obj, "random_1", json_string(lan->random_one));
	json_object_set_new(obj, "time_1", json_integer(lan->time_one));
	json_object_set_new(obj, "proto", json_integer(CLIENT_LAN_PROTO_NUM));
	if (!lan->pubkey) {
		json_object_set_new(obj, "key_id",
			json_integer(dev->lan.key_id));
	} else {
		/* Encrypt the secret using RSA public key */
		if (crypto_init_rsa(&rsa, RSA_KEY_PUBLIC, lan->pubkey) < 0) {
			goto failure;
		}
		rsa_len = crypto_encrypt(&rsa, dev->lan.lanip_random_key,
		    sizeof(dev->lan.lanip_random_key),
		    rsa_buf, sizeof(rsa_buf));
		crypto_cleanup(&rsa);
		if (rsa_len <= 0) {
			log_err("RSA key encryption failed");
			goto failure;
		}
		log_debug("encrypted lanip random key");
		if (json_obj_set_base64(obj, "sec", rsa_buf, rsa_len)) {
			goto failure;
		}
	}

	ds_json_dump(__func__, root);
	out = json_dumps(root, JSON_COMPACT);
	rc = client_lan_send(dev, lan, lan->uri, "key_exchange.json",
	    HTTP_POST, out, __func__);
	free(out);
failure:
	json_decref(root);
	if (rc) {
		client_lan_remove(dev, lan);
		return rc;
	}
	return client_lan_key_parse(dev, lan);
}

/*
 * Check signature.
 * Return zero on successful match.
 */
static int client_lan_sig_err(u8 *key, size_t key_len,
		void *buf, size_t buf_len, u8 *sign, size_t sign_len)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_len;

	if (HMAC(EVP_sha256(), key, key_len, buf, buf_len, md, &md_len) ==
	    NULL) {
		log_debug("client_lan_sig_check: HMAC failed");
		return -1;
	}
	return md_len != sign_len || memcmp(sign, md, sign_len);
}

/*
 * Handle GET of commands from mobile app.
 *
 * Sample response:  {
 *	"seq_no":8,
 *	"data":{
 *		"properties":[
 *			{
 *				 "property": {
 *					"base_type":"boolean",
 *					"value":1,
 *					"name":"Blue_LED"
 *				}
 *			}
 *		]
 *		"cmds":[
 *			{
 *				"cmd": {
 *					"cmd_id": 0,
 *					"method": "GET",
 *					"resource": "property.json?name=LED",
 *					"data": "none",
 *					"uri":
 *					    "local_lan/property/datapoint.json"
 *				}
 *			}
 *		]
 *	}
 *  }
 *
 * data may be either cmds or properties.
 */
static void client_lan_cmds(struct device_state *dev,
		struct client_lan_reg *lan, char *json, size_t len)
{
	json_error_t jerr;
	json_t *root;
	json_t *obj;
	json_t *arr;
	json_t *item;
	int i;
	int size;
	int source;

	root = json_loadb(json, len, 0, &jerr);
	if (!root) {
		log_warn("parse error: %s json '%s'", jerr.text, json);
		return;
	}

	obj = json_object_get(root, "data");
	if (!json_is_object(obj)) {
		log_warn("no data");
		goto done;
	}
	/* Ignore property updates from LAN clients when using RSA key */
	if (lan->rsa_ke) {
		goto get_cmds;
	}
	source = LAN_ID_TO_SOURCE(lan->id);
	arr = json_object_get(obj, "properties");
	if (json_is_array(arr)) {
		size = json_array_size(arr);
		for (i = 0; i < size; i++) {
			item = json_array_get(arr, i);
			if (!json_is_object(item)) {
				log_warn("no properties");
				continue;
			}
			item = json_object_get(item, "property");
			if (!json_is_object(item)) {
				log_warn("no property object");
				continue;
			}
			prop_send_prop_update(arr, source);
			if (ds_echo_for_prop_is_needed(item, source)) {
				prop_prepare_echo(dev, item, source);
			}
		}
		if (size && lan->pending) {
			/*
			 * more property updates pending. go fetch them
			 * we don't do this for commands because we want to
			 * first process appd's response.
			 */
			timer_reset(&dev->timers, &lan->step_timer,
			    client_lan_step, 0);
		}
	}
	arr = json_object_get(obj, "node_properties");
	if (json_is_array(arr)) {
		size = json_array_size(arr);
		if (size) {
			if (lan->pending) {
				/*
				 * more property updates pending. go fetch them
				 * we don't do this for commands because we want
				 * to first process appd's response.
				*/
				timer_reset(&dev->timers, &lan->step_timer,
				    client_lan_step, 0);
			}
			gateway_process_node_update(dev, arr, source);
		}
	}
get_cmds:
	arr = json_object_get(obj, "cmds");
	if (json_is_array(arr)) {
		size = json_array_size(arr);
		for (i = 0; i < size; i++) {
			item = json_array_get(arr, i);
			if (!json_is_object(item)) {
				log_warn("no cmd");
				continue;
			}
			item = json_object_get(item, "cmd");
			if (!json_is_object(item)) {
				log_warn("no cmd object");
				continue;
			}
			serv_json_cmd(item, lan);
		}
	}
done:
	json_decref(root);
}

static int client_lan_get(struct device_state *dev, struct client_lan_reg *lan)
{
	json_error_t jerr;
	json_t *root;
	int rc = -1;
	const char *enc;
	const char *sign;
	char *buf = NULL;
	char *dec_sign = NULL;
	size_t len;
	size_t sign_len;

	lan->pending = 0;
	if (client_lan_send(dev, lan, lan->uri, "commands.json",
	    HTTP_GET, NULL, __func__)) {
		client_lan_remove(dev, lan);
		return -1;
	}

	switch (lan->status) {
	case HTTP_STATUS_OK:
		break;
	case HTTP_STATUS_PAR_CONTENT:
		lan->pending = 1;
		/*
		 * Reset timer because we're doing a subsequent GET
		 * which is essentially like app sending another notify.
		 */
		lan->mtime = time_mtime_ms();
		break;
	case HTTP_STATUS_NO_CONTENT:
	default:
		return 0;
	}

	if (!lan->recved_len) {
		return 0;
	}

	if (CLIENT_LAN_CRYPT_DEBUG) {
		log_debug("GET of commmands: buf '%s'", lan->buf);
	}
	root = json_loadb(lan->buf, lan->recved_len, 0, &jerr);
	if (!root) {
		log_warn("parse error: %s len %u buf '%s'",
		    jerr.text, lan->recved_len, lan->buf);
		return -1;
	}
	enc = json_get_string(root, "enc");
	sign = json_get_string(root, "sign");
	if (!enc || !sign) {
		log_warn("missing field. buf '%s'", lan->buf);
		json_decref(root);
		return -1;
	}
	buf = base64_decode(enc, strlen(enc), &len);
	if (!buf) {
		log_warn("decode err on %s", enc);
		json_decref(root);
		return -1;
	}

	/*
	 * Decrypt command.
	 */
	if (crypto_decrypt(&lan->app_enc, buf, len, buf, len) != len) {
		log_err("failed to decrypt %zu bytes", len);
		goto out;
	}
	log_debug("cmd '%s'", buf);

	dec_sign = base64_decode(sign, strlen(sign), &sign_len);
	if (!dec_sign) {
		log_warn("decode err on %s", sign);
		goto out;
	}
	if (client_lan_sig_err(lan->app_sign_key, sizeof(lan->app_sign_key),
	    buf, strlen(buf), (u8 *)dec_sign, sign_len)) {
		log_warn("signature mismatch");
		goto out;
	}
	/* Only add the LAN client to the destination mask if using lanip key */
	if (!lan->rsa_ke) {
		ds_dest_avail_set(dev->dests_avail | BIT(lan->id));
	}
	client_lan_cmds(dev, lan, buf, strlen(buf));
	rc = 0;

out:
	free(dec_sign);
	free(buf);
	json_decref(root);

	return rc;
}

static void client_lan_step(struct timer *timer)
{
	struct device_state *dev = &device;
	struct client_lan_reg *lan;

	lan = CONTAINER_OF(struct client_lan_reg, step_timer, timer);

	if (lan->uri[0] == '\0') {
		return;
	}
	if (!lan->valid_key) {
		if (client_lan_key_exch(dev, lan)) {
			return;
		}
	}
	if (lan->pending) {
		if (client_lan_get(dev, lan)) {
			return;
		}
	}
}

/*
 * Check preconditions for LAN client registration or refresh.
 */
static int client_lan_check_precon(struct device_state *dev)
{
	/* LAN mode disabled in build */
	if (!CLIENT_LAN_REGS) {
		return HTTP_STATUS_NOT_FOUND;
	}
	/* Allow LAN mode for secure Wi-Fi setup when in AP mode */
	if (dev->wifi_ap_enabled) {
		return 0;
	}
	/* Return 404 if LAN mode disabled */
	if (!dev->lan.enable) {
		log_debug("LAN mode disabled");
		return HTTP_STATUS_NOT_FOUND;
	}
	/* Return 412 if a valid LAN key has not been received */
	if (dev->lan.key[0] == '\0') {
		log_debug("no LAN key");
		return HTTP_STATUS_PRE_FAIL;
	}
	return 0;
}

static void client_json_lan_add_helper(struct client_lan_reg *lan,
	struct client_lan_reg *parse, u8 lan_id, int notify)
{
	struct device_state *dev = &device;

	*lan = *parse;		/* struct copy */
	if (parse->pubkey) {
		lan->pubkey = strdup(parse->pubkey);
	}
	if (lan->rsa_ke) {
		/*
		 * Need new secret for every new connection.
		 */
		random_fill(dev->lan.lanip_random_key,
		    sizeof(dev->lan.lanip_random_key));
	}

	lan->id = lan_id;
	lan->valid_key = 0;

	/* lan->conn_state = CS_WAIT_EVENT;  XXX */
	if (notify) {
		lan->pending = 1;
	}

	client_lan_reset_timer(dev, lan);

	/*
	 * Trigger start of key exchange.
	 * Using timer as a callback mechanism.
	 */
	timer_reset(&dev->timers, &lan->step_timer, client_lan_step, 0);
}

/*
 * Create LAN registration session.
 * Kills an existing LAN connection to the same ip address.
 * Does a new key exchange.
 * Does a GET command.
 */
void client_lan_reg_post(struct server_req *req)
{
	struct device_state *dev = &device;
	struct client_lan_reg parse;
	struct client_lan_reg *lan;
	int status;
	u8 lan_id;
	int notify;

	status = client_lan_check_precon(dev);
	if (status) {
		server_put_end(req, status);
		return;
	}
	status = client_json_lan_parse(req, &parse, &notify);
	if (status) {
		goto put_status;
	}
	lan = client_lan_lookup(&parse, &lan_id);
	if (!lan) {
		status = HTTP_STATUS_UNAVAIL;
		goto put_status;
	}
	if (lan->uri[0] != '\0') {
		/* Kill the previous connection to lan if it existed */
		client_lan_remove(dev, lan);
	}
	log_debug("add, lan #%u", lan->id);
	server_put_end(req, HTTP_STATUS_ACCEPTED);
	client_json_lan_add_helper(lan, &parse, lan_id, 1);
	client_lan_free_pubkey(&parse);
	return;

put_status:
	server_put_end(req, status);
	client_lan_free_pubkey(&parse);
}

/*
 * LAN client refresh
 * Pends a GET command if notify == 1 or if this is the nth refresh.
 */
void client_lan_reg_put(struct server_req *req)
{
	struct device_state *dev = &device;
	struct client_lan_reg parse;
	struct client_lan_reg *lan;
	int status;
	u8 lan_id;
	int notify;

	status = client_lan_check_precon(dev);
	if (status) {
		server_put_end(req, status);
		return;
	}
	status = client_json_lan_parse(req, &parse, &notify);
	if (status) {
		goto put_status;
	}
	lan = client_lan_lookup(&parse, &lan_id);
	if (!lan) {
		status = HTTP_STATUS_UNAVAIL;
		goto put_status;
	}
	if (lan->uri[0] == '\0') {
		server_put_end(req, HTTP_STATUS_ACCEPTED);
		client_json_lan_add_helper(lan, &parse, lan_id, notify);
		client_lan_free_pubkey(&parse);
		return;
	}
	lan->refresh_count++;
	if (lan->refresh_count >= CLIENT_LAN_REFRESH_LIM || notify) {
		lan->pending = 1;
		lan->refresh_count = 0;
	}
	client_lan_reset_timer(dev, lan);
	if (notify) {
		lan->pending = 1;
		timer_set(&dev->timers, &lan->step_timer, 0);
	}
	status = HTTP_STATUS_ACCEPTED;
put_status:
	server_put_end(req, status);
	client_lan_free_pubkey(&parse);
}
