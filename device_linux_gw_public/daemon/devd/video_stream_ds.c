/*
 * Copyright 2013-2023 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include "video_stream_ds.h"

#include <string.h>

#include <ayla/json_parser.h>
#include <ayla/log.h>

#include "app_if.h"
#include "ds_client.h"
#include "dapi.h"


/*
 * GET KVS Signalling Channel request complete handler.
 */
static void ds_get_kvs_streaming_channel_done(enum http_client_err err,
											  const struct http_client_req_info *info,
											  const struct ds_client_data *resp_data, void *arg)
{
	json_t *dev_info;
	int rc;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			/* Initialization failed, go to cloud down state */
			ds_cloud_failure(0);
		}
		return;
	}
	dev_info = ds_client_data_parse_json(resp_data);
	if (!dev_info) {
		log_err("failed parsing device info");
		return;
	}

	/* Forward json to appd */
	ds_json_dump(__func__, dev_info);
	rc = app_send_json(dev_info);
	json_decref(dev_info);
	if (rc < 0) {
		log_err("invalid kvs streaming info");
		return;
	}
	log_debug("we have sent the json to appd with rc '%d'",rc);

	ds_enable_ads_listen();
}

/*
 * GET WEBRTC Signalling Channel request complete handler.
 */
static void ds_get_webrtc_signalling_channel_done(enum http_client_err err,
												  const struct http_client_req_info *info,
												  const struct ds_client_data *resp_data, void *arg)
{
	json_t *dev_info;
	int rc;

	if (!HTTP_STATUS_IS_SUCCESS(info->http_status)) {
		if (err != HTTP_CLIENT_ERR_CANCELED) {
			/* Initialization failed, go to cloud down state */
			ds_cloud_failure(0);
		}
		return;
	}

	dev_info = ds_client_data_parse_json(resp_data);
	if (!dev_info) {
		log_err("failed parsing device info");
		return;
	}

	/* Forward json to appd */
	ds_json_dump(__func__, dev_info);
	rc = app_send_json(dev_info);
	json_decref(dev_info);
	if (rc < 0) {
		log_err("invalid kvs streaming info");
		return;
	}
	log_debug("we have sent the json to appd with rc '%d'",rc);

	ds_step();
}

int ds_get_webrtc_signalling_channel(struct device_state *dev, const char* addr)
{
	char buff[256];
	snprintf(buff, sizeof(buff), "videoservice/dsns/$DSN/signaling_channels.json?camera_ids=%s", addr);

	log_debug("get WebRTC stream for ADDR: '%s'", addr);
	log_debug("API CALL: '%s'", buff);

	struct ds_client_req_info info = {
			.method = HTTP_GET,
			.host = dev->ads_host,
			.uri = buff
	};

	if (ds_client_busy(&dev->client)) {
		return -1;
	}

	log_debug2("sending the webrtc signalling channel GET request**************");
	if (ds_send(&dev->client, &info, ds_get_webrtc_signalling_channel_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}

	return 0;
}


int ds_get_kvs_streaming_channel(struct device_state *dev, const char* addr)
{
	char buff[256];
	snprintf(buff, sizeof(buff), "videoservice/dsns/$DSN/streams.json?camera_ids=%s", addr);

	log_debug("get KVS stream for ADDR: '%s'", addr);
	log_debug("API CALL: '%s'", buff);

	struct ds_client_req_info info = {
			.method = HTTP_GET,
			.host = dev->ads_host,
			.uri = buff
	};

	if (ds_client_busy(&dev->client)) {
		return -1;
	}

	log_debug2("sending the kvs streaming channel GET request**************");
	if (ds_send(&dev->client, &info, ds_get_kvs_streaming_channel_done, NULL) < 0) {
		log_warn("send failed");
		return -1;
	}

	return 0;
}

int ds_update_kvs_streaming_channel(const char* addr)
{
	log_debug("Received request for update KVS stream for ADDR: '%s'", addr);

	return ds_get_kvs_streaming_channel(&device, addr);
}

int ds_update_webrtc_streaming_channel(const char* addr)
{
	log_debug("Received request for update WebRTC stream for ADDR: '%s'", addr);

	return ds_get_webrtc_signalling_channel(&device, addr);
}
