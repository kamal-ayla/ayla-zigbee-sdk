/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/types.h>
#include <sys/socket.h>

#include <sys/poll.h>

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/file_event.h>
#include <ayla/conf_io.h>
#include <ayla/time_utils.h>
#include <ayla/timer.h>
#include <ayla/log.h>

#include "cond.h"
#include "wifi.h"
#include "wifi_platform.h"

static void (*wifi_scan_complete_handler)(void);
static void (*wifi_connect_state_change_handler)(void);
static void (*wifi_ap_mode_change)(bool);

struct wifi_state wifi_state;

static struct timer step_timer;		/* Timer for state change handling */
static struct timer join_timer;		/* Timer for join timeout */
static struct timer idle_timer;		/* Timer for idle timeout */
static struct timer ap_stop_timer;	/* Timer for delayed AP stop */
static struct timer error_timer;	/* Timer for retry after error */

DEF_NAME_TABLE(wifi_errors, WIFI_ERRORS);
DEF_NAME_TABLE(wifi_states, WIFI_CONN_STATES);
DEF_NAME_TABLE(wifi_wps_states, WIFI_WPS_STATES);

static void wifi_step_handler(struct timer *timer);


/*
 * Schedule the state change handler.  Should be called
 * after an event that may result in a state change.
 * Using a timer with no delay as a mechanism to avoid reentrant
 * step handler calls.
 */
void wifi_step(void)
{
	if (!timer_active(&step_timer)) {
		wifi_timer_set(&step_timer, 0);
	}
}

/*
 * Fetch the state of the Wi-Fi station network interface and apply
 * it to the Wi-Fi state and current history entry.
 */
static int wifi_if_info_update(struct wifi_state *wifi)
{
	if (net_get_ifinfo(wifi->ifname, &wifi->if_info) < 0) {
		log_err("failed to get %s info", wifi->ifname);
		return -1;
	}
	log_debug("updated %s IP addr: %s", wifi->ifname,
	    inet_ntoa(wifi->if_info.addr.sin_addr));

	/* XXX set default GW */

	/* Update history with current IP information */
	if (wifi->curr_hist) {
		wifi_hist_update(wifi->curr_hist->error, NULL, NULL);
	}
	return 0;
}

/*
 * Return the number of valid profiles.
 */
static size_t wifi_prof_count(struct wifi_state *wifi)
{
	struct wifi_profile *prof;
	size_t count = 0;

	for (prof = wifi->profile; prof < &wifi->profile[WIFI_PROF_CT];
	    prof++) {
		if (!prof->enable) {
			continue;
		}
		++count;
	}
	return count;
}

/*
 * Create a new profile in an empty slot.  If all slots are full,
 * overwrite one not in use.
 */
static struct wifi_profile *wifi_prof_new(struct wifi_state *wifi)
{
	struct wifi_profile *prof = NULL;

	for (prof = wifi->profile;
	     prof < wifi->profile + WIFI_PROF_CT; ++prof) {
		if (!prof->ssid.len) {
			break;
		}
	}
	if (prof == wifi->profile + WIFI_PROF_CT) {
		/*
		 * No free slots, so override the oldest one,
		 * or second to oldest, if the oldest one is in use.
		 */
		prof = wifi->profile;
		if (wifi->curr_profile == prof) {
			++prof;
		}
	}
	/* clear any old profile data */
	memset(prof, 0, sizeof(*prof));
	return prof;
}

/*
 * Save the profile in the unsaved_profile slot.  This should be called
 * at the appropriate time when save-on-connect or save-on-cloud-up policies
 * are selected.
 */
static void wifi_prof_commit_unsaved(struct wifi_state *wifi)
{
	struct wifi_profile *unsaved_prof = &wifi->unsaved_prof;
	struct wifi_profile *prof;

	ASSERT(wifi->prof_save_mode != WC_SAVE_NEVER &&
	    wifi->prof_save_mode != WC_SAVE_ON_ADD);

	/* Only save if valid and it is the current profile */
	if (!unsaved_prof->enable || wifi->curr_profile != unsaved_prof) {
		return;
	}
	prof = wifi_prof_lookup(wifi, &unsaved_prof->ssid);
	if (!prof) {
		prof = wifi_prof_new(wifi);
	}
	memcpy(prof, unsaved_prof, sizeof(*prof));
	memset(unsaved_prof, 0, sizeof(*unsaved_prof));
	if (wifi->curr_profile == unsaved_prof) {
		wifi->curr_profile = prof;
	}
	if (wifi->pref_profile == unsaved_prof) {
		wifi->pref_profile = prof;
	}
	conf_save();
}

/*
 * Updates the scan association for any profiles that have matching SSIDs
 * and do not exceed the scan result's best advertised security.
 * If multiple scan results are found for one profile, the result with the
 * best signal strength is used.
 */
static void wifi_scan_prof_update(struct wifi_state *wifi)
{
	struct wifi_scan_result *scan;
	struct wifi_profile *prof;
	enum wifi_sec sec;

	for (scan = wifi->scan;
	    scan < &wifi->scan[WIFI_SCAN_CT]; ++scan) {
		if (!scan->time_ms) {
			continue;
		}
		prof = wifi_prof_lookup(wifi, &scan->ssid);
		if (!prof) {
			continue;
		}
		sec = wifi_scan_get_best_security(scan, &prof->key);
		if (!(sec & WSEC_VALID)) {
			continue;
		}
		if (wifi_sec_downgrade(sec, prof->sec)) {
			continue;
		}
		if (prof->scan && prof->scan->signal >= scan->signal) {
			continue;
		}
		prof->scan = scan;

		log_debug("SSID=%s BSSID=%s signal=%d",
		    wifi_ssid_to_str(&prof->ssid),
		    net_ether_to_str(&scan->bssid), scan->signal);
	}
}

/*
 * Return true if AP mode window is open, otherwise, false.
 */
static bool wifi_ap_allow(struct wifi_state *wifi)
{
	/* Window is closed if AP mode is disabled */
	if (!wifi->ap_enable) {
		return false;
	}
	/* Window is closed if unconfigured or if close time has passed */
	if (!wifi->ap_window_close_ms ||
	    wifi->ap_window_close_ms <= time_mtime_ms()) {
		return false;
	}
	/* Window is closed if secure mode enabled and valid profile(s) exist */
	if (wifi->ap_window_secure && wifi_prof_count(wifi) > 0) {
		return false;
	}
	return true;
}

/*
 * Returns true if a scan/rescan is needed.
 */
static bool wifi_scan_needed(void)
{
	struct wifi_state *wifi = &wifi_state;

	/* Scan needed if a scan has never been completed */
	if (!wifi->scan_time_ms) {
		return true;
	}
	/* Okay to defer scan if a preferred network was set */
	if (wifi->pref_profile) {
		return false;
	}
	/* Scan needed if more than the rescan delay time has elapsed */
	if (time_mtime_ms() - wifi->scan_time_ms >= WIFI_RESCAN_DELAY_MS) {
		return true;
	}
	/* Otherwise, no need to scan */
	return false;
}

static int wifi_leave_idle_state(void)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->state != WS_IDLE) {
		log_debug("not in idle state");
		return -1;
	}
	wifi_timer_clear(&idle_timer);
	wifi->state = WS_SELECT;
	return 0;
}

/*
 * Clear connection state and return to SELECT.
 */
static int wifi_disconnect(struct wifi_state *wifi)
{
	if (wifi->state < WS_JOIN || wifi->state > WS_UP) {
		return -1;
	}
	if (wifi->state > WS_JOIN && wifi->state <= WS_UP &&
	    wifi->curr_profile) {
		log_debug("disconnecting from: %s",
		    wifi_ssid_to_str(&wifi->curr_profile->ssid));
	}
	if (!use_net_events) {
		wifi_net_update(false);	/* Reset the network interface state */
	}
	wifi_timer_clear(&join_timer);
	wifi->curr_profile = NULL;
	wifi->curr_hist = NULL;
	if (wifi_platform_associating()) {
		/* Cleanup pending association attempt */
		wifi_platform_associate_cancel();
	} else {
		wifi_platform_leave_network();
	}
	wifi_interface_notify_info_updated();	/* Clear SSID */
	wifi->state = WS_SELECT;
	return 0;
}


/*
 * Record a failed attempt to join a network.  Affects
 * the current profile error count and the current history entry.
 */
static void wifi_join_err(enum wifi_error err)
{
	struct wifi_state *wifi = &wifi_state;

	ASSERT(err != WIFI_ERR_NONE);
	if (wifi->state < WS_JOIN ||
	    wifi->state > WS_WAIT_CLIENT) {
		log_err("not joining network");
		return;
	}
	log_debug("%s", wifi_errors[err]);

	if (wifi->curr_profile) {
		++wifi->curr_profile->join_errs;
	}
	if (wifi->curr_hist) {
		wifi_hist_update(err, NULL, NULL);
	}
	/* Disconnect cleans up and returns to SELECT state */
	wifi_disconnect(wifi);
}

/*
 * Called when a critical Wi-Fi hardware or driver error occurs.
 * Resets state machine and attempts to reconnect.
 */
static void wifi_module_err(const char *msg)
{
	struct wifi_state *wifi = &wifi_state;

	log_err("error: %s", msg);
	if (wifi->curr_hist) {
		wifi_hist_update(WIFI_ERR_MEM, NULL, NULL);
	}
	wifi->state = WS_ERR;
	wifi_step();
}

/*
 * Called when the Wi-Fi network has been associated and the state machine
 * needs to wait for an IP address.
 */
static int wifi_wait_dhcp(struct wifi_state *wifi)
{
	if (wifi->state < WS_SELECT || wifi->state > WS_UP) {
		log_err("not in expected state");
		return -1;
	}
	if (use_net_events) {
		wifi->state = WS_DHCP;
		wifi_timer_set(&join_timer, WIFI_DHCP_TIMEOUT_MS);
		log_debug("set DHCP timer for %u ms",
		    WIFI_DHCP_TIMEOUT_MS);
	} else {
		wifi->state = WS_WAIT_CLIENT;
		wifi_timer_set(&join_timer,
		    WIFI_DHCP_TIMEOUT_MS + WIFI_CLOUD_TIMEOUT_MS);
		log_debug("set cloud timer for %u ms",
		    WIFI_DHCP_TIMEOUT_MS + WIFI_CLOUD_TIMEOUT_MS);
	}
	return 0;
}

/*
 * Called when an IP address has been acquired and the state machine needs
 * to wait for the cloud to be up.
 */
static int wifi_wait_client(struct wifi_state *wifi)
{
	if (wifi->state != WS_DHCP) {
		log_err("not in expected state");
		return -1;
	}
	wifi->state = WS_WAIT_CLIENT;
	wifi_timer_set(&join_timer, WIFI_CLOUD_TIMEOUT_MS);
	log_debug("set cloud timer for %u ms", WIFI_CLOUD_TIMEOUT_MS);
	return 0;
}

/*
 * Select next candidate for joining.
 * Choose the AP with the strongest signal first.
 */
static struct wifi_profile *wifi_select(struct wifi_state *wifi)
{
	struct wifi_profile *prof;
	struct wifi_profile *best = NULL;

	if (wifi->pref_profile) {
		prof = wifi->pref_profile;
		if (prof->enable && prof->join_errs < WIFI_PREF_TRY_LIMIT) {
			return prof;
		}
		wifi->pref_profile = NULL;
	}
	for (prof = wifi->profile; prof < &wifi->profile[WIFI_PROF_CT];
	    prof++) {
		if (!prof->enable) {
			continue;
		}
		if (prof->join_errs >= WIFI_JOIN_TRY_LIMIT) {
			continue;
		}
		if (!prof->scan) {
			continue;
		}
		if (!best || prof->scan->signal > best->scan->signal) {
			best = prof;
		}
	}
	return best;
}

/*
 * Association complete callback
 */
static void wifi_association_complete(enum wifi_platform_result result)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->state != WS_JOIN) {
		log_warn("not in join state");
		return;
	}
	switch (result) {
	case PLATFORM_FAILURE:
		log_warn("association ended with error");
		wifi_join_err(WIFI_ERR_WRONG_KEY);
		break;
	case PLATFORM_CANCELED:
		log_info("association canceled");
		/* Disconnect cleans up and returns to SELECT state */
		wifi_disconnect(wifi);
		break;
	case PLATFORM_SUCCESS:
		if (wifi->prof_save_mode == WC_SAVE_ON_CONNECT) {
			wifi_prof_commit_unsaved(wifi);
		}
		log_info("association complete");
		wifi_wait_dhcp(wifi);
		wifi_interface_notify_info_updated();	/* Set new SSID */
	}
	wifi_step();
}

/*
 * Find the strongest network in the scan results that has a profile
 * and try to join it.
 * If no valid network is found, start AP mode.
 */
static int wifi_select_network(struct wifi_state *wifi)
{
	struct wifi_profile *prof;

	if (wifi->state != WS_SELECT) {
		log_err("not in select state");
		return -1;
	}
	wifi->curr_profile = NULL;
	prof = wifi_select(wifi);
	if (!prof) {
		log_info("no valid network profile");
		/* Reset all join error counters when going into AP mode */
		for (prof = wifi->profile; prof < &wifi->profile[WIFI_PROF_CT];
		    prof++) {
			prof->join_errs = 0;
		}
		/* AP mode is enabled in IDLE state */
		wifi->state = WS_IDLE;
		return 0;
	}

	log_info("selected SSID: %s  signal: %d  join errors: %hhu",
	    wifi_ssid_to_str(&prof->ssid),
	    prof->scan ? prof->scan->signal : WIFI_SIGNAL_MIN, prof->join_errs);

	/* Network selection requires station mode */
	if (!wifi->simultaneous_ap_sta && wifi_platform_ap_enabled()) {
		wifi_ap_mode_stop();
	}
	/*
	 * Attempt to join a network.
	 * Update state before calling wifi_platform_associate() in case the
	 * platform implementation invokes the complete callback immediately.
	 */
	wifi->curr_profile = prof;
	wifi_hist_new(&prof->ssid, prof->scan ? &prof->scan->bssid : NULL,
	    WIFI_ERR_IN_PROGRESS, prof->join_errs >= WIFI_PREF_TRY_LIMIT - 1);
	wifi->state = WS_JOIN;
	wifi_timer_set(&join_timer, WIFI_JOIN_TIMEOUT_MS);
	log_debug("set join timer for %u ms", WIFI_JOIN_TIMEOUT_MS);
	if (wifi_platform_associate(prof, wifi_association_complete) < 0) {
		wifi_association_complete(PLATFORM_FAILURE);
		wifi_module_err("wifi_platform_associate");
		return -1;
	}
	return 0;
}

/*
 * Handle WPS success from the SELECT state.
 */
static int wifi_wps_success(void)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->state != WS_SELECT) {
		log_err("not in select state");
		return -1;
	}
	if (wifi->wps_state != WPS_SUCCESS) {
		log_err("WPS not complete");
		return -1;
	}
	wifi->curr_profile = NULL;
	/* XXX get info on connected AP and populate a new history entry */
	wifi->wps_state = WPS_IDLE;
	/* WPS complete, so wait for IP address */
	wifi_wait_dhcp(wifi);
	return 0;
}

/*
 * The join process did not complete within the allowed time window.
 * Handle the error and attempt to join again.
 */
static void wifi_join_timeout(struct timer *timer)
{
	struct wifi_state *wifi = &wifi_state;

	switch (wifi->state) {
	case WS_JOIN:
		log_warn("Wi-Fi join timed out");
		wifi_join_err(WIFI_ERR_TIME);
		break;
	case WS_DHCP:
		log_warn("DHCP client timed out");
		wifi_join_err(WIFI_ERR_NO_IP);
		break;
	case WS_WAIT_CLIENT:
		log_warn("timed out waiting for cloud up");
		wifi_join_err(WIFI_ERR_CLIENT_TIME);
		break;
	default:
		log_debug("unexpected timeout");
		return;
	}
	wifi_step();
}

/*
 * Periodically switch to SELECT state if idle for too long and
 * no stations are connected to our AP.  Step will restart the idle
 * timer.
 */
static void wifi_idle_timeout(struct timer *timer)
{
	log_debug("leaving idle state");
	if (wifi_leave_idle_state() < 0) {
		log_debug("unexpected timeout");
		return;
	}
	wifi_step();
}

/*
 * Delayed AP mode exit
 */
static void wifi_ap_stop_timeout(struct timer *timer)
{
	if (!wifi_platform_ap_enabled()) {
		return;
	}
	/* Reset timeout if a station is still connected to our AP */
	if (wifi_platform_ap_stations_connected() > 0) {
		wifi_timer_set(&ap_stop_timer, WIFI_AP_STOP_DELAY_MS);
		return;
	}
	wifi_ap_mode_stop();
	wifi_step();
}

/*
 * Reset after error
 */
static void wifi_error_timeout(struct timer *timer)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->state != WS_ERR) {
		return;
	}
	wifi_shutdown();
}

/*
 * Initialize the Wi-Fi manager and hardware interface
 */
void wifi_init(void)
{
	struct wifi_state *wifi = &wifi_state;

	wifi_timer_init(&step_timer, wifi_step_handler);
	wifi_timer_init(&join_timer, wifi_join_timeout);
	wifi_timer_init(&idle_timer, wifi_idle_timeout);
	wifi_timer_init(&ap_stop_timer, wifi_ap_stop_timeout);
	wifi_timer_init(&error_timer, wifi_error_timeout);

	wifi_platform_init(wifi);
}

/*
 * Exit handler.  Performs any necessary system cleanup.
 */
void wifi_exit(void)
{
	log_info("terminating Wi-Fi");
	wifi_platform_exit();
}

int wifi_start(void)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->ap_window_at_startup) {
		wifi_ap_window_start();
	}
	return wifi_platform_start();
}

/*
 * Perform any actions required as a result of a state change
 */
void wifi_poll(void)
{
	wifi_timer_clear(&step_timer);
	wifi_step_handler(&step_timer);
}

/*
 * Handle event indicating IP connectivity changed
 */
int wifi_net_update(bool up)
{
	struct wifi_state *wifi = &wifi_state;

	/* Update network interface info any time there is an net up event */
	if (up) {
		wifi_if_info_update(wifi);
	}
	if (up == wifi->network_up) {
		return 0;
	}
	wifi->network_up = up;
	if (!up) {
		/* Clear network interface state on a net down event */
		memset(&wifi->if_info.addr, 0, sizeof(wifi->if_info.addr));
		memset(&wifi->if_info.netmask, 0,
		    sizeof(wifi->if_info.netmask));
		wifi->if_info.flags = 0;
		memset(&wifi->def_route, 0, sizeof(wifi->def_route));

		if (wifi->state == WS_UP) {
			/*
			 * If cloud is already up and there are other saved
			 * network profiles, restart join process.
			 */
			if (wifi_prof_count(wifi) > 1) {
				wifi_wait_dhcp(wifi);
			}
		}
	}
	wifi_step();
	return 0;
}

/*
 * Handle event indicating cloud connectivity has been achieved
 */
int wifi_cloud_update(bool up)
{
	struct wifi_state *wifi = &wifi_state;

	if (up == wifi->cloud_up) {
		return 0;
	}
	wifi->cloud_up = up;
	if (up && !use_net_events) {
		/* Get net info here if net events are not being used */
		wifi_net_update(true);
	}
	wifi_step();
	return 0;
}

/*
 * Disconnect and shutdown Wi-Fi
 */
int wifi_shutdown(void)
{
	struct wifi_state *wifi = &wifi_state;

	wifi_disconnect(wifi);
	wifi_timer_clear(&idle_timer);
	wifi->state = WS_DISABLED;
	wifi_step();
	return 0;
}

/*
 * Initiate a factory rest.  This clears the startup config and disconnects
 * from the current station.
 */
int wifi_factory_reset(void)
{
	struct wifi_state *wifi = &wifi_state;

	/* Force disconnect, since all profiles will be deleted */
	if (wifi->state >= WS_JOIN) {
		wifi_disconnect(wifi);
	}
	if (conf_factory_reset() < 0) {
		return -1;
	}
	return 0;
}

/*
 * Scan complete callback
 */
static void wifi_scan_complete(enum wifi_platform_result result)
{
	struct wifi_state *wifi = &wifi_state;

	switch (result) {
	case PLATFORM_FAILURE:
		wifi_module_err("wifi_platform_scan");
		break;
	case PLATFORM_CANCELED:
		log_info("scan canceled");
		break;
	case PLATFORM_SUCCESS:
		/* Update scan result timestamp */
		wifi->scan_time_ms = time_mtime_ms();
		/* Update profiles' scan associations */
		wifi_scan_prof_update(wifi);
		log_info("scan complete");

		/* Added for BLE WiFi setup */
		if (wifi_scan_complete_handler) {
			wifi_scan_complete_handler();
		}
	}
	/* Server may need to know when scan is done */
	wifi_interface_notify_scan_complete();

	wifi_step();
}

/*
 * Begin a scan.
 */
int wifi_scan(void)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->state != WS_SELECT &&
	    wifi->state != WS_IDLE &&
	    wifi->state != WS_UP) {
		log_warn("busy: scan not allowed");
		return -1;
	}
	if (wifi_platform_scanning()) {
		log_debug("scan already requested");
		return 0;
	}
	log_info("starting scan");
	if (wifi_platform_scan(wifi_scan_complete) < 0) {
		wifi_scan_complete(PLATFORM_FAILURE);
		return -1;
	}
	return 0;
}

/*
 * Handle a request to immediately attempt to associate with a
 * preferred network.
 *
 */
int wifi_connect(struct wifi_profile *prof)
{
	struct wifi_state *wifi = &wifi_state;

	wifi->pref_profile = prof;
	/*
	 * If already in the SELECT state, the preferred profile will
	 * be taken into account when selection occurs.
	 */
	if (wifi->state == WS_SELECT) {
		goto step;
	}
	/* If idle, leave IDLE state and transition to SELECT */
	if (wifi->state == WS_IDLE) {
		wifi_leave_idle_state();
		goto step;
	}
	/* If currently connecting or connected, disconnect and select */
	if (!wifi_disconnect(wifi)) {
		goto step;
	}
	/* Otherwise, we must be disabled or in an error state */
	log_warn("unable to connect from current state");
	return -1;
step:
	wifi_step();
	return 0;
}

/*
 * Enable Access Point mode and allow stations to connect.
 */
static int wifi_ap_mode_start(void)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_profile *prof = &wifi->ap_profile;

	if (wifi->state != WS_IDLE) {
		log_err("can only start AP mode from IDLE state");
		return -1;
	}
	if (!prof->enable) {
		log_warn("AP profile disabled");
		return -1;
	}
	if (!wifi_ap_allow(wifi)) {
		log_warn("AP mode not allowed");
		return -1;
	}
	if (wifi_platform_ap_enabled()) {
		log_debug("AP mode already started");
		return 0;
	}
	if (!(prof->sec & WSEC_VALID)) {
		log_debug("defaulting to unsecured AP");
		prof->sec = WSEC_VALID | WSEC_NONE;
	}
	if (wifi_platform_station_enabled() && !wifi->simultaneous_ap_sta) {
		wifi_platform_station_stop();
	}

	/* Notify AP mode state change */
	if (wifi_ap_mode_change) {
		log_info("Notify AP mode state change: true");
		wifi_ap_mode_change(true);
	}

	log_info("starting AP mode: SSID %s", wifi_ssid_to_str(&prof->ssid));
	if (wifi_platform_ap_start(prof, wifi->ap_channel,
	    &wifi->ap_ip_addr) < 0) {
		wifi_module_err("wifi_platform_ap_start");
		return -1;
	}
	wifi_interface_notify_info_updated();
	return 0;
}

/*
 * Disable AP mode.
 */
int wifi_ap_mode_stop(void)
{
	struct wifi_state *wifi = &wifi_state;
	int rc;

	if (!wifi_platform_ap_enabled()) {
		log_warn("AP mode not enabled");
		return 0;
	}

	/* Notify AP mode state change */
	if (wifi_ap_mode_change) {
		log_info("Notify AP mode state change: false");
		wifi_ap_mode_change(false);
	}

	log_info("stopping AP mode");
	wifi_timer_clear(&ap_stop_timer);
	rc = wifi_platform_ap_stop();
	if (!wifi_platform_station_enabled() && !wifi->simultaneous_ap_sta) {
		if (wifi_platform_station_start() < 0) {
			rc = -1;
		}
	}
	wifi_interface_notify_info_updated();
	return rc;
}

/*
 * Callback indicating WPS has ended.
 */
static void wifi_wps_complete(enum wifi_platform_result result)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->wps_state != WPS_SCAN) {
		log_warn("not in WPS scan state");
		return;
	}
	switch (result) {
	case PLATFORM_FAILURE:
		log_warn("WPS configuration timed out");
		wifi->wps_state = WPS_IDLE;
		break;
	case PLATFORM_CANCELED:
		log_info("WPS canceled");
		wifi->wps_state = WPS_IDLE;
		break;
	case PLATFORM_SUCCESS:
		log_info("WPS configuration successful");
		wifi->wps_state = WPS_SUCCESS;
	}
	wifi_step();
}

/*
 * Initiate WPS virtual button press.
 */
int wifi_wps_pbc(void)
{
	struct wifi_state *wifi = &wifi_state;

	/* WPS performs an AP scan and attempts to join an available network */
	if (wifi->wps_state == WPS_SCAN) {
		log_debug("WPS already requested");
		return 0;
	}
	/* If already in the SELECT state, start WPS scan immediately */
	if (wifi->state == WS_SELECT) {
		goto scan;
	}
	/* If idle, leave IDLE state and transition to SELECT */
	if (wifi->state == WS_IDLE) {
		wifi_leave_idle_state();
		goto scan;
	}
	/* If currently connecting, disconnect and start WPS */
	if (wifi->state != WS_UP && !wifi_disconnect(wifi)) {
		goto scan;
	}
	/* Otherwise, we must be disabled or in an error state */
	log_warn("unable to initiate WPS from current state");
	return -1;
scan:
	/* WPS scan requires station mode */
	if (!wifi_platform_station_enabled() && !wifi->simultaneous_ap_sta) {
		wifi_ap_mode_stop();
	}
	wifi->wps_state = WPS_SCAN;
	if (wifi_platform_wps_start(wifi_wps_complete) < 0) {
		wifi->wps_state = WPS_ERR;
		wifi_module_err("wifi_platform_wps_start");
		return -1;
	}
	wifi_step();
	return 0;
}

/*
 * Add or update a Wi-Fi profile.
 */
struct wifi_profile *wifi_prof_add(const struct wifi_ssid *ssid,
    enum wifi_sec sec, const struct wifi_key *key, enum wifi_error *error)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_profile *prof;

	if ((!SEC_MATCH(sec, WSEC_NONE) && !key) ||
	    wifi_check_key(key, sec) < 0) {
		log_warn("profile add failed: invalid PSK");
		if (error) {
			*error = WIFI_ERR_INV_KEY;
		}
		return NULL;
	}
	prof = wifi_prof_lookup(wifi, ssid);
	if (!prof) {
		/* If not found, create new profile */
		if (wifi->prof_save_mode == WC_SAVE_ON_ADD) {
			prof = wifi_prof_new(wifi);
		} else {
			prof = &wifi->unsaved_prof;
			memset(prof, 0, sizeof(*prof));
		}
		memcpy(&prof->ssid, ssid, sizeof(prof->ssid));
	} else if (wifi->prof_save_mode != WC_SAVE_ON_ADD) {
		/* Copy existing profile info unsaved profile slot */
		memcpy(&wifi->unsaved_prof, prof, sizeof(wifi->unsaved_prof));
		prof = &wifi->unsaved_prof;
	}
	prof->enable = true;
	prof->sec = sec;
	if (key) {
		memcpy(&prof->key, key, sizeof(prof->key));
	} else {
		memset(&prof->key, 0, sizeof(prof->key));
	}
	prof->join_errs = 0;

	/* Other profile save policies will save later */
	if (wifi->prof_save_mode == WC_SAVE_ON_ADD) {
		conf_save();
	}
	if (error) {
		*error = WIFI_ERR_NONE;
	}
	return prof;
}

/*
 * Remove a Wi-Fi profile.  Disconnect from current network if
 * the the current profile was deleted.
 */
int wifi_prof_delete(const struct wifi_ssid *ssid, enum wifi_error *error)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_profile *prof;

	prof = wifi_prof_lookup(wifi, ssid);
	if (!prof) {
		log_warn("profile with ssid \"%s\" does not exist",
		    wifi_ssid_to_str(ssid));
		if (error) {
			*error = WIFI_ERR_NOT_FOUND;
		}
		return -1;
	}

	memset(prof, 0, sizeof(*prof));
	if (prof == wifi->pref_profile) {
		log_debug("clearing preferred profile");
		wifi->pref_profile = NULL;
	}
	/* Force reassociation if current profile deleted */
	if (wifi->state >= WS_JOIN && prof == wifi->curr_profile) {
		log_debug("current profile deleted");
		wifi_disconnect(wifi);
	}
	conf_save();
	wifi_step();
	if (error) {
		*error = WIFI_ERR_NONE;
	}
	return WIFI_ERR_NONE;
}

/*
 * Erase all cached scan results
 */
int wifi_scan_clear(void)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_profile *prof;

	/* Clear scan result associations for all profiles */
	for (prof = wifi->profile; prof < &wifi->profile[WIFI_PROF_CT];
	    ++prof) {
		prof->scan = NULL;
	}
	wifi->unsaved_prof.scan = NULL;
	memset(&wifi->scan, 0, sizeof(wifi->scan));
	return 0;
}

/*
 * Cache a new scan entry
 */
int wifi_scan_add(struct wifi_scan_result *rp)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_scan_result *scan;
	struct wifi_scan_result *best = NULL;
	u8 i;

	/* Ignore hidden and empty SSIDs */
	for (i = 0; i < rp->ssid.len; ++i) {
		if (rp->ssid.val[i]) {
			break;
		}
	}
	if (i == rp->ssid.len) {
		return 0;
	}
	/* Ignore our own AP's SSID */
	if (wifi->ap_profile.enable &&
	    wifi_ssid_match(&rp->ssid, &wifi->ap_profile.ssid)) {
		return 0;
	}
	/* If doing scan for specific target, make sure there's spot for it. */
	if (wifi_ssid_match(&rp->ssid, &wifi->scan4)) {
		best = &wifi->scan[0];
	}
	/*
	 * Replace an entry in the scan list that is empty,
	 * has the same SSID but a weaker signal,
	 * or has a weakest signal that's also weaker than the new result.
	 * But drop the scan result if it is a known SSID but not stronger.
	 * If hearing from multiple base stations with different security or
	 * band, keep both entries.
	 */
	for (scan = wifi->scan; scan < &wifi->scan[WIFI_SCAN_CT]; scan++) {
		if (!scan->time_ms) {
			if (!best) {
				best = scan;
			}
			break;
		}
		if (!best && rp->signal > scan->signal) {
			best = scan;
		}
		if (wifi_ssid_match(&scan->ssid, &rp->ssid) &&
		    scan->sec == rp->sec &&
		    rp->band == scan->band) {
			break;
		}
	}
	if (best) {
		/*
		 * Move from scan to best.
		 */
		if (best != scan) {
			if (scan == &wifi->scan[WIFI_SCAN_CT]) {
				/*
				 * Last item will fall off.
				 */
				scan--;
			}
			memmove(best + 1, best, (scan - best) * sizeof(*scan));
		}
		*best = *rp;
		best->time_ms = time_mtime_ms();
	}
	return 0;
}

/*
 * Open the AP mode window for the configured duration.
 */
void wifi_ap_window_start(void)
{
	struct wifi_state *wifi = &wifi_state;

	if (wifi->ap_window_mins) {
		log_debug("AP window opened for %u minutes",
		    wifi->ap_window_mins);
		wifi->ap_window_close_ms = time_mtime_ms() +
		    wifi->ap_window_mins * 60000;
	} else {
		if (!wifi->ap_window_close_ms) {
			if (wifi->ap_window_secure) {
				log_debug("AP window opened until valid "
				    "profile is saved");
			} else {
				log_debug("AP window opened indefinitely");
			}
		}
		wifi->ap_window_close_ms = MAX_U64;
	}
	/* Immediately start AP mode if in correct state */
	if (wifi->state == WS_IDLE && !wifi_platform_ap_enabled()) {
		wifi_ap_mode_start();
	}
}

/*
 * Get new connection history entry.
 * If scan result is non-null, use its SSID and BSSID.
 * Otherwise, history SSID can be provided in ssid argument
 */
struct wifi_history *wifi_hist_new(const struct wifi_ssid *ssid,
    const struct ether_addr *bssid,
    enum wifi_error err, bool last_attempt)
{
	struct wifi_state *wifi = &wifi_state;

#if WIFI_HIST_CT < 1
	return NULL;
#endif
	/* Shift history entries down, and insert on top */
	memmove(wifi->hist + 1, wifi->hist,
	    (WIFI_HIST_CT - 1) * sizeof(*wifi->hist));
	memset(wifi->hist, 0, sizeof(wifi->hist[0]));
	wifi->curr_hist = wifi->hist;
	wifi_hist_update(err, ssid, bssid);
	wifi->curr_hist->time_ms = time_mtime_ms();
	wifi->curr_hist->last = last_attempt;
	return wifi->curr_hist;
}

/*
 * Update the network interface and DNS server info in the specified
 * history entry.  SSID and BSSID are optional.  Set to NULL to retain
 * the existing values.
 */
void wifi_hist_update(enum wifi_error err, const struct wifi_ssid *ssid,
    const struct ether_addr *bssid)
{
	struct wifi_state *wifi = &wifi_state;
	struct wifi_history *hist;
	bool updated = false;

	if (!wifi->curr_hist) {
		log_err("no current history entry");
		return;
	}
	hist = wifi->curr_hist;
	if (ssid) {
		memcpy(&hist->ssid, ssid, sizeof(hist->ssid));
	}
	if (bssid) {
		memcpy(&hist->bssid, bssid, sizeof(hist->bssid));
	}

	log_debug("curr hist->error %d, input err %d", hist->error, err);
	if (hist->error != err) {
		updated = true;
	}

	hist->error = err;
	hist->ip_addr = wifi->if_info.addr.sin_addr;
	hist->netmask = wifi->if_info.netmask.sin_addr;
	hist->def_route = wifi->def_route;
	net_get_dnsservers(&hist->dns_servers);
	/* No more connection attempts needed if there were no errors */
	if (err == WIFI_ERR_NONE) {
		hist->last = true;
	}

	/* Added for BLE WiFi setup */
	if (updated && wifi_connect_state_change_handler) {
		wifi_connect_state_change_handler();
	}
}

/*
 * Handle a Wi-Fi state change
 */
static void wifi_step_handler(struct timer *timer)
{
	static enum wifi_conn_state prev_state = WS_DISABLED - 1;
	struct wifi_state *wifi = &wifi_state;
	enum wifi_conn_state state;

	if (wifi->state != prev_state) {
		log_debug("prev_state=%d, state=%d, state str=%s",
		    prev_state, wifi->state, wifi_states[wifi->state]);
		if (wifi_connect_state_change_handler) {
			wifi_connect_state_change_handler();
		}
	}
	prev_state = wifi->state;

	do {
#ifdef STATE_DEBUG
		log_debug("state=%s scan=%s WPS=%s station=%s AP=%s",
		    wifi_states[wifi->state],
		    wifi_platform_scanning() ? "ACTIVE" : "IDLE",
		    wifi_wps_states[wifi->wps_state],
		    wifi_platform_station_enabled() ? "ENABLED" : "DISABLED",
		    wifi_platform_ap_enabled() ? "ENABLED" : "DISABLED");
#endif
		state = wifi->state;
		switch (state) {
		case WS_DISABLED:
			if (wifi_platform_scanning()) {
				wifi_platform_scan_cancel();
			}
			if (wifi_platform_wps_started()) {
				wifi_platform_wps_cancel();
			}
			wifi->wps_state = WPS_IDLE;
			if (wifi_platform_ap_enabled()) {
				wifi_ap_mode_stop();
			}
			if (wifi_platform_station_enabled()) {
				wifi_platform_station_stop();
			}
			if (wifi->enable) {
				log_info("Wi-Fi starting");
				if (!wifi_platform_station_enabled() &&
				    wifi_platform_station_start() < 0) {
					wifi_module_err("wifi_platform_"
					    "station_start");
					break;
				}
				/* Always go to SELECT state on start */
				wifi->state = WS_SELECT;
			}
			break;
		case WS_SELECT:
			/* Wait for scans to complete before selection */
			if (wifi_platform_scanning() ||
			    wifi_platform_wps_started()) {
				break;
			}
			/* WPS completed successfully, so advance state */
			if (wifi->wps_state == WPS_SUCCESS) {
				wifi_wps_success();
				break;
			}
			/* Request a scan if results not current */
			if (wifi_scan_needed()) {
				log_debug("scan results out of date");
				wifi_scan();
				break;
			}
			/* Attempt to associate with a network */
			wifi_select_network(wifi);
			break;
		case WS_IDLE:
			/* Do something if idle for too long */
			if (!timer_active(&idle_timer)) {
				wifi_timer_set(&idle_timer,
				    WIFI_IDLE_TIMEOUT_MS);
				log_debug("set idle timer for %u ms",
				    WIFI_IDLE_TIMEOUT_MS);
			}
			/* If AP window open, enable AP mode while idle */
			if (wifi_ap_allow(wifi)) {
				if (!wifi_platform_ap_enabled()) {
					wifi_ap_mode_start();
				}
			} else if (wifi_platform_ap_enabled()) {
				/* AP window closed, so stop AP mode ASAP */
				wifi_timer_set(&ap_stop_timer, 0);
			}
			break;
		case WS_JOIN:
			/* Nothing to do here. Waiting for join result. */
			break;
		case WS_DHCP:
			/* Wait for DHCP event */
			if (!wifi->network_up) {
				break;
			}
			log_info("DHCP client acquired address");
			wifi_wait_client(wifi);
			break;
		case WS_WAIT_CLIENT:
			/* Wait for client devd connected */
			if (!wifi->cloud_up) {
				break;
			}
			log_info("cloud connection is up");
			wifi->state = WS_UP;
			wifi_timer_clear(&join_timer);
			if (wifi_platform_ap_enabled()) {
				wifi_timer_set(&ap_stop_timer,
				    WIFI_AP_STOP_DELAY_MS);
				log_debug("set AP stop timer for %u ms",
				    WIFI_AP_STOP_DELAY_MS);
			}
			if (wifi->curr_profile) {
				wifi->curr_profile->join_errs = 0;
			}
			if (wifi->curr_hist) {
				wifi_hist_update(WIFI_ERR_NONE, NULL, NULL);
				wifi->curr_hist = NULL;
			}
			if (wifi->prof_save_mode == WC_SAVE_ON_CLOUD_UP) {
				wifi_prof_commit_unsaved(wifi);
			}
			break;
		case WS_UP:
			/* Nothing to do here. Successfully connected. */
			break;
		case WS_ERR:
			if (!timer_active(&error_timer)) {
				wifi_timer_set(&error_timer,
				    WIFI_ERROR_DELAY_MS);
				log_debug("error state: stopping for %u ms",
				    WIFI_ERROR_DELAY_MS);
			}
			break;
		}
	} while (state != wifi->state);
}

/*
 * Register scan complete callback handler
 */
void wifi_reg_scan_complete_cb(void (*handler)(void))
{
	wifi_scan_complete_handler = handler;
}

/*
 * Register connect state change callback handler
 */
void wifi_reg_connect_state_change_cb(void (*handler)(void))
{
	wifi_connect_state_change_handler = handler;
}

/*
 * Register ap mode state change callback handler
 */
void wifi_reg_ap_mode_change_cb(void (*handler)(bool))
{
	wifi_ap_mode_change = handler;
}

/*
 * Get scan results for BLE WiFi setup feature
 */
int wifi_get_scan_results(struct wifi_scan_result *scan, int size)
{
	struct wifi_state *wifi = &wifi_state;
	int i = 0, count = 0;

	log_debug("scan results size %d", size);

	if (wifi_platform_scanning()) {
		log_debug("scan in progress");
		return 0;
	}

	for (i = 0; (i < size) && (i < WIFI_SCAN_CT); i++) {
		if (!wifi->scan[i].time_ms) {
			continue;
		}
		scan[count++] = wifi->scan[i];
	}

	return count;
}
