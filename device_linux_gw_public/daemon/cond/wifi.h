/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __COND_WIFI_H__
#define __COND_WIFI_H__

#include <netinet/ether.h>
#include <arpa/inet.h>
#include <jansson.h>

#include <ayla/token_table.h>
#include <ayla/nameval.h>
#include <ayla/network_utils.h>
#include <ayla/wifi.h>


#define WIFI_PROF_CT		10	/* number of saved profiles */
#define WIFI_SCAN_CT		30	/* number of scan results kept */
#define WIFI_SCAN_SEC_CT	3	/* security types per scan result */
#define WIFI_HIST_CT		5	/* connection histories to keep */

#define WIFI_PREF_TRY_LIMIT	3	/* pref profile connection attempts */
#define WIFI_JOIN_TRY_LIMIT	3	/* profile connection attempts */
#define WIFI_RESCAN_DELAY_MS	60000	/* ms, scan result expiration age */
#define WIFI_JOIN_TIMEOUT_MS	10000	/* ms, time before join failure */
#define WIFI_DHCP_TIMEOUT_MS	15000	/* ms, time to wait for DHCP lease */
#define WIFI_CLOUD_TIMEOUT_MS	15000	/* ms, time to wait for cloud up */
#define WIFI_IDLE_TIMEOUT_MS	WIFI_RESCAN_DELAY_MS /* time in idle state */
#define WIFI_AP_STOP_DELAY_MS	30000	/* ms, AP stop delay after connect */
#define WIFI_ERROR_DELAY_MS	1000	/* ms, time to hold in ERR state */

#define WIFI_AP_IP_ADDR_DEFAULT	"192.168.0.1"	/* Default IP address for AP */
#define WIFI_AP_SSID_DEFAULT	"Ayla-$MAC"	/* Default SSID for AP */
#define WIFI_PROF_SAVE_MODE_DEFAULT	WC_SAVE_ON_CLOUD_UP /* Save policy */

/* Check for unpopulated 6-byte MAC address */
#define EMPTY_HWADDR(addr) \
	(!((u8 *)&addr)[0] && !((u8 *)&addr)[1] && !((u8 *)&addr)[2] && \
	!((u8 *)&addr)[3] && !((u8 *)&addr)[4] && !((u8 *)&addr)[5])

/*
 * Wi-Fi security mode bits
 */
enum wifi_sec {
	WSEC_NONE = 0,
	WSEC_WEP = BIT(0),
	WSEC_WPA = BIT(1),
	WSEC_WPA2 = BIT(2),
	WSEC_SEC_MASK = WSEC_WEP | WSEC_WPA | WSEC_WPA2,
	WSEC_PSK = BIT(3),
	WSEC_AES = BIT(4),
	WSEC_TKIP = BIT(5),
	WSEC_CCMP = BIT(6),
	WSEC_WEP104 = BIT(7),
	WSEC_WEP40 = BIT(8),
	WSEC_VALID = BIT(15),
};

#define SEC_MATCH(s1, s2)	((s1 & WSEC_SEC_MASK) == (s2 & WSEC_SEC_MASK))

/*
 * Wi-Fi profile save modes
 */
#define WIFI_PROF_SAVE_MODES(def)			\
	def(save-never,		WC_SAVE_NEVER)		\
	def(save-on-add,	WC_SAVE_ON_ADD)		\
	def(save-on-connect,	WC_SAVE_ON_CONNECT)	\
	def(save-on-cloud-up,	WC_SAVE_ON_CLOUD_UP)

DEF_ENUM(wifi_prof_save_mode, WIFI_PROF_SAVE_MODES);

/*
 * Wi-Fi BSS types
 */
#define WIFI_BSS_TYPES(def)				\
	def(Unknown,		BT_UNKNOWN)		\
	def(AP,			BT_INFRASTRUCTURE)	\
	def(Ad hoc,		BT_AD_HOC)

DEF_ENUM(wifi_bss_type, WIFI_BSS_TYPES);

/*
 * Wi-Fi scan station info
 */
struct wifi_scan_result {
	u64 time_ms;	/* non-zero if valid. time when last received */
	struct ether_addr bssid;
	struct wifi_ssid ssid;
	u8 band;
	u8 chan;
	s8 signal;
	enum wifi_bss_type type;
	bool wps_supported;
	bool ess_supported;
	enum wifi_sec sec[WIFI_SCAN_SEC_CT];
};

/*
 * Wi-Fi connection profile
 */
struct wifi_profile {
	bool enable;		/* enabled in config or by successful join */
	struct wifi_ssid ssid;
	enum wifi_sec sec;
	struct wifi_key key;
	u8 join_errs;		/* count of join errors */
	struct wifi_scan_result *scan;	/* most recent scan result */
	bool hidden;
};

/*
 * History of a Wi-Fi connection attempt.
 */
struct wifi_history {
	u64 time_ms;		/* time since boot when connection was tried */
	struct wifi_ssid ssid;	/* SSID, if known */
	struct ether_addr bssid;/* BSSID, if known */
	enum wifi_error error;	/* error code */
	struct in_addr ip_addr;	/* IP address assigned by DHCP */
	struct in_addr netmask;	/* netmask assigned by DHCP */
	struct in_addr def_route; /* default route from DCHP */
	struct net_dnsservers dns_servers; /* DNS servers from DHCP */
	bool last;		/* true if this is the final attempt */
};

#define WIFI_CONN_STATES(def)			\
	def(DISABLED,	WS_DISABLED)		\
	def(SELECT,	WS_SELECT)		\
	def(IDLE,	WS_IDLE)		\
	def(JOIN,	WS_JOIN)		\
	def(DHCP,	WS_DHCP)		\
	def(WAIT_CLIENT, WS_WAIT_CLIENT)	\
	def(UP,		WS_UP)			\
	def(ERR,	WS_ERR)

DEF_ENUM(wifi_conn_state, WIFI_CONN_STATES);

#define WIFI_WPS_STATES(def)			\
	def(IDLE,	WPS_IDLE)		\
	def(SCAN,	WPS_SCAN)		\
	def(SUCCESS,	WPS_SUCCESS)		\
	def(ERR,	WPS_ERR)

DEF_ENUM(wps_state, WIFI_WPS_STATES);

struct wifi_state {
	bool enable;		/* enable Wi-Fi connection manager */
	bool ap_enable;		/* enable Wi-Fi AP mode */
	bool simultaneous_ap_sta; /* allow concurrent AP and station mode */
	char *ifname;		/* network interface name (malloc'd) */
	char *ap_ifname;	/* AP-specific ifname name (malloc'd) */
	struct in_addr ap_ip_addr; /* AP IP address */
	u32 ap_window_mins;	/* duration of AP mode window in minutes */
	bool ap_window_at_startup; /* open AP window at startup */
	bool ap_window_secure;	/* disable AP window when >= 1 profiles saved */
	enum wifi_prof_save_mode prof_save_mode; /* Profile save policy */

	struct net_ifinfo if_info;	/* wireless interface info */
	struct in_addr def_route;	/* default route from DCHP */
	char *dsn;			/* device serial number */

	enum wifi_conn_state state;
	enum wps_state wps_state;
	bool network_up;	/* received an IP address */
	bool cloud_up;		/* received a cloud-up notification */
	u64 ap_window_close_ms;	/* mtime when AP window closes */

	u64 scan_time_ms;			/* time of the last scan */
	struct wifi_profile *curr_profile;	/* selected profile */
	struct wifi_profile *pref_profile;	/* preferred profile */
	struct wifi_profile profile[WIFI_PROF_CT]; /* saved profiles */
	struct wifi_profile unsaved_prof;	/* new profile */
	struct wifi_profile ap_profile;		/* AP mode network profile */
	int ap_channel;				/* AP mode Wi-Fi channel */
	struct wifi_ssid scan4;		/* specific SSID being scanned */
	struct wifi_scan_result scan[WIFI_SCAN_CT];
	struct wifi_history *curr_hist;		/* current history entry */
	struct wifi_history hist[WIFI_HIST_CT];
};

extern const char * const wifi_errors[];
extern const struct name_val wifi_sec_names[];

extern struct wifi_state wifi_state;

/* XXX missing from some stdio.h versions */
int vasprintf(char **, const char *, va_list);

/*
 * Utility functions
 */
void wifi_timer_init(struct timer *timer, void (*handler)(struct timer *));
void wifi_timer_set(struct timer *timer, unsigned long ms);
void wifi_timer_clear(struct timer *timer);
int wifi_freq_chan(u32 freq);
u8 wifi_bars(int signal);
bool wifi_ssid_match(const struct wifi_ssid *a, const struct wifi_ssid *b);
int wifi_parse_ssid(const char *input, struct wifi_ssid *ssid);
const char *wifi_ssid_to_str(const struct wifi_ssid *ssid);
struct wifi_scan_result *wifi_scan_lookup_ssid(struct wifi_state *wifi,
    const struct wifi_ssid *ssid);
struct wifi_scan_result *wifi_scan_lookup_bssid(struct wifi_state *wifi,
    const struct ether_addr *bssid);
struct wifi_profile *wifi_prof_lookup(struct wifi_state *wifi,
    const struct wifi_ssid *ssid);
struct wifi_profile *wifi_prof_search(struct wifi_state *wifi,
    const struct wifi_ssid *ssid);
int wifi_sec_downgrade(enum wifi_sec new, enum wifi_sec old);
int wifi_check_key(const struct wifi_key *key, enum wifi_sec sec);
enum wifi_sec wifi_scan_get_best_security(const struct wifi_scan_result *scan,
    struct wifi_key *key);
int wifi_script_run(const char *directory, const char *cmd, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Wi-Fi controls
 */
void wifi_conf_init(void);
void wifi_init(void);
void wifi_exit(void);
int wifi_start(void);
void wifi_poll(void);
void wifi_step(void);
int wifi_net_update(bool up);
int wifi_cloud_update(bool up);
int wifi_shutdown(void);
int wifi_factory_reset(void);
int wifi_scan(void);
int wifi_connect(struct wifi_profile *prof);
int wifi_ap_mode_stop(void);
int wifi_wps_pbc(void);

/*
 * Wi-Fi helper functions
 */
struct wifi_profile *wifi_prof_add(const struct wifi_ssid *ssid, enum wifi_sec,
    const struct wifi_key *key, enum wifi_error *error);
int wifi_prof_delete(const struct wifi_ssid *ssid, enum wifi_error *error);
int wifi_scan_clear(void);
int wifi_scan_add(struct wifi_scan_result *scan);
void wifi_ap_window_start(void);
struct wifi_history *wifi_hist_new(const struct wifi_ssid *ssid,
    const struct ether_addr *bssid, enum wifi_error err, bool last_attempt);
void wifi_hist_update(enum wifi_error err, const struct wifi_ssid *ssid,
    const struct ether_addr *bssid);

/*
 * Local messaging interface.
 */
void wifi_interface_init(void);
void wifi_interface_notify_scan_complete(void);
void wifi_interface_notify_info_updated(void);


/*
 * Register scan complete callback handler
 */
void wifi_reg_scan_complete_cb(void (*handler)(void));

/*
 * Register connect state change callback handler
 */
void wifi_reg_connect_state_change_cb(void (*handler)(void));

/*
 * Register ap mode state change callback handler
 */
void wifi_reg_ap_mode_change_cb(void (*handler)(bool));

/*
 * Get scan results for BLE WiFi setup feature
 */
int wifi_get_scan_results(struct wifi_scan_result *scan, int size);

/*
 * Get dsn
 */
char *wifi_get_dsn(void);

/*
 * Set setup_token
 */
void wifi_set_setup_token(char *token);

#endif /* __COND_WIFI_H__ */
