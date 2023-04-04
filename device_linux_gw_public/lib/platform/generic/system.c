/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ayla/utypes.h>
#include <ayla/log.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <platform/system.h>


/*
 * Get the mac address of the device
 */
int platform_get_mac_addr(struct ether_addr *addr)
{
	return -1;
}

/*
 * Get a unique hardware id of the device
 */
int platform_get_hw_id(char *buf, size_t size)
{
	return -1;
}

/*
 * Configures the system when setup mode is enabled or disabled.
 */
void platform_apply_setup_mode(bool enable)
{
	return;
}

/*
 * Configure LED settings if available on the platform
 */
void platform_configure_led(bool cloud_up, bool registered,
	bool reg_window_open)
{
	return;
}

/*
 * Perform any actions needed to factory reset the system.  This is invoked
 * after devd has performed all operations needed to factory reset Ayla modules.
 * Actions that could be performed here might include setting LEDs to indicate
 * the reset status or running an external script.  During a normal factory
 * reset, platform_reset() will be called after this function returns.
 */
void platform_factory_reset(void)
{
	log_debug("platform factory reset: /usr/bin/rtfd --soft");
	if(system("/usr/bin/rtfd --soft"))
	{
		log_warn("/usr/bin/rtfd --soft factory reset failed");
	}

	return;
}

/*
 * Reboot the system.
 */
void platform_reset(void)
{
     if (system("reboot")) {
         log_warn("reboot failed");
    }
}
