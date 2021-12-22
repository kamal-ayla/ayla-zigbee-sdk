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
#include <string.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <platform/conf.h>
#include <platform/system.h>


#define PLATFORM_MAC_ADDR_SIZE	(18) /* 6 hex bytes + 5 delims + '\0' = 18 */

#define OLD_OPENWRT_LED_DRIVER	/* Support OpenWRT Backfire */
#define LED_CLOUD_PATH		"/sys/class/leds/jnrd6040:red:B"
#define LED_BLINK_FAST_MS	100
#define LED_BLINK_SLOW_MS	500

enum led_state {
	LED_OFF,
	LED_ON,
	LED_BLINK_SLOW,
	LED_BLINK_FAST
};

#define ENABLE_SETUP_MODE_CMD	"/sbin/apply-setup-mode.sh enable"
#define DISABLE_SETUP_MODE_CMD	"/sbin/apply-setup-mode.sh disable"
#define HARDWARE_ID_FILE	"/sys/devices/platform/hwid/id"

static int platform_led_write(const char *led_path, const char *field,
	const char *val, ...) __attribute__((format(printf, 3, 4)));

/*
 * Helper function to write an OpenWRT LED driver field.
 */
static int platform_led_write(const char *led_path, const char *field,
	const char *val, ...)
{
	size_t path_len = strlen(led_path) + strlen(field) + 1;
	char path[path_len + 1];
	FILE *fp;
	va_list args;

	snprintf(path, sizeof(path), "%s/%s", led_path, field);
	fp = fopen(path, "w");
	if (!fp) {
		log_err("failed to open LED driver %s: %m", path);
		return -1;
	}
	va_start(args, val);
	vfprintf(fp, val, args);
	va_end(args);
	fclose(fp);
	return 0;
}

/*
 * Set an LED using OpenWRT's LED driver
 */
int platform_led_set(const char *driver_path, enum led_state state)
{
	int rc = 0;

	switch (state) {
	case LED_OFF:
		rc |= platform_led_write(driver_path, "trigger", "none");
#ifndef OLD_OPENWRT_LED_DRIVER
		rc |= platform_led_write(driver_path, "default", "0");
#endif
		break;
	case LED_ON:
#ifndef OLD_OPENWRT_LED_DRIVER
		rc |= platform_led_write(driver_path, "trigger", "none");
		rc |= platform_led_write(driver_path, "default", "1");
#else
		rc |= platform_led_write(driver_path, "trigger", "default-on");
#endif
		break;
	case LED_BLINK_SLOW:
	case LED_BLINK_FAST:
		rc |= platform_led_write(driver_path, "trigger", "timer");
		if (state == LED_BLINK_FAST) {
			rc |= platform_led_write(driver_path, "delay_off", "%u",
			    LED_BLINK_FAST_MS);
			rc |= platform_led_write(driver_path, "delay_on", "%u",
			    LED_BLINK_FAST_MS);
		} else {
			rc |= platform_led_write(driver_path, "delay_off", "%u",
			    LED_BLINK_SLOW_MS);
			rc |= platform_led_write(driver_path, "delay_on", "%u",
			    LED_BLINK_SLOW_MS);
		}
		break;
	}
	return rc;
}

/*
 * Get the mac address of the device
 */
int platform_get_mac_addr(struct ether_addr *addr)
{
	char mac_str[PLATFORM_MAC_ADDR_SIZE];
	struct ether_addr *mac;

	if (platform_conf_read("ethaddr", mac_str, sizeof(mac_str)) < 0) {
		return -1;
	}
	mac = ether_aton(mac_str);
	if (!mac) {
		log_err("failed to parse MAC address");
		return -1;
	}
	*addr = *mac;
	return 0;
}

/*
 * Get a unique hardware id of the device
 */
int platform_get_hw_id(char *buf, size_t size)
{
	FILE *id_file = NULL;
	int rc = -1;
	size_t nbytes;

	id_file = fopen(HARDWARE_ID_FILE, "r");
	if (!id_file) {
		log_err("cannot open %s: %m", HARDWARE_ID_FILE);
		goto cleanup;
	}

	nbytes = fread(buf, 1, size - 1, id_file);
	if (nbytes <= 0) {
		log_err("cannot read %s", HARDWARE_ID_FILE);
		goto cleanup;
	}
	buf[nbytes] = '\0';
	rc = 0;

cleanup:
	if (id_file) {
		fclose(id_file);
	}
	return rc;
}

/*
 * Configures the system when setup mode is enabled or disabled.
 */
void platform_apply_setup_mode(bool enable)
{
	static bool initialized;
	static bool last_enable;
	const char *cmd;

	if (initialized && enable == last_enable) {
		return;
	}
	/* Invoke platform specific script */
	cmd = enable ? ENABLE_SETUP_MODE_CMD : DISABLE_SETUP_MODE_CMD;
	if (system(cmd)) {
		log_warn("\'%s\' failed", cmd);
		return;
	}
	initialized = true;
	last_enable = enable;
}

/*
 * Configure cloud LED
 */
void platform_configure_led(bool cloud_up, bool registered,
	bool reg_window_open)
{
	static enum led_state last_state = -1;
	enum led_state new_state;

	if (!cloud_up) {
		new_state = LED_OFF;
	} else if (reg_window_open) {
		new_state = LED_BLINK_FAST;
	} else if (!registered) {
		new_state = LED_BLINK_SLOW;
	} else {
		new_state = LED_ON;
	}
	if (last_state != new_state &&
	    !platform_led_set(LED_CLOUD_PATH, new_state)) {
		last_state = new_state;
	}
}

/*
 * Perform any actions needed to factory reset the system.  This is invoked
 * after devd has performed all operations needed to factory reset Ayla modules.
 * Actions that could be performed here might include setting LEDs to indicate
 * the reset status or running an external script.  During a normal factory
 * reset, platform_reboot() will be called after this function returns.
 */
void platform_factory_reset(void)
{
	if (system("/sbin/factory-reset.sh")) {
		log_err("factory reset script failed");
	}
}

/*
 * Perform a platform reset
 */
void platform_reset(void)
{
	if (system("reboot")) {
		log_err("reboot failed");
	}
}
