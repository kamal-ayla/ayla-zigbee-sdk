/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/timer.h>
#include <ayla/file_event.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/clock.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>
#include <ayla/msg_conf.h>

#include <app/msg_client.h>
#include "msg_client_internal.h"


static struct {
	struct amsg_client client;
	bool listen_enabled;
	bool cloud_up;
	bool lan_up;
	bool registered;
	char *regtoken;

	void (*connection_status)(bool);
	void (*cloud_changed)(bool);
	void (*lan_changed)(bool);
	void (*reg_changed)(bool);
	void (*regtoken_changed)(const char *);
	void (*time_changed)(void);
	void (*factory_reset)(void);
} state;

/*
 * Handle a client destination update message.
 */
static enum amsg_err msg_client_dests(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_client_dests *msg = (struct msg_client_dests *)info->payload;

	if (msg->cloud_up != state.cloud_up) {
		state.cloud_up = msg->cloud_up;
		if (state.cloud_changed) {
			state.cloud_changed(state.cloud_up);
		}
	}
	if (msg->lan_up != state.lan_up) {
		state.lan_up = msg->lan_up;
		if (state.lan_changed) {
			state.lan_changed(state.lan_up);
		}
	}
	return AMSG_ERR_NONE;
}

/*
 * Handle a client time update message.
 */
static enum amsg_err msg_client_time(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_client_time *msg = (struct msg_client_time *)info->payload;

	clock_set_source((enum clock_src)msg->source);
	if (state.time_changed) {
		state.time_changed();
	}
	return AMSG_ERR_NONE;
}

/*
 * Handle a client registration update message.
 */
static enum amsg_err msg_client_registration(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_client_reg_info *msg =
	    (struct msg_client_reg_info *)info->payload;

	state.registered = msg->registered;
	if (msg->status_changed && state.reg_changed) {
		state.reg_changed(state.registered);
	}
	if (!state.regtoken ||
	    strncmp(state.regtoken, msg->regtoken, sizeof(msg->regtoken))) {
		free(state.regtoken);
		state.regtoken = strdup(msg->regtoken);
		if (state.regtoken_changed) {
			state.regtoken_changed(state.regtoken);
		}
	}
	return AMSG_ERR_NONE;
}

/*
 * Message interface handler for the Client interface.
 */
static enum amsg_err msg_client_interface_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_CLIENT);

	/* Only accept messages from the expected endpoint */
	if (endpoint != &state.client.endpoint) {
		return AMSG_ERR_PRIVS;
	}
	switch (info->type) {
	case MSG_CLIENT_DESTS:
		return msg_client_dests(endpoint, info);
	case MSG_CLIENT_TIME:
		return msg_client_time(endpoint, info);
	case MSG_CLIENT_USERREG:
		return msg_client_registration(endpoint, info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Handle a request to factory reset the app.
 */
static enum amsg_err msg_client_handle_factory_reset(
	const struct amsg_endpoint *endpoint)
{
	if (endpoint != &state.client.endpoint) {
		return AMSG_ERR_PRIVS;
	}
	if (state.factory_reset) {
		state.factory_reset();
	}
	return AMSG_ERR_NONE;
}

/*
 * Handle a config notification for devd's "sys" config group.
 * This updates the local timezone and daylight savings time state and
 * invokes the time changed callback, if needed.
 */
static void msg_client_conf_sys(const char *path, json_t *val, void *arg)
{
	bool notify = false;
	bool set;
	int timezone;
	unsigned daylight;

	if (!val) {
		/* Object deleted */
		memset(&timezone_ayla, 0, sizeof(timezone_ayla));
		memset(&daylight_ayla, 0, sizeof(daylight_ayla));
		notify = true;
	} else {
		if (!json_get_int(val, "timezone", &timezone)) {
			timezone *= -1;
			timezone_ayla.valid = 1;
			if (timezone_ayla.mins != timezone) {
				log_debug("timezone=%d", timezone);
				timezone_ayla.mins = timezone;
				notify = true;
			}
		}
		if (!json_get_bool(val, "dst_valid", &set)) {
			if (daylight_ayla.valid != set) {
				daylight_ayla.valid = set;
				notify = true;
			}
		}
		if (!json_get_bool(val, "dst_active", &set)) {
			if (daylight_ayla.active != set) {
				log_debug("dst_active=%s",
				    set ? "true" : "false");
				daylight_ayla.active = set;
				notify = true;
			}
		}
		if (!json_get_uint(val, "dst_change", &daylight)) {
			if (daylight_ayla.change != daylight) {
				log_debug("dst_change=%u", daylight);
				daylight_ayla.change = daylight;
				notify = true;
			}
		}
	}
	if (notify && state.time_changed) {
		state.time_changed();
	}
}

/*
 * Setup the cloud client message interface for this endpoint, and subscribe
 * to push notifications about client status changes.
 */
static int msg_client_register(struct amsg_endpoint *endpoint)
{
	enum amsg_err err;

	/* Register for client event notifications */
	err = amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_DESTS_REG,
	    NULL, 0, NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("dests subscribe failed: %s", amsg_err_string(err));
		return -1;
	}
	err = amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_TIME_REG,
	    NULL, 0, NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("time subscribe failed: %s", amsg_err_string(err));
		return -1;
	}
	err = amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_USERREG_REG,
	    NULL, 0, NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("user-registration subscribe failed: %s",
		    amsg_err_string(err));
		return -1;
	}
	/* Subscribe to timezone and daylight savings config notifications */
	err = msg_conf_register(endpoint, "sys", msg_client_conf_sys, NULL,
	    false);
	if (err != AMSG_ERR_NONE) {
		log_err("sys config subscribe failed: %s",
		    amsg_err_string(err));
		return -1;
	}
	if (state.listen_enabled) {
		/* Re-enable client listen, if already set */
		if (msg_client_listen_enable() < 0) {
			return -1;
		}
	}
	return 0;
}

/*
 * Handler for client connection events.
 */
static int msg_client_event_handler(struct amsg_endpoint *endpoint,
	enum amsg_endpoint_event event)
{
	switch (event) {
	case AMSG_ENDPOINT_CONNECT:
		log_info("connected to cloud client");
		/* Send app identification as soon as connection established */
		if (msg_send_app_info(endpoint, MSG_APP_NAME_APP) < 0) {
			log_err("msg_send_app_info failed");
			amsg_disconnect(endpoint);
			return -1;
		}
		/* Subscribe to cloud client updates */
		if (msg_client_register(endpoint) < 0) {
			log_err("msg_client_register failed");
			amsg_disconnect(endpoint);
			return -1;
		}
		if (state.connection_status) {
			state.connection_status(true);
		}
		break;
	case AMSG_ENDPOINT_DISCONNECT:
		log_info("disconnected from cloud client");
		/* Reset connection-related state and invoke callbacks */
		if (state.cloud_up) {
			state.cloud_up = false;
			if (state.cloud_changed) {
				state.cloud_changed(false);
			}
		}
		if (state.lan_up) {
			state.lan_up = false;
			if (state.lan_changed) {
				state.lan_changed(false);
			}
		}
		if (state.connection_status) {
			state.connection_status(false);
		}
		break;
	}
	return 0;
}

/*
 * Initialize the cloud client message interface.
 */
int msg_client_init(struct file_event_table *file_events,
	struct timer_head *timers)
{
	ASSERT(file_events != NULL);
	/*
	 * Currently, timers are not required for the Amsg features used by
	 * this library, but that may change in the future.
	 * ASSERT(timers != NULL);
	 */

	if (amsg_client_init(&state.client, file_events, timers) < 0) {
		log_err("failed to initialize message client");
		return -1;
	}
	amsg_client_set_event_callback(&state.client, msg_client_event_handler);
	amsg_set_interface_handler(MSG_INTERFACE_CLIENT,
	    msg_client_interface_handler);
	/* Setup configuration message interface */
	msg_conf_init(NULL, 0, NULL, msg_client_handle_factory_reset);
	return 0;
}

/*
 * Cleanup all resources associated with the cloud client message interface.
 */
void msg_client_cleanup(void)
{
	amsg_client_cleanup(&state.client);
	free(state.regtoken);
	state.regtoken = NULL;
}

/*
 * Connect to the cloud client at the specified path.  A callback may be
 * provided to indicate if/when the connection is ended.
 */
int msg_client_connect(const char *path)
{
	ASSERT(path != NULL);

	if (amsg_connected(&state.client.endpoint)) {
		amsg_disconnect(&state.client.endpoint);
	}
	return amsg_client_connect(&state.client, path);
}

/*
 * Close the cloud client connect.
 */
void msg_client_connect_close(void)
{
	log_debug("close app msg sock %d", state.client.endpoint.sock);
	amsg_disconnect(&state.client.endpoint);
}

/*
 * Enable the cloud client to receive commands and properties from the cloud.
 */
int msg_client_listen_enable(void)
{
	enum amsg_err err;

	state.listen_enabled = true;
	if (!amsg_connected(&state.client.endpoint)) {
		/* Defer message until connected */
		return 0;
	}
	err = amsg_send(&state.client.endpoint, MSG_INTERFACE_CLIENT,
	    MSG_CLIENT_LISTEN, NULL, 0, NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("message failed: %s", amsg_err_string(err));
		return -1;
	}
	log_debug("enable cloud updates");
	return 0;
}

/*
 * Register a callback for when the connection to the cloud client
 * goes up or down.
 */
void msg_client_set_connection_status_callback(void (*callback)(bool))
{
	state.connection_status = callback;
}

/*
 * Register a callback for cloud up/down events.
 */
void msg_client_set_cloud_event_callback(void (*callback)(bool))
{
	state.cloud_changed = callback;
}

/*
 * Register a callback for LAN client up/down events.  This is only called
 * when the first client connects or the last client disconnects.
 */
void msg_client_set_lan_event_callback(void (*callback)(bool))
{
	state.lan_changed = callback;
}

/*
 * Register a callback for user registration events.
 */
void msg_client_set_registration_callback(void (*callback)(bool))
{
	state.reg_changed = callback;
}

/*
 * Register a callback for when a new registration token is received.
 */
void msg_client_set_regtoken_callback(void (*callback)(const char *))
{
	state.regtoken_changed = callback;
}

/*
 * Register a callback for when the cloud client has changed the system time.
 */
void msg_client_set_time_change_callback(void (*callback)(void))
{
	state.time_changed = callback;
}

/*
 * Register a callback for when the cloud client has requested a factory reset.
 */
void msg_client_set_factory_reset_callback(void (*callback)(void))
{
	state.factory_reset = callback;
}

/*
 * Return true if a connection to the cloud has been established.
 */
bool msg_client_cloud_up(void)
{
	return state.cloud_up;
}

/*
 * Return true if a connection to at least one LAN client has been established.
 */
bool msg_client_lan_up(void)
{
	return state.lan_up;
}

/*
 * Return true if user registration has been completed.
 */
bool msg_client_registered(void)
{
	return state.registered;
}

/*
 * Return the most recently received user registration token, if known,
 * otherwise an empty string.
 */
const char *msg_client_regtoken(void)
{
	if (!state.regtoken) {
		return "";
	}
	return state.regtoken;
}

/*
 * Return a handle to the connection to the cloud client.
 */
struct amsg_endpoint *msg_client_endpoint(void)
{
	return &state.client.endpoint;
}

/*
 * Push a new setup token and/or location.  The setup token is generally
 * generated by a mobile app for use in device setup and registration.  The
 * location is informational, and may also be used to update the device's
 * timezone.
 */
int msg_client_setup_info_update(const char *token,
	const char *location)
{
	enum amsg_err err;
	struct msg_client_setup_info msg = { { 0 } };

	ASSERT(token != NULL || location != NULL);

	if (token) {
		snprintf(msg.setup_token, sizeof(msg.setup_token), "%s", token);
	}
	if (location) {
		snprintf(msg.location, sizeof(msg.location), "%s",
		    location);
	}
	err = amsg_send(&state.client.endpoint, MSG_INTERFACE_CLIENT,
	    MSG_CLIENT_SETUP_INFO, &msg, sizeof(msg),
	    NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("message failed: %s", amsg_err_string(err));
		return -1;
	}
	return 0;
}

/*
 * Start user registration window.  This is used with the push-button
 * registration method.
 */
int msg_client_reg_window_start(void)
{
	enum amsg_err err;

	err = amsg_send(&state.client.endpoint, MSG_INTERFACE_CLIENT,
	    MSG_CLIENT_USERREG_WINDOW_START, NULL, 0, NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("message failed: %s", amsg_err_string(err));
		return -1;
	}
	return 0;
}

/*
 * Initiate a factory reset on the entire system.
 */
int msg_client_global_factory_reset(void)
{
	enum amsg_err err;

	err = amsg_send(&state.client.endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_FACTORY_RESET, NULL, 0, NULL, NULL, 0);
	if (err != AMSG_ERR_NONE) {
		log_err("message failed: %s", amsg_err_string(err));
		return -1;
	}
	return 0;
}
