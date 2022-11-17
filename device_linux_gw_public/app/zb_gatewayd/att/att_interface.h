/*
 * Copyright 2019 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __ATT_INTERFACE_H__
#define __ATT_INTERFACE_H__

#define VNODE_OEM_MODEL 	       "virtualnode"

#define ATT_POC_ACTIVE                 "Active"
#define ATT_POC_MACADDRESS             "MACAddress"
#define ATT_POC_RSSI	       	       "RSSI"
#define ATT_POC_NOISE                  "Noise"
#define ATT_POC_STATION_TYPE	       "StationType"
#define ATT_POC_SSID		       "SSID"
#define ATT_POC_CHANNEL		       "Channel"
#define ATT_POC_PARENT_NODE	       "ParentNode"


#define ATT_POC_GET_ACTIVESTATUS       "get_stainfo.sh -status %s"
#define ATT_POC_GET_MACADDRESS	       "get_stainfo.sh -mac"
#define ATT_POC_GET_RSSI	       "get_stainfo.sh -rssi %s"	
#define ATT_POC_GET_NOISE              "get_stainfo.sh -noise %s"
#define ATT_POC_GET_STATION_TYPE       "get_stainfo.sh -stationtype %s"
#define ATT_POC_GET_SSID	       "get_stainfo.sh -ssid %s"
#define ATT_POC_GET_CHANNEL	       "get_stainfo.sh -channel %s"
#define ATT_POC_GET_PARENT_NODE        "get_stainfo.sh -parent"


#define ATT_POC_ADDR_LEN         32
#define ATT_POC_STR_LEN          50
#define ATT_POC_NODE_MAX         100


#define COMMAND_LEN 64
#define DATA_SIZE   128


#define ATT_DATA_DEVIATION_DB 	10
#define MAC_ADDR_LEN 		6
/*
 * Get AT&T node data
 */
struct att_poc_node_data *att_get_node_data(uint16_t node_id);

/*
 * Handle prop set reply
 */
void att_prop_complete_handler(uint16_t node_id, char *name, uint8_t status);

/*
 * Update node prop to cloud
 */
void att_update_node_prop(uint16_t node_id, char *name, void *value);

/*
 * Update node decimal prop
 */
void att_update_decimal_prop(uint16_t node_id, char *name, double value);

/*
 * Update node int prop
 */
void att_update_int_prop(uint16_t node_id, char *name, int value);


/*
 * Add node to gateway node list.
 */
int att_node_add(const char *mac_addr);



int att_set_node_data(char *id, char *name, char *value);

int att_check_data_deviation(char *macaddr, char *name, char *value);

void att_update_nodes_data(void);

void att_poll(void);

void att_node_left(const char *addr);

uint16_t att_get_node_index_by_mac(const char *mac);

uint16_t att_get_available_node_id(void);

void att_set_poll_period(int period);

void att_node_left_handler(const char *macaddr);

int exec_systemcmd(char *cmd, char *retBuf, int retBufSize);
#endif /* __ATT_INTERFACE_H__ */

