/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#define _GNU_SOURCE	/* for asprintf() */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <curl/curl.h>

#include <arpa/inet.h>
#include <ayla/assert.h>
#include <ayla/nameval.h>
#include <ayla/utypes.h>
#include <ayla/ayla_interface.h>
#include <ayla/conf_io.h>
#include <ayla/clock.h>
#include "libtransformer.h"
#include <inttypes.h>
#include <ayla/json_parser.h>
#include <ayla/base64.h>
#include <ayla/json_interface.h>
#include <ayla/timer.h>
#include <ayla/network_utils.h>
#include <ayla/file_io.h>
#include <ayla/time_utils.h>
#include <ayla/log.h>
#include <ayla/parse.h>
#include <platform/system.h>

#include "ds.h"
#include "ds_client.h"
#include "dapi.h"
#include "notify.h"
#include "devd_conf.h"
#include "dnss.h"
#include "serv.h"
#include "ops_devd.h"
#include "props_client.h"
#include "props_if.h"
#include "app_if.h"
#include "msg_server.h"

#include "gateway_if.h"
#include "gateway_client.h"


#define REG_WINDOW_MS		120000	/* ms reg window is open for */
#define ADS_PING_RETRY_MS	1000	/* ms before ADS ping retry */
#define ADS_PING_EXP_MAX	5	/* max retry multiplier is 2^5 = 32 */
#define PROP_LOC_URL_LEN	500
#define PROP_FILE_URL_LEN	500
#define CLIENT_RESP_SIZE	4096	/* response buffer size */

#define REQUEST_TIMEOUT_SECS	1200	/* max s for a request to complete */
#define REQUEST_TIMEOUT_PING_SECS 5	/* max s for a ping to complete */
#define REQUEST_TIMEOUT_DEFAULT_SECS 60	/* max s allowed with no throughput */

struct client_lan_reg client_lan_reg[CLIENT_LAN_REGS];

/* Points to new ADS URL, if server changed */
static char *ds_new_ads_host;

/*
 * Maps server hostnames to region for both dev and OEM
 */
struct hostname_info {
	const char *region;		/* unique numeric region code */
	const char *fmt_default;	/* default host printf format string */
	const char *fmt_oem;		/* OEM host printf format string */
	const char *domain;		/* region-specific domain name */
};

/*
 * Update this table to support new regions and server hostnames
 */
static const struct hostname_info server_region_table[] = {
	{ "US", CLIENT_SERVER_HOST_DEFAULT ".%s",
		"%s-%s-device.%s",
		"aylanetworks.com" },
	{ "CN", CLIENT_SERVER_HOST_DEFAULT ".%s",
		"%s-%s-device.%s",
		"ayla.com.cn" },
};

/* First hostname table entry is the default */
static const struct hostname_info *SERVER_HOSTINFO_DEFAULT =
	server_region_table;

/*
 * Lookup an entry in the server_region_table by region code
 * Returns NULL if code is invalid.
 */
static const struct hostname_info *ds_lookup_host(const char *region)
{
	int i;

	if (!region) {
		return NULL;
	}
	for (i = 0; i < ARRAY_LEN(server_region_table); ++i) {
		if (!strcasecmp(region, server_region_table[i].region)) {
			return server_region_table + i;
		}
	}
	return NULL;
}


void ds_json_dump(const char *msg, const json_t *obj)
{
	char *output;

	if (log_debug_enabled()) {
		output = json_dumps(obj, JSON_INDENT(4) | JSON_SORT_KEYS);
		log_base(msg, LOG_AYLA_DEBUG, "\n%s", output);
		free(output);
	}
}

/*
 * Test if echo is needed for an object
 */
int ds_echo_for_prop_is_needed(json_t *prop, int source)
{
	struct device_state *dev = &device;
	u8 echo_dest = dev->dests_avail & ~SOURCE_TO_DEST_MASK(source);

	if (!dev->lan.auto_sync || !echo_dest) {
		return 0;
	}
	/* don't automatically echo props that need explicit ack. They
	 * will be echoed once appd returns success for
	 * the property update.
	 */
	if (json_get_string(prop, "id")) {
		return 0;
	}
	return 1;
}

static void ds_reg_window_timeout(struct timer *timer)
{
	struct device_state *dev = &device;

	log_debug("registration window closed");
	platform_configure_led(dev->cloud_state == DS_CLOUD_UP, dev->registered,
	    false);
}

/*
 * Pop one command off the cached command list from the most recent
 * GET commands response, and execute it.
 */
static void ds_process_cmd(struct device_state *dev)
{
	json_t *elem;
	json_t *cmd;

	if (!dev->commands) {
		return;
	}
	if (!json_array_size(dev->commands)) {
		return;
	}
	elem = json_array_get(dev->commands, 0);
	cmd = json_object_get(elem, "cmd");
	if (!json_is_object(cmd)) {
		log_warn("invalid cached command");
		dev->get_cmds = 1;
		json_array_clear(dev->commands);
		return;
	}
	serv_json_cmd(cmd, NULL);
	json_array_remove(dev->commands, 0);
}

/*
 * Handle commands in the response of GET commands.json
 */
static void ds_parse_cmds(struct device_state *dev, json_t *commands)
{
	json_t *cmds;
	json_t *node_cmds;

	/* Clear cached commands */
	if (!dev->commands) {
		dev->commands = json_array();
	} else {
		json_array_clear(dev->commands);
	}
	/* Parse commands */
	cmds = json_object_get(commands, "cmds");
	if (json_array_size(cmds)) {
		json_array_extend(dev->commands, cmds);
	}

	/* Parse node commands */
	node_cmds = json_object_get(commands, "node_cmds");
	if (json_array_size(node_cmds)) {
		json_array_extend(dev->commands, node_cmds);
	}
	/* Run the first command immediately */
	ds_process_cmd(dev);
}

/*
 * Handle properties in the response of GET commands.json.
 * Custom handlers may be used to override the default behavior.
 */
void ds_parse_props(struct device_state *dev, json_t *commands,
	void (*prop_handler)(void *arg, json_t *props),
	void (*node_prop_handler)(void *arg, json_t *props),
	void *arg)
{
	json_t *properties;
	json_t *schedules;
	json_t *elem;
	json_t *prop;
	int i;
	int prop_size;
	json_t *node_properties;
	json_t *node_schedules;
	/* Parse schedules */
	schedules = json_object_get(commands, "schedules");
	if (json_array_size(schedules)) {
		app_send_sched(schedules, DEST_ADS);
	}

	/* Parse properties */
	properties = json_object_get(commands, "properties");
	prop_size = json_array_size(properties);
	if (prop_size) {
		if (prop_handler) {
			prop_handler(arg, properties);
		} else {
			prop_send_prop_update(properties, SOURCE_ADS);
			for (i = 0; i < prop_size; i++) {
				elem = json_array_get(properties, i);
				prop = json_object_get(elem, "property");
				if (!json_is_object(prop)) {
					continue;
				}
				if (ds_echo_for_prop_is_needed(prop,
				    SOURCE_ADS)) {
					prop_prepare_echo(&device, prop,
					    SOURCE_ADS);
				}
			}
		}
	}

	/* Parse node schedules */
	node_schedules = json_object_get(commands, "node_schedules");
	if (json_array_size(node_schedules)) {
		gateway_process_node_scheds(dev, node_schedules, SOURCE_ADS);
	}

	/* Parse node properties */
	node_properties = json_object_get(commands, "node_properties");
	if (json_array_size(node_properties)) {
		if (node_prop_handler) {
			node_prop_handler(arg, node_properties);
		} else {
			gateway_process_node_update(dev, node_properties,
			    SOURCE_ADS);
		}
	}
}

/*
 * Start polling ADS for commands and property updates.
 */
static int ds_polling_start(struct device_state *dev)
{
	ASSERT(dev->cloud_state == DS_CLOUD_UP);

	if (!dev->poll_interval) {
		log_debug("polling disabled");
		return 0;
	}
	if (dev->poll_ads) {
		return 0;
	}
	log_info("ADS polling started: period %hus", dev->poll_interval);
	dev->poll_ads = 1;
	timer_set(&dev->timers, &dev->poll_ads_timer,
	    dev->poll_interval * 1000);
	return 0;
}

/*
 * Stop polling ADS
 */
static void ds_polling_stop(struct device_state *dev)
{
	if (!dev->poll_ads) {
		return;
	}
	log_info("ADS polling stopped");
	dev->poll_ads = 0;
	timer_cancel(&dev->timers, &dev->poll_ads_timer);
}

/*
 * Poll device commands from ADS.
 */
static void ds_polling_timeout(struct timer *timer)
{
	struct device_state *dev =
	    CONTAINER_OF(struct device_state, poll_ads_timer, timer);

	if (!dev->poll_ads) {
		return;
	}
	log_debug("polling ADS");
	dev->get_cmds = 1;
	ds_step();

	/* Attempt recovery of ANS if server configured */
	if (np_server_is_set()) {
		/* Force refresh ANS DNS info */
		np_clear_dns_info();
	}
	/* Reset polling timer */
	timer_set(&dev->timers, &dev->poll_ads_timer,
	    dev->poll_interval * 1000);
}

/*
 * Stop notifier client.
 */
static void ds_notify_stop(struct device_state *dev)
{
	np_stop();
	dev->np_up = 0;
	dev->np_started = 0;
}

/*
 * Transition to the cloud-up state.  This may only be called from the
 * cloud-init or cloud-update states.
 */
static void ds_cloud_up(struct device_state *dev)
{
	ASSERT(dev->cloud_state == DS_CLOUD_INIT ||
	    dev->cloud_state == DS_CLOUD_UPDATE);

	if (dev->cloud_state == DS_CLOUD_UP) {
		return;
	}
	log_info("ADS connection up");
	dev->cloud_state = DS_CLOUD_UP;
	ds_dest_avail_set(dev->dests_avail | DEST_ADS);
	/* Poll for device commands if notifier is not up */
	if (!dev->np_up) {
		ds_polling_start(dev);
	}
	/* Invoke platform-specific cloud LED callback */
	platform_configure_led(true, dev->registered,
	    timer_active(&dev->reg_window_timer));
	ds_step();
}

/*
 * Transition to the cloud-down state.
 */
static void ds_cloud_down(struct device_state *dev)
{
	if (dev->cloud_state == DS_CLOUD_DOWN) {
		return;
	}
	dev->cloud_state = DS_CLOUD_DOWN;
	log_info("ADS connection down");
	ds_dest_avail_set(dev->dests_avail & ~DEST_ADS);

	/* Cancel any pending requests */
	ds_client_reset(&dev->app_client);
	ds_client_reset(&dev->client);

	ds_notify_stop(dev);
	ds_polling_stop(dev);
	dev->do_ping = 0;
	dev->req_auth_errors = 0;
	/* Application will enable listen again when cloud is up */
	dev->ads_listen = 0;
	/* Invoke platform-specific cloud LED callback */
	platform_configure_led(false, dev->registered,
	    timer_active(&dev->reg_window_timer));
	ds_step();
}

/*
 * Schedule a ping after a short delay.  If no delay is specified, uses the
 * number of failed ping attempts to apply an exponential back-off with
 * random variation.
 */
static void ds_ping_schedule_retry(struct device_state *dev, u32 delay_ms)
{
	u64 cur_delay;
	u32 exp;

	if (!delay_ms) {
		/*
		 * Calculate exponential back-off.  After 4 attempts with a
		 * certain delay, double the retry time.  Timeout increases to
		 * 32 seconds.
		 */
		exp = (dev->ping_attempts >> 2);
		if (exp > ADS_PING_EXP_MAX) {
			exp = ADS_PING_EXP_MAX;
		}
		delay_ms = ADS_PING_RETRY_MS << exp;
		/* Add random offset of up to 50% of delay_ms */
		delay_ms += random() % (delay_ms >> 1);
	}
	cur_delay = timer_delay_get_ms(&dev->ping_ads_timer);
	if (!cur_delay || cur_delay < delay_ms) {
		/* Set or lengthen ping retry timer delay as needed */
		timer_set(&dev->timers, &dev->ping_ads_timer, delay_ms);
		log_debug("schedule ADS ping in %u ms", delay_ms);
	}
}

/*
 * Ping retry timeout.
 */
static void ds_ping_retry(struct timer *timer)
{
	struct device_state *dev;

	dev = CONTAINER_OF(struct device_state, ping_ads_timer, timer);
	if (!dev->do_ping) {
		dev->do_ping = 1;
		ds_step();
	}
}

/*
 * Parse a Ping-specific header.
 */
static void ds_parse_ping(int argc, char **argv, void *arg)
{
	struct device_state *dev = &device;

	if (argc < 1) {
		return;
	}
	if (!strcasecmp(argv[0], "Ping")) {
		dev->ping_ok = 1;
	}
}

/*
 * Parse a Date-specific header.
 */
static void ds_parse_date(int argc, char **argv, void *arg)
{
	struct device_state *dev = &device;
	u32 time;
	int rc;

	if (!dev->update_time) {
		log_debug("No need to update time");
		return;
	}

	rc = parse_http_date(&time, argc, argv);
	if (rc != 0) {
		log_debug("parse_http_date error, argc %d, argv[0] %s"
		    ", argv[1] %s, argv[2] %s, argv[3] %s, argv[4] %s"
		    ", argv[5] %s", argc, argv[0], argv[1], argv[2],
		    argv[3], argv[4], argv[5]);
		return;
	}

	rc = clock_set_time(time, clock_source());
	if (rc != 0) {
		log_err("clock_set_time error, time %u", time);
		return;
	}

	dev->update_time = 0;
	return;
}

/*
 * Ping response header parser table.
 */
const struct http_tag ds_ads_ping_header_tags[] = {
	{ "Text",		ds_parse_ping },
	{ "Date",		ds_parse_date },
	{ NULL }
};

/*
 * ADS ping complete handler.  Note that unlike other request complete handlers,
 * this function explicitly calls ds_cloud_failure() on error conditions.
 * This is necessary, because Ayla auth is not enabled for pings, so
 * ds_client does not automatically handle a cloud failure.
 */
static void ds_ping_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;

	dev->do_ping = 0;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		log_warn("ADS ping failed");
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			ds_cloud_failure(0);
		}
		return;
	}
	if (!dev->ping_ok) {
		log_warn("ADS ping response invalid");
		ds_cloud_failure(0);
		return;
	}
	log_debug("ADS ping succeeded");
	/* Reset ping count */
	dev->ping_attempts = 0;
	/* Initialize cloud session, if not connected */
	if (dev->cloud_state == DS_CLOUD_DOWN) {
		dev->cloud_state = DS_CLOUD_INIT;
	}
	ds_step();
}

/*
 * Send a ping request to ADS to verify connectivity.  Unlike other ADS
 * requests, this request uses HTTP, and does not include an authentication
 * header.
 */
static int ds_ping(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.proto = DS_PROTO_HTTP,
		.method = HTTP_GET,
		.host = dev->ads_host,
		.uri = "ping",
		.timeout_secs = REQUEST_TIMEOUT_PING_SECS
	};

	if (ds_client_busy(&dev->client)) {
		return -1;
	}
	/* Parse ping-specific header */
	if (http_client_set_header_parsers(dev->client.context,
	    ds_ads_ping_header_tags) < 0) {
		log_err("failed to set Ping header parser");
		return -1;
	}
	if (ds_send(&dev->client, &info, ds_ping_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	/* Pinging now, so cancel any scheduled retries */
	timer_cancel(&dev->timers, &dev->ping_ads_timer);
	dev->ping_ok = 0;
	++dev->ping_attempts;
	return 0;
}

/*
 * Update the device key received in the GET dsns response.
 */
static int ds_set_dev_key(json_t *dev_node)
{
	unsigned key;

	if (json_get_uint(dev_node, "key", &key) < 0) {
		log_err("no device key from service");
		return -1;
	}
	snprintf(device.key, sizeof(device.key), "%u", key);
	return 0;
}

/*
 * Update the Cipher Key for ANS.
 */
static int ds_set_cipher_key(json_t *dev_node)
{
	struct device_state *dev = &device;
	size_t key_len;
	char *decode;
	const char *key;

	key = json_get_string(dev_node, DS_CLIENT_ANS_CIPHER_KEY);
	if (!key) {
		log_warn("no ANS cipher key");
		return -1;
	}

	decode = base64_decode(key, strlen(key), &key_len);
	if (!decode) {
		log_warn("base64 decode failed");
		return -1;
	}
	if (key_len < NP_KEY_LEN) {
		log_err("len %zu too short", key_len);
		free(decode);
		return -1;
	}
	dev->cipher_key_len = (key_len > NP_KEY_LEN) ? NP_KEY_LEN : key_len;
	memcpy(dev->cipher_key, decode, dev->cipher_key_len);

	free(decode);

	return 0;
}

/*
 * Update ANS host address.
 */
static int ds_set_ns_host(json_t *dev_node)
{
	const char *host;

	host = json_get_string(dev_node, DS_CLIENT_ANS_SERVER);
	if (!host) {
		log_warn("no ANS host value");
		return -1;
	}
	if (host[0] == '\0') {
		log_info("ANS disabled");
	} else {
		log_info("ANS server: %s", host);
		np_set_server(host);
	}
	return 0;
}

/*
 * Checks the value of the 'node' key for the in json structure.
 * Returns 1 if it doesn't exist or is different from "new"
 */
static bool ds_node_value_is_different(json_t *in, const char *node,
    const char *new)
{
	const char *old;

	old = json_get_string(in, node);
	if (!old || !new || strcmp(old, new)) {
		return true;
	}
	return false;
}

/*
 * Generate a diff for an integer value.
 */
static void ds_update_int(json_t *in, json_t *out, const char *node, int new)
{
	int old;

	if (json_get_int(in, node, &old) < 0 || old != new) {
		json_object_set_new(out, node, json_integer(new));
	}
}

/*
 * Generate a diff for a string value.
 */
static void ds_update_string(json_t *in, json_t *out, const char *node,
    const char *new)
{
	if (ds_node_value_is_different(in, node, new)) {
		json_object_set_new(out, node, json_string(new));
	}
}

/*
 * Create update for any out-of-date info from GET /dsns.
 */
static json_t *ds_update_info(struct device_state *dev, json_t *dev_node)
{
	/*****************************Extract homeware version*****************************************/
	
    char *homeware_version;
    log_debug("Extract homeware version");
    tf_ctx_t *ctx = tf_new_ctx(NULL, 0);
    tf_req_t req = {
        .type = TF_REQ_GPV,
        .u.gpv.path = "uci.version.version.@version[0].version"};
    tf_fill_request(ctx, &req);
    const tf_resp_t *resp;
    while ((resp = tf_next_response(ctx, false)))
    {
        switch (resp->type)
        {
        case TF_RESP_GPV:
            printf("%s%s=%s (%d)\n", resp->u.gpv.partial_path,
                   resp->u.gpv.param, resp->u.gpv.value, resp->u.gpv.ptype);
            printf("\nhomeware sw version: %s\n", resp->u.gpv.value);
            homeware_version = (char *)malloc(1 + strlen(resp->u.gpv.value));
            strcpy(homeware_version, resp->u.gpv.value);
            break;
        case TF_RESP_ERROR:
            puts("** Error reading homeware version**");
            printf("%" PRIu16 ": %s\n", resp->u.error.code, resp->u.error.msg);
            return NULL;
            break;
        default:
            break;
        }
    }
    tf_free_ctx(ctx);

    tf_ctx_t *ctx1 = tf_new_ctx(NULL, 0);
	char *product;
	tf_req_t req1 = {
        .type = TF_REQ_GPV,
        .u.gpv.path = "uci.version.version.@version[0].product"};
    tf_fill_request(ctx1, &req1);
    const tf_resp_t *resp1;
    while ((resp1 = tf_next_response(ctx1, false)))
    {
        switch (resp1->type)
        {
        case TF_RESP_GPV:
            printf("%s%s=%s (%d)\n", resp1->u.gpv.partial_path,
                   resp1->u.gpv.param, resp1->u.gpv.value, resp1->u.gpv.ptype);
            printf("\nhomeware sw product version: %s\n", resp1->u.gpv.value);
            product = (char *)malloc(1 + strlen(resp1->u.gpv.value));
            strcpy(product, resp1->u.gpv.value);
            break;
        case TF_RESP_ERROR:
            puts("** Error reading homeware product version**");
            printf("%" PRIu16 ": %s\n", resp1->u.error.code, resp1->u.error.msg);
            return NULL;
            break;
        default:
            break;
        }
    }
    tf_free_ctx(ctx1);

 	char *homeware_version_extract = strtok(homeware_version, "-");
	printf("%s\n", homeware_version_extract);

   char ayla_new_version_homeware[100];

   strcpy(ayla_new_version_homeware, product);
   strcat(ayla_new_version_homeware, "_");
   strcat(ayla_new_version_homeware, homeware_version_extract);
   strcat(ayla_new_version_homeware, "/");

   strcat(ayla_new_version_homeware, version);
   printf("%s\n", ayla_new_version_homeware);
   /****************************************************************************************/
	json_t *root;
	json_t *update;
	struct ether_addr mac_addr;
	char hw_id[PLATFORM_HW_ID_MAX_SIZE];

	root = json_object();
	update = json_object();
	json_object_set_new(root, "device", update);

	ds_update_int(dev_node, update, DS_API_MAJOR, CLIENT_API_MAJOR);
	ds_update_int(dev_node, update, DS_API_MINOR, CLIENT_API_MINOR);
	//ds_update_string(dev_node, update, DS_CLIENT_SW_VER, version); //David's code

	ds_update_string(dev_node, update, DS_CLIENT_SW_VER, ayla_new_version_homeware); //Added by Saritha for showing HW version on Dashboard
	ds_update_string(dev_node, update, DS_CLIENT_MODEL, CLIENT_MODEL);
	ds_update_string(dev_node, update, DS_CLIENT_LAN_IP,
	    inet_ntoa(dev->lan_ip));
	if (ds_node_value_is_different(dev_node, DS_CLIENT_OEM, dev->oem) ||
	    ds_node_value_is_different(dev_node, DS_CLIENT_OEM_MODEL,
	    dev->oem_model)) {
		json_object_set_new(update, DS_CLIENT_OEM,
		    json_string(dev->oem));
		json_object_set_new(update, DS_CLIENT_OEM_MODEL,
		    json_string(dev->oem_model));
		json_object_set_new(update, DS_CLIENT_OEM_KEY,
		    json_string(dev->oem_key));
	}

	if (!platform_get_mac_addr(&mac_addr)) {
		ds_update_string(dev_node, update, DS_CLIENT_MAC_ADDR,
		    net_ether_to_str(&mac_addr));
	}
	if (!platform_get_hw_id(hw_id, sizeof(hw_id))) {
		ds_update_string(dev_node, update, DS_CLIENT_HW_ID, hw_id);
	}

	/* Update Wi-Fi info */
	ds_update_string(dev_node, update, DS_CLIENT_SETUP,
	    dev->setup_token ? dev->setup_token : "");
	ds_update_string(dev_node, update, DS_CLIENT_LOC,
	    dev->location ? dev->location : "");
	ds_update_string(dev_node, update, DS_CLIENT_SSID,
	    dev->connected_ssid ? dev->connected_ssid : "");

	/*
	 * If nothing needs to be updated, just return.
	 */
	if (json_object_size(update) == 0) {
		json_decref(root);
		return NULL;
	}
	return root;
}

/*
 * Parse response from GET /dsns/<dsn>.json
 */
static int ds_parse_info(struct device_state *dev, json_t *info)
{
	json_t *dev_node;
	json_t *update;
	bool registered = false;
	int time;

	ds_json_dump(__func__, info);

	dev_node = json_object_get(info, "device");
	if (!dev_node || !json_is_object(dev_node)) {
		log_err("no device object");
		return -1;
	}
	/* Update device key */
	if (ds_set_dev_key(dev_node)) {
		return -1;
	}
	/* Update notifier client info */
	if (ds_set_cipher_key(dev_node)) {
		return -1;
	}
	if (ds_set_ns_host(dev_node)) {
		return -1;
	}

	free(dev->template_version);
	dev->template_version = json_get_string_dup(dev_node,
	    "template_version");
	free(dev->regtoken);
	dev->regtoken = json_get_string_dup(dev_node, "regtoken");
	free(dev->prod_name);
	dev->prod_name = json_get_string_dup(dev_node, "product_name");
	free(dev->reg_type);
	dev->reg_type = json_get_string_dup(dev_node, "registration_type");
	json_get_bool(dev_node, "registered", &registered);
	dev->registered = registered;
	platform_configure_led(dev->cloud_state == DS_CLOUD_UP, dev->registered,
	    timer_active(&dev->reg_window_timer));

	/* Send registration info to connected applications */
	msg_server_registration_event(false, NULL, NULL);

	/* Extract other info from get dsns */
	if (!json_get_int(dev_node, "unix_time", &time)) {
		ds_clock_set(time, CS_SERVER);
	} else {
		log_warn("bad unix time from ADS");
	}
	/* Check for device attributes that need to be updated */
	update = ds_update_info(dev, dev_node);
	if (update) {
		json_decref(dev->update_info);
		dev->update_info = update;
		return 1;
	}
	log_debug("device attributes are up to date");
	return 0;
}

/*
 * GET dsns request complete handler.
 */
static void ds_get_dsns_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;
	json_t *dev_info;
	int rc;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			/* Initialization failed, go to cloud down state */
			ds_cloud_failure(0);
		}
		return;
	}
	if (conf_reset) {
		conf_set_new("sys/factory", json_integer(0));
		conf_apply();
		/* Cloud removes all gateway nodes on reset=1 flag */
		gateway_mapping_delete_all();
		conf_save();
		conf_reset = false;
	}
	dev_info = ds_client_data_parse_json(resp_data);
	if (!dev_info) {
		log_err("failed parsing device info");
		return;
	}
	/* Parse GET dsns response.  Returns 1 if an update is needed */
	rc = ds_parse_info(dev, dev_info);
	json_decref(dev_info);
	if (rc < 0) {
		log_err("invalid device info");
		return;
	}
	if (rc > 0) {
		dev->cloud_state = DS_CLOUD_UPDATE;
		ds_step();
	} else {
		ds_cloud_up(dev);
	}
}

/*
 * Make GET dsns request to initialize the device with the cloud, and to
 * fetch device attributes.
 */
static int ds_get_dsns(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.method = HTTP_GET,
		.host = dev->ads_host,
		.uri = "dsns/$DSN.json"
	};

	ASSERT(dev->cloud_state == DS_CLOUD_INIT);

	if (ds_client_busy(&dev->client)) {
		return -1;
	}
	/*
	 * The "sys/factory" config flag used to set conf_reset is now optional,
	 * so ask the config library if it has loaded the factory config file.
	 */
	if (conf_factory_loaded()) {
		conf_reset = true;
	}
	if (ds_test_mode) {
		log_warn("CONNECTING IN TEST MODE TO: %s", dev->ads_host);
	}
	/* Indicate factory config and/or test connection */
	if (conf_reset && ds_test_mode) {
		info.uri_args = "reset=1&test=1";
	} else if (conf_reset) {
		info.uri_args = "reset=1";
	} else if (ds_test_mode) {
		info.uri_args = "test=1";
	}
	if (ds_send(&dev->client, &info, ds_get_dsns_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	/* Stop notifier before updating key and server address */
	ds_notify_stop(dev);
	return 0;
}

/*
 * PUT oem info request complete handler.
 */
static void ds_put_oem_info_done(enum http_client_err err,
		const struct http_client_req_info *info,
		const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;
	json_t *template_info;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		log_err("http request err %d, http status %d",
		    err, info->http_status);
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			/* Update failed, go to cloud down state */
			ds_cloud_failure(0);
		}
		return;
	}

	template_info = ds_client_data_parse_json(resp_data);
	if (!template_info) {
		log_err("failed parsing template info");
		return;
	}

	ds_json_dump(__func__, template_info);

	free(dev->prod_name);
	dev->prod_name = json_get_string_dup(template_info, "product_name");
	free(dev->reg_type);
	dev->reg_type = json_get_string_dup(template_info,
	    "registration_type");

	json_decref(template_info);

	/* update template_version */
	free(dev->template_version);
	dev->template_version = dev->template_version_curr;
	dev->template_version_curr = NULL;

	log_debug("product name %s, registration type %s, template version %s",
	    dev->prod_name, dev->reg_type, dev->template_version);

	if (dev->template_version_next) {
		dev->template_version_curr = dev->template_version_next;
		dev->template_version_next = NULL;
		log_debug("template version %s to %s",
		    dev->template_version, dev->template_version_curr);
	} else {
		dev->template_assoc = 1;
		dev->update_oem_info = 0;
	}

	ds_step();
	return;
}

/*
 * Do a PUT oem info to update device oem attributes in the cloud.
 */
static int ds_put_oem_info(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.method = HTTP_PUT,
		.host = dev->ads_host,
		.uri = "devices/$DEV_KEY/oem_info.json",
	};
	json_t *update_oem;

	if (ds_client_busy(&dev->client)) {
		log_debug("client is busy");
		return -1;
	}

	update_oem = json_object();
	json_object_set_new(update_oem, DS_CLIENT_OEM,
	    json_string(dev->oem));
	json_object_set_new(update_oem, DS_CLIENT_OEM_MODEL,
	    json_string(dev->oem_model));
	json_object_set_new(update_oem, DS_CLIENT_OEM_KEY,
	    json_string(dev->oem_key));
	json_object_set_new(update_oem, DS_CLIENT_TEMPLATE_VER,
	    json_string(dev->template_version_curr));

	if (!ds_client_data_init_json(&info.req_data, update_oem)) {
		log_debug("ds_client_data_init_json failed");
		return -1;
	}

	if (ds_send(&dev->client, &info, ds_put_oem_info_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	return 0;
}

/*
 * Update template version to cloud
 */
void ds_update_template_ver_to_cloud(const char *template_ver)
{
	struct device_state *dev = &device;
	if (dev->template_version_curr) {
		if (!strcmp(dev->template_version_curr, template_ver)) {
			log_debug("template version updating is in progress");
			return;
		} else {
			if (dev->template_version_next) {
				free(dev->template_version_next);
			}
			dev->template_version_next = strdup(template_ver);
			return;
		}
	} else if (dev->template_version) {
		if (!strcmp(dev->template_version, template_ver)) {
			log_debug("cloud template version %s,"
			    " local template %s, no need update",
			    dev->template_version, template_ver);
			dev->template_assoc = 1;
			return;
		}
	}

	log_debug("template version %s to %s",
	    dev->template_version ? dev->template_version : "", template_ver);

	dev->template_version_curr = strdup(template_ver);
	if (!dev->template_version_curr) {
		log_err("malloc memory failed for length %u",
		    strlen(template_ver));
		return;
	}

	dev->update_oem_info = 1;
	ds_step();
	return;
}

/*
 * GET regtoken(dsns) request complete handler.
 */
static void ds_get_regtoken_done(enum http_client_err err,
		const struct http_client_req_info *info,
		const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;
	json_t *dev_info;
	json_t *dev_node;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		log_err("http request err %d, http status %d",
		    err, info->http_status);
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			/* Update failed, go to cloud down state */
			ds_cloud_failure(0);
		}
		return;
	}

	dev_info = ds_client_data_parse_json(resp_data);
	if (!dev_info) {
		log_err("failed parsing device info");
		return;
	}

	dev_node = json_object_get(dev_info, "device");
	if (!dev_node || !json_is_object(dev_node)) {
		log_err("no device object");
		json_decref(dev_info);
		return;
	}

	free(dev->regtoken);
	dev->regtoken = json_get_string_dup(dev_node, "regtoken");

	json_decref(dev_info);

	log_debug("updated regtoken %s", dev->regtoken);
	dev->get_regtoken = 0;

	ds_step();
}

/*
 * Make GET regtoken(dsns) request to initialize the device with the cloud,
 * and to fetch device attributes(regtoken).
 */
static int ds_get_regtoken(struct device_state *dev)
{
	struct ds_client_req_info info = {
	.method = HTTP_GET,
	.host = dev->ads_host,
	.uri = "dsns/$DSN.json"
	};

	log_debug("start to get regtoken, use dsn API");

	if (ds_client_busy(&dev->client)) {
		log_debug("client is busy");
		return -1;
	}

	if (ds_send(&dev->client, &info, ds_get_regtoken_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}

	return 0;
}

/*
 * Get regtoken(dsns) from cloud
 */
void ds_get_regtoken_from_cloud(void)
{
	struct device_state *dev = &device;
	log_debug("set get regtoken flag");
	dev->get_regtoken = 1;
	ds_step();
	return;
}

/*
 * PUT info request complete handler.
 */
static void ds_put_info_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			/* Update failed, go to cloud down state */
			ds_cloud_failure(0);
		}
		return;
	}
	json_decref(dev->update_info);
	dev->update_info = NULL;
	ds_cloud_up(dev);
}

/*
 * Do a PUT info to update device attributes in the cloud.
 */
static int ds_put_info(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.method = HTTP_PUT,
		.host = dev->ads_host,
		.uri = "devices/$DEV_KEY.json",
	};

	ASSERT(dev->cloud_state == DS_CLOUD_UPDATE);

	if (ds_client_busy(&dev->client)) {
		return -1;
	}
	if (!dev->update_info) {
		log_err("nothing to update");
		return -1;
	}
	if (!ds_client_data_init_json(&info.req_data, dev->update_info)) {
		return -1;
	}
	log_debug("%s", (const char *)info.req_data.buf.ptr);
	if (ds_send(&dev->client, &info, ds_put_info_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	return 0;
}

/*
 * PUT registration window request complete handler.
 */
static void ds_reg_window_start_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		return;
	}
	dev->reg_window_start = 0;
	platform_configure_led(dev->cloud_state == DS_CLOUD_UP, dev->registered,
	    true);
	log_debug("registration window open for %u seconds",
	    REG_WINDOW_MS / 1000);
	timer_set(&dev->timers, &dev->reg_window_timer, REG_WINDOW_MS);
	ds_step();
}

/*
 * Open a user registration window on the service
 */
static int ds_reg_window_start(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.method = HTTP_PUT,
		.host = dev->ads_host,
		.uri = "devices/$DEV_KEY/start_reg_window.json",
	};

	ASSERT(dev->cloud_state == DS_CLOUD_UP);

	if (ds_client_busy(&dev->client)) {
		return -1;
	}
	if (ds_send(&dev->client, &info, ds_reg_window_start_done,
	    NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	return 0;
}

/*
 * PUT OTA status request complete handler.
 */
static void ds_ota_status_put_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		return;
	}
	log_debug("OTA status %d sent", dev->ota_status);
	dev->ota_status = 0;
	ds_step();
}

/*
 * PUT OTA status.
 */
static int ds_ota_status_put(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.method = HTTP_PUT,
		.host = dev->ads_host,
		.uri = "devices/$DEV_KEY/ota_failed.json",
	};
	json_t *root;
	json_t *status;
	char *buf;

	ASSERT(dev->cloud_state == DS_CLOUD_UP);

	if (ds_client_busy(&dev->client)) {
		return -1;
	}
	root = json_object();
	status = json_object();
	json_object_set_new(root, "ota-status", status);
	json_object_set_new(status, "status", json_integer(dev->ota_status));
	json_object_set_new(status, "type", json_string("host_mcu"));
	buf = ds_client_data_init_json(&info.req_data, status);
	json_decref(root);
	if (!buf) {
		return -1;
	}
	if (ds_send(&dev->client, &info, ds_ota_status_put_done,
	    NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	return 0;
}

/*
 * GET commands request complete handler.
 */
static void ds_get_cmds_done(enum http_client_err err,
	const struct http_client_req_info *info,
	const struct ds_client_data *resp_data, void *arg)
{
	struct device_state *dev = &device;
	json_t *root;
	json_t *commands;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		return;
	}
	root = ds_client_data_parse_json(resp_data);
	if (!root) {
		log_err("failed to parse commands");
		return;
	}
	commands = json_object_get(root, "commands");
	if (json_is_object(commands)) {
		ds_json_dump(__func__, commands);
		ds_parse_cmds(dev, commands);
		ds_parse_props(dev, commands, NULL, NULL, NULL);
	} else {
		log_err("missing commands object");
	}
	json_decref(root);
	dev->get_cmds = 0;
	/*
	 * A 206 status has a special meaning in the GET cmds response.
	 * It indicates there are queued properties to fetch, and cmds should
	 * be requested again once these properties are processed.
	 */
	dev->par_content = (info->http_status == HTTP_STATUS_PAR_CONTENT);
	ds_step();
}

/*
 * Retrieve pending command list and property updates from ADS
 */
static int ds_get_commands(struct device_state *dev)
{
	struct ds_client_req_info info = {
		.method = HTTP_GET,
		.host = dev->ads_host,
		.uri = "devices/$DEV_KEY/commands.json",
	};

	ASSERT(dev->cloud_state == DS_CLOUD_UP);

	if (ds_client_busy(&dev->client)) {
		return -1;
	}
	if (dev->poll_ads) {
		if (dev->np_up_once) {
			info.uri_args = "polling=1";
		} else {
			info.uri_args = "polling=2";
		}
	}
	if (ds_send(&dev->client, &info, ds_get_cmds_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}
	return 0;
}

/*
 * Notifier client event callback.
 */
static void ds_notify_event(enum notify_event event)
{
	struct device_state *dev = &device;

	switch (event) {
	case NS_EV_CHANGE:
		log_info("ANS change event");
		dev->np_up = 0;
		dev->np_started = 0;
		ds_cloud_init();
		break;
	case NS_EV_DOWN:
	case NS_EV_DOWN_RETRY:
		if (dev->np_up) {
			log_warn("ANS down event");
			if (dev->cloud_state == DS_CLOUD_UP) {
				ds_polling_start(dev);
				ds_ping_schedule_retry(dev, 0);
			}
		} else {
			log_warn("ANS reg/reach fail");
		}
		dev->np_up = 0;
		dev->np_started = 0;
		break;
	case NS_EV_DNS_PASS:
		log_debug("ANS DNS pass event");
		if (dev->np_started) {
			break;
		}
		if (!np_start(dev->cipher_key, dev->cipher_key_len)) {
			dev->np_started = 1;
		}
		break;
	case NS_EV_CHECK:
		log_info("ANS check event");
		/* fall through */
	default:
		dev->np_up = 1;
		dev->np_up_once = 1;
		dev->get_cmds = 1;
		ds_polling_stop(dev);
		break;
	}
	ds_step();
}

/*
 * Execute a hard or factory reset.  Expect this function to immediately
 * reboot the system, so make sure all pending operations are completed
 * before calling it.
 */
static void ds_reset_execute(struct device_state *dev, bool factory)
{
	log_info("%s reset", factory ? "factory" : "hard");

	if (factory) {
		if (conf_factory_reset() < 0) {
			log_err("factory reset failed");
		}
		/* Perform any factory reset actions on system */
		platform_factory_reset();
		dev->factory_reset = 0;
	}
	platform_reset();
	dev->hard_reset = 0;
}

/*
 * Force ADS connection to reset.
 */
static void ds_ads_reconnect(struct device_state *dev)
{
	dev->do_reconnect = 0;
	dev->auth_header[0] = '\0';
	if (dev->net_up) {
		log_debug("reset ADS connection");
		ds_net_down();
		ds_net_up();
	}
}

/*
 * Handle next step required in device connection.
 */
static void ds_step_timeout(struct timer *timer)
{
	struct device_state *dev =
	    CONTAINER_OF(struct device_state, ds_step_timer, timer);

	if (ds_client_busy(&dev->client)) {
		/* return if in middle of completing a request */
		/* only one curl request is outstanding at a time */
		return;
	}
	if (dev->hard_reset || dev->factory_reset) {
		/* Terminate appd on reset, if managed by devd */
		ds_kill_appd();
		/* Perform the factory reset (may not return) */
		ds_reset_execute(dev, dev->factory_reset);
		/* Force reconnect in case platform_reset() not implemented */
		dev->do_reconnect = 1;
		ds_step();
		return;
	}
	if (dev->do_reconnect) {
		ds_ads_reconnect(dev);
		return;
	}
	if (!dev->net_up) {
		/* No connectivity: network interface is down */
		return;
	}
	/* Handle cloud state transitions */
	switch (dev->cloud_state) {
	case DS_CLOUD_DOWN:
		if (dev->do_ping) {
			/* Attempt to contact the service */
			if (ds_ping(dev) < 0) {
				ds_cloud_failure(0);
			}
		}
		break;
	case DS_CLOUD_INIT:
		/* Initialize and get device attributes */
		if (ds_get_dsns(dev) < 0) {
			ds_cloud_failure(0);
		}
		break;
	case DS_CLOUD_UPDATE:
		if (dev->update_info) {
			/* Update device attributes */
			if (ds_put_info(dev) < 0) {
				ds_cloud_failure(0);
			}
		} else {
			dev->cloud_state = DS_CLOUD_UP;
			ds_step();
		}
		break;
	case DS_CLOUD_UP:
		if (dev->do_ping) {
			/* Check connectivity */
			if (ds_ping(dev) < 0) {
				ds_cloud_failure(0);
			}
			break;
		}
		if (dev->update_oem_info) {
			/* Update oem info to cloud */
			if (ds_put_oem_info(dev) < 0) {
				ds_cloud_failure(0);
			}
			break;
		}
		if (dev->reg_window_start) {
			/* Open a push-button user registration window */
			if (ds_reg_window_start(dev) < 0) {
				ds_cloud_failure(0);
			}
			break;
		}
		if (dev->ota_status) {
			/* Send an OTA error status */
			if (ds_ota_status_put(dev) < 0) {
				ds_cloud_failure(0);
			}
			break;
		}
		if (dev->ads_listen) {
			/* Execute cached commands */
			if (json_array_size(dev->commands)) {
				ds_process_cmd(dev);
				break;
			}
			/* Handle notifier event */
			if (dev->get_cmds) {
				if (ds_get_commands(dev) < 0) {
					ds_cloud_failure(0);
				}
				break;
			}
		}
		if (dev->get_regtoken) {
			/* Get regtoken from cloud */
			if (ds_get_regtoken(dev) < 0) {
				ds_cloud_failure(0);
			}
			break;
		}
	}
}

/*
 * Initialize client.
 */
int ds_init(void)
{
	struct device_state *dev = &device;

	srandom((unsigned)time_mtime_ms());

	ops_devd_init();
	timer_init(&dev->ping_ads_timer, ds_ping_retry);
	timer_init(&dev->poll_ads_timer, ds_polling_timeout);
	timer_init(&dev->ds_step_timer, ds_step_timeout);
	timer_init(&dev->reg_window_timer, ds_reg_window_timeout);

	if (time(NULL) < CLOCK_START) {
		if (clock_set_time(CLOCK_START, CS_DEFAULT) >= 0) {
			log_warn("using default system time");
		}
	} else {
		clock_set_source(CS_SYSTEM);
	}
	dev->poll_interval = CLIENT_POLL_INTERVAL;

	/* Initialize HTTP client and cloud request context */
	dev->http_client = http_client_init(&dev->file_events, &dev->timers);
	if (!dev->http_client) {
		log_err("failed to initialize HTTP client");
		return -1;
	}
	if (ds_client_init(dev->http_client, &dev->client,
	    CLIENT_RESP_SIZE, NULL) < 0) {
		log_err("failed to initialize cloud client context");
		return -1;
	}
	if (ds_client_init(dev->http_client, &dev->app_client,
	    CLIENT_RESP_SIZE, "app") < 0) {
		log_err("failed to initialize app cloud client context");
		return -1;
	}

	/* Initialize mDNS client */
	dnss_mdns_up(INADDR_ANY);

	/* Initialize notifier client */
	np_init(ds_notify_event);

	platform_configure_led(false, false, false);
	return 0;
}

/*
 * Cleanup resources.
 */
void ds_cleanup(void)
{
	struct device_state *dev = &device;

	ds_client_cleanup(&dev->app_client);
	ds_client_cleanup(&dev->client);
	http_client_cleanup(dev->http_client);
}

/*
 * Run ds_step_timeout in the next file_event_poll
 */
void ds_step(void)
{
	struct device_state *dev = &device;

	if (!timer_active(&dev->ds_step_timer)) {
		timer_set(&dev->timers, &dev->ds_step_timer, 0);
	}
}

/*
 * Send an HTTP request.
 */
int ds_send(struct ds_client *client, struct ds_client_req_info *info,
	void (*handler)(enum http_client_err,
	const struct http_client_req_info *, const struct ds_client_data *,
	void *), void *handler_arg)
{
	struct ds_client_req *req;

	ASSERT(client != NULL);
	ASSERT(info != NULL);

	if (http_client_busy(client->context)) {
		log_err("request already pending");
		ds_client_data_cleanup(&info->req_data);
		return -1;
	}
	req = &client->req;
	if (ds_client_req_init(req, info) < 0) {
		return -1;
	}
	/* Default content type to JSON */
	switch (req->method) {
	case HTTP_GET:
		if (req->resp_data.content == HTTP_CONTENT_UNKNOWN) {
			req->resp_data.content = HTTP_CONTENT_JSON;
		}
		break;
	case HTTP_PUT:
	case HTTP_POST:
		if (req->req_data.content == HTTP_CONTENT_UNKNOWN) {
			req->req_data.content = HTTP_CONTENT_JSON;
		}
		break;
	default:
		break;
	}
	/* Use default timeout */
	if (!req->timeout_secs) {
		req->timeout_secs = REQUEST_TIMEOUT_DEFAULT_SECS;
	}
	req->complete = handler;
	req->complete_arg = handler_arg;

	log_debug2("%s %s", http_method_names[req->method], req->link);
	if (ds_client_send(client, req) < 0) {
		ds_client_req_reset(req);
		return -1;
	}
	return 0;
}

/*
 * Transition to the cloud-init state to re-initialize with the cloud and
 * update device info.
 */
int ds_cloud_init(void)
{
	struct device_state *dev = &device;

	if (dev->cloud_state == DS_CLOUD_DOWN) {
		return -1;
	}
	dev->cloud_state = DS_CLOUD_INIT;
	ds_step();
	return 0;
}

/*
 * Update the network interface state to indicate network connectivity has
 * been achieved.
 */
void ds_net_up(void)
{
	struct device_state *dev = &device;

	if (dev->net_up) {
		return;
	}
	dev->net_up = 1;
	dev->do_ping = 1;
	dev->ping_attempts = 0;

	/* Apply new server URL, if needed */
	if (ds_new_ads_host) {
		if (dev->ads_host) {
			log_info("ADS server changed");
			free(dev->ads_host);
		}
		dev->ads_host = ds_new_ads_host;
		ds_new_ads_host = NULL;

	}
	log_info("Network Up, ADS Server: %s", dev->ads_host);
	ds_step();
}

/*
 * Update the network interface state to indicate network connectivity has
 * been lost.
 */
void ds_net_down(void)
{
	struct device_state *dev = &device;

	if (!dev->net_up) {
		return;
	}
	log_warn("Network down");
	dev->net_up = 0;
	ds_cloud_down(dev);
	ds_step();
}

/*
 * Mark ADS failure and schedule a periodic ping to determine when
 * connectivity is restored.  Use a non-zero holdoff_time to delay the
 * reconnect attempt for a specific number of seconds.
 */
void ds_cloud_failure(u32 holdoff_time)
{
	struct device_state *dev = &device;

	ds_cloud_down(dev);
	ds_ping_schedule_retry(dev, holdoff_time * 1000);
}

/*
 * Update the data destination mask.  This mask indicates which connections
 * are available to receive property updates.
 */
void ds_dest_avail_set(u8 dest_mask)
{
	struct device_state *dev = &device;

	if (dev->dests_avail != dest_mask) {
		dev->dests_avail = dest_mask;
		/* XXX may send unneeded messages when LAN clients change */
		msg_server_dests_changed_event(NULL, NULL);
	}
	/* Check for pending ops from the application */
	ops_devd_step();

	ds_step();
}

/*
 * Allow devd to fetch commands and property updates from ADS.
 */
void ds_enable_ads_listen(void)
{
	struct device_state *dev = &device;

	if (!dev->ads_listen) {
		log_info("ADS listen enabled");
	}
	dev->ads_listen = 1;

	/* Get LAN mode info from cloud when ANS does not connect */
	dev->get_cmds = 1;

	ds_step();
}

/*
 * Generate ADS URL from config.  Forces reconnection if URL changed.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_ads_host(void)
{
	struct device_state *dev = &device;
	const struct hostname_info *host_entry;
	int len;

	if (ds_new_ads_host) {
		free(ds_new_ads_host);
	}
	if (dev->setup_mode && conf_ads_host_override) {
		ds_new_ads_host = strdup(conf_ads_host_override);
	} else {
		host_entry = ds_lookup_host(conf_ads_region);
		if (!host_entry) {
			host_entry = SERVER_HOSTINFO_DEFAULT;
		}
		if (dev->oem && dev->oem_model && !dev->ads_host_dev_override) {
			len = asprintf(&ds_new_ads_host, host_entry->fmt_oem,
			    dev->oem_model, dev->oem, host_entry->domain);
		} else {
			len = asprintf(&ds_new_ads_host,
			    host_entry->fmt_default, host_entry->domain);
		}
		if (len == -1) {
			ds_new_ads_host = NULL;
		}
	}
	if (!ds_new_ads_host) {
		log_err("ADS hostname allocation failed");
		return -1;
	}
	if (!dev->ads_host || !conf_loaded) {
		return 1;
	}
	if (!strcmp(ds_new_ads_host, dev->ads_host)) {
		free(ds_new_ads_host);
		ds_new_ads_host = NULL;
		return 1;
	}
	/* Service may have changed, so force reconnect */
	dev->do_reconnect = 1;
	conf_reset = true;	/* Do get DSNs with reset=1 flag */
	ds_step();
	return 0;
}

/*
 * Update OEM model.  Forces reconnection if oem model is changed.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_oem_model(const char *oem_model)
{
	struct device_state *dev = &device;
	int rc;

	ASSERT(oem_model != NULL);

	if (!oem_model[0]) {
		log_err("blank OEM model");
		return -1;
	}
	if (dev->oem_model) {
		if (!strcmp(oem_model, dev->oem_model)) {
			return 1;
		}
		free(dev->oem_model);
	}
	dev->oem_model = strdup(oem_model);
	rc = ds_update_ads_host();
	if (rc != 0) {
		/*
		 * If OEM model change did not change the ADS
		 * hostname and force a reconnect, just do
		 * GET dsns to PUT the new model.
		 */
		ds_cloud_init();
	}
	return rc < 0 ? -1 : 0;
}

/*
 * Update device setup token.  Forces the device to send the setup token
 * to ADS immediately, if in the cloud up state.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_setup_token(const char *setup_token)
{
	struct device_state *dev = &device;

	ASSERT(setup_token != NULL);

	if (!setup_token[0]) {
		log_err("blank setup token");
		return -1;
	}
	if (dev->setup_token) {
		if (!strcmp(setup_token, dev->setup_token)) {
			return 1;
		}
		free(dev->setup_token);
	}
	dev->setup_token = strdup(setup_token);
	log_debug("%s", dev->setup_token);
	/* Re-initialize to force update the setup token */
	if (dev->cloud_state > DS_CLOUD_INIT) {
		ds_cloud_init();
	}
	return 0;
}

/*
 * Update device location.  Forces the device to send the location to ADS
 * immediately, if in the cloud up state.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_location(const char *location)
{
	struct device_state *dev = &device;

	ASSERT(location != NULL);

	if (!location[0]) {
		log_err("blank location");
		return -1;
	}
	if (dev->location) {
		if (!strcmp(location, dev->location)) {
			return 1;
		}
		free(dev->location);
	}
	dev->location = strdup(location);
	log_debug("%s", dev->location);
	/* Re-initialize to force update the location */
	if (dev->cloud_state > DS_CLOUD_INIT) {
		ds_cloud_init();
	}
	return 0;
}

/*
 * Request to open a registration window.
 */
int ds_push_button_reg_start(void)
{
	struct device_state *dev = &device;

	if (dev->cloud_state != DS_CLOUD_UP) {
		log_warn("cannot start registration window when cloud down");
		return -1;
	}
	if (!dev->reg_window_start) {
		dev->reg_window_start = 1;
		ds_step();
	}
	return 0;
}

/*
 * Set system clock and notify interested client applications.
 * Returns 0 on success, 1 if no change, or -1 on error.
 */
int ds_clock_set(time_t new_time, enum clock_src src)
{
	time_t now;
	int rc = 1;

	if (new_time < CLOCK_START) {
		log_err("invalid time: %u", (unsigned)new_time);
		return -1;
	}
	time(&now);
	if (new_time == now) {
		/* Time didn't change, so just update the source */
		rc = clock_set_source(src);
	} else {
		rc = clock_set_time(new_time, src);
	}
	if (!rc) {
		msg_server_clock_event(NULL, NULL);
	}
	return rc;
}

/*
 * Complete a global factory reset by factory resetting devd.
 */
static void ds_reset_factory_event_complete(bool success, void *arg)
{
	struct device_state *dev = &device;

	dev->factory_reset = 1;
	dev->hard_reset = 1;
	ds_step();
}

/*
 * Perform a hard or factory reset of the system.  This will notify connected
 * daemons of the reset, and then reset devd.
 */
int ds_reset(bool factory)
{
	struct device_state *dev = &device;
	int rc = 0;

	if (factory) {
		/* Notify connected applications of factory reset event */
		rc = msg_server_factory_reset_event(
		    ds_reset_factory_event_complete, NULL);
		if (!rc) {
			/* Finish reset after notifications are complete */
			return 0;
		}
		dev->factory_reset = 1;
	}
	dev->hard_reset = 1;
	ds_step();
	return rc;
}

/*
 * registration.json put Reverse-Rest command from Service indicating
 * a change in the device's user registration status.
 */
void ds_registration_put(struct server_req *req)
{
	struct device_state *dev = &device;
	json_t *reg_obj;
	int status;
	const char *dsn;

	reg_obj = json_object_get(req->body_json, "registration");
	if (!json_is_object(reg_obj)) {
		log_warn("missing registration object");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}
	dsn = json_get_string(reg_obj, "dsn");
	if (dsn && strcmp(dsn, dev->dsn)) {
		/* TODO Handle node registration events. */
		log_debug("node %s registration status changed", dsn);
		gateway_node_reg_put(req, dsn);
		return;
	}
	if (json_get_int(reg_obj, "status", &status) < 0) {
		log_warn("missing registration status");
		server_put_end(req, HTTP_STATUS_BAD_REQUEST);
		return;
	}

	/* The regtoken had changed when registration status changed,
	* so clear regtoken after unregister device */
	if (!status) {
		log_debug("device %s unregistered", dsn);
		free(dev->regtoken);
		dev->regtoken = NULL;
	}

	/* Update device status and cloud LED state */
	dev->registered = (status != 0);
	msg_server_registration_event(true, NULL, NULL);
	platform_configure_led(dev->cloud_state == DS_CLOUD_UP, dev->registered,
	    timer_active(&dev->reg_window_timer));

	server_put_end(req, HTTP_STATUS_NO_CONTENT);
}

