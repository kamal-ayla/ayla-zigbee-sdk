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
#include <ayla/network_utils.h>
#include <platform/system.h>


/*
 * Get the mac address of the device
 */
int platform_get_mac_addr(struct ether_addr *addr)
{
	struct net_ifinfo info;

	if (net_get_ifinfo("eth0", &info) < 0) {
		log_err("failed to get network info");
		return -1;
	}
	*addr = info.hw_addr;
	return 0;
}

/*
 * Get a unique hardware id of the device
 */
int platform_get_hw_id(char *buf, size_t size)
{
	FILE *fp;
	size_t bytes;

	/* Read Raspberry Pi hardware ID from CPU info printout */
	fp = popen("awk '/Serial/{print $3}' /proc/cpuinfo", "r");
	if (!fp) {
		log_err("hardware serial # read failed");
		return -1;
	}
	bytes = fread(buf, 1, size - 1, fp);
	fclose(fp);
	if (!bytes) {
		log_warn("hardware serial # empty");
		return -1;
	}
	/* Trim trailing newline */
	if (bytes > 0 && buf[bytes - 1] == '\n') {
		--bytes;
	}
	buf[bytes] = '\0';
	log_debug("%s", buf);
	return 0;
}

/*
 * Configures the system when setup mode is enabled or disabled.
 */
void platform_apply_setup_mode(bool enable)
{
	int rc;
	/*
	 * Demonstrate setup mode by enabling or disabling
	 * SSH on the system.
	 */
	if (enable) {
		log_debug("enabling SSH");
		rc = system("/etc/init.d/ssh start");
	} else {
		log_debug("disabling SSH");
		rc = system("/etc/init.d/ssh stop");
	}
	if (rc == -1) {
		log_err("command failed");
	}
	return;
}

/*
 * Configure LED settings if available on the platform
 */
void platform_configure_led(bool cloud_up, bool registered,
	bool reg_window_open)
{
	log_debug("cloud_up=%u registered=%u reg_window=%u",
	    cloud_up, registered, reg_window_open);
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
