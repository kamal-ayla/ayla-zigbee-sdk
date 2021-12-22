/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/poll.h>
#include <wait.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/hex.h>

#include "cond.h"
#include "wifi.h"


void wifi_timer_init(struct timer *timer, void (*handler)(struct timer *))
{
	timer_init(timer, handler);
}

void wifi_timer_set(struct timer *timer, unsigned long ms)
{
	struct cond_state *cond = &cond_state;

	timer_set(&cond->timers, timer, ms);
}

void wifi_timer_clear(struct timer *timer)
{
	struct cond_state *cond = &cond_state;

	timer_cancel(&cond->timers, timer);
}

/*
 * Convert frequency in MHz to channel number.
 * This function should cover channels commonly used by IEEE 802.11 a/b/g/n
 * Wi-Fi radios.
 * Return -1 if frequency is unknown.
 */
int wifi_freq_chan(u32 freq)
{
	/* 2.4 GHz channels 1-13 have a 5 MHz spacing starting at 2412 MHz */
	if (freq >= 2412 && freq <= 2472) {
		freq -= 2412;
		if (freq % 5) {
			return -1;
		}
		return 1 + freq / 5;
	}
	/* 2.4 GHz channel 14 is 2484 MHz */
	if (freq == 2484) {
		return 14;
	}
	/* 5 GHz channels 36-165 have a 5 MHz spacing starting at 5180 MHz */
	if (freq >= 5180 && freq <= 5825) {
		freq -= 5180;
		if (freq % 5) {
			return -1;
		}
		return 36 + freq / 5;
	}
	return -1;
}

/*
 * Convert received signal strength to bar-graph intensity.
 * Any signal should give at least one bar.  Max signal is 5 bars.
 * Lean towards giving 5 bars for a wide range of usable signals.
 */
u8 wifi_bars(int signal)
{
	if (signal <= WIFI_SIGNAL_MIN) {
		return WIFI_BARS_MIN;
	}
	if (signal < -70) {
		return 1;
	}
	if (signal < -60) {
		return 2;
	}
	if (signal < -50) {
		return 3;
	}
	if (signal < -40) {
		return 4;
	}
	return 5;
}

/*
 * Return non-zero if SSIDs a and b match.
 */
bool wifi_ssid_match(const struct wifi_ssid *a,
    const struct wifi_ssid *b)
{
	return a->len == b->len && !memcmp(a->val, b->val, a->len);
}

/*
 * Populate an SSID struct from an ASCII string with "\x"
 * escaped hex bytes for unprintable bytes
 */
int wifi_parse_ssid(const char *input, struct wifi_ssid *ssid)
{
	const char *cp;

	memset(ssid, 0, sizeof(*ssid));
	cp = input;
	while (*cp != '\0') {
		if (ssid->len >= sizeof(ssid->val)) {
			log_err("data too long");
			return -1;
		}
		if (*cp == '\\') {
			++cp;
			if (*cp != 'x') {
				log_err("invalid escape sequence: \\%c", *cp);
				return -1;
			}
			++cp;
			cp = hex_parse_byte(cp, ssid->val + ssid->len);
			if (!cp) {
				log_err("invalid hex byte: %c%c", cp[0], cp[1]);
				return -1;
			}
		} else {
			ssid->val[ssid->len] = *cp++;
		}
		++ssid->len;
	}
	return 0;
}

/*
 * Returns a pointer to a static buffer with SSID representation.
 * Unprintable bytes are displayed in hex with a "\x" prefix.
 */
const char *wifi_ssid_to_str(const struct wifi_ssid *ssid)
{
	static char buf[WIFI_SSID_LEN * 4 + 1]; /* reserve room for "\x" */
	int i;
	char *cp;

	for (i = 0, cp = buf; i < ssid->len; ++i) {
		if (isprint(ssid->val[i])) {
			*cp++ = ssid->val[i];
		} else {
			*cp++ = '\\';
			*cp++ = 'x';
			hex_string(cp, 3, ssid->val + i, 1, true, 0);
			cp += 2;
		}
	}
	*cp = '\0';
	return buf;
}

/*
 * Lookup scan result by SSID.
 * Return NULL if not found.
 */
struct wifi_scan_result *wifi_scan_lookup_ssid(struct wifi_state *wifi,
    const struct wifi_ssid *ssid)
{
	struct wifi_scan_result *scan;

	for (scan = wifi->scan; scan < &wifi->scan[WIFI_SCAN_CT]; ++scan) {
		if (wifi_ssid_match(&scan->ssid, ssid)) {
			return scan;
		}
	}
	return NULL;
}

/*
 * Lookup scan result by SSID.
 * Return NULL if not found.
 */
struct wifi_scan_result *wifi_scan_lookup_bssid(struct wifi_state *wifi,
    const struct ether_addr *bssid)
{
	struct wifi_scan_result *scan;

	for (scan = wifi->scan; scan < &wifi->scan[WIFI_SCAN_CT]; ++scan) {
		if (!memcmp(&scan->bssid, bssid, sizeof(scan->bssid))) {
			return scan;
		}
	}
	return NULL;
}

/*
 * Lookup profile by SSID.
 * Return NULL if not found.
 */
struct wifi_profile *wifi_prof_lookup(struct wifi_state *wifi,
    const struct wifi_ssid *ssid)
{
	struct wifi_profile *prof;

	for (prof = wifi->profile;
	    prof < &wifi->profile[WIFI_PROF_CT]; ++prof) {
		if (wifi_ssid_match(&prof->ssid, ssid)) {
			return prof;
		}
	}
	return NULL;
}

/*
 * Return non-zero if a new security setting is a downgrade.
 */
int wifi_sec_downgrade(enum wifi_sec new, enum wifi_sec old)
{
	ASSERT(WSEC_NONE < WSEC_WEP);
	ASSERT(WSEC_WEP < WSEC_WPA);
	ASSERT(WSEC_WPA < WSEC_WPA2);

	return (new & WSEC_SEC_MASK) < (old & WSEC_SEC_MASK);
}

/*
 * Validate Wi-Fi key length based on security type.
 */
int wifi_check_key(const struct wifi_key *key, enum wifi_sec sec)
{
	if (SEC_MATCH(sec, WSEC_NONE)) {
		if (key->len != 0) {
			return -1;
		}
	} else if (SEC_MATCH(sec, WSEC_WEP)) {
		/*
		 * WEP keys are composed of a pre-shared key + 24-bit IV.
		 * 64, 128, 152, and 256 bit WEP may be used.
		 */
		if (key->len != WIFI_KEY_LEN_WEP64 &&
		    key->len != WIFI_KEY_LEN_WEP128 &&
		    key->len != WIFI_KEY_LEN_WEP152 &&
		    key->len != WIFI_KEY_LEN_WEP256) {
			return -1;
		}
	} else {
		/* WPA/WPA2 */
		if (key->len < WIFI_MIN_KEY_LEN ||
		    key->len > WIFI_MAX_KEY_LEN) {
			return -1;
		}
	}
	return 0;
}

/*
 * Determine the most secure security mode from a station's scan entry.
 * If a key is available, its validity is used when
 * determining the security mode.  Otherwise, set key to NULL.
 * A valid security mask is returned on success.
 */
enum wifi_sec wifi_scan_get_best_security(const struct wifi_scan_result *scan,
    struct wifi_key *key)
{
	const enum wifi_sec *sec;
	enum wifi_sec best_sec;

	best_sec = WSEC_NONE | WSEC_VALID;
	for (sec = scan->sec; sec < scan->sec + WIFI_SCAN_SEC_CT; ++sec) {
		if (!(*sec & WSEC_VALID)) {
			continue;
		}
		if (key && wifi_check_key(key, *sec) < 0) {
			continue;
		}
		if (!wifi_sec_downgrade(*sec, best_sec)) {
			best_sec = *sec;
		}
	}
	return best_sec;
}

/*
 * Run a script in the system.  If directory is not specified, the shell
 * attempts to find the script in the PATH.
 * Returns -1 if system() failed, the script could not be executed, or
 * was terminated by a signal. Otherwise, the exit status of the script is
 * returned.
 */
int wifi_script_run(const char *directory, const char *cmd, ...)
{
	int rc;
	va_list args;
	char buf[PATH_MAX];
	size_t len;

	len = (directory && *directory) ?
	    snprintf(buf, sizeof(buf), "%s/", directory) : 0;
	va_start(args, cmd);
	len += vsnprintf(buf + len, sizeof(buf) - len, cmd, args);
	va_end(args);

	log_debug("%s", buf);
	rc = system(buf);
	if (!rc) {
		return 0;
	}
	/* exit status 127 is returned if the shell failed to exec the cmd */
	if (WIFEXITED(rc) && WEXITSTATUS(rc) != 127) {
		return WEXITSTATUS(rc);
	}
	return -1;
}

