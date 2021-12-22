/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_MSG_DEFS_H__
#define __AYLA_MSG_DEFS_H__

#include <ayla/wifi.h>

/*****************************************
 * Amsg Interface definition.
 * Interfaces are used to separate messages
 * into different functional domains.
 * Message types for each interface may be
 * defined below.
 *****************************************/

enum msg_interface {
	/* Interface 0x00 is reserved for internal use */
	MSG_INTERFACE_APPLICATION = 0x01,
	MSG_INTERFACE_CONFIG,
	MSG_INTERFACE_CLI,
	MSG_INTERFACE_SYSTEM,
	MSG_INTERFACE_CLIENT,
	MSG_INTERFACE_WIFI,
	MSG_INTERFACE_OTA
};


/*****************************************
 * INTERFACE_APPLICATION message definition
 *****************************************/

enum msg_app_type {
	MSG_APP_INFO,
	MSG_APP_TEMPLATE_VER
};

struct msg_app_info {
	pid_t pid;
	char name[0];
};

struct msg_template_ver {
	char template_ver[0];
};


/*****************************************
 * INTERFACE_CONFIG message definition
 *****************************************/

enum msg_config_type {
	MSG_CONFIG_VALUE_REQ,
	MSG_CONFIG_VALUE_RESP,
	MSG_CONFIG_VALUE_SET,
	MSG_CONFIG_VALUE_DELETE,
	MSG_CONFIG_REG,
	MSG_CONFIG_UNREG,
	MSG_CONFIG_NOTIFY,
	MSG_CONFIG_FACTORY_RESET
};


/*****************************************
 * INTERFACE_CLI message definition
 *****************************************/

enum msg_cli_type {
	MSG_CLI_INPUT,
	MSG_CLI_OUTPUT,
};


/*****************************************
 * MSG_INTERFACE_SYSTEM message definition
 *****************************************/
enum msg_system_dhcp_event {
	MSG_SYSTEM_DHCP_UNBOUND,
	MSG_SYSTEM_DHCP_BOUND,
	MSG_SYSTEM_DHCP_REFRESH
};

enum msg_system_type {
	MSG_SYSTEM_DHCP_EVENT
};

struct msg_system_dhcp {
	enum msg_system_dhcp_event event;
	char interface[16];	/* IFNAMSIZ */
};


/*****************************************
 * MSG_INTERFACE_CLIENT message definition
 *****************************************/
enum msg_client_type {
	MSG_CLIENT_DESTS,
	MSG_CLIENT_DESTS_REQ,
	MSG_CLIENT_DESTS_REG,
	MSG_CLIENT_TIME,
	MSG_CLIENT_TIME_REQ,
	MSG_CLIENT_TIME_REG,
	MSG_CLIENT_TIME_SET,
	MSG_CLIENT_USERREG,
	MSG_CLIENT_USERREG_REQ,
	MSG_CLIENT_USERREG_REG,
	MSG_CLIENT_USERREG_WINDOW_START,
	MSG_CLIENT_SETUP_INFO,
	MSG_CLIENT_LISTEN
};

struct msg_client_dests {
	bool cloud_up;
	bool lan_up;
};

struct msg_client_time {
	uint8_t source;
	time_t time_utc;
};

struct msg_client_reg_info {
	bool registered;
	bool status_changed;
	char regtoken[8];	/* REGTOKEN_LEN */
};

struct msg_client_setup_info {
	char setup_token[WIFI_SETUP_TOKEN_LEN + 1];
	char location[WIFI_LOC_STR_MAX_SIZE];
};


/*****************************************
 * MSG_INTERFACE_WIFI message definition
 *****************************************/
enum msg_wifi_type {
	MSG_WIFI_INFO,
	MSG_WIFI_INFO_REQ,
	MSG_WIFI_AP_WINDOW_OPEN,
	MSG_WIFI_AP_STOP,
	MSG_WIFI_STATUS,
	MSG_WIFI_STATUS_REQ,
	MSG_WIFI_PROFILE_ADD,
	MSG_WIFI_PROFILE_DELETE,
	MSG_WIFI_PROFILE_LIST,
	MSG_WIFI_PROFILE_LIST_REQ,
	MSG_WIFI_SCAN_START,
	MSG_WIFI_SCAN_RESULTS,
	MSG_WIFI_SCAN_RESULTS_REQ,
	MSG_WIFI_CONNECT,
	MSG_WIFI_WPS_PBC,
	MSG_WIFI_SETUP_TOKEN,
	MSG_WIFI_SETUP_TOKEN_RESP,
	MSG_WIFI_ERR
};


/*****************************************
 * MSG_INTERFACE_OTA message definition
 *****************************************/
enum msg_ota_type {
	MSG_OTA_STATUS
};

struct msg_ota_status {
	int32_t status;
};


#endif /* __AYLA_MSG_DEFS_H__ */
