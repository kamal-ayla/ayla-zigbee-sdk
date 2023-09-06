/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include "video_stream.h"
#include <string.h>
#include <ayla/json_parser.h>
#include "check.h"
#include "ayla/log.h"
#include <unistd.h>
#include <sys/socket.h>
#include "node_camera.h"


static const char* kvs_data_str[] = {
		"kvs_channel_name",
		"kvs_arn",
		"kvs_region",
		"kvs_access_key_id",
		"kvs_secret_access_key",
		"kvs_session_token",
		"kvs_expiration_time",
		"kvs_retention_days"
};

static const char* webrtc_data_str[] = {
		"webrtc_channel_name",
		"webrtc_arn",
		"webrtc_region",
		"webrtc_access_key_id",
		"webrtc_secret_access_key",
		"webrtc_session_token",
		"webrtc_expiration_time",
};

void kvs_data_destroy(struct hls_data *kvs_data)
{
	if(kvs_data->kvs_channel_name) {
		free(kvs_data->kvs_channel_name);
		kvs_data->kvs_channel_name = NULL;
	}
	if(kvs_data->arn)
	{
		free(kvs_data->arn);
		kvs_data->arn = NULL;
	}
	if(kvs_data->region) {
		free(kvs_data->region);
		kvs_data->region = NULL;
	}
	if(kvs_data->access_key_id) {
		free(kvs_data->access_key_id);
		kvs_data->access_key_id = NULL;
	}
	if(kvs_data->secret_access_key) {
		free(kvs_data->secret_access_key);
		kvs_data->secret_access_key = NULL;
	}
	if(kvs_data->session_token) {
		free(kvs_data->session_token);
		kvs_data->session_token = NULL;
	}
	kvs_data->retention_days = 0;
	kvs_data->expiration_time = 0;
}

void webrtc_data_destroy(struct webrtc_data *webrtc_data)
{
	if(webrtc_data->webrtc_channel_name) {
		free(webrtc_data->webrtc_channel_name);
		webrtc_data->webrtc_channel_name = NULL;
	}
	if(webrtc_data->arn) {
		free(webrtc_data->arn);
		webrtc_data->arn = NULL;
	}
	if(webrtc_data->region) {
		free(webrtc_data->region);
		webrtc_data->region = NULL;
	}
	if(webrtc_data->access_key_id) {
		free(webrtc_data->access_key_id);
		webrtc_data->access_key_id = NULL;
	}
	if(webrtc_data->secret_access_key) {
		free(webrtc_data->secret_access_key);
		webrtc_data->secret_access_key = NULL;
	}
	if(webrtc_data->session_token) {
		free(webrtc_data->session_token);
		webrtc_data->session_token = NULL;
	}
	webrtc_data->expiration_time = 0;
}

const char* get_kvs_data_str(enum kvs_data_str_index index)
{
	if(index < 0 || index >= KVS_STR_CNT)
		return NULL;
	return kvs_data_str[index];
}

const char* get_webrtc_data_str(enum webrtc_data_str_index index)
{
	if(index < 0 || index >= WEBRTC_STR_CNT)
		return NULL;
	return webrtc_data_str[index];
}

void kvs_data_init(struct hls_data* kvs_data)
{
	memset(kvs_data, 0, sizeof(struct hls_data));
}

void webrtc_data_init(struct webrtc_data* webrtc_data)
{
	memset(webrtc_data, 0, sizeof(struct webrtc_data));
}
