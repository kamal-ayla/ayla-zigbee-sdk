/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/queue.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/build.h>
#include <ayla/ayla_interface.h>

#include <app/app.h>
#include <app/msg_client.h>
#include <app/ops.h>
#include <app/props.h>

#include "appd.h"

const char *appd_version = "appd " BUILD_VERSION_LABEL;
const char *appd_template_version = "appd_demo 1.4";

#define APP_FILE_UP_PATH "etc/files/ayla_solution.png"
#define APP_FILE_DOWN_PATH "etc/files/file_down"

static u8 blue_button;
static u8 blue_led;
static u8 green_led;
static u8 batch_hold;
static struct prop_batch_list *batched_dps; /* to demo batching */
static u8 file_up_test;
static int input;
static int output;
static double decimal_in;
static double decimal_out;
static char cmd[PROP_STRING_LEN + 1];	/* add 1 for \0 */
static char log[PROP_STRING_LEN + 1];	/* add 1 for \0 */

static u8 large_msg_down[PROP_MSG_LEN];
static u8 large_msg_up[PROP_MSG_LEN];
static char large_msg_up_test[PROP_STRING_LEN + 1];	/* add 1 for \0 */

/* install path of agent */
static char install_root[PATH_MAX];

/* file location of the latest value of file_down */
static char file_down_path[PATH_MAX];

/* file location of the latest value of file_up */
static char file_up_path[PATH_MAX];


/*
 * Send the appd software version.
 */
static enum err_t appd_send_version(struct prop *prop, int req_id,
	const struct op_options *opts)
{
	return prop_val_send(prop, req_id, appd_version, 0, NULL);
}

/*
 * File download complete callback
 */
static int appd_file_down_confirm_cb(struct prop *prop, const void *val,
	size_t len, const struct op_options *opts,
	const struct confirm_info *confirm_info)
{
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		log_info("%s download succeeded (requested at %llu)",
		    prop->name, opts->dev_time_ms);
	} else {
		log_info("%s download from %d failed with err %u "
		    "(requested at %llu)", prop->name, DEST_ADS,
		    confirm_info->err, opts->dev_time_ms);
	}

	return 0;
}

/*
 * File upload complete callback
 */
static int appd_file_up_confirm_cb(struct prop *prop, const void *val,
	size_t len, const struct op_options *opts,
	const struct confirm_info *confirm_info)
{
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		log_info("%s upload succeeded (requested at %llu)",
		    prop->name, opts->dev_time_ms);
	} else {
		log_info("%s upload to %d failed with err %u "
		    "(requested at %llu)", prop->name, DEST_ADS,
		    confirm_info->err, opts->dev_time_ms);
	}

	return 0;
}

/*
 * Confirm callback for properties
 */
static int appd_prop_confirm_cb(struct prop *prop, const void *val, size_t len,
	    const struct op_options *opts,
	    const struct confirm_info *confirm_info)
{
	if (!prop) {
		log_debug("NULL prop argument");
		return 0;
	}

	if (confirm_info->status == CONF_STAT_SUCCESS) {
		log_info("%s = %s send at %llu to dests %d succeeded",
		    prop->name, prop_val_to_str(val, prop->type),
		    opts->dev_time_ms, confirm_info->dests);
	} else {
		log_info("%s = %s send at %llu to dests %d failed with err %u",
		    prop->name, prop_val_to_str(val, prop->type),
		    opts->dev_time_ms, confirm_info->dests, confirm_info->err);
	}
	return 0;
}

/*
 * Confirm callback for properties
 */
static int appd_batch_confirm_handler(int batch_id,
	    const struct op_options *opts,
	    const struct confirm_info *confirm_info)
{
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		log_info("Batch id %d send at %llu to dests %d succeeded",
		    batch_id, opts->dev_time_ms, confirm_info->dests);
	} else {
		log_info("Batch id %d send at %llu to dests %d failed "
		    "with err %u", batch_id, opts->dev_time_ms,
		    confirm_info->dests, confirm_info->err);
	}

	return 0;
}

/*
 * Ads failure callback for properties. Called whenever a particular property
 * update failed to reach the cloud due to connectivity loss.
 */
static int appd_prop_ads_failure_cb(struct prop *prop, const void *val,
	size_t len, const struct op_options *opts)
{
	if (!prop) {
		log_debug("NULL prop argument");
		return 0;
	}

	log_info("%s = %s failed to send to ADS at %llu",
	    prop->name,
	    prop_val_to_str(val ? val : prop->arg, prop->type),
	    opts ? opts->dev_time_ms : 0);
	return 0;
}

/*
 * Sample set handler for "input" property.  Squares "input" and sends result
 * as the "output" property.
 */
static int appd_input_set(struct prop *prop, const void *val, size_t len,
			const struct op_args *args)
{
	struct op_options opts = {.confirm = 1};
	struct prop_batch_list *result;
	struct prop *output_prop;
	struct prop_metadata *metadata;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		return -1;
	}

	/* For purposes of the demo, put the square of input into "output" */
	if (input > 46340 || input < -46340) {
		/* square would overflow */
		output = -1;
	} else {
		output = input * input;
	}
	/* Add some datapoint metadata */
	metadata = prop_metadata_alloc();
	prop_metadata_addf(metadata, "source", "%d x %d", input, input);
	opts.metadata = metadata;

	output_prop = prop_lookup("output");
	if (batch_hold) {
		/* batch the datapoint for sending later */
		result = prop_arg_batch_append(batched_dps, output_prop, &opts);
		if (result) {
			batched_dps = result;
		}
	} else {
		/* send out immediately */
		output_prop->send(output_prop, 0, &opts);
	}
	prop_metadata_free(metadata);
	return 0;
}

/*
 * Sample set handler for "cmd" property.  Copies new value to a "log"
 * property and sends it.
 */
static int appd_cmd_set(struct prop *prop, const void *val, size_t len,
			const struct op_args *args)
{
	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		return -1;
	}

	/* for purposes of the demo, copy the value of cmd into log */
	snprintf(log, sizeof(log), "%s", cmd);
	prop_send_by_name("log");

	return 0;
}

/*
 * Sample set handler for "decimal_in" property.  Copies new value
 * to a "decimal_out" property and sends it.
 */
static int appd_decimal_in_set(struct prop *prop, const void *val, size_t len,
				const struct op_args *args)
{
	struct prop *decimal_out_prop;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		return -1;
	}

	/* for purposes of the demo, copy the val to decimal_out */
	decimal_out = *(double *)val;
	decimal_out_prop = prop_lookup("decimal_out");

	if (batch_hold) {
		/* batch the datapoint for sending later */
		batched_dps = prop_arg_batch_append(batched_dps,
		    decimal_out_prop, NULL);
	} else {
		/* send out immediately */
		decimal_out_prop->send(decimal_out_prop, 0, NULL);
	}

	return 0;
}

/*
 * Set handler for batch_hold property. When 'batch_hold' is set to 1,
 * 'decimal_out' and 'output' datapoints will be batched until 'batch_hold' is
 * set back to zero.
 */
static int appd_batch_hold_set(struct prop *prop, const void *val, size_t len,
			const struct op_args *args)
{
	struct op_options opts = {.confirm = 1};

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		return -1;
	}
	if (!batch_hold && batched_dps) {
		prop_batch_send(&batched_dps, &opts, NULL);
	}

	return 0;
}

/*
 * Sample set handler for green and blue LED properties.  If both LEDs
 * are enabled, the "Blue_button" property is set.
 */
static int appd_led_set(struct prop *prop, const void *val, size_t len,
			const struct op_args *args)
{
	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		return -1;
	}

	/*
	 * To test sending properties, use green & blue to toggle blue_button.
	 */
	if ((blue_led && green_led) != blue_button) {
		blue_button = blue_led && green_led;
		prop_send_by_name("Blue_button");
	}
	return 0;
}

/*
 * Send up a FILE property
 */
static int appd_file_up_test_set(struct prop *prop, const void *val, size_t len,
				const struct op_args *args)
{
	struct op_options opts = {.confirm = 1};
	struct prop_metadata *metadata;
	struct prop *file_up_prop;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		return -1;
	}

	if (!file_up_test) {
		return 0;
	}
	/* Set file_up_test back to 0 and send it up */
	file_up_test = 0;
	prop_send(prop);

	/* Include some datapoint metadata with the file */
	metadata = prop_metadata_alloc();
	prop_metadata_add(metadata, "path", file_up_path);
	prop_metadata_add(metadata, "trigger", prop->name);

	/* Begin sending file */
	file_up_prop = prop_lookup("file_up");
	opts.metadata = metadata;
	file_up_prop->send(file_up_prop, 0, &opts);

	prop_metadata_free(metadata);
	return 0;
}

static void debug_print_memory(const void *addr, uint32_t len)
{
	char buf[64];
	uint8_t *paddr = (uint8_t *)addr;
	int i, j, k;

	i = 0;
	while (i < len) {
		memset(buf, 0, sizeof(buf));
		for (j = 0; ((j < 20) && (i + j < len)); j++) {
			k = ((paddr[i + j] & 0xF0) >> 4);
			buf[j * 3 + 0] = ((k < 10)
			    ? (k + '0') : (k - 10 + 'A'));
			k = (paddr[i + j] & 0x0F);
			buf[j * 3 + 1] = ((k < 10)
			    ? (k + '0') : (k - 10 + 'A'));
			buf[j * 3 + 2] = ' ';
		}
		log_debug("%s", buf);
		i += j;
	}
}

/*
 * Sample set handler for "large message" property.
 */
static int appd_large_prop_set(struct prop *prop, const void *val, size_t len,
			const struct op_args *args)
{
	log_debug("name %s, len %u", prop->name, len);
	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("prop_arg_set failed");
		return 0;
	}
	debug_print_memory(prop->arg, prop->len);
	return 0;
}

/*
 * Sample send handler for "large message" property.
 */
static int appd_large_prop_send(struct prop *prop, int req_id,
			const struct op_options *opts)
{
	struct op_options st_opts;
	if (opts) {
		st_opts = *opts;
	} else {
		memset(&st_opts, 0, sizeof(st_opts));
	}
	st_opts.confirm = 1;
	debug_print_memory(prop->arg, prop->len);
	return prop_arg_send(prop, req_id, &st_opts);
}

/*
 * Large message prop complete callback
 */
static int appd_large_prop_confirm_cb(struct prop *prop, const void *val,
	size_t len, const struct op_options *opts,
	const struct confirm_info *confirm_info)
{
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		log_info("%s len %u succeeded (requested at %llu)",
		    prop->name, prop->len, opts->dev_time_ms);
		debug_print_memory(prop->arg, prop->len);
	} else {
		log_info("%s len %u from %d failed with err %u "
		    "(requested at %llu)", prop->name, prop->len, DEST_ADS,
		    confirm_info->err, opts->dev_time_ms);
	}

	return 0;
}

/*
 * Sample set handler for "large_msg_up_test" property.
 * Copies new value to "large_msg_up" property and sends it.
 */
static int appd_large_msg_up_test_set(struct prop *prop,
		const void *val, size_t len, const struct op_args *args)
{
	struct prop *large_msg_up_prop;

	if (prop_arg_set(prop, val, len, args) != ERR_OK) {
		log_err("set %s prop value failed", prop->name);
		return -1;
	}

	large_msg_up_prop = prop_lookup("large_msg_up");
	if (!large_msg_up_prop) {
		log_info("no large_msg_up prop");
		return 0;
	}

	/* for purposes of the demo, copy the value into large_msg_up */
	snprintf(large_msg_up_prop->arg, large_msg_up_prop->buflen,
	    "%s", large_msg_up_test);
	large_msg_up_prop->len = strlen(large_msg_up_test);
	prop_send(large_msg_up_prop);

	return 0;
}


static struct prop appd_prop_table[] = {
	/* Application software version property */
	{
		.name = "version",
		.type = PROP_STRING,
		.send = appd_send_version
	},
	/* Sample properties for testing with demo app */
	/****** Boolean Props ******/
	{
		.name = "Green_LED",
		.type = PROP_BOOLEAN,
		.set = appd_led_set,
		.send = prop_arg_send,
		.arg = &green_led,
		.len = sizeof(green_led),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "Blue_LED",
		.type = PROP_BOOLEAN,
		.set = appd_led_set,
		.send = prop_arg_send,
		.arg = &blue_led,
		.len = sizeof(blue_led),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "Blue_button",
		.type = PROP_BOOLEAN,
		.send = prop_arg_send,
		.arg = &blue_button,
		.len = sizeof(blue_button),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	/****** Integer Props ******/
	{
		.name = "input",
		.type = PROP_INTEGER,
		.set = appd_input_set,
		.send = prop_arg_send,
		.arg = &input,
		.len = sizeof(input),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "output",
		.type = PROP_INTEGER,
		.send = prop_arg_send,
		.arg = &output,
		.len = sizeof(output),
		.confirm_cb = appd_prop_confirm_cb,
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	/****** Decimal Props ******/
	{
		.name = "decimal_in",
		.type = PROP_DECIMAL,
		.set = appd_decimal_in_set,
		.send = prop_arg_send,
		.arg = &decimal_in,
		.len = sizeof(decimal_in),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "decimal_out",
		.type = PROP_DECIMAL,
		.send = prop_arg_send,
		.arg = &decimal_out,
		.len = sizeof(decimal_out),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	/****** String Props ******/
	{
		.name = "cmd",
		.type = PROP_STRING,
		.set = appd_cmd_set,
		.send = prop_arg_send,
		.arg = cmd,
		.len = sizeof(cmd),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "log",
		.type = PROP_STRING,
		.send = prop_arg_send,
		.arg = log,
		.len = sizeof(log),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	/****** File Props ******/
	{
		.name = "file_down",
		.type = PROP_FILE,
		.set = prop_arg_set,
		.arg = file_down_path,
		.len = sizeof(file_down_path),
		.confirm_cb = appd_file_down_confirm_cb,
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "file_up",
		.type = PROP_FILE,
		.send = prop_arg_send,
		.arg = file_up_path,
		.len = sizeof(file_up_path),
		.confirm_cb = appd_file_up_confirm_cb,
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	/* Helper props for demo'ing file props */
	{
		.name = "file_up_test",
		.type = PROP_BOOLEAN,
		.set = appd_file_up_test_set,
		.send = prop_arg_send,
		.arg = &file_up_test,
		.len = sizeof(file_up_test),
	},
	/*
	 * Helper prop for demo'ing batching. When 'batch_hold' is set to 1,
	 * 'decimal_out' and 'output' datapoints will be batched until
	 * 'batch_hold' is set back to zero.
	 */
	{
		.name = "batch_hold",
		.type = PROP_BOOLEAN,
		.set = appd_batch_hold_set,
		.send = prop_arg_send,
		.arg = &batch_hold,
		.len = sizeof(batch_hold),
	},
	{
		.name = "large_msg_down",
		.type = PROP_MESSAGE,
		.set = appd_large_prop_set,
		.send = appd_large_prop_send,
		.arg = large_msg_down,
		.len = sizeof(large_msg_down),
		.buflen = sizeof(large_msg_down),
		.confirm_cb = appd_large_prop_confirm_cb,
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "large_msg_up",
		.type = PROP_MESSAGE,
		.send = appd_large_prop_send,
		.arg = large_msg_up,
		.len = sizeof(large_msg_up),
		.buflen = sizeof(large_msg_up),
		.confirm_cb = appd_large_prop_confirm_cb,
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
	{
		.name = "large_msg_up_test",
		.type = PROP_STRING,
		.set = appd_large_msg_up_test_set,
		.send = prop_arg_send,
		.arg = large_msg_up_test,
		.len = sizeof(large_msg_up_test),
		.ads_failure_cb = appd_prop_ads_failure_cb,
	},
};

/*
 * Hook for the app library to initialize the user-defined application.
 */
int appd_init(void)
{
	int len;

	log_info("application initializing");

	/* Determine install root path and set file paths */
	len = readlink("/proc/self/exe",
	    install_root, sizeof(install_root));
	install_root[len] = '\0';
	dirname(dirname(install_root));
	snprintf(file_up_path, sizeof(file_up_path), "%s/%s",
	    install_root, APP_FILE_UP_PATH);
	snprintf(file_down_path, sizeof(file_down_path), "%s/%s",
	    install_root, APP_FILE_DOWN_PATH);

	/* Load property table */
	prop_add(appd_prop_table, ARRAY_LEN(appd_prop_table));

	/* Set property confirmation handlers */
	prop_batch_confirm_handler_set(appd_batch_confirm_handler);

	return 0;
}

/*
 * Hook for the app library to start the user-defined application.  Once
 * This function returns, the app library will enable receiving updates from
 * the cloud, and begin to process tasks on the main thread.
 */
int appd_start(void)
{
	log_info("application starting");

	/* Set template version to select the correct cloud template */
	app_set_template_version(appd_template_version);

	/*
	 * Perform any application-specific tasks needed prior to starting.
	 */

	return 0;
}

/*
 * Hook for the app library to notify the user-defined application that the
 * process is about to terminate.
 */
void appd_exit(int status)
{
	log_info("application exiting with status: %d", status);

	/*
	 * Perform any application-specific tasks needed prior to exiting.
	 */
}

/*
 * Hook for the app library to notify the user-defined application that a
 * factory reset is about to occur.
 */
void appd_factory_reset(void)
{
	log_info("application factory reset");

	/*
	 * Perform any application-specific tasks needed for a factory reset.
	 */
}

/*
 * Hook for the app library to notify the user-defined application that the
 * the connectivity status has changed.
 */
void appd_connectivity_event(enum app_conn_type type, bool up)
{
	static bool first_connection = true;

	log_info("%s connection %s", app_conn_type_strings[type],
	    up ? "UP" : "DOWN");

	/* Some tasks should be performed when first connecting to the cloud */
	if (type == APP_CONN_CLOUD && up && first_connection) {
		/*
		 * Send all from-device properties to update the service on
		 * first connection.  This is helpful to ensure that the
		 * application's startup state is immediately synchronized
		 * with the cloud.
		 */
		prop_send_from_dev(true);

		/* Request all to-device properties from the cloud */
		prop_request_to_dev();

		first_connection = false;
	}
}

/*
 * Hook for the app library to notify the user-defined application that the
 * the user registration status has changed.
 */
void appd_registration_event(bool registered)
{
	log_info("device user %s", registered ? "registered" : "unregistered");

	if (registered) {
		/*
		 * Send all from-device properties to update the service after
		 * user registration.  This is helpful to ensure that the
		 * device's current state is visible to the new user, since
		 * the cloud hides all user-level property datapoints created
		 * prior to user registration.
		 */
		prop_send_from_dev(true);
	}
}

