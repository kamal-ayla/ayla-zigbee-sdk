/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_PROPS_CLIENT_H__
#define __AYLA_PROPS_CLIENT_H__

struct prop_send_handlers {
	int (*lan_init_helper)(struct device_state *,
	    struct prop_send_handlers *);
	void (*ads_init_helper)(struct device_state *,
	    struct prop_send_handlers *);
	enum http_method *method;
	char *link;
	int link_size;
	struct ops_buf_info *info;
	struct ops_devd_cmd *op_cmd;
	json_t *prop_info;
};

/*
 * Setup an internal pcmd to echo property updates
 * if other destinations exist
 */
void prop_prepare_echo(struct device_state *dev, json_t *prop_info,
	int source);

/*
 * Handle a property operation
 */
enum app_parse_rc prop_handle_data_pkt(json_t *cmd, int recv_id, json_t *opts);

/*
 * Send a property object to LAN clients based on the target
 */
void prop_send_prop_to_lan_clients(struct ops_devd_cmd *op_cmd,
	struct device_state *dev, u8 targets, json_t *prop_info,
	const char *url);

/*
 * Shared by props and gateway. Init function for sending prop information
 * to LAN and ADS.
 */
int prop_send_init_execute(struct prop_send_handlers *handler);

/*
 * Shared by props and gateway. Init function for acknowledgment of a property
 * update.
 */
int prop_ack_init_execute(struct prop_send_handlers *handler,
			    const char *lan_ack_url, const char *dsn);

/*
 * Helper function for prop_get_success. Shared by props and gateway clients.
 * Process_prop_obj can be NULL if no additional processing needs to be
 * performed on the property object in the response. All other parameters are
 * required.
 */
int prop_get_success_helper(struct ops_devd_cmd *op_cmd,
		struct device_state *dev, void (*prop_resp)(void *, json_t *));

/*
 * Get the dests from the opts object.
 */
int prop_get_dests_helper(struct device_state *dev, json_t *opts,
			u8 *dests_specified);

/*
 * Setup datapoint POST/PUT buffer for property sends and acks
 */
void prop_curl_buf_info_setup(struct ops_buf_info *info, json_t *prop_info);

/*
 * Contruct the payload + set the link for a batch datapoint
 */
void prop_batch_payload_construct(struct device_state *dev,
	struct prop_send_handlers *handler, const char *base_url,
	json_t *raw_payload);

/*
 * Batch update successfully sent to the service
 */
int prop_batch_post_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev);

/*
 * Sends batch prop updates to LAN clients
 */
void prop_send_batch_to_lan_clients(struct ops_devd_cmd *op_cmd,
	struct device_state *dev, json_t *prop_info_arr_j,
	const char *url);

#endif /*  __AYLA_PROPS_CLIENT_H__ */

