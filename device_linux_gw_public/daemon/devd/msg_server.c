/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/json_parser.h>
#include <ayla/amsg.h>
#include <ayla/time_utils.h>
#include <ayla/msg_utils.h>
#include <ayla/msg_conf.h>
#include <ayla/ayla_interface.h>

#include "ds.h"
#include "dapi.h"
#include "msg_server.h"

#define MSG_SERVER_MAX_SESSIONS		10	/* Maximum connected clients */

static int msg_server_client_state_id;


/* State for tracking a broadcast to all clients */
struct msg_server_broadcast_state {
	size_t num_pending;
	void (*complete_callback)(bool, void *);
	void *complete_arg;
	bool error;
};

/* Remote config access permissions */
const struct msg_conf_privs devd_conf_privs[] = {
	{ "sys/setup_mode",	MSG_CONF_ALL },
	{ "sys/*",		MSG_CONF_READ | MSG_CONF_NOTIFY },
	{ "id/dsn",		MSG_CONF_READ },
	{ "oem/model",		MSG_CONF_ALL },
	{ "client/region",	MSG_CONF_ALL },
	{ "log/*",		MSG_CONF_READ | MSG_CONF_NOTIFY },
	{ "log/enabled",	MSG_CONF_WRITE },
};

static struct amsg_server server;


/*
 * Cleanup client state.
 */
static void msg_server_client_state_free(void *data)
{
	struct msg_client_state *state = (struct msg_client_state *)data;

	if (!state) {
		return;
	}
	free(state->name);
	free(state);
}

/*
 * Allocate a client state structure.  This should be freed using
 * msg_server_client_state_free().
 */
static struct msg_client_state *msg_server_client_state_alloc(
	struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state;

	state = (struct msg_client_state *)calloc(1, sizeof(*state));
	if (!state) {
		log_err("malloc failed");
		return NULL;
	}
	state->pid = -1;	/* Application info unknown */
	state->endpoint = endpoint;
	state->connect_time_ms = time_mtime_ms();
	amsg_set_user_data(endpoint, msg_server_client_state_id,
	    state, msg_server_client_state_free);
	return state;
}

/*
 * Return the client state, if it was set.
 */
static struct msg_client_state *msg_server_client_state(
	const struct amsg_endpoint *endpoint)
{
	return (struct msg_client_state *)amsg_get_user_data(endpoint,
	    msg_server_client_state_id);
}

/*
 * Broadcast message filter.  Returns true if the endpoint has registered
 * for destination change updates.
 */
static bool msg_server_dest_change_filter(const struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state = msg_server_client_state(endpoint);

	if (!state) {
		return false;
	}
	return state->reg_dest_changes;
}

/*
 * Broadcast message filter.  Returns true if the endpoint has registered
 * for time and date change updates.
 */
static bool msg_server_clock_event_filter(const struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state = msg_server_client_state(endpoint);

	if (!state) {
		return false;
	}
	return state->reg_clock_events;
}

/*
 * Broadcast message filter.  Returns true if the endpoint has registered
 * for user registration change updates.
 */
static bool msg_server_reg_change_filter(const struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state = msg_server_client_state(endpoint);

	if (!state) {
		return false;
	}
	return state->reg_registration_changes;
}

/*
 * Response handler for messages broadcasted to all clients.
 */
static void msg_server_broadcast_resp_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, const struct amsg_msg_info *info, void *resp_arg)
{
	struct msg_server_broadcast_state *state =
	    (struct msg_server_broadcast_state *)resp_arg;

	if (!state->num_pending) {
		log_err("unexpected response");
		free(state);
		return;
	}
	switch (err) {
	case AMSG_ERR_NONE:
	case AMSG_ERR_INTERFACE_UNSUPPORTED:
	case AMSG_ERR_TYPE_UNSUPPORTED:
		break;
	default:
		state->error = true;
	}
	if (--state->num_pending) {
		return;
	}
	if (state->complete_callback) {
		state->complete_callback(!state->error, state->complete_arg);
	}
	free(state);
}

/*
 * Populate a cloud client destination info message structure.
 */
static void msg_server_client_dests_init(struct msg_client_dests *msg)
{
	struct device_state *dev = &device;

	msg->cloud_up = (dev->dests_avail & DEST_ADS);
	msg->lan_up = (dev->dests_avail & DEST_LAN_APPS) != 0;
}

/*
 * Populate a cloud client time info message structure.
 */
static void msg_server_client_time_init(struct msg_client_time *msg)
{
	msg->source = clock_source();
	msg->time_utc = time(NULL);
}

/*
 * Populate a cloud client user registration info message structure.
 */
static void msg_server_reg_info_init(struct msg_client_reg_info *msg,
	bool status_changed)
{
	struct device_state *dev = &device;

	msg->registered = dev->registered;
	msg->status_changed = status_changed;
	if (dev->regtoken) {
		snprintf(msg->regtoken, sizeof(msg->regtoken), "%s",
		    dev->regtoken);
	} else {
		msg->regtoken[0] = '\0';
	}
}

/*
 * Process config notifications for each connected client.
 */
static void msg_server_conf_notify(const char *path, const json_t *val)
{
	struct amsg_server_session *s, *s_next;

	/* Iterate through server sessions in a delete-safe manner */
	s = LIST_FIRST(&server.session_list);
	while (s) {
		s_next = LIST_NEXT(s, list_entry);
		msg_conf_notify_process(&s->endpoint, path, val, false);
		s = s_next;
	}
}

/*
 * Handle a config factory reset message.  This message handler returns
 * immediately, and the full factory reset may take some additional time to
 * complete.
 */
static enum amsg_err msg_server_handle_factory_reset(
	const struct amsg_endpoint *endpoint) {
	if (ds_reset(true) < 0) {
		return AMSG_ERR_APPLICATION;
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for an application info message.  Associates a client
 * state with the session.
 */
static enum amsg_err msg_server_handle_app_info(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_app_info *msg = (struct msg_app_info *)info->payload;
	struct msg_client_state *state;

	state = msg_server_client_state(endpoint);
	if (!state) {
		state = msg_server_client_state_alloc(endpoint);
		if (!state) {
			return AMSG_ERR_MEM;
		}
	}
	if (!state->name) {
		/* Require unique names for connected applications */
		if (msg_server_lookup_client(msg->name)) {
			log_warn("application called %s already connected",
			    msg->name);
			amsg_disconnect(endpoint);
			return AMSG_ERR_APPD_EXIST;
		}
		log_debug("application info populated: %s [%d]",
		    msg->name, msg->pid);
	} else {
		free(state->name);
		log_debug("application info updated: %s [%d]",
		    msg->name, msg->pid);
	}
	state->name = strdup(msg->name);
	state->pid = msg->pid;
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a template ver message.
 */
static enum amsg_err msg_server_handle_template_ver(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_template_ver *msg
	    = (struct msg_template_ver *)info->payload;

	log_debug("Received template_ver %s", msg->template_ver);
	ds_update_template_ver_to_cloud(msg->template_ver);

	return AMSG_ERR_NONE;
}

/*
 * Message handler for a client destinations request message.  Sends
 * destinations or responds to the requester.
 */
static enum amsg_err msg_server_handle_dests_req(struct amsg_endpoint *endpoint,
	struct amsg_resp_info *resp_info)
{
	struct msg_client_dests msg;

	msg_server_client_dests_init(&msg);
	if (resp_info) {
		return amsg_send_resp(&resp_info,
		    MSG_INTERFACE_CLIENT, MSG_CLIENT_DESTS, &msg, sizeof(msg));
	}
	return amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_DESTS,
	    &msg, sizeof(msg), NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Message handler for a client destinations update registration.
 */
static enum amsg_err msg_server_handle_dests_reg(struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state = msg_server_client_state(endpoint);

	if (!state) {
		return AMSG_ERR_PRIVS;
	}
	if (!state->reg_dest_changes) {
		state->reg_dest_changes = true;
		/* Send current dests on registration */
		msg_server_handle_dests_req(endpoint, NULL);
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a client time set request.  Time will only be updated
 * if the reported time source is higher than or equal to the current
 * time source.  Setting the time source to CS_SYSTEM_LOCKED will prevent
 * all further updates to the system time.
 */
static enum amsg_err msg_server_handle_time_set(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct msg_client_time *msg = (struct msg_client_time *)info->payload;
	int rc;

	if (!msg->source) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	if (!msg->time_utc) {
		/* Just set the source */
		msg->time_utc = time(NULL);
	}
	rc = ds_clock_set(msg->time_utc, msg->source);
	if (rc < 0) {
		return AMSG_ERR_APPLICATION;
	}
	if (rc > 0) {
		return AMSG_ERR_PRIVS;
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a client time info request.  Since all processes have
 * access to the system time, this is only useful to check the current time
 * source.
 */
static enum amsg_err msg_server_handle_time_req(struct amsg_endpoint *endpoint,
	struct amsg_resp_info *resp_info)
{
	struct msg_client_time msg;

	msg_server_client_time_init(&msg);
	if (resp_info) {
		return amsg_send_resp(&resp_info,
		    MSG_INTERFACE_CLIENT, MSG_CLIENT_TIME, &msg, sizeof(msg));
	}
	return amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_TIME,
	    &msg, sizeof(msg), NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Message handler for a client time update registration.  Time info will be
 * set to all registered clients when the time or time source is changed by
 * devd.
 */
static enum amsg_err msg_server_handle_time_reg(struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state = msg_server_client_state(endpoint);

	if (!state) {
		return AMSG_ERR_PRIVS;
	}
	if (!state->reg_clock_events) {
		state->reg_clock_events = true;
		/* Send current dests on registration */
		msg_server_handle_time_req(endpoint, NULL);
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a user registration info request.
 */
static enum amsg_err msg_server_handle_registration_req(
	struct amsg_endpoint *endpoint, struct amsg_resp_info *resp_info)
{
	struct msg_client_reg_info msg;

	msg_server_reg_info_init(&msg, false);
	if (resp_info) {
		return amsg_send_resp(&resp_info,
		    MSG_INTERFACE_CLIENT, MSG_CLIENT_USERREG,
		    &msg, sizeof(msg));
	}
	return amsg_send(endpoint, MSG_INTERFACE_CLIENT, MSG_CLIENT_USERREG,
	    &msg, sizeof(msg), NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Message handler for a user registration info update registration.
 */
static enum amsg_err msg_server_handle_registration_reg(
	struct amsg_endpoint *endpoint)
{
	struct msg_client_state *state = msg_server_client_state(endpoint);

	if (!state) {
		return AMSG_ERR_PRIVS;
	}
	if (!state->reg_registration_changes) {
		state->reg_registration_changes = true;
		/* Send current dests on registration */
		msg_server_handle_registration_req(endpoint, NULL);
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a client push-button registration window event.
 */
static enum amsg_err msg_server_handle_registration_button(
	struct amsg_endpoint *endpoint)
{
	if (ds_push_button_reg_start() < 0) {
		return AMSG_ERR_APPLICATION;
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a client setup info update.
 */
static enum amsg_err msg_server_handle_setup_info(
	struct amsg_endpoint *endpoint, const struct amsg_msg_info *info)
{
	struct msg_client_setup_info *msg =
	    (struct msg_client_setup_info *)info->payload;

	if (msg->setup_token[0] &&
	    ds_update_setup_token(msg->setup_token) < 0) {
		return AMSG_ERR_APPLICATION;
	}
	if (msg->location[0] &&
	    ds_update_location(msg->location) < 0) {
		return AMSG_ERR_APPLICATION;
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler to allow devd to fetch commands, properties, and schedules
 * from the cloud.  This is generally sent from the primary application to
 * indicate it is ready to handle incoming data.
 */
static enum amsg_err msg_server_handle_listen(
	struct amsg_endpoint *endpoint)
{
	ds_enable_ads_listen();
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a system DHCP client event update.
 */
static enum amsg_err msg_server_dhcp_event(const struct amsg_msg_info *info)
{
	struct msg_system_dhcp *msg = (struct msg_system_dhcp *)info->payload;

	switch (msg->event) {
	case MSG_SYSTEM_DHCP_UNBOUND:
		ds_net_down();
		break;
	case MSG_SYSTEM_DHCP_BOUND:
		ds_net_up();
		break;
	case MSG_SYSTEM_DHCP_REFRESH:
		ds_net_down();
		ds_net_up();
		break;
	}
	return AMSG_ERR_NONE;
}

/*
 * Message handler for a Wi-Fi info message.  Updates feature list and SSID.
 */
static enum amsg_err msg_server_handle_wifi_info(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	struct device_state *dev = &device;
	json_t *msg_obj;
	json_t *features_arr;
	bool bool_val;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}

	features_arr = json_object_get(msg_obj, "features");
	if (json_is_array(features_arr)) {
		json_decref(dev->wifi_features);
		dev->wifi_features = json_incref(features_arr);
	}
	free(dev->connected_ssid);
	dev->connected_ssid = json_get_string_dup(msg_obj, "connected_ssid");
	if (!json_get_bool(msg_obj, "ap_enabled", &bool_val)) {
		dev->wifi_ap_enabled = bool_val ? 1 : 0;
	}
	ds_step();
	json_decref(msg_obj);
	return AMSG_ERR_NONE;
}

/*
 * Message interface handler for the application interface.
 */
static enum amsg_err msg_server_app_interface_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_APPLICATION);

	switch (info->type) {
	case MSG_APP_INFO:
		return msg_server_handle_app_info(endpoint, info);
	case MSG_APP_TEMPLATE_VER:
		return msg_server_handle_template_ver(endpoint, info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Message interface handler for the client interface.
 */
static enum amsg_err msg_server_client_interface_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_CLIENT);

	switch (info->type) {
	case MSG_CLIENT_DESTS_REQ:
		return msg_server_handle_dests_req(endpoint, resp_info);
	case MSG_CLIENT_DESTS_REG:
		return msg_server_handle_dests_reg(endpoint);
	case MSG_CLIENT_TIME_REQ:
		return msg_server_handle_time_req(endpoint, resp_info);
	case MSG_CLIENT_TIME_REG:
		return msg_server_handle_time_reg(endpoint);
	case MSG_CLIENT_TIME_SET:
		return msg_server_handle_time_set(endpoint, info);
	case MSG_CLIENT_USERREG_REQ:
		return msg_server_handle_registration_req(endpoint, resp_info);
	case MSG_CLIENT_USERREG_REG:
		return msg_server_handle_registration_reg(endpoint);
	case MSG_CLIENT_USERREG_WINDOW_START:
		return msg_server_handle_registration_button(endpoint);
	case MSG_CLIENT_SETUP_INFO:
		return msg_server_handle_setup_info(endpoint, info);
	case MSG_CLIENT_LISTEN:
		return msg_server_handle_listen(endpoint);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Message interface handler for the System Status interface.
 */
static enum amsg_err wifi_interface_system_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_SYSTEM);

	switch (info->type) {
	case MSG_SYSTEM_DHCP_EVENT:
		return msg_server_dhcp_event(info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Handle cond setup token request.
 */
static enum amsg_err serv_wifi_setup_token(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	json_t *msg_obj;
	const char *token;
	enum amsg_err err;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}

	token = json_get_string(msg_obj, "setup_token");

	if (token) {
		/* Update devd's setup token field */
		ds_update_setup_token(token);
	}

	json_decref(msg_obj);

	err = msg_send_json(endpoint,
	    MSG_INTERFACE_WIFI, MSG_WIFI_SETUP_TOKEN_RESP, NULL,
	    NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		log_err("msg_send_json ret %s", amsg_err_string(err));
	}

	return AMSG_ERR_NONE;
}


/*
 * Message interface handler for the Wi-Fi interface.
 */
static enum amsg_err msg_server_wifi_interface_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_WIFI);

	switch (info->type) {
	case MSG_WIFI_INFO:
		return msg_server_handle_wifi_info(endpoint, info);
	case MSG_WIFI_SETUP_TOKEN:
		return serv_wifi_setup_token(endpoint, info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Message interface handler for the OTA interface.
 */
static enum amsg_err msg_server_ota_interface_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	struct device_state *dev = &device;
	struct msg_ota_status *msg = (struct msg_ota_status *)info->payload;

	ASSERT(info->interface == MSG_INTERFACE_OTA);

	if (info->type != MSG_OTA_STATUS) {
		return AMSG_ERR_TYPE_UNSUPPORTED;
	}
	dev->ota_status = msg->status;
	ds_step();
	return AMSG_ERR_NONE;
}

/*
 * Register message interface handlers for devd.
 */
static void msg_server_set_interface_handlers(void)
{
	amsg_set_interface_handler(MSG_INTERFACE_APPLICATION,
	    msg_server_app_interface_handler);
	amsg_set_interface_handler(MSG_INTERFACE_CLIENT,
	    msg_server_client_interface_handler);
	amsg_set_interface_handler(MSG_INTERFACE_SYSTEM,
	    wifi_interface_system_handler);
	amsg_set_interface_handler(MSG_INTERFACE_WIFI,
	    msg_server_wifi_interface_handler);
	amsg_set_interface_handler(MSG_INTERFACE_OTA,
	    msg_server_ota_interface_handler);
	msg_conf_init(devd_conf_privs, ARRAY_LEN(devd_conf_privs),
	    msg_server_conf_notify, msg_server_handle_factory_reset);
}

/*
 * Handle a connect or disconnect event for a message client.
 */
static void msg_server_handle_client_event(struct amsg_endpoint *endpoint,
	enum amsg_endpoint_event event)
{
	struct device_state *dev = &device;
	struct msg_client_state *state;

	switch (event) {
	case AMSG_ENDPOINT_CONNECT:
		log_debug("connected: fd=%d", endpoint->sock);
		break;
	case AMSG_ENDPOINT_DISCONNECT:
		if (!log_debug_enabled()) {
			break;
		}
		state = msg_server_client_state(endpoint);
		if (state && state->name) {
			if (dev->ads_listen &&
			    !strcmp(MSG_APP_NAME_APP, state->name)) {
				/*
				 * Application disconnected, so automatically
				 * clear listen flag so new commands are not
				 * fetched.  Application will enable listen
				 * again when reconnected and ready to receive
				 * updates.
				 */
				log_info("ADS listen disabled");
				dev->ads_listen = 0;
			}
			log_debug("disconnected: name=%s duration=%llums",
			    state->name, (long long unsigned)time_mtime_ms() -
			    state->connect_time_ms);
		} else {
			log_debug("disconnected");
		}
		break;
	}
}

/*
 * Setup and start devd's amsg server for local communications.
 */
int msg_server_create(struct device_state *dev, const char *path, int mode)
{
	if (amsg_server_init(&server, &dev->file_events, &dev->timers) < 0) {
		log_err("failed to initialize local messaging server");
		return -1;
	}
	msg_server_client_state_id = msg_generate_unique_id();
	amsg_server_set_max_sessions(&server, MSG_SERVER_MAX_SESSIONS);
	amsg_server_set_session_event_callback(&server,
	    msg_server_handle_client_event);
	msg_server_set_interface_handlers();
	if (amsg_server_start(&server, path, mode) < 0) {
		log_err("failed to start local messaging server");
		return -1;
	}
	return 0;
}

/*
 * Stop and free resources associated with devd's amsg server.
 */
void msg_server_cleanup(void)
{
	amsg_server_cleanup(&server);
}

/*
 * Lookup a msg server client by application name.
 * Returns NULL if not found.
 */
struct msg_client_state *msg_server_lookup_client(const char *app_name)
{
	struct amsg_endpoint *endpoint;
	struct msg_client_state *state;

	AMSG_SERVER_SESSION_FOREACH(endpoint, &server) {
		state = msg_server_client_state(endpoint);
		if (!state || !state->name) {
			continue;
		}
		if (!strcmp(state->name, app_name)) {
			return state;
		}
	}
	return NULL;
}

/*
 * Send a message to connected clients.  A filter function may be used to
 * send to a subset of clients.  If a complete_callback is provided,
 * it will be invoked after all message responses have been received.
 * Returns 0 on success or -1 if no messages were sent.
 */
int msg_server_broadcast(uint8_t interface, uint8_t type,
	const void *payload, size_t size,
	void (*complete_callback)(bool, void *), void *complete_arg,
	bool (*filter)(const struct amsg_endpoint *),
	uint32_t timeout_ms)
{
	struct amsg_server_session *s, *s_next;
	struct msg_server_broadcast_state *state;

	state = (struct msg_server_broadcast_state *)calloc(1, sizeof(*state));
	if (!state) {
		log_err("malloc failed");
		return -1;
	}
	state->complete_callback = complete_callback;
	state->complete_arg = complete_arg;

	/* Iterate through server sessions in a delete-safe manner */
	s = LIST_FIRST(&server.session_list);
	while (s) {
		s_next = LIST_NEXT(s, list_entry);
		if (!filter || filter(&s->endpoint)) {
			if (amsg_send(&s->endpoint, interface, type,
			    payload, size, msg_server_broadcast_resp_handler,
			    state, timeout_ms) == AMSG_ERR_NONE) {
				++state->num_pending;
			}
		}
		s = s_next;
	}
	if (!state->num_pending) {
		/* No messages were sent, so cleanup immediately */
		free(state);
		return -1;
	}
	return 0;
}

/*
 * Send dest mask to clients registered for updates.
 */
int msg_server_dests_changed_event(void (*complete_callback)(bool, void *),
	void *complete_arg)
{
	struct msg_client_dests msg;

	msg_server_client_dests_init(&msg);
	return msg_server_broadcast(MSG_INTERFACE_CLIENT, MSG_CLIENT_DESTS,
	    &msg, sizeof(msg), complete_callback, complete_arg,
	    msg_server_dest_change_filter, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Send clock info to clients registered for updates.
 */
int msg_server_clock_event(void (*complete_callback)(bool, void *),
	void *complete_arg)
{
	struct msg_client_time msg;

	msg_server_client_time_init(&msg);
	return msg_server_broadcast(MSG_INTERFACE_CLIENT, MSG_CLIENT_TIME,
	    &msg, sizeof(msg), complete_callback, complete_arg,
	    msg_server_clock_event_filter, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Send user registration status and regtoken to clients registered for updates.
 */
int msg_server_registration_event(bool status_changed,
	void (*complete_callback)(bool, void *), void *complete_arg)
{
	struct msg_client_reg_info msg;

	msg_server_reg_info_init(&msg, status_changed);
	return msg_server_broadcast(MSG_INTERFACE_CLIENT, MSG_CLIENT_USERREG,
	    &msg, sizeof(msg), complete_callback, complete_arg,
	    msg_server_reg_change_filter, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Send factory reset command to connected clients.
 */
int msg_server_factory_reset_event(void (*complete_callback)(bool, void *),
	void *complete_arg)
{
	return msg_server_broadcast(MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_FACTORY_RESET, NULL, 0, complete_callback, complete_arg,
	    NULL, MSG_TIMEOUT_DEFAULT_MS);
}
