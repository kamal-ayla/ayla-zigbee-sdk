/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

/*
 *  api to be used by DEVICE
 *
 */
#ifndef __AYLA_DEVICE_API_H__
#define __AYLA_DEVICE_API_H__

struct device_state;
struct ds_client;
struct ds_client_req_info;
struct ds_client_data;
struct http_client_req_info;
enum http_client_err;
enum clock_src;
struct server_req;
struct queue_buf;

/*
 * Initialize client.
 */
int ds_init(void);

/*
 * Cleanup resources.
 */
void ds_cleanup(void);

/*
 * Schedule a ds_step_timeout() call in the next main loop iteration.
 */
void ds_step(void);

/*
 * Send an HTTP request.
 */
int ds_send(struct ds_client *client, struct ds_client_req_info *info,
	void (*handler)(enum http_client_err,
	const struct http_client_req_info *, const struct ds_client_data *,
	void *), void *handler_arg);

/*
 * Transition to the cloud-init state to re-initialize with the cloud and
 * update device info.
 */
int ds_cloud_init(void);

/*
 * Update the network interface state to indicate network connectivity has
 * been achieved.
 */
void ds_net_up(void);

/*
 * Update the network interface state to indicate network connectivity has
 * been lost.
 */
void ds_net_down(void);

/*
 * Mark ADS failure and schedule a periodic ping to determine when
 * connectivity is restored.  Use a non-zero holdoff_time to delay the
 * reconnect attempt for a specific number of seconds.
 */
void ds_cloud_failure(u32 holdoff_time);

/*
 * Update the data destination mask.  This mask indicates which connections
 * are available to receive property updates.
 */
void ds_dest_avail_set(u8 dest_mask);

/*
 * Allow devd to fetch commands and property updates from ADS.
 */
void ds_enable_ads_listen(void);

/*
 * Generate ADS URL from config.  Forces reconnection if URL changed.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_ads_host(void);

/*
 * Update OEM model.  Forces reconnection if oem model is changed.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_oem_model(const char *oem_model);

/*
 * Update device setup token.  Forces the device to send the setup token
 * to ADS immediately, if in the cloud up state.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_setup_token(const char *setup_token);

/*
 * Update device location.  Forces the device to send the location to ADS
 * immediately, if in the cloud up state.
 * Returns 1 on no-change, 0 on success, and -1 on error.
 */
int ds_update_location(const char *location);

/*
 * Request to open a registration window.
 */
int ds_push_button_reg_start(void);

/*
 * Set system clock and notify interested client applications.
 * Returns 0 on success, 1 if no change, or -1 on error.
 */
int ds_clock_set(time_t new_time, enum clock_src src);

/*
 * Perform a hard or factory reset of the system.  This will notify connected
 * daemons of the reset, and then reset devd.
 */
int ds_reset(bool factory);

/*
 * registration.json put Reverse-Rest command from Service indicating
 * a change in the device's user registration status.
 */
void ds_registration_put(struct server_req *req);

/*
 * Handle properties in the response of GET commands.json.
 * Custom handlers may be used to override the default behavior.
 */
void ds_parse_props(struct device_state *dev, json_t *commands,
	void (*prop_handler)(void *arg, json_t *props),
	void (*node_prop_handler)(void *arg, json_t *props),
	void *arg);

/*
 * Update template version to cloud
 */
void ds_update_template_ver_to_cloud(const char *template_ver);

/*
 * Get regtoken(dsns) from cloud
 */
void ds_get_regtoken_from_cloud(void);

#endif
