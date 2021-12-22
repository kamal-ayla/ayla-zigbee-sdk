/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_WIFI_PLATFORM_H__
#define __AYLA_WIFI_PLATFORM_H__


/*
 * Hardware-specific Wi-Fi interface.  The public functions in this interface
 * should be implemented to interact with the Wi-Fi hardware.
 */

/* Indicate the result of a platform operation */
enum wifi_platform_result {
	PLATFORM_SUCCESS	= 0,
	PLATFORM_FAILURE	= -1,
	PLATFORM_CANCELED	= -2
};

/*
 * Initialize platform interface.  Perform actions needed before configuration
 * is loaded.
 */
void wifi_platform_init(const struct wifi_state *wifi_state);

/*
 * Perform startup checks and setup needed after configuration is loaded.
 */
int wifi_platform_start(void);

/*
 * Stop Wi-Fi
 */
int wifi_platform_exit(void);


/*
 * Enable station mode and prepare to connect to a network.
 */
int wifi_platform_station_start(void);

/*
 * Disable station mode.
 */
int wifi_platform_station_stop(void);

/*
 * Return true if station mode is enabled.
 */
bool wifi_platform_station_enabled(void);


/*
 * Enable AP mode, and configure the AP and associated network interface
 * with the specified parameters.
 */
int wifi_platform_ap_start(const struct wifi_profile *prof, int channel,
	const struct in_addr *ip_addr);

/*
 * Disable AP mode.
 */
int wifi_platform_ap_stop(void);

/*
 * Return true if AP mode is enabled.
 */
bool wifi_platform_ap_enabled(void);

/*
 * Return the number of stations connected to the AP, or -1 on error;
 */
int wifi_platform_ap_stations_connected(void);


/*
 * Start a new scan job.  Return 0 on success and -1 on error.  If a callback
 * is provided and this function returned success, the callback must be invoked
 * to indicate the result of the operation.
 */
int wifi_platform_scan(void (*)(enum wifi_platform_result));

/*
 * Cancel an ongoing scan job.
 */
void wifi_platform_scan_cancel(void);

/*
 * Return true if a scan is in progress.
 */
bool wifi_platform_scanning(void);


/*
 * Configure a network and attempt to associate with it.
 * Return 0 on success and -1 on error.  If a callback is provided and this
 * function returned success, the callback must be invoked to indicate the
 * result of the operation.
 */
int wifi_platform_associate(const struct wifi_profile *prof,
	void (*)(enum wifi_platform_result));

/*
 * Cancel an ongoing attempt to associate with a network.
 */
void wifi_platform_associate_cancel(void);

/*
 * Return true if associating.
 */
bool wifi_platform_associating(void);

/*
 * Disable current network
 */
int wifi_platform_leave_network(void);


/*
 * Start WPS.  Return 0 on success and -1 on error.  If a callback
 * is provided and this function returned success, the callback must be invoked
 * to indicate the result of the operation.
 */
int wifi_platform_wps_start(void (*)(enum wifi_platform_result));

/*
 * Cancel WPS.
 */
void wifi_platform_wps_cancel(void);

/*
 * Return true if WPS is active.
 */
bool wifi_platform_wps_started(void);

#endif /* __AYLA_WIFI_PLATFORM_H__ */
