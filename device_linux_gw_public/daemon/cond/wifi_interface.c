/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/ether.h>
#include <sys/poll.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>

#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/hex.h>
#include <ayla/file_event.h>
#include <ayla/json_parser.h>
#include <ayla/buffer.h>
#include <ayla/timer.h>
#include <ayla/conf_io.h>
#include <ayla/uri_code.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>
#include <ayla/msg_conf.h>
#include <ayla/msg_cli.h>

#include "cond.h"
#include "wifi.h"
#include "wifi_platform.h"

#define COND_INTERFACE_RECONNECT_DELAY_MS	1000
#define COND_MSG_SERVER_MAX_SESSIONS		3

static struct amsg_server msg_server;
static struct amsg_client devd;

static struct timer reconnect_timer;
static struct amsg_resp_info *pending_scan_resp;

DEF_NAME_TABLE(wifi_bss_type_names, WIFI_BSS_TYPES);

/*
 * Return JSON object with SSID.  Only supports SSIDs with valid UTF-8
 * characters to be consistent with current JSON interface support.
 */
static json_t *wifi_ssid_obj(const struct wifi_ssid *ssid)
{
	return json_stringn((const char *)ssid->val, ssid->len);
}

/*
 * Return JSON object with 6-byte hex BSSID
 */
static json_t *wifi_bssid_obj(const struct ether_addr *ether)
{
	return json_string(net_ether_to_str(ether));
}

/*
 * Return JSON object with IPv4 IP representation
 */
static json_t *wifi_ipaddr_obj(const struct in_addr *ip)
{
	return json_string(inet_ntoa(*ip));
}

/*
 * Populate a wifi_ssid structure from a URI encoded string.
 */
static int wifi_decode_ssid(struct wifi_ssid *ssid, const char *str)
{
	ssize_t len;

	len = uri_decode((char *)ssid->val, sizeof(ssid->val), str);
	if (len < 0) {
		return -1;
	}
	ssid->len = len;
	return 0;
}

/*
 * Populate a wifi_key structure from a URI encoded string.
 */
static int wifi_decode_key(struct wifi_key *key, const char *str)
{
	ssize_t len;

	len = uri_decode((char *)key->val, sizeof(key->val), str);
	if (len < 0) {
		return -1;
	}
	key->len = len;
	return 0;
}

/*
 * Return a statically allocated string with security mode description
 */
static const char *wifi_sec_export(enum wifi_sec sec)
{
	static char buf[32];
	const char *mode;
	const char *key;
	const char *crypto;

	switch (sec & WSEC_SEC_MASK) {
	case WSEC_NONE:
		mode = "none";
		break;
	case WSEC_WEP:
		mode = "WEP";
		break;
	case WSEC_WPA:
		mode = "WPA";
		break;
	case WSEC_WPA2:
		mode = "WPA2";
		break;
	default:
		mode = "unknown";
		break;
	}
	key = (sec & WSEC_PSK) ? " Personal" : "";
	if ((sec & (WSEC_CCMP | WSEC_TKIP)) == (WSEC_CCMP | WSEC_TKIP)) {
		crypto = " Mixed";
	} else if (sec & WSEC_TKIP) {
		crypto = " TKIP";
	} else if (sec & (WSEC_CCMP | WSEC_AES)) {
		crypto = " AES";
	} else {
		crypto = "";
	}
	snprintf(buf, sizeof(buf), "%s%s%s", mode, key, crypto);
	return buf;
}

/*
 * Return JSON object containing a single scan result
 */
static json_t *wifi_interface_scan_export(const struct wifi_scan_result *scan)
{
	json_t *obj;
	char *secu;
	char *src[] = {"none", "unknown"};

	obj = json_object();

	json_object_set_new(obj, "ssid", wifi_ssid_obj(&scan->ssid));
	json_object_set_new(obj, "type",
	    json_string(wifi_bss_type_names[scan->type]));
	json_object_set_new(obj, "chan", json_integer(scan->chan));
	json_object_set_new(obj, "signal", json_integer(scan->signal));
	json_object_set_new(obj, "bars", json_integer(wifi_bars(scan->signal)));

	secu = (char *)wifi_sec_export(
	    wifi_scan_get_best_security(scan, NULL));
	if (!strncmp(secu, src[0], strlen(src[0]))) {
		secu[0] = 'N';
	} else if (!strncmp(secu, src[1], strlen(src[1]))) {
		secu[0] = 'U';
	}
	json_object_set_new(obj, "security", json_string(secu));

	json_object_set_new(obj, "bssid", wifi_bssid_obj(&scan->bssid));

	return obj;
}

/*
 * Return JSON object with a single history entry
 */
static json_t *wifi_interface_history_export(const struct wifi_history *hist)
{
	char ssid_info[3] = { 0 };
	json_t *obj;
	json_t *array;
	int i;

	obj = json_object();
	array = json_array();
	if (hist->ssid.len > 0) {
		ssid_info[0] = hist->ssid.val[0];
		if (hist->ssid.len > 1) {
			ssid_info[1] = hist->ssid.val[hist->ssid.len - 1];
		}
	}

	for (i = 0; i < hist->dns_servers.num; ++i) {
		json_array_append_new(array,
		    wifi_ipaddr_obj(&hist->dns_servers.addrs[i].sin_addr));
	}
	json_object_set_new(obj, "ssid_info", json_string(ssid_info));
	json_object_set_new(obj, "ssid_len", json_integer(hist->ssid.len));
	json_object_set_new(obj, "bssid", wifi_bssid_obj(&hist->bssid));
	json_object_set_new(obj, "error", json_integer(hist->error));
	json_object_set_new(obj, "msg", json_string(wifi_errors[hist->error]));
	json_object_set_new(obj, "mtime", json_integer(hist->time_ms));
	json_object_set_new(obj, "last", json_integer(hist->last ? 1 : 0));
	json_object_set_new(obj, "ip_addr", wifi_ipaddr_obj(&hist->ip_addr));
	json_object_set_new(obj, "netmask", wifi_ipaddr_obj(&hist->netmask));
	json_object_set_new(obj, "default_route",
	    wifi_ipaddr_obj(&hist->def_route));
	json_object_set_new(obj, "dns_servers", array);

	return obj;
}

/*
 * Return JSON object with a single Wi-Fi profile
 */
static json_t *wifi_interface_prof_export(struct wifi_profile *prof)
{
	json_t *obj;

	obj = json_object();
	if (!obj) {
		return obj;
	}
	json_object_set_new(obj, "ssid", wifi_ssid_obj(&prof->ssid));
	/* only export profiles' security and key info */
	json_object_set_new(obj, "security",
	    json_string(wifi_sec_export(
	    prof->sec & (WSEC_SEC_MASK | WSEC_PSK | WSEC_VALID))));

	return obj;
}

/*
 * Send a message with a JSON body.  If resp_info_ptr is valid, the message
 * will be sent as a response to the original request.
 */
static enum amsg_err wifi_interface_send_json(struct amsg_endpoint *endpoint,
	struct amsg_resp_info **resp_info_ptr, uint8_t interface, uint8_t type,
	const json_t *json)
{
	if (resp_info_ptr && *resp_info_ptr) {
		return msg_send_json_resp(resp_info_ptr, interface, type, json);
	} else if (!endpoint) {
		log_err("null endpoint");
		return AMSG_ERR_APPLICATION;
	}
	return msg_send_json(endpoint, interface, type, json, NULL, NULL,
	    MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Send a Wi-Fi error response message.
 */
static enum amsg_err wifi_interface_send_err_resp(
	struct amsg_resp_info *resp_info, enum wifi_error error)
{
	json_t *msg_obj;
	enum amsg_err err;

	log_debug("send error: %d: %s", error, wifi_errors[error]);

	msg_obj = json_object();
	json_object_set_new(msg_obj, "wifi_error", json_integer(error));
	err = msg_send_json_resp(&resp_info, MSG_INTERFACE_WIFI, MSG_WIFI_ERR,
	    msg_obj);
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a request for scan results
 */
static enum amsg_err wifi_interface_scan_results(struct amsg_endpoint *endpoint,
	struct amsg_resp_info *resp_info)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_scan_result *scan;
	json_t *root;
	json_t *obj;
	json_t *array;
	enum amsg_err err;

	/* Defer response for first request if scan is in progress */
	if (wifi_platform_scanning()) {
		if (pending_scan_resp) {
			log_debug("scan results already requested");
			return AMSG_ERR_INTERRUPTED;
		}
		pending_scan_resp = amsg_alloc_async_resp_info(resp_info);
		return AMSG_ERR_NONE;
	}
	root = json_object();
	obj = json_object();
	array = json_array();

	for (scan = wifi->scan; scan < &wifi->scan[WIFI_SCAN_CT]; scan++) {
		if (!scan->time_ms) {
			continue;
		}
		json_array_append_new(array,
		    wifi_interface_scan_export(scan));
	}
	json_object_set_new(root, "wifi_scan", obj);
	json_object_set_new(obj, "mtime", json_integer(wifi->scan_time_ms));
	json_object_set_new(obj, "results", array);

	log_debug("sending scan results");
	/* Clear deferred response info */
	if (resp_info == pending_scan_resp) {
		pending_scan_resp = NULL;
	}
	err = wifi_interface_send_json(endpoint, &resp_info,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SCAN_RESULTS, root);
	json_decref(root);
	return err;
}

/*
 * Callback invoked when scan has completed.  Replies to deferred
 * scan results request.
 */
void wifi_interface_notify_scan_complete(void)
{
	if (pending_scan_resp) {
		wifi_interface_scan_results(NULL, pending_scan_resp);
	}
}

/*
 * Handle a request for Wi-Fi status
 */
static enum amsg_err wifi_interface_status(struct amsg_endpoint *endpoint,
	struct amsg_resp_info *resp_info)
{
	struct wifi_state *wifi = &wifi_state;
	const struct wifi_profile *prof;
	const struct wifi_scan_result *scan;
	const struct wifi_history *hist;
	const char *state;
	const char *wps_state;
	json_t *root;
	json_t *obj;
	json_t *array = NULL;
	enum amsg_err err;

	root = json_object();
	obj = json_object();

	prof = wifi->curr_profile;
	if (prof && (!prof->enable || wifi->state < WS_JOIN)) {
		prof = NULL;
	}
	scan = prof ? prof->scan : NULL;

	switch (wifi->state) {
	case WS_DISABLED:
		state = "disabled";
		break;
	case WS_JOIN:
		state = "wifi_connecting";
		break;
	case WS_DHCP:
		state = "network_connecting";
		break;
	case WS_WAIT_CLIENT:
		state = "cloud_connecting";
		break;
	case WS_UP:
		state = "up";
		break;
	default:
		state = "down";
		break;
	}

	switch (wifi->wps_state) {
	case WPS_SCAN:
		wps_state = "in progress";
		break;
	case WPS_SUCCESS:
		wps_state = "ok";
		break;
	case WPS_ERR:
		wps_state = "fail";
		break;
	default:
		wps_state = "none";
	}

	for (hist = wifi->hist; hist < &wifi->hist[WIFI_HIST_CT]; ++hist) {
		if (!hist->time_ms) {
			break;
		}
		if (!array) {
			array = json_array();
		}
		json_array_append_new(array,
		    wifi_interface_history_export(hist));
	}
	if (!array) {
		array = json_null();
	}

	json_object_set_new(root, "wifi_status", obj);
	json_object_set_new(obj, "state", json_string(state));
	json_object_set_new(obj, "connect_history", array);
	json_object_set_new(obj, "connected_ssid",
	    prof ? wifi_ssid_obj(&prof->ssid) : json_string(""));
	/* json_object_set_new(obj, "ant", json_integer(0)); */ /* optional */
	json_object_set_new(obj, "wps", json_string(wps_state));
	json_object_set_new(obj, "rssi",
	    json_integer(scan ? scan->signal : WIFI_SIGNAL_NONE));
	json_object_set_new(obj, "bars",
	    json_integer(scan ? wifi_bars(scan->signal) : WIFI_BARS_MIN));

	log_debug("sending status");

	err = wifi_interface_send_json(endpoint, &resp_info,
	    MSG_INTERFACE_WIFI, MSG_WIFI_STATUS, root);
	json_decref(root);
	return err;
}

/*
 * Handle a request for Wi-Fi info
 */
static enum amsg_err wifi_interface_info(struct amsg_endpoint *endpoint,
	struct amsg_resp_info *resp_info)
{
	struct wifi_state *wifi = &wifi_state;
	const struct wifi_profile *prof;
	bool ap_enabled;
	json_t *root;
	json_t *features;
	enum amsg_err err;

	prof = wifi->curr_profile;
	if (prof && (!prof->enable || wifi->state <= WS_JOIN)) {
		prof = NULL;
	}

	root = json_object();
	features = json_array();

	if (wifi->simultaneous_ap_sta) {
		/* Simultaneous AP and station mode */
		json_array_append_new(features, json_string("ap-sta"));
	}
	/* Secure Wi-Fi setup */
	json_array_append_new(features, json_string("rsa-ke"));

	/*
	 * TODO advertise WPS after functionality verified.
	 * json_array_append_new(features, json_string("wps"));
	 */

	json_object_set_new(root, "features", features);
	json_object_set_new(root, "enabled", json_boolean(wifi->enable));
	json_object_set_new(root, "connected_ssid",
	    prof ? wifi_ssid_obj(&prof->ssid) : json_string(""));
	ap_enabled = wifi_platform_ap_enabled();
	json_object_set_new(root, "ap_enabled", json_boolean(ap_enabled));

	log_debug("sending Wi-Fi info");

	err = wifi_interface_send_json(endpoint, &resp_info,
	    MSG_INTERFACE_WIFI, MSG_WIFI_INFO, root);
	json_decref(root);
	return err;
}

/*
 * Send a Wi-Fi info update.
 */
void wifi_interface_notify_info_updated(void)
{
	if (devd.endpoint.sock != -1) {
		wifi_interface_info(&devd.endpoint, NULL);
	}
}

/*
 * Handle a request to start a scan.
 */
static enum amsg_err wifi_interface_scan_start(const struct amsg_msg_info *info)
{
	struct wifi_state *wifi = &wifi_state;
	json_t *msg_obj = NULL;
	const char *ssid_str;
	enum amsg_err err = AMSG_ERR_NONE;

	if (!info->payload_size) {
		goto start_scan;
	}
	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	/* Message may contain hidden SSID to scan for */
	ssid_str = json_get_string(msg_obj, "ssid");
	if (ssid_str) {
		if (wifi_decode_ssid(&wifi->scan4, ssid_str) < 0 ||
		    !wifi->scan4.len) {
			log_err("invalid SSID: %s", ssid_str);
			err = AMSG_ERR_DATA_CORRUPT;
			goto error;
		}
		log_debug("scan4 %s", wifi_ssid_to_str(&wifi->scan4));
	}
start_scan:
	wifi_scan();
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a request to connect to an AP
 */
static enum amsg_err wifi_interface_connect(const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_scan_result *scan;
	struct wifi_profile *prof;
	enum wifi_sec sec;
	struct wifi_ssid ssid = { 0 };
	struct ether_addr bssid = { { 0 } };
	struct wifi_key key = { 0 };
	bool test_connect = false;
	bool hidden = false;
	const char *str;
	ssize_t rc;
	json_t *msg_obj;
	enum wifi_error wifi_err = WIFI_ERR_NONE;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		goto invalid;
	}
	/* Test connection only */
	json_get_bool(msg_obj, "test", &test_connect);
	/* SSID */
	str = json_get_string(msg_obj, "ssid");
	if (str && wifi_decode_ssid(&ssid, str) < 0) {
		goto invalid;
	}
	/* BSSID */
	str = json_get_string(msg_obj, "bssid");
	if (str && !ether_aton_r(str, &bssid)) {
		goto invalid;
	}
	/* Key */
	str = json_get_string(msg_obj, "key");
	if (str && wifi_decode_key(&key, str) < 0) {
		goto invalid;
	}
	/* Hidden */
	json_get_bool(msg_obj, "hidden", &hidden);

	/* BSSID is the most specific, so check that first */
	if (!EMPTY_HWADDR(bssid)) {
		scan = wifi_scan_lookup_bssid(wifi, &bssid);
	} else if (ssid.len) {
		scan = wifi_scan_lookup_ssid(wifi, &ssid);
	} else {
		log_err("missing SSID/BSSID");
		goto invalid;
	}
	if (!scan) {
		log_err("station not found in scan: %s", ssid.len ?
		    wifi_ssid_to_str(&ssid) : net_ether_to_str(&bssid));
		wifi_err = WIFI_ERR_NOT_FOUND;
		goto failed;
	}

	/* Validate key */
	sec = wifi_scan_get_best_security(scan, NULL);
	if (SEC_MATCH(sec, WSEC_NONE)) {
		/* No security available, so make sure the key is empty */
		if (key.len) {
			log_warn("no security available for selected network: "
			    "ignoring key");
			key.len = 0;
		}
	} else {
		if (!key.len) {
			log_err("missing key");
			wifi_err = WIFI_ERR_INV_KEY;
			goto failed;
		}
		/* Convert WEP key hex string to bytes */
		if (SEC_MATCH(sec, WSEC_WEP)) {
			rc = hex_parse_n(key.val, sizeof(key.val),
			    (const char *)key.val, key.len, NULL);
			if (rc < 0) {
				log_warn("failed to parse WEP hex key");
				wifi_err = WIFI_ERR_INV_KEY;
				goto failed;
			}
			key.len = rc;
		}
		/* Re-check security, this time validating the key */
		sec = wifi_scan_get_best_security(scan, &key);
		if (SEC_MATCH(sec, WSEC_NONE)) {
			log_err("key not valid for available security modes");
			wifi_err = WIFI_ERR_INV_KEY;
			goto failed;
		}
	}
	if (test_connect) {
		log_debug("%s connect test passed", wifi_ssid_to_str(&ssid));
		goto cleanup;
	}
	/* Find or add profile */
	prof = wifi_prof_add(&scan->ssid, sec, &key, &wifi_err);
	if (!prof) {
		goto failed;
	}
	prof->scan = scan;
	prof->hidden = hidden;
	log_debug("SSID='%s' BSSID=%s key_len=%u sec=%04x",
	    wifi_ssid_to_str(&ssid), net_ether_to_str(&prof->scan->bssid),
	    key.len, (unsigned)sec);

failed:
	if (wifi_err != WIFI_ERR_NONE) {
		if (scan) {
			wifi_hist_new(&scan->ssid, &scan->bssid, wifi_err,
			    true);
		} else {
			wifi_hist_new(&ssid, NULL, wifi_err, true);
		}
		if (resp_info) {
			/* Send connect error response */
			err = wifi_interface_send_err_resp(resp_info, wifi_err);
		}
		goto cleanup;
	}

	/* Set preferred profile and associate with it */
	wifi_connect(prof);
	goto cleanup;

invalid:
	err = AMSG_ERR_DATA_CORRUPT;
cleanup:
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a request to start WPS
 */
static enum amsg_err wifi_interface_wps_start(void)
{
	if (wifi_wps_pbc() < 0) {
		return AMSG_ERR_APPLICATION;
	}
	return AMSG_ERR_NONE;
}

/*
 * Handle a request to delete a profile
 */
static enum amsg_err wifi_interface_profile_delete(
	const struct amsg_msg_info *info, struct amsg_resp_info *resp_info)
{
	struct wifi_ssid ssid;
	const char *ssid_str;
	json_t *msg_obj;
	enum wifi_error wifi_err;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	ssid_str = json_get_string(msg_obj, "ssid");
	if (!ssid_str) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	if (wifi_decode_ssid(&ssid, ssid_str) < 0 || !ssid.len) {
		log_err("invalid SSID: %s", ssid_str);
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}

	log_debug("ssid=\"%s\"", wifi_ssid_to_str(&ssid));

	if (wifi_prof_delete(&ssid, &wifi_err) < 0) {
		if (resp_info) {
			err = wifi_interface_send_err_resp(resp_info, wifi_err);
		} else {
			err = AMSG_ERR_APPLICATION;
		}
	}
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a request for configured Wi-Fi profiles
 */
static enum amsg_err wifi_interface_profiles(struct amsg_endpoint *endpoint,
	struct amsg_resp_info *resp_info)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_profile *prof;
	json_t *root;
	json_t *profs;
	enum amsg_err err;

	root = json_object();
	profs = json_array();
	json_object_set_new(root, "wifi_profiles", profs);

	for (prof = wifi->profile; prof < &wifi->profile[WIFI_PROF_CT];
	    prof++) {
		if (!prof->enable) {
			continue;
		}
		json_array_append_new(profs, wifi_interface_prof_export(prof));
	}
	log_debug("sending profiles");

	err = wifi_interface_send_json(endpoint, &resp_info,
	    MSG_INTERFACE_WIFI, MSG_WIFI_PROFILE_LIST, root);
	json_decref(root);
	return err;
}

/*
 * Handle "print" remote CLI command.
 */
static int wifi_interface_cli_print(const char *cmd, size_t argc,
	char **argv, struct queue_buf *output)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_profile *prof;
#if WIFI_HIST_CT > 0
	struct wifi_history *hist;
#endif /* WIFI_HIST_CT */
	struct wifi_scan_result *scan;
	struct net_dnsservers dns_servers;
	int i;

	prof = wifi->curr_profile;

	switch (wifi->state) {
	case WS_DISABLED:
		queue_buf_putf(output, "Wi-Fi disabled\n");
		break;
	case WS_SELECT:
		queue_buf_putf(output, "Wi-Fi selecting network\n");
		break;
	case WS_IDLE:
		if (wifi_platform_ap_enabled()) {
			queue_buf_putf(output, "Wi-Fi AP mode ssid %s\n",
			    wifi_ssid_to_str(&wifi->ap_profile.ssid));
		} else {
			queue_buf_putf(output, "Wi-Fi idle\n");
		}

		break;
	case WS_JOIN:
		queue_buf_putf(output, "Wi-Fi associating with SSID %s\n",
		    prof ? wifi_ssid_to_str(&prof->ssid) : "");
		break;
	case WS_DHCP:
	case WS_WAIT_CLIENT:
	case WS_UP:
		queue_buf_putf(output, "Wi-Fi associated with SSID %s\n",
		    prof ? wifi_ssid_to_str(&prof->ssid) : "");
		switch (wifi->state) {
		case WS_DHCP:
			queue_buf_putf(output, "Wi-Fi waiting for DHCP\n");
			break;
		case WS_WAIT_CLIENT:
			queue_buf_putf(output, "Wi-Fi waiting for ADS\n");
			break;
		default:
			break;
		}
		if (prof && prof->scan) {
			queue_buf_putf(output, "RSSI %d antenna %d\n",
			    prof->scan->signal, 0);
		}
		break;
	case WS_ERR:
		queue_buf_putf(output, "Wi-Fi cond error detected\n");
		break;
	}

	queue_buf_putf(output, "Scan state: ");
	if (wifi_platform_scanning()) {
		queue_buf_putf(output, "scanning\n");
	} else {
		queue_buf_putf(output, "idle\n");
	}

	if (wifi->if_info.flags & IFF_UP) {
		queue_buf_putf(output, "IP %s\n",
		    inet_ntoa(wifi->if_info.addr.sin_addr));
		if (!net_get_dnsservers(&dns_servers)) {
			for (i = 0; i < dns_servers.num; ++i) {
				queue_buf_putf(output, "DNS %s\n",
				    inet_ntoa(dns_servers.addrs[i].sin_addr));
			}
		}
	} else {
		queue_buf_putf(output, "Network interface down");
	}

	queue_buf_putf(output, "\nProfiles:\n%-5s %-24s %-20s\n",
	    "Index", "SSID", "Security");
	for (i = 0, prof = wifi->profile;
	    i < WIFI_PROF_CT; prof++, i++) {
		if (prof->ssid.len == 0) {
			continue;
		}
		queue_buf_putf(output, "%-5d %-24s %-20s %s\n",
		    i, wifi_ssid_to_str(&prof->ssid),
		    wifi_sec_export(prof->sec), prof->enable ? "" : "disabled");
	}
	prof = &wifi->ap_profile;
	queue_buf_putf(output, "%-5s %-24s %-20s %s\n", "AP",
	    wifi_ssid_to_str(&prof->ssid), wifi_sec_export(prof->sec),
	    prof->enable ? "" : "disabled");

	queue_buf_putf(output, "\nScan Results:\n%-24s %-4s %-6s %-20s\n",
	    "SSID", "Chan", "Signal", "Security");
	for (scan = wifi->scan; scan < &wifi->scan[WIFI_SCAN_CT]; scan++) {
		if (scan->ssid.len == 0) {
			continue;
		}
		queue_buf_putf(output, "%-24s %4u %6d %-20s%s%s\n",
		    wifi_ssid_to_str(&scan->ssid),
		    scan->chan, scan->signal,
		    wifi_sec_export(wifi_scan_get_best_security(scan, NULL)),
		    scan->wps_supported ? " (WPS)" : "",
		    scan->time_ms == BT_AD_HOC ? " (Ad hoc)" : "");
	}

#if WIFI_HIST_CT > 0
	queue_buf_putf(output, "\nConnection History:\n"
	    "%-10s %-20s %-17s %-15s %s\n",
	    "Time", "SSID", "BSSID", "Address", "Status");
	hist = wifi->curr_hist ? wifi->curr_hist + 1 : wifi->hist;
	for (hist = wifi->hist; hist < &wifi->hist[WIFI_HIST_CT]; ++hist) {
		if (!hist->time_ms) {
			break;
		}
		queue_buf_putf(output,
		    "%-10llu %-20s %-17s %-15s %2d: %s\n",
		    (long long unsigned)hist->time_ms,
		    wifi_ssid_to_str(&hist->ssid),
		    net_ether_to_str(&hist->bssid),
		    inet_ntoa(hist->ip_addr),
		    hist->error, wifi_errors[hist->error]);
	}
#endif /* WIFI_HIST_CT */
	return 0;
}

/*
 * Handle a request to open an AP mode window.
 */
static enum amsg_err wifi_interface_ap_window_open(void)
{
	log_debug("open AP window");
	wifi_ap_window_start();
	return AMSG_ERR_NONE;
}

/*
 * Handle a request to disable AP mode.
 */
static enum amsg_err wifi_interface_ap_stop(struct amsg_resp_info *resp_info)
{
	log_debug("stop AP mode");
	/* Only accepted when AP and station are enabled */
	if (!wifi_platform_ap_enabled() || !wifi_platform_station_enabled()) {
		return AMSG_ERR_PRIVS;
	}
	wifi_ap_mode_stop();
	return AMSG_ERR_NONE;
}

/*
 * Handle a system DHCP client event update.
 */
static enum amsg_err wifi_interface_dhcp_event(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_system_dhcp *msg = (struct msg_system_dhcp *)info->payload;
	struct wifi_state *wifi = &wifi_state;

	/* Ignore events specifically for a different network interface */
	if (msg->interface[0] && strcmp(wifi->ifname, msg->interface)) {
		log_debug("ignoring DHCP event for %s", msg->interface);
		return AMSG_ERR_NONE;
	}
	switch (msg->event) {
	case MSG_SYSTEM_DHCP_UNBOUND:
		log_debug("DHCP unbound (%s)", wifi->ifname);
		wifi_net_update(false);
		break;
	case MSG_SYSTEM_DHCP_BOUND:
		log_debug("DHCP bound (%s)", wifi->ifname);
		wifi_net_update(true);
		break;
	case MSG_SYSTEM_DHCP_REFRESH:
		log_debug("DHCP refresh (%s)", wifi->ifname);
		wifi_net_update(false);
		wifi_net_update(true);
		break;
	}
	return AMSG_ERR_NONE;
}

/*
 * Handle an Ayla client network destination update.
 */
static enum amsg_err wifi_interface_client_dests(
	const struct amsg_msg_info *info)
{
	struct msg_client_dests *msg = (struct msg_client_dests *)info->payload;

	log_debug("cloud %s", msg->cloud_up ? "up" : "down");
	wifi_cloud_update(msg->cloud_up);
	return AMSG_ERR_NONE;
}

/*
 * Handle a request to a perform a factory reset
 */
static enum amsg_err wifi_interface_factory_reset(
	const struct amsg_endpoint *endpoint)
{
	log_debug("factory reset");
	if (wifi_factory_reset() < 0) {
		return AMSG_ERR_APPLICATION;
	}
	return AMSG_ERR_NONE;
}

/*
 * Get dsn
 */
char *wifi_get_dsn(void)
{
	return wifi_state.dsn;
}

#define COND_INTERFACE_REDO_DELAY_MS	1000
static char setup_token[WIFI_SETUP_TOKEN_LEN + 1];
static struct timer token_resp_timer;

/*
 * Send setup_token to devd
 */
int wifi_interface_send_setup_token(void)
{
	json_t *root;
	enum amsg_err err;

	root = json_object();
	json_object_set_new(root, "setup_token",
	    json_string(setup_token));
	err = wifi_interface_send_json(&devd.endpoint, NULL,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SETUP_TOKEN, root);
	json_decref(root);

	log_debug("err %d", err);

	if (err == AMSG_ERR_NONE) {
		return 0;
	} else {
		return -1;
	}
}

/*
 * Set setup_token
 */
void wifi_set_setup_token(char *token)
{
	if (!strcmp(setup_token, token)) {
		log_debug("same setup token %s", setup_token);
		return;
	}

	log_debug("setup token %s", token);

	memset(setup_token, 0, sizeof(setup_token));
	memcpy(setup_token, token, WIFI_SETUP_TOKEN_LEN);

	wifi_timer_set(&token_resp_timer, 0);
}

/*
 * Get setup token response timeout handler.
 */
static void wifi_interface_token_resp_timeout(struct timer *timer)
{
	if (wifi_interface_send_setup_token() < 0) {
		wifi_timer_set(&token_resp_timer,
		    COND_INTERFACE_REDO_DELAY_MS);
	}
}

/*
 * Handle a DSN message from devd
 */
static enum amsg_err wifi_setup_token_resp_recv(void)
{
	log_debug("received setup_token_resp from devd");
	wifi_timer_clear(&token_resp_timer);
	return AMSG_ERR_NONE;
}

/*
 * Message interface handler for the Wi-Fi interface.
 */
static enum amsg_err wifi_interface_msg_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_WIFI);

	switch (info->type) {
	case MSG_WIFI_INFO_REQ:
		return wifi_interface_info(endpoint, resp_info);
	case MSG_WIFI_AP_WINDOW_OPEN:
		return wifi_interface_ap_window_open();
	case MSG_WIFI_AP_STOP:
		return wifi_interface_ap_stop(resp_info);
	case MSG_WIFI_STATUS_REQ:
		return wifi_interface_status(endpoint, resp_info);
	case MSG_WIFI_PROFILE_ADD:
		return AMSG_ERR_TYPE_UNSUPPORTED;
	case MSG_WIFI_PROFILE_DELETE:
		return wifi_interface_profile_delete(info, resp_info);
	case MSG_WIFI_PROFILE_LIST_REQ:
		return wifi_interface_profiles(endpoint, resp_info);
	case MSG_WIFI_SCAN_START:
		return wifi_interface_scan_start(info);
	case MSG_WIFI_SCAN_RESULTS_REQ:
		return wifi_interface_scan_results(endpoint, resp_info);
	case MSG_WIFI_CONNECT:
		return wifi_interface_connect(info, resp_info);
	case MSG_WIFI_WPS_PBC:
		return wifi_interface_wps_start();
	case MSG_WIFI_SETUP_TOKEN_RESP:
		return wifi_setup_token_resp_recv();
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Message interface handler for the System interface.
 */
static enum amsg_err wifi_interface_system_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_SYSTEM);

	switch (info->type) {
	case MSG_SYSTEM_DHCP_EVENT:
		return wifi_interface_dhcp_event(endpoint, info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Message interface handler for the Client interface.
 */
static enum amsg_err wifi_interface_client_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_CLIENT);

	switch (info->type) {
	case MSG_CLIENT_DESTS:
		return wifi_interface_client_dests(info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

static void wifi_interface_conf_dsn_resp(struct amsg_endpoint *endpoint,
	enum amsg_err err, const char *path, json_t *val)
{
	struct wifi_state *wifi = &wifi_state;
	const char *dsn;

	if (err != AMSG_ERR_NONE) {
		goto error;
	}
	dsn = json_string_value(val);
	if (!dsn) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	if (wifi->dsn) {
		if (!strcmp(wifi->dsn, dsn)) {
			return;
		}
		free(wifi->dsn);
	}
	wifi->dsn = strdup(dsn);
	log_debug("loaded DSN from client: %s", dsn);
	/* Reload config to apply DSN to AP SSID, if needed */
	conf_load();
	return;
error:
	log_err("failed to load DSN: %s", amsg_err_string(err));
}

/*
 * Handler for devd connection events.
 */
static int wifi_interface_msg_client_event(struct amsg_endpoint *endpoint,
	enum amsg_endpoint_event event)
{
	switch (event) {
	case AMSG_ENDPOINT_CONNECT:
		/* Send app identification as soon as connection established */
		if (msg_send_app_info(endpoint, MSG_APP_NAME_WIFI) < 0) {
			amsg_disconnect(endpoint);
			return -1;
		}
		/* Send Wi-Fi feature flags */
		wifi_interface_info(endpoint, NULL);
		/* Request cloud status */
		amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_DESTS_REG,
		    NULL, 0, NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
		/* Fetch DSN, to support AP SSIDs using the DSN */
		msg_conf_get(endpoint, "id/dsn", wifi_interface_conf_dsn_resp,
		    false);
		break;
	case AMSG_ENDPOINT_DISCONNECT:
		wifi_timer_set(&reconnect_timer,
		    COND_INTERFACE_RECONNECT_DELAY_MS);
		break;
	}
	return 0;
}

/*
 * Connect/reconnect handler.
 */
static void wifi_interface_reconnect_timeout(struct timer *timer)
{
	if (amsg_client_connect(&devd, devd_msg_sock_path) < 0) {
		wifi_timer_set(&reconnect_timer,
		    COND_INTERFACE_RECONNECT_DELAY_MS);
	}
}

/*
 * Remote config access privileges.
 */
static const struct msg_conf_privs conf_privs_table[] = {
	{ "wifi/enable",		MSG_CONF_ALL },
};

/*
 * Remote CLI command handlers.
 */
static const struct msg_cli_cmd cli_cmd_table[] = {
	{ "print", wifi_interface_cli_print }
};

/*
 * Configures and starts a messaging server for incoming connections, then
 * connects to devd's server.
 */
void wifi_interface_init(void)
{
	struct cond_state *cond = &cond_state;

	wifi_timer_init(&reconnect_timer, wifi_interface_reconnect_timeout);

	amsg_server_init(&msg_server, &cond->file_events, &cond->timers);
	amsg_client_init(&devd, &cond->file_events, &cond->timers);

	amsg_server_set_max_sessions(&msg_server, COND_MSG_SERVER_MAX_SESSIONS);
	amsg_client_set_event_callback(&devd, wifi_interface_msg_client_event);

	/* Register message interface handlers */
	amsg_set_interface_handler(MSG_INTERFACE_WIFI,
	    wifi_interface_msg_handler);
	amsg_set_interface_handler(MSG_INTERFACE_SYSTEM,
	    wifi_interface_system_handler);
	amsg_set_interface_handler(MSG_INTERFACE_CLIENT,
	    wifi_interface_client_handler);
	msg_conf_init(conf_privs_table, ARRAY_LEN(conf_privs_table),
	    NULL, wifi_interface_factory_reset);
	msg_cli_init(cli_cmd_table, ARRAY_LEN(cli_cmd_table));

	/* Start a server with user and group permissions */
	amsg_server_start(&msg_server, msg_sock_path, S_IRWXU | S_IRWXG);

	/* Connect to devd immediately */
	wifi_timer_set(&reconnect_timer, 0);

	wifi_timer_init(&token_resp_timer, wifi_interface_token_resp_timeout);
}
