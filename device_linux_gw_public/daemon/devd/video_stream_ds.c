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

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>


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
    struct DsClientSendData *ds_data = malloc(sizeof(struct DsClientSendData));

	snprintf(ds_data->buff, sizeof(ds_data->buff), "videoservice/dsns/$DSN/signaling_channels.json?camera_ids=%s", addr);

	log_debug("get WebRTC stream for ADDR: '%s'", addr);
	log_debug("API CALL: '%s'", ds_data->buff);

    ds_data->info.method = HTTP_GET;
    ds_data->info.host = dev->ads_host;
    ds_data->info.uri = ds_data->buff;
    ds_data->handler = ds_get_webrtc_signalling_channel_done;
    ds_data->handler_arg = NULL;

	if (ds_client_busy(&dev->client)) {
	    ds_send_later(ds_data);
		return 0;
	}

	log_debug2("sending the webrtc signalling channel GET request**************");
	if (ds_send(ds_data->client, &ds_data->info, ds_data->handler, ds_data->handler_arg) < 0) {
	    free(ds_data);
		log_warn("send failed");
		return -1;
	}

    free(ds_data);

	return 0;
}


int ds_get_kvs_streaming_channel(struct device_state *dev, const char* addr)
{
    struct DsClientSendData *ds_data = malloc(sizeof(struct DsClientSendData));

    snprintf(ds_data->buff, sizeof(ds_data->buff), "videoservice/dsns/$DSN/streams.json?camera_ids=%s", addr);

	log_debug("get KVS stream for ADDR: '%s'", addr);
	log_debug("API CALL: '%s'", ds_data->buff);

    ds_data->info.method = HTTP_GET;
    ds_data->info.host = dev->ads_host;
    ds_data->info.uri = ds_data->buff;
    ds_data->handler = ds_get_kvs_streaming_channel_done;
    ds_data->handler_arg = NULL;

    if (ds_client_busy(&dev->client)) {
        ds_send_later(ds_data);
        return 0;
    }

	log_debug2("sending the kvs streaming channel GET request**************");
	if (ds_send(ds_data->client, &ds_data->info, ds_data->handler, ds_data->handler_arg) < 0) {
	    free(ds_data);
		log_warn("send failed");
		return -1;
	}

    free(ds_data);

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

static pthread_mutex_t ds_send_later_mtx = PTHREAD_MUTEX_INITIALIZER;
static void* ds_send_later_thread(void *arg)
{
    struct DsClientSendData *ds_data = (struct DsClientSendData *)arg;
    int ret = 0;
    unsigned int retry = 0;
    const unsigned int max_retry = 10;

    pthread_mutex_lock(&ds_send_later_mtx);
    log_debug2("sending the webrtc signalling channel GET request**************");
    do
    {
        ret = ds_send(ds_data->client, &ds_data->info, ds_data->handler, ds_data->handler_arg);
        ++retry;
        usleep(250000);
    } while(ret != 0 && retry < max_retry);
    free(ds_data);
    pthread_mutex_unlock(&ds_send_later_mtx);

    if(retry >= max_retry)
    {
        log_warn("ds_send_later_thread: send failed, reached max retry");
        return (void *)-1;
    }

    return NULL;
}

void ds_send_later(struct DsClientSendData *ds_data)
{
	pthread_t thread;
    pthread_create(&thread, NULL, ds_send_later_thread, ds_data);
}

