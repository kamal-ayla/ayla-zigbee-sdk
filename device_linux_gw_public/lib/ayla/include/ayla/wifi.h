/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_WIFI_H__
#define __AYLA_WIFI_H__

#include <ayla/token_table.h>

#define WIFI_SIGNAL_NONE	MIN_S8	/* non-existent signal level */
#define WIFI_SIGNAL_MIN		(WIFI_SIGNAL_NONE + 1)	/* min signal level */
#define WIFI_BARS_MIN		0	/* bars at minimum signal */

#define WIFI_MIN_KEY_LEN	8	/* minimum legal pre-shared key len */
#define WIFI_MAX_KEY_LEN	64	/* max key length (any type) */
#define WIFI_KEY_LEN_WEP64	5	/* key length for 64-bit WEP */
#define WIFI_KEY_LEN_WEP128	13	/* key length for 128-bit WEP */
#define WIFI_KEY_LEN_WEP152	16	/* key length for 152-bit WEP */
#define WIFI_KEY_LEN_WEP256	29	/* key length for 256-bit WEP */
#define WIFI_SSID_LEN		32	/* bytes */

#define WIFI_SETUP_TOKEN_LEN	8	/* characters */
#define WIFI_LOC_STR_MAX_SIZE	32	/* example: -123.456789,-123.456789 */

/*
 * Wi-Fi connection error codes.  Order MUST NOT change.
 *
 * 1 WIFI_ERR_MEM - resource problem, possibly temporary
 * 2 WIFI_ERR_TIME - connection timed out
 * 3 WIFI_ERR_INV_KEY - invalid key
 * 4 WIFI_ERR_NOT_FOUND - SSID not found
 * 5 WIFI_ERR_NOT_AUTH - not authenticated
 * 6 WIFI_ERR_WRONG_KEY - incorrect key
 * 7 WIFI_ERR_NO_IP - failed to get IP address from DHCP
 * 8 WIFI_ERR_NO_ROUTE - failed to get default gateway from DHCP
 * 9 WIFI_ERR_NO_DNS - failed to get DNS server from DHCP
 * 10 WIFI_ERR_AP_DISC - AP disconnected the module
 * 11 WIFI_ERR_LOS - Loss of signal / beacon miss
 * 12 WIFI_ERR_CLIENT_DNS - ADS not reached due to DNS
 * 13 WIFI_ERR_CLIENT_REDIR - failed to reach ADS due to redirect
 * 14 WIFI_ERR_CLIENT_TIME - failed to reach ADS - timeout
 * 15 WIFI_ERR_NO_PROF - no empty profile slots
 * 16 WIFI_ERR_SEC_UNSUP - security method not supported
 * 17 WIFI_ERR_NET_UNSUP - network type (e.g. ad-hoc) not supported
 * 18 WIFI_ERR_PROTOCOL - server incompatible.  May be a hotspot.
 * 19 WIFI_ERR_CLIENT_AUTH - failed to authenticate to service
 * 20 WIFI_ERR_IN_PROGRESS - attempt still in progress
 */
#define WIFI_ERRORS(def)						\
	def(none,				WIFI_ERR_NONE)		\
	def(resource problem,			WIFI_ERR_MEM)		\
	def(connection timed out,		WIFI_ERR_TIME)		\
	def(invalid key,			WIFI_ERR_INV_KEY)	\
	def(SSID not found,			WIFI_ERR_NOT_FOUND)	\
	def(not authenticated,			WIFI_ERR_NOT_AUTH)	\
	def(incorrect key,			WIFI_ERR_WRONG_KEY)	\
	def(failed to get IP address from DHCP,	WIFI_ERR_NO_IP)	\
	def(failed to get default gateway from DHCP, WIFI_ERR_NO_ROUTE)	\
	def(failed to get DNS server from DHCP,	WIFI_ERR_NO_DNS)	\
	def(disconnected by AP,			WIFI_ERR_AP_DISC)	\
	def(loss of signal / beacon miss,	WIFI_ERR_LOS)		\
	def(device service host name lookup failed, WIFI_ERR_CLIENT_DNS) \
	def(device service GET redirected,	WIFI_ERR_CLIENT_REDIR)	\
	def(device service connection timed out, WIFI_ERR_CLIENT_TIME)	\
	def(no empty profile slots,		WIFI_ERR_NO_PROF)	\
	def(security method not supported,	WIFI_ERR_SEC_UNSUP)	\
	def(network type not supported,		WIFI_ERR_NET_UNSUP)	\
	def(server incompatible,		WIFI_ERR_PROTOCOL)	\
	def(failed to authenticate to service,	WIFI_ERR_CLIENT_AUTH)	\
	def(attempt in progress,		WIFI_ERR_IN_PROGRESS)

DEF_ENUM(wifi_error, WIFI_ERRORS);

/*
 * Wi-Fi station ID
 */
struct wifi_ssid {
	u8 len;
	u8 val[WIFI_SSID_LEN];
};

/*
 * Key structure large enough for any security type
 */
struct wifi_key {
	u8 len;
	u8 val[WIFI_MAX_KEY_LEN];
};

#endif /* __AYLA_WIFI_H__ */
