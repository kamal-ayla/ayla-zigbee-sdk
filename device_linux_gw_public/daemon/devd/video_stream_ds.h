/*
 * Copyright 2013-2023 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef VIDEO_STREAM_DS_H
#define VIDEO_STREAM_DS_H

#include "ds.h"

/*
 * Make GET WEBRTC Signalling Channel for RPi Camera, and to
 * fetch kvs attributes.
 */
int ds_get_kvs_streaming_channel(struct device_state *dev, const char* addr);

int ds_update_kvs_streaming_channel(const char* addr);
int ds_update_webrtc_streaming_channel(const char* addr);

/*
 * Make GET WEBRTC Signalling Channel for RPi Camera, and to
 * fetch webrtc attributes.
 */
int ds_get_webrtc_signalling_channel(struct device_state *dev, const char* addr);

#endif /* VIDEO_STREAM_DS_H */
