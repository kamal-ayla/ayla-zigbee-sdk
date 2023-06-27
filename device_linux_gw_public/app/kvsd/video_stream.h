/*
 * Copyright 2013-2023 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H


struct kvs_data {
	char * kvs_channel_name;
	char * arn;
	char * region;
	char * access_key_id;
	char * secret_access_key;
	char * session_token;
	int expiration_time;
	int retention_days;
};

enum kvs_data_str_index {
	KVS_CHANNEL_NAME=0,
	KVS_ARN,
	KVS_REGION,
	KVS_ACCESS_KEY_ID,
	KVS_SECRET_ACCESS_KEY,
	KVS_SESSION_TOKEN,
	KVS_EXPIRATION_TIME,
	KVS_RETENTION_DAYS,
	KVS_STR_CNT
};


struct webrtc_data {
	char * webrtc_channel_name;
	char * arn;
	char * region;
	char * access_key_id;
	char * secret_access_key;
	char * session_token;
	int expiration_time;
};

enum webrtc_data_str_index {
	WEBRTC_CHANNEL_NAME=0,
	WEBRTC_ARN,
	WEBRTC_REGION,
	WEBRTC_ACCESS_KEY_ID,
	WEBRTC_SECRET_ACCESS_KEY,
	WEBRTC_SESSION_TOKEN,
	WEBRTC_EXPIRATION_TIME,
	WEBRTC_STR_CNT
};

void kvs_data_init(struct kvs_data* kvs_data);
void webrtc_data_init(struct webrtc_data* webrtc_data);

void kvs_data_destroy(struct kvs_data *kvs_data);
void webrtc_data_destroy(struct webrtc_data *webrtc_data);

const char* get_kvs_data_str(enum kvs_data_str_index index);
const char* get_webrtc_data_str(enum webrtc_data_str_index index);

#endif /* VIDEO_STREAM_H */
