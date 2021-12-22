/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_PROPS_IF_H__
#define __AYLA_PROPS_IF_H__

/*
 * Send request to appd for a single property.
 * Request has args name=<prop>
 */
void prop_json_get(struct server_req *req);

/*
 * Handle appd's response to a prop get request
 */
void prop_handle_prop_resp(json_t *cmd, int req_id);

/*
 * Send response to a GET prop request to appd
 */
void prop_send_prop_resp(void *arg, json_t *props);

/*
 * Send property updates to appd. Originated from ADS or a client lan app
 */
void prop_send_prop_update(json_t *props, int source);

/*
 * Send the location of a file datapoint created for a FILE property
 */
int prop_send_file_location_info(const int req_id, const char *prop_name,
				const char *location);
#endif /*  __AYLA_PROPS_IF_H__ */

