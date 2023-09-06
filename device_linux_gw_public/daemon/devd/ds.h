/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_DS_CLIENT_H__
#define __AYLA_DS_CLIENT_H__

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/queue.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <ayla/utypes.h>
#include <ayla/file_event.h>
#include <ayla/clock.h>
#include <ayla/timer.h>
#include <ayla/buffer.h>
#include <ayla/http_client.h>
#include <ayla/crypto.h>

#include "ds_client.h"

#define CLIENT_SERVER_DOMAIN_US    "aylanetworks.com"
#define CLIENT_SERVER_HOST_DEFAULT "ads-dev"
#define CLIENT_SERVER_DEFAULT	CLIENT_SERVER_HOST_DEFAULT "." \
					CLIENT_SERVER_DOMAIN_US

#define ADS_TIME_FIELD			"x-Ayla-server-time"
#define ADS_CLIENT_AUTH_FIELD		"x-Ayla-client-auth"
#define ADS_TEMP_AUTH_FIELD		"x-Ayla-auth-key"
#define ADS_AUTH_VERS			"Ayla1.0"

#define MAX_DSN_LEN 20
#define DEV_KEY_LEN 10
#define LAN_KEY_LEN 32
#define NP_KEY_LEN	16	/* max AES crypto key length in 8-bit bytes */
#define SESSION_KEY_LEN 40
#define REGTOKEN_LEN	8

#define CLIENT_API_MAJOR	1	/* API version we support */
#define CLIENT_API_MINOR	0	/* API minor version we support */
#define CLIENT_API_VERSION	"1.0"
#define CLIENT_MODEL		"AY001MRT1" /* Ayla model for all Linux WBs */

#define DS_API_MAJOR		"api_major"
#define DS_API_MINOR		"api_minor"
#define DS_CLIENT_SW_VER	"sw_version"
#define DS_CLIENT_LAN_IP	"lan_ip"
#define DS_CLIENT_MODEL		"model"
#define DS_CLIENT_SETUP		"setup_token"
#define DS_CLIENT_LOC		"setup_location"
#define DS_CLIENT_SSID		"ssid"
#define DS_CLIENT_OEM		"oem"
#define DS_CLIENT_OEM_MODEL	"oem_model"
#define DS_CLIENT_OEM_KEY	"oem_key"
#define DS_CLIENT_SSID		"ssid"
#define DS_CLIENT_MAC_ADDR	"mac"
#define DS_CLIENT_HW_ID		"hwsig"
#define DS_CLIENT_PROD_NAME	"product_name"
#define DS_CLIENT_ANS_CIPHER_KEY "ans_cipher_key"
#define DS_CLIENT_ANS_SERVER	"ans_server"
#define DS_CLIENT_TEMPLATE_VER	"template_version"

#define CLIENT_POLL_INTERVAL	300	/* default polling time, seconds */
#define CLIENT_MAX_POLLING_TIME	\
    (24 * 60 * 60)			/* time until forced reconnect, sec */

/* defines for lan client */
#define CLIENT_LAN_KEEPALIVE	30	/* default LAN keepalive time, secs */
#define CLIENT_LAN_URI_LEN	25	/* max URI length for LAN app */
#define CLIENT_LAN_KEY_LEN	32	/* LAN Key length */
#define CLIENT_LAN_REGS		5	/* max number of LAN registrations */
#define CLIENT_LAN_IV_SIZE	16	/* CBC IV Seed */
#define CLIENT_LAN_RAND_SIZE	16	/* random data size for LAN key exch */
#define CLIENT_LAN_ENC_BUF_SIZE 600	/* buffer alloc - lan encr/decrypt */
#define CLIENT_LAN_MAX_URL_LEN	50	/* max len of a LAN URL */

/*
 * Cloud state definitions
 */
#define DS_CLOUD_STATES(def)				\
	def(DOWN,		DS_CLOUD_DOWN)		\
	def(INIT,		DS_CLOUD_INIT)		\
	def(UPDATE,		DS_CLOUD_UPDATE)	\
	def(UP,			DS_CLOUD_UP)

DEF_ENUM(ds_cloud_state, DS_CLOUD_STATES);

enum ds_video_stream_request_step {
	DS_VIDEO_STREAM_REQUEST_STEP_IDLE = 0,		/* idle */
	DS_VIDEO_STREAM_REQUEST_STEP_KVS,			/* request KVS stream */
	DS_VIDEO_STREAM_REQUEST_STEP_WEBRTC,		/* request WebRTC stream */
	DS_VIDEO_STREAM_REQUEST_STEP_DONE,			/* request done */
	DS_VIDEO_STREAM_REQUEST_STEP_COUNT			/* step count */
};

struct device_state {
	char *dsn;
	char *pub_key;
	char *oem;
	char *oem_model;
	char *oem_key;

	char *template_version;
	char *template_version_curr;
	char *template_version_next;
	char *ads_host;
	unsigned ads_host_dev_override:1;

	u16 poll_interval;		/* polling interval, seconds */
	u32 ads_polls;			/* polls since polling started */

	unsigned setup_mode:1;
	char auth_header[SESSION_KEY_LEN + 30];

	char key[DEV_KEY_LEN];		/* device index number for server */

	enum ds_cloud_state cloud_state;
	int req_auth_errors;
	char *regtoken;			/* registration token string */
	char *prod_name;		/* user-assigned product name */
	char *reg_type;			/* registration type */

	json_t *wifi_features;		/* features advertised by cond */
	char *connected_ssid;		/* wi-fi SSID */
	char *location;			/* device lat/lon */
	char *setup_token;		/* wi-fi setup token */

	struct timer_head timers;
	struct timer ping_ads_timer;
	struct timer poll_ads_timer;
	struct timer ds_step_timer;
	struct timer reg_window_timer;
	struct file_event_table file_events;
	struct in_addr lan_ip;		/* local network address for ADS */
	int app_sock;
	u8 dests_avail;			/* available destination mask */

	unsigned net_up:1;		/* network is up */
	unsigned do_reconnect:1;	/* need to reconnect to ADS */
	unsigned ping_ok:1;		/* ping was successful */
	unsigned do_ping:1;		/* ping ADS */
	unsigned np_started:1;		/* notify service started */
	unsigned np_up:1;		/* notify service is live */
	unsigned np_up_once:1;		/* notifier has come up at some point */
	unsigned get_cmds:1;		/* an event is pending */
	unsigned par_content:1;		/* received status 206 last get_cmds */
	unsigned poll_ads:1;		/* set to 1 if polling ADS */
	unsigned registered:1;		/* device is registered to a user */
	unsigned reg_window_start:1;	/* open registration window */
	unsigned ads_listen:1;		/* appd enabling GETs from ads */
	unsigned hard_reset:1;		/* hard reset */
	unsigned factory_reset:1;	/* factory reset */
	unsigned wifi_ap_enabled:1;	/* wifi in ap mode */
	unsigned update_time:1;	        /* update time flag */
	unsigned update_oem_info:1;	/* update oem info is pending */
	unsigned template_assoc:1;	/* template association done */
	unsigned get_regtoken:1;	/* get regtoken when unregistered */

	u64 conn_mtime;			/* last connection mtime */
	time_t conn_time;		/* last connection time (UTC) */
	u32 ping_attempts;

	struct lan_state {		/* controls local-network mode */
		u8 enable:1;
		u8 auto_sync:1;
		u16 keep_alive;
		u16 key_id;
		char key[LAN_KEY_LEN + 1];
		char lanip_random_key[LAN_KEY_LEN];
	} lan;

	struct http_client *http_client; /* HTTP client state */
	struct ds_client client;	/* Primary cloud client for devd */
	struct ds_client app_client;	/* Cloud client for application reqs */

	u8 cipher_key[NP_KEY_LEN];	/* ans cipher key */
	u8 cipher_key_len;		/* ans cipher key len */

	json_t *commands;		/* cached cmds needing to be exec'd */
	json_t *update_info;		/* information for the PUT */
	int ota_status;			/* OTA status to put if non-zero */

	struct video_stream_request {
		bool request;		/* start request for KVS and WebRTC data */
		enum ds_video_stream_request_step step;		    /* step of the request */
		struct timer timer;	/* timer for the state machine */
		u32 timeout_ms;		/* state machine period timeout */
		char* addr_curr;	/* current address to be processed */
	} video_stream_req;
};

/*
 * LAN registration entries.
 */
struct client_lan_reg {
	u8 id;				/* id of cli (pos. in dest_mask) */
	u64 mtime;			/* time of creation or refresh */
	struct in_addr host_addr;	/* LAN host address */
	u16 host_port;
	char uri[CLIENT_LAN_URI_LEN];	/* URI is empty if entry nvalid */

	u8 pending:1;			/* set to 1 if updates pending */
	u8 prefer_get:1;		/* set to 1 if GET is preferred */
	u8 valid_key:1;			/* set to 1 after key exchange */
	u8 rsa_ke:1;			/* RSA key exchange */
	u16 send_seq_no;		/* seq no of outgoing packets */
	long status;			/* status of last HTTP get */
	u32 connect_time;		/* time of last connection */
	char buf[CLIENT_LAN_ENC_BUF_SIZE]; /* send/recv buff for LAN */
	u16 recved_len;			/* length of recved so far */
	u16 refresh_count;		/* # of refresh in a row without np */
	char *pubkey; /* RSA public key, if secure Wi-Fi */
	char random_one[CLIENT_LAN_RAND_SIZE + 1]; /* rand str for sess enc */
	unsigned long long time_one;	/* random time number for sess enc */
	u8 mod_sign_key[CLIENT_LAN_KEY_LEN]; /* module signature key */
	u8 app_sign_key[CLIENT_LAN_KEY_LEN]; /* lan signature key */
	u8 mod_enc_key[CLIENT_LAN_KEY_LEN]; /* module AES 256 key */
	u8 app_enc_key[CLIENT_LAN_KEY_LEN]; /* app AES 256 key */
	struct crypto_state mod_enc;	/* module AES context */
	struct crypto_state app_enc;	/* app AES context */
	struct timer timer;
	struct timer step_timer;	/* used to trigger next step */
};

extern struct device_state device;
extern struct client_lan_reg client_lan_reg[CLIENT_LAN_REGS];

extern int debug;
extern bool foreground;
extern bool ds_test_mode;
extern const char version[];
extern char app_sock_path[];


/*
 * Terminate appd, if managed by devd.
 */
void ds_kill_appd(void);

/*
 * Print a JSON object.
 */
void ds_json_dump(const char *msg, const json_t *obj);

/*
 * Test if echo is needed for an object
 */
int ds_echo_for_prop_is_needed(json_t *prop, int source);

struct server_req;
void client_lan_reg_post(struct server_req *);
void client_lan_reg_put(struct server_req *);
void client_lan_cmd_resp(struct server_req *, int status);

/*
 * This function can be used to POST any JSON to a LAN client given a URL
 * If err_type is provided, it'll be set to the appropriate error type if
 * an error occurs.
 */
int client_lan_post(struct device_state *dev, struct client_lan_reg *lan,
			json_t *body, const char *url, const char **err_type);

#endif /* __AYLA_DS_CLIENT_H__ */
