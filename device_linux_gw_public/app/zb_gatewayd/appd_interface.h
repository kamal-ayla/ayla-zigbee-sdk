/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __APPD_INTERFACE_H__
#define __APPD_INTERFACE_H__

#define ZB_NODE_ADDR_LEN    16
#define ZB_MODEL_ID_LEN     32

#define ZB_OEM_HOST_VERION		"oem_host_version"
#define ZB_ON_OFF_PROP_NAME		"onoff"
#define ZB_IS_ON_OFF_PROP_NAME		"is_onoff"
#define ZB_LEVEL_CTRL_PROP_NAME		"level_control"
#define ZB_BATTERY_LOW_NAME		"battery_low"
#define ZB_BATTERY_DEFECT_NAME		"battery_defect"
#define ZB_SHORT_ADDR_PROP_NAME		"short_address"
#define ZB_LONG_ADDR_PROP_NAME		"long_address"
#define ZB_POWER_SRC_PROP_NAME		"power_source"
#define ZB_POWER_LEV_PROP_NAME		"power_level"
#define ZB_STATUS_PROP_NAME		"status"
#define ZB_MODEL_PROP_NAME		"model_id"
#define ZB_ALIAS_PROP_NAME		"alias"
#define ZB_SYSTEM_MODE			"system_mode"
#define ZB_COOLING_SETPOINT		"cooling_setpoint"
#define ZB_HEATING_SETPOINT		"heating_setpoint"
#define ZB_LOCAL_TEMPERATURE		"local_temperature"
#define ZB_LOCAL_HUMIDITY		"local_humidity"
#define ZB_MEASURE_TEMPERATURE		"measure_temperature"
#define ZB_MEASURE_HUMIDITY		"measure_humidity"
#define ZB_FAN_MODE			"fan_mode"
#define ZB_TEMPERATURE_DISPLAY		"temperature_display"
#define ZB_LEAK_ALARM_PROP_NAME		"leak_alarm"
#define ZB_OPEN_CLOSE_PROP_NAME		"openclose"
#define ZB_SMOKE_ALARM_PROP_NAME	"smoke_alarm"
#define ZB_SMOKE_ALARM_TAMPER_NAME	"tamper"
#define ZB_SMOKE_ALARM_TEST_NAME	"test_mode"
#define ZB_PRESS_PROP_NAME		"press"
#define ZB_PIN_PROP_NAME		"pin_code"
#define ZB_USER_ID_PROP_NAME		"user_id"
#define ZB_IS_LOCKED_PROP_NAME		"is_locked"
#define ZB_SET_LOCKED_PROP_NAME		"set_locked"
#define ZB_LIGHT_PROP_NAME		"light"
#define ZB_TAP_PROP_NAME		"tap"
#define ZB_TAMPER_PROP_NAME		"tamper"
#define ZB_BATTERY_VOLTAGE		"battery_voltage"

/*
 * Update node as online status
 */
void appd_update_as_online_status(uint16_t node_id);

/*
 * Handle a node join network event from the network stack.
 */
void appd_node_join_network(const uint8_t *node_eui, uint16_t node_id);

/*
 * Handle a node left event from the network stack.
 */
void appd_node_left(const uint8_t *node_eui);

/*
 * Start all node power descriptor query for loaded node from config file
 * after ZigBee network up
 */
void appd_start_all_power_query(void);

/*
 * Appd handle simple descriptor reply
 */
void appd_simple_complete_handler(uint16_t node_id,
				uint16_t profileId, uint16_t deviceId);

/*
 * Handle power source info reply
 */
void appd_power_source_complete_handler(uint16_t node_id,
			uint8_t primary_power);

/*
 * Appd handle model id info reply
 */
void appd_model_identifier_complete_handler(uint16_t node_id, char *model_id);

/*
 * Handle read zone state reply
 */
void appd_read_zone_state_complete_handler(uint16_t node_id, uint8_t state);

/*
 * Handle prop set reply
 */
void appd_prop_complete_handler(uint16_t node_id, char *name, uint8_t status);

/*
 * Handle power descriptor reply
 */
void appd_power_complete_handler(uint16_t node_id,
				uint8_t powerType, uint8_t powerLevel);

/*
 * Starts node binding
 */
int appd_start_node_bind(uint16_t node_id, uint16_t cluster_id);

/*
 * Starts a node bind / unbind request
 */
int appd_node_bind_control(uint16_t node_id, uint16_t cluster_id, bool onoff);

/*
 * Query ieee address handler
 */
void appd_ieee_query_handler(uint16_t node_id);

/*
 * Update node_id
 */
void appd_update_node_id(const uint8_t *node_eui, uint16_t node_id);

/*
 * Update node prop to cloud
 */
void appd_update_node_prop(uint16_t node_id, char *name, void *value);

/*
 * Obtain node prop from cloud
 */
struct node_prop *appd_get_node_prop_val(uint16_t node_id, char *prop_name);

/*
 * Update ias zone node prop to cloud
 */
void appd_ias_zone_status_change_handler(uint16_t node_id, uint8_t *msg);

/*
 * Update gateway ZigBee network status to cloud
 */
void appd_update_network_status(bool status);

/*
 * Handle IAS device enroll request
 */
void appd_ias_enroll_req_handler(uint16_t node_id, uint16_t zone_type,
			uint16_t manufacter_code);

/*
 * Handle write CIE address reply
 */
void appd_write_cie_complete_handler(uint16_t node_id, uint8_t status);

/*
 * Handle motion sensor match descriptor request.
 */
void appd_motion_match_handler(uint16_t node_id, uint8_t seq_no);

/*
 * Handle bind response result.
 */
void appd_bind_response_handler(uint16_t node_id,
			uint8_t status, bool unbind);

/*
 * Update node decimal prop
 */
void appd_update_decimal_prop(uint16_t node_id, char *name, double value);

/*
 * Update node int prop
 */
void appd_update_int_prop(uint16_t node_id, char *name, int value);

void appd_zb_send_battery_voltage_request(char *node_addr);

void appd_zb_send_power_source_request(char *node_addr);

#endif /* __APPD_INTERFACE_H__ */

