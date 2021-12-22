/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_H__
#define __LIB_APP_H__

#include <ayla/utypes.h>
#include <ayla/token_table.h>


struct timer_head;
struct file_event_table;

/*
 * Connectivity event type definitions.
 */
#define APP_CONN_TYPES(def)				\
	def(Cloud,		APP_CONN_CLOUD)		\
	def(LAN,		APP_CONN_LAN)

DEF_ENUM(app_conn_type, APP_CONN_TYPES);

/* Connectivity event type names */
extern const char * const app_conn_type_strings[];

/*
 * Set the factory config file path and the startup config directory.
 * If the startup_dir is NULL, the configuration library will create the
 * startup config file in the same directory as the factory config.
 * Returns 0 on success and -1 on error.
 */
int app_set_conf_file(const char *factory_file, const char *startup_dir);

/*
 * Set a custom socket directory.  This is optional, and should only be used
 * if there is a need to override the default socket directory: /var/run.
 * Returns 0 on success and -1 on error.
 */
int app_set_socket_directory(const char *socket_dir);

/*
 * Select the cloud template version to use with this application.  The
 * selected template should contain properties that match those defined in
 * the application.  The template version is generally set when the
 * application starts, but it may be updated later if required.
 * Returns 0 on success and -1 on error.
 */
int app_set_template_version(const char *template_version);

/*
 * Register a callback to notify the application that it is about to terminate.
 */
void app_set_exit_func(void (*func)(int));

/*
 * Register a callback to be called for each main loop iteration.  This is
 * useful for routine operations that should be performed following an event.
 * Set max_period_ms to a non-zero value to wake up the main loop
 * periodically to ensure the poll function is called, even if there are no
 * events.
 */
void app_set_poll_func(void (*func)(void), u32 max_period_ms);

/*
 * Register a callback to notify the application that a factory reset has
 * been requested.  Any application-specific factory reset actions should be
 * performed by this callback.
 */
void app_set_factory_reset_func(void (*func)(void));

/*
 * Register a callback to notify the application when cloud or LAN
 * connections have gone up or down.
 */
void app_set_conn_event_func(void (*func)(enum app_conn_type, bool));

/*
 * Register a callback to notify the application when device user
 * registration has changed.  This may be useful to re-send user-accessible
 * properties on behalf of a new user.
 */
void app_set_registration_event_func(void (*func)(bool));

/*
 * Enable or disable debug logging.  This may be done at any time.
 */
void app_set_debug(bool enable);

/*
 * Returns true if debug is enabled.
 */
bool app_get_debug(void);

/*
 * Returns a pointer to the timer_head structure for the main thread.  This
 * allows use of the timer library interface defined in ayla/timer.h.
 */
struct timer_head *app_get_timers(void);

/*
 * Returns a pointer to the file_event_table structure for the main thread.
 * This allows use of the file event library interface defined in
 * ayla/file_event.h.
 */
struct file_event_table *app_get_file_events(void);

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
	int (*init_func)(void), int (*start_func)(void));

/*
 * Setup and run the application.  This function contains the program main
 * loop, so it will consume the calling thread and only return if an error
 * occurred during setup, or app_exit() was called.
 * Returns -1 on error, or the exit status on exit.
 */
int app_run(bool foreground);

/*
 * Terminate the application and exit with the specified status.
 * Cleanup functions are called before exiting.  If force is set to true, the
 * program will exit immediately (meaning this function will never return).
 * Otherwise, the main loop is signaled to stop and app_run() returns with the
 * exit status, allowing the application to shutdown gracefully.  This function
 * is thread-safe.
 */
void app_exit(int status, bool force);

/*
 * Schedule a callback to be run by the main loop.  This is useful to raise
 * events without the risk of building up a large call stack.  In addition,
 * this may be used as a simple timer, by setting delay_ms to a non-zero value.
 * Returns 0 on success and -1 on error.
 */
int app_dispatch(void (*callback)(void *), void *callback_arg, u32 delay_ms);

#endif /* __LIB_APP_H__ */
