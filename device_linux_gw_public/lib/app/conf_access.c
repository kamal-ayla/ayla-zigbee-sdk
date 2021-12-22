/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/conf_io.h>
#include <ayla/log.h>

#include <app/conf_access.h>
#include "msg_client_internal.h"


static void (*conf_factory_reset_handler)(void);

/*
 * Execute a factory reset on the appd side
 */
void conf_factory_reset_execute(void)
{
	log_info("factory reset");
	if (conf_factory_reset_handler) {
		conf_factory_reset_handler();
	}
	conf_factory_reset();
}

/*
 * Register a handler for handling a factory reset. Lib will take care of
 * replacing the startup config with factory config and rebooting appd.
 */
void conf_factory_reset_handler_set(void (*handler)(void))
{
	conf_factory_reset_handler = handler;
	msg_client_set_factory_reset_callback(conf_factory_reset_execute);
}
