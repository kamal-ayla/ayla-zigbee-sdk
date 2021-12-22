
/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/timer.h>
#include <ayla/file_event.h>
#include <ayla/file_io.h>
#include <ayla/conf_io.h>
#include <ayla/socket.h>	/* Needed for legacy socket interface */
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>

#include <app/props.h>
#include <app/sched.h>
#include <app/ops.h>
#include <app/msg_client.h>

#include <ayla/gateway_interface.h>
#include <app/gateway.h>
#include "msg_client_internal.h"
#include "data_internal.h"
#include "ops_internal.h"

#include <app/app.h>


#define APP_POLL_PERIOD_MIN_MS	100
#define APP_CONNECT_RETRY_MS	1000

#define APP_PROP_NAME_TEMPLATE_VERSION		"oem_host_version"

/* Connectivity event type names */
DEF_NAME_TABLE(app_conn_type_strings, APP_CONN_TYPES);

/*
 * Main loop event dispatch state.
 */
struct app_dispatch_state {
	struct timer timer;
	void (*callback)(void *);
	void *callback_arg;
};

/*
 * Application state.
 */
struct app_state {
	bool debug;
	bool foreground;

	char *name;
	char *sw_version;
	char *template_version;
	char *conf_factory_file;
	char *conf_startup_dir;
	char *socket_dir;
	u32 poll_period_max_ms;

	int (*init)(void);
	int (*start)(void);
	void (*exit)(int);
	void (*poll)(void);
	void (*factory_reset)(void);
	void (*connectivity_event)(enum app_conn_type, bool);
	void (*registration_event)(bool);

	bool initialized;
	bool started;
	bool daemonized;

	bool exit_pending;
	int exit_status;

	struct file_event_table file_events;
	struct timer_head timers;
	struct timer connect_timer;
};

static struct app_state app_state;

/* Forward declarations */
static int app_template_version_send(struct prop *prop);
static int app_template_version_confirm_handler(struct prop *prop,
	const void *val, size_t len, const struct op_options *opts,
	const struct confirm_info *confirm_info);

/*
 * The template version is currently a special property named oem_host_version.
 * It is managed internally by the application library for convenience.
 * By not defining a send function or value arg, we prevent the props library
 * from including this property in any group send or automatic recovery
 * operations.
 */
static struct prop app_template_version = {
	.name = APP_PROP_NAME_TEMPLATE_VERSION,
	.type = PROP_STRING,
	.send = prop_arg_send,
	.ads_recovery_cb = app_template_version_send,
	.confirm_cb = app_template_version_confirm_handler
};

static int app_template_version_send(struct prop *prop)
{
	struct app_state *app = &app_state;
	struct op_options opts = {
		.dests = DEST_ADS,
	};
	ASSERT(prop == &app_template_version);

	if (!app->template_version) {
		log_err("no template version");
		return -1;
	}
	prop->arg = app->template_version;
	prop->len = (strlen(app->template_version) + 1);
	if (ops_cloud_up()) {
		if (prop_arg_send(prop, 0, &opts) != ERR_OK) {
			log_err("%s send failed", prop->name);
			return -1;
		}
	} else {
		/* Schedule the property to be sent when the cloud is up */
		prop->ads_failure = 1;
	}
	return 0;
}

/*
 * Confirmation handler for template version property.
 */
static int app_template_version_confirm_handler(struct prop *prop,
	const void *val, size_t len, const struct op_options *opts,
	const struct confirm_info *confirm_info)
{
	struct app_state *app = &app_state;

	ASSERT(prop == &app_template_version);

	if (!app->template_version) {
		/* Set externally */
		return 0;
	}
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		if (val && !strcmp((const char *)val, app->template_version)) {
			log_debug("%s sent successfully: %s", prop->name,
			    (const char *)val);
		}
	} else if (confirm_info->err != CONF_ERR_CONN) {
		/* Will not retry for non-connection errors */
		log_err("failed sending %s: %s", prop->name, (const char *)val);
	}
	return 0;
}

/*
 * Using the ops library event handler for consistency.
 */
static void app_client_event_handler(enum ops_client_event event)
{
	struct app_state *app = &app_state;

	switch (event) {
	case OPS_EVENT_CLOUD_DOWN:
		if (app->connectivity_event) {
			app->connectivity_event(APP_CONN_CLOUD, false);
		}
		break;
	case OPS_EVENT_CLOUD_UP:
		if (app->connectivity_event) {
			app->connectivity_event(APP_CONN_CLOUD, true);
		}
		break;
	case OPS_EVENT_LAN_DOWN:
		if (app->connectivity_event) {
			app->connectivity_event(APP_CONN_LAN, false);
		}
		break;
	case OPS_EVENT_LAN_UP:
		if (app->connectivity_event) {
			app->connectivity_event(APP_CONN_LAN, true);
		}
		break;
	case OPS_EVENT_UNREGISTERED:
		if (app->registration_event) {
			app->registration_event(false);
		}
		break;
	case OPS_EVENT_REGISTERED:
		if (app->registration_event) {
			app->registration_event(true);
		}
		break;
	default:
		break;
	}
}

/*
 * Handle a factory reset request.
 */
static void app_handle_factory_reset(void)
{
	struct app_state *app = &app_state;

	if (app->factory_reset) {
		app->factory_reset();
	}
	conf_factory_reset();
}

/*
 * The app/data library is in the process of being deprecated, but is still
 * used for exchanging property and schedule data with the cloud client.
 */
static int app_client_connect_legacy(struct app_state *app)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s/%s", app->socket_dir,
	    MSG_APP_NAME_APP, SOCKET_NAME);
	return data_client_init_internal(&app->file_events, path);
}

/*
 * If client is disconnected, retry after a short delay.
 */
static void app_client_connection_status_handler(bool connected)
{
	struct app_state *app = &app_state;

	if (!connected) {
		log_warn("set connect_timer %d ms", APP_CONNECT_RETRY_MS);
		timer_set(&app->timers, &app->connect_timer,
		    APP_CONNECT_RETRY_MS);
	}
}

/*
 * Connect to the cloud client daemon.
 */
static int app_client_connect(struct app_state *app)
{
	char path[PATH_MAX];

	if (!app->socket_dir) {
		/* Use default socket directory, if not set */
		if (app_set_socket_directory(MSG_SOCKET_DIR_DEFAULT) < 0) {
			return -1;
		}
	}
	snprintf(path, sizeof(path), "%s/%s/%s", app->socket_dir,
	    MSG_APP_NAME_CLIENT, MSG_SOCKET_DEFAULT);
	if (msg_client_connect(path) < 0) {
		log_err("open app msg socket failed");
		return -1;
	}

	/* Maybe amsg_disconnect reset connect_timer, so cancel again */
	timer_cancel(&app_state.timers, &app_state.connect_timer);

	/* XXX properties are still handled by the legacy socket interface */
	if (app_client_connect_legacy(app) < 0) {
		log_err("open app data socket failed");
		/* close msg socket if app sock open failed */
		msg_client_connect_close();
		return -1;
	}
	return 0;
}

/*
 * Timed reconnect to message interface.
 */
static void app_connect_timeout(struct timer *timer)
{
	log_warn("reconnect to devd");
	timer_cancel(&app_state.timers, &app_state.connect_timer);
	if (app_client_connect(&app_state) < 0) {
		timer_set(&app_state.timers, &app_state.connect_timer,
		    APP_CONNECT_RETRY_MS);
	}
}

/*
 * set reconnect to devd timer.
 */
void app_set_reconnect(void)
{
	log_warn("set reconnect to devd timer");
	timer_set(&app_state.timers, &app_state.connect_timer,
	    APP_CONNECT_RETRY_MS);
}

/*
 * Dispatch event callback.
 */
static void app_dispatch_timeout(struct timer *timer)
{
	struct app_dispatch_state *state =
	    CONTAINER_OF(struct app_dispatch_state, timer, timer);

	if (state->callback) {
		state->callback(state->callback_arg);
	}
	free(state);
}

/*
 * Initialize internal data structures and libraries.  This is called before
 * the user application init hook.
 */
static int app_init_internal(struct app_state *app)
{
	/* Initialize connection timer */
	timer_init(&app->connect_timer, app_connect_timeout);
	/* Initialize file event listener state */
	file_event_init(&app->file_events);
	/* Initialize config subsystem */
	if (conf_init(app->conf_factory_file, app->conf_startup_dir) < 0) {
		log_err("failed to initialize config");
		return -1;
	}
	/* Initialize the cloud client message interface */
	if (msg_client_init(&app->file_events, &app->timers) < 0) {
		log_err("failed to initialize client interface");
		return -1;
	}
	msg_client_set_connection_status_callback(
	    app_client_connection_status_handler);
	msg_client_set_factory_reset_callback(app_handle_factory_reset);
	/* Initialize the application operations queue */
	ops_init_internal(&app->file_events, &app->timers);
	ops_set_client_event_handler(app_client_event_handler);
	/* Initialize the properties library and add internal props */
	prop_initialize();
	prop_add(&app_template_version, 1);
	/* Initialize the gateway subsystem */
	gw_initialize();
	/* Initialize the schedules library */
	sched_init(&app->timers);
	return 0;
}

/*
 * Start internal data structures and libraries.  This is called before
 * the user application start hook.
 */
static int app_start_internal(struct app_state *app)
{
	/* Load config file and apply to application state */
	if (conf_load() < 0) {
		log_err("failed to load config");
		return -1;
	}
	/* Attempt to connect to cloud client service */
	if (app_client_connect(app) < 0) {
		return -1;
	}
	/* Begin processing schedule events */
	sched_start();
	return 0;
}

/*
 * Cleanup internal data structures and libraries.  This is called after
 * the user application exit hook.
 */
static void app_exit_internal(struct app_state *app)
{
	timer_cancel(&app->timers, &app->connect_timer);
	msg_client_cleanup();
	sched_destroy();
	conf_cleanup();
}

/*
 * Prepare to terminate the process.  This is invoked directly or indirectly by
 * app_exit().
 */
static void app_exit_callback(struct app_state *app)
{
	log_debug("terminating with status %d", app->exit_status);
	if (app->exit) {
		app->exit(app->exit_status);
	}
	app_exit_internal(app);
}

/*
 * Set the factory config file path and the startup config directory.
 * If the startup_dir is NULL, the configuration library will create the
 * startup config file in the same directory as the factory config.
 * Returns 0 on success and -1 on error.
 */
int app_set_conf_file(const char *factory_file, const char *startup_dir)
{
	struct app_state *app = &app_state;
	char *factory = NULL;
	char *startup = NULL;
	char *temp;

	ASSERT(factory_file != NULL);

	if (app->started) {
		log_warn("cannot set config paths after start");
		return -1;
	}
	factory = realpath(factory_file, NULL);
	if (!factory) {
		log_err("invalid factory config path: %s (%m)", factory_file);
		goto error;
	}
	/* Fill in default file name for backward compatibility */
	if (file_is_dir_ayla(factory)) {
		if (!app->name) {
			log_err("app name needed for default file name");
			goto error;
		}
		if (asprintf(&temp, "%s/%s.conf", factory, app->name) < 0) {
			log_err("malloc failed");
			goto error;
		}
		free(factory);
		factory = temp;
	}
	if (startup_dir) {
		startup = realpath(startup_dir, NULL);
		if (!startup) {
			log_err("invalid startup config path: %s (%m)",
			    startup_dir);
			goto error;
		}
	}
	free(app->conf_factory_file);
	free(app->conf_startup_dir);
	app->conf_factory_file = factory;
	app->conf_startup_dir = startup;
	log_debug("factory config: %s, startup config dir: %s", factory,
	    startup ? startup : "default");
	return 0;
error:
	free(factory);
	free(startup);
	return -1;
}

/*
 * Set a custom socket directory.  This is optional, and should only be used
 * if there is a need to override the default socket directory: /var/run.
 * Returns 0 on success and -1 on error.
 */
int app_set_socket_directory(const char *socket_dir)
{
	struct app_state *app = &app_state;
	char *socket;

	ASSERT(socket_dir != NULL);

	if (app->started) {
		log_warn("cannot set socket directory while connected");
		return -1;
	}
	socket = realpath(socket_dir, NULL);
	if (!socket) {
		log_err("invalid socket directory: %s (%m)", socket_dir);
		return -1;
	}
	free(app->socket_dir);
	app->socket_dir = socket;
	log_debug("socket dir: %s", socket);
	return 0;
}

/*
 * Select the cloud template version to use with this application.  The
 * selected template should contain properties that match those defined in
 * the application.  The template version is generally set when the
 * application starts, but it may be updated later if required.
 */
int app_set_template_version(const char *template_version)
{
	struct app_state *app = &app_state;
	struct amsg_endpoint *endpoint = msg_client_endpoint();

	ASSERT(template_version != NULL);

	free(app->template_version);
	app->template_version = strdup(template_version);
#ifdef USE_OEM_HOST_VERSION_PROP
	return app_template_version_send(&app_template_version);
#else
	app_template_version.arg = app->template_version;
	app_template_version.len = (strlen(app->template_version) + 1);
	/* Send template version to devd(cloud) */
	if (msg_send_template_ver(endpoint, app->template_version) < 0) {
		log_err("msg_send_template_ver failed");
		amsg_disconnect(endpoint);
		return -1;
	}
	return 0;
#endif
}

/*
 * Register a callback to notify the application that it is about to terminate.
 */
void app_set_exit_func(void (*func)(int))
{
	app_state.exit = func;
}

/*
 * Register a callback to be called for each main loop iteration.  This is
 * useful for routine operations that should be performed following an event.
 * Set max_period_ms to a non-zero value to wake up the main loop
 * periodically to ensure the poll function is called, even if there are no
 * events.
 */
void app_set_poll_func(void (*func)(void), u32 max_period_ms)
{
	struct app_state *app = &app_state;

	app->poll = func;
	if (func) {
		if (max_period_ms < APP_POLL_PERIOD_MIN_MS) {
			log_warn("max poll period must be at least %ums",
			    APP_POLL_PERIOD_MIN_MS);
			max_period_ms = APP_POLL_PERIOD_MIN_MS;
		}
		app->poll_period_max_ms = max_period_ms;
	} else {
		app->poll_period_max_ms = 0;
	}
}

/*
 * Register a callback to notify the application that a factory reset has
 * been requested.  Any application-specific factory reset actions should be
 * performed by this callback.
 */
void app_set_factory_reset_func(void (*func)(void))
{
	app_state.factory_reset = func;
}

/*
 * Register a callback to notify the application when cloud or LAN
 * connections have gone up or down.
 */
void app_set_conn_event_func(void (*func)(enum app_conn_type, bool))
{
	app_state.connectivity_event = func;
}

/*
 * Register a callback to notify the application when device user
 * registration has changed.  This may be useful to re-send user-accessible
 * properties on behalf of a new user.
 */
void app_set_registration_event_func(void (*func)(bool))
{
	app_state.registration_event = func;
}

/*
 * Enable or disable debug logging.  This may be done at any time.
 */
void app_set_debug(bool enable)
{
	struct app_state *app = &app_state;

	if (app->debug == enable) {
		return;
	}
	app->debug = enable;
	if (enable) {
		log_set_options(LOG_OPT_DEBUG | LOG_OPT_TIMESTAMPS);
	} else {
		log_clear_options(LOG_OPT_DEBUG | LOG_OPT_TIMESTAMPS);
	}
}

/*
 * Returns true if debug is enabled.
 */
bool app_get_debug(void)
{
	return app_state.debug;
}

/*
 * Returns a pointer to the timer_head structure for the main thread.  This
 * allows use of the timer library interface defined in ayla/timer.h.
 */
struct timer_head *app_get_timers(void)
{
	return &app_state.timers;
}

/*
 * Returns a pointer to the file_event_table structure for the main thread.
 * This allows use of the file event library interface defined in
 * ayla/file_event.h.
 */
struct file_event_table *app_get_file_events(void)
{
	return &app_state.file_events;
}

/*
 * Initialize the application state and set handlers.  The name parameter
 * should be the application name.  This may be used to tag log messages and
 * for the config file name, so it should not contain spaces or slashes.
 * The version is the appd software version and will be sent up as a
 * property named "version", which must be added to the cloud template and
 * configured as the Host SW Version.
 * Returns 0 on success and -1 on error.
 */
int app_init(const char *name, const char *version,
	int (*init_func)(void), int (*start_func)(void))
{
	struct app_state *app = &app_state;

	ASSERT(name != NULL);
	ASSERT(version != NULL);
	ASSERT(strpbrk(name, " /") == NULL);

	free(app->name);
	app->name = strdup(name);

	/* XXX Not currently used */
	free(app->sw_version);
	app->sw_version = strdup(version);

	app->init = init_func;
	app->start = start_func;

	log_init(app->name, LOG_OPT_FUNC_NAMES | LOG_OPT_CONSOLE_OUT);
	log_set_subsystem(LOG_SUB_APP);

	return 0;
}

/*
 * Setup and run the application.  This function contains the program main
 * loop, so it will consume the calling thread and only return if an error
 * occurred during setup, or app_exit() was called.
 * Returns -1 on error, or the exit status on exit.
 */
int app_run(bool foreground)
{
	struct app_state *app = &app_state;
	s64 next_timeout_ms;
	int rc;

	if (app->started) {
		log_warn("already running");
		return -1;
	}
	if (!app->conf_factory_file) {
		log_err("factory config file not set");
		return -1;
	}
	/* Create a blank default config file, if none was provided */
	if (access(app->conf_factory_file, R_OK) < 0) {
		log_info("creating new factory config: %s",
		    app->conf_factory_file);
		conf_save_empty(app->conf_factory_file);
	}
	/* Initialize application */
	if (!app->initialized) {
		/* Initialize app and libraries */
		if (app_init_internal(app) < 0) {
			log_err("internal initialization routine failed");
			return -1;
		}
		/* Init application */
		if (app->init && app->init() < 0) {
			log_err("application initialization failed");
			return -1;
		}
		app->initialized = true;
	}
	/* Daemonize process */
	if (!foreground && !app->daemonized) {
		if (daemon(0, 0) < 0) {
			log_err("daemon() failed: %m");
			return -1;
		}
		log_clear_options(LOG_OPT_CONSOLE_OUT);
		app->daemonized = true;
	}
	/* Start app and libraries */
	if (app_start_internal(app) < 0) {
		log_err("internal start routine failed");
		return -1;
	}
	/* Start application */
	if (app->start && app->start() < 0) {
		log_err("application start failed");
		return -1;
	}
	/* Enable cloud updates.  Ops lib schedules listen enable as needed */
	ops_app_ready_for_cloud_updates();
	app->started = true;

	/* Main loop */
	while (!app->exit_pending) {
		/* Run timer events and calculate the next timeout */
		next_timeout_ms = timer_advance(&app->timers);
		/* Enforce the user-defined maximum poll period */
		if (app->poll_period_max_ms && (next_timeout_ms < 0 ||
		    next_timeout_ms > app->poll_period_max_ms)) {
			next_timeout_ms = app->poll_period_max_ms;
		}
		/* Wait for file event or timer timeout */
		rc = file_event_poll(&app->file_events, next_timeout_ms);
		if (rc < 0) {
			/* Poll failed/interrupted: sleep 1ms for spin safety */
			usleep(1000);
			continue;
		}
		/* Poll application */
		if (app->poll) {
			app->poll();
		}
	}
	/* Only return on exit */
	app_exit_callback(app);
	return app->exit_status;
}

/*
 * Terminate the application and exit with the specified status.
 * Cleanup functions are called before exiting.  If force is set to true, the
 * program will exit immediately (meaning this function will never return).
 * Otherwise, the main loop is signaled to stop and app_run() returns with the
 * exit status, allowing the application to shutdown gracefully.  This function
 * is thread-safe.
 */
void app_exit(int status, bool force)
{
	struct app_state *app = &app_state;

	app->exit_pending = true;
	app->exit_status = status;
	if (force) {
		app_exit_callback(app);
		exit(status);
		return;
	}
	/* Wake up the main loop in a thread-safe manner */
	ops_notify();
}

/*
 * Schedule a callback to be run by the main loop.  This is useful to raise
 * events without the risk of building up a large call stack.  In addition,
 * this may be used as a simple timer, by setting delay_ms to a non-zero value.
 * Returns 0 on success and -1 on error.
 */
int app_dispatch(void (*callback)(void *), void *callback_arg, u32 delay_ms)
{
	struct app_state *app = &app_state;
	struct app_dispatch_state *state;

	state = (struct app_dispatch_state *)malloc(sizeof(*state));
	if (!state) {
		log_err("malloc failed");
		return -1;
	}
	state->callback = callback;
	state->callback_arg = callback_arg;
	timer_init(&state->timer, app_dispatch_timeout);
	timer_set(&app->timers, &state->timer, delay_ms);
	return 0;
}

