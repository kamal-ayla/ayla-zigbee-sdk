// This file is generated by Simplicity Studio.  Please do not edit manually.
//
//

// Enclosing macro to prevent multiple inclusion
#ifndef SILABS_ZNET_CONFIG
#define SILABS_ZNET_CONFIG


/**** Included Header Section ****/



// Networks
#define EM_AF_GENERATED_NETWORK_TYPES { \
  EM_AF_NETWORK_TYPE_ZIGBEE_PRO, /* Primary */ \
}
#define EM_AF_GENERATED_ZIGBEE_PRO_NETWORKS { \
  { \
    /* Primary */ \
    ZA_COORDINATOR, \
    EMBER_AF_SECURITY_PROFILE_HA, \
  }, \
}
#define EM_AF_GENERATED_NETWORK_STRINGS  \
  "Primary (pro)", \
/**** ZCL Section ****/
#define ZA_PROMPT "ember"
#define ZCL_USING_BASIC_CLUSTER_SERVER
#define ZCL_USING_IDENTIFY_CLUSTER_CLIENT
#define ZCL_USING_IDENTIFY_CLUSTER_SERVER
#define ZCL_USING_TIME_CLUSTER_SERVER
#define ZCL_USING_POWER_PROFILE_CLUSTER_CLIENT
#define ZCL_USING_THERMOSTAT_CLUSTER_CLIENT
#define ZCL_USING_FAN_CONTROL_CLUSTER_CLIENT
#define ZCL_USING_DEHUMID_CONTROL_CLUSTER_CLIENT
#define ZCL_USING_THERMOSTAT_UI_CONFIG_CLUSTER_CLIENT
#define ZCL_USING_COLOR_CONTROL_CLUSTER_CLIENT
#define ZCL_USING_ILLUM_MEASUREMENT_CLUSTER_SERVER
#define ZCL_USING_ILLUM_LEVEL_SENSING_CLUSTER_SERVER
#define ZCL_USING_TEMP_MEASUREMENT_CLUSTER_SERVER
#define ZCL_USING_PRESSURE_MEASUREMENT_CLUSTER_SERVER
#define ZCL_USING_FLOW_MEASUREMENT_CLUSTER_SERVER
#define ZCL_USING_RELATIVE_HUMIDITY_MEASUREMENT_CLUSTER_SERVER
#define ZCL_USING_OCCUPANCY_SENSING_CLUSTER_SERVER
#define ZCL_USING_IAS_ZONE_CLUSTER_CLIENT
#define ZCL_USING_IAS_ACE_CLUSTER_CLIENT
#define ZCL_USING_IAS_WD_CLUSTER_CLIENT
#define ZCL_USING_SIMPLE_METERING_CLUSTER_CLIENT
#define ZCL_USING_METER_IDENTIFICATION_CLUSTER_CLIENT
#define ZCL_USING_APPLIANCE_STATISTICS_CLUSTER_CLIENT
#define ZCL_USING_ZLL_COMMISSIONING_CLUSTER_SERVER
#define EMBER_AF_MANUFACTURER_CODE 0x1002
#define EMBER_AF_DEFAULT_RESPONSE_POLICY_ALWAYS

/**** Cluster endpoint counts ****/
#define EMBER_AF_BASIC_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_IDENTIFY_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_IDENTIFY_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_TIME_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_POWER_PROFILE_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_THERMOSTAT_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_FAN_CONTROL_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_DEHUMID_CONTROL_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_THERMOSTAT_UI_CONFIG_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_COLOR_CONTROL_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_ILLUM_MEASUREMENT_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_ILLUM_LEVEL_SENSING_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_TEMP_MEASUREMENT_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_PRESSURE_MEASUREMENT_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_FLOW_MEASUREMENT_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_RELATIVE_HUMIDITY_MEASUREMENT_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_OCCUPANCY_SENSING_CLUSTER_SERVER_ENDPOINT_COUNT (1)
#define EMBER_AF_IAS_ZONE_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_IAS_ACE_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_IAS_WD_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_SIMPLE_METERING_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_METER_IDENTIFICATION_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_APPLIANCE_STATISTICS_CLUSTER_CLIENT_ENDPOINT_COUNT (1)
#define EMBER_AF_ZLL_COMMISSIONING_CLUSTER_SERVER_ENDPOINT_COUNT (1)

/**** Cluster Endpoint Summaries ****/
#define EMBER_AF_MAX_SERVER_CLUSTER_COUNT (11)
#define EMBER_AF_MAX_CLIENT_CLUSTER_COUNT (13)
#define EMBER_AF_MAX_TOTAL_CLUSTER_COUNT (24)

/**** CLI Section ****/
#define EMBER_AF_GENERATE_CLI
#define EMBER_AF_ENABLE_CUSTOM_COMMANDS
#define EMBER_COMMAND_INTEPRETER_HAS_DESCRIPTION_FIELD

/**** Security Section ****/
#define EMBER_AF_HAS_SECURITY_PROFILE_HA

/**** Network Section ****/
#define EMBER_SUPPORTED_NETWORKS (1)
#define EMBER_AF_NETWORK_INDEX_PRIMARY (0)
#define EMBER_AF_DEFAULT_NETWORK_INDEX EMBER_AF_NETWORK_INDEX_PRIMARY
#define EMBER_AF_HAS_COORDINATOR_NETWORK
#define EMBER_AF_HAS_ROUTER_NETWORK
#define EMBER_AF_HAS_RX_ON_WHEN_IDLE_NETWORK
#define EMBER_AF_TX_POWER_MODE EMBER_TX_POWER_MODE_USE_TOKEN

/**** Callback Section ****/
#define EMBER_CALLBACK_IAS_ZONE_CLUSTER_IAS_ZONE_CLUSTER_CLIENT_INIT
#define EMBER_CALLBACK_IAS_ZONE_CLUSTER_ZONE_ENROLL_REQUEST
#define EMBER_CALLBACK_IAS_ZONE_CLUSTER_ZONE_STATUS_CHANGE_NOTIFICATION
#define EMBER_CALLBACK_UNUSED_PAN_ID_FOUND
#define EMBER_CALLBACK_SCAN_ERROR
#define EMBER_CALLBACK_FIND_UNUSED_PAN_ID_AND_FORM
#define EMBER_CALLBACK_START_SEARCH_FOR_JOINABLE_NETWORK
#define EMBER_CALLBACK_GET_FORM_AND_JOIN_EXTENDED_PAN_ID
#define EMBER_CALLBACK_SET_FORM_AND_JOIN_EXTENDED_PAN_ID
#define EMBER_CALLBACK_ENERGY_SCAN_RESULT
#define EMBER_CALLBACK_SCAN_COMPLETE
#define EMBER_CALLBACK_NETWORK_FOUND
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_IDENTIFY_CLUSTER_SERVER_INIT
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_IDENTIFY_CLUSTER_SERVER_TICK
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_IDENTIFY_CLUSTER_SERVER_ATTRIBUTE_CHANGED
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_IDENTIFY
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_IDENTIFY_QUERY
#define EMBER_CALLBACK_MAIN_START
#define EMBER_CALLBACK_INCOMING_ROUTE_ERROR_HANDLER
#define EMBER_APPLICATION_HAS_INCOMING_ROUTE_ERROR_HANDLER
#define EMBER_CALLBACK_EZSP_INCOMING_ROUTE_ERROR_HANDLER
#define EZSP_APPLICATION_HAS_INCOMING_ROUTE_ERROR_HANDLER
#define EMBER_CALLBACK_GET_SOURCE_ROUTE_OVERHEAD
#define EMBER_CALLBACK_SET_SOURCE_ROUTE_OVERHEAD
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_TRIGGER_EFFECT
#define EMBER_CALLBACK_MAC_FILTER_MATCH_MESSAGE
#define EMBER_APPLICATION_HAS_MAC_FILTER_MATCH_MESSAGE_HANDLER
#define EMBER_CALLBACK_EZSP_MAC_FILTER_MATCH_MESSAGE
#define EZSP_APPLICATION_HAS_MAC_FILTER_MATCH_HANDLER
#define EMBER_CALLBACK_INTERPAN_SEND_MESSAGE
#define EMBER_CALLBACK_IDENTIFY_CLUSTER_IDENTIFY_QUERY_RESPONSE
#define EMBER_CALLBACK_ZLL_TOUCH_LINK_TARGET_HANDLER
#define EMBER_APPLICATION_HAS_ZLL_TOUCH_LINK_TARGET_HANDLER
#define EMBER_CALLBACK_EZSP_ZLL_TOUCH_LINK_TARGET_HANDLER
#define EZSP_APPLICATION_HAS_ZLL_TOUCH_LINK_TARGET_HANDLER
#define EMBER_CALLBACK_COUNTER_HANDLER
#define EMBER_APPLICATION_HAS_COUNTER_HANDLER
#define EMBER_CALLBACK_EZSP_COUNTER_ROLLOVER_HANDLER
#define EZSP_APPLICATION_HAS_COUNTER_ROLLOVER_HANDLER
/**** Debug printing section ****/

// Global switch
#define EMBER_AF_PRINT_ENABLE
// Individual areas
#define EMBER_AF_PRINT_CORE 0x0001
#define EMBER_AF_PRINT_APP 0x0002
#define EMBER_AF_PRINT_ATTRIBUTES 0x0004
#define EMBER_AF_PRINT_BITS { 0x07 }
#define EMBER_AF_PRINT_NAMES { \
  "Core",\
  "Application",\
  "Attributes",\
  NULL\
}
#define EMBER_AF_PRINT_NAME_NUMBER 3


#define EMBER_AF_SUPPORT_COMMAND_DISCOVERY


// Generated plugin macros

// Use this macro to check if Address Table plugin is included
#define EMBER_AF_PLUGIN_ADDRESS_TABLE
// User options for plugin Address Table
#define EMBER_AF_PLUGIN_ADDRESS_TABLE_SIZE 2
#define EMBER_AF_PLUGIN_ADDRESS_TABLE_TRUST_CENTER_CACHE_SIZE 2

// Use this macro to check if Concentrator Support plugin is included
#define EMBER_AF_PLUGIN_CONCENTRATOR
// User options for plugin Concentrator Support
#define EMBER_AF_PLUGIN_CONCENTRATOR_CONCENTRATOR_TYPE LOW_RAM_CONCENTRATOR
#define EMBER_AF_PLUGIN_CONCENTRATOR_MIN_TIME_BETWEEN_BROADCASTS_SECONDS 10
#define EMBER_AF_PLUGIN_CONCENTRATOR_MAX_TIME_BETWEEN_BROADCASTS_SECONDS 60
#define EMBER_AF_PLUGIN_CONCENTRATOR_ROUTE_ERROR_THRESHOLD 3
#define EMBER_AF_PLUGIN_CONCENTRATOR_DELIVERY_FAILURE_THRESHOLD 1
#define EMBER_AF_PLUGIN_CONCENTRATOR_MAX_HOPS 0
#define EMBER_AF_PLUGIN_CONCENTRATOR_DEFAULT_ROUTER_BEHAVIOR FULL

// Use this macro to check if Counters plugin is included
#define EMBER_AF_PLUGIN_COUNTERS
// User options for plugin Counters

// Use this macro to check if Device Database plugin is included
#define EMBER_AF_PLUGIN_DEVICE_DATABASE
// User options for plugin Device Database
#define EMBER_AF_PLUGIN_DEVICE_DATABASE_MAX_DEVICES 20
#define EMBER_AF_MAX_ENDPOINTS_PER_DEVICE 5
#define EMBER_AF_MAX_CLUSTERS_PER_ENDPOINT 10

// Use this macro to check if EZSP Common plugin is included
#define EMBER_AF_PLUGIN_EZSP

// Use this macro to check if EZSP UART plugin is included
#define EMBER_AF_PLUGIN_EZSP_UART

// Use this macro to check if File Descriptor Dispatch plugin is included
#define EMBER_AF_PLUGIN_FILE_DESCRIPTOR_DISPATCH

// Use this macro to check if Find and Bind Initiator plugin is included
#define EMBER_AF_PLUGIN_FIND_AND_BIND_INITIATOR
// User options for plugin Find and Bind Initiator
#define EMBER_AF_PLUGIN_FIND_AND_BIND_INITIATOR_TARGET_RESPONSES_COUNT 5
#define EMBER_AF_PLUGIN_FIND_AND_BIND_INITIATOR_TARGET_RESPONSES_DELAY_MS MILLISECOND_TICKS_PER_SECOND*3

// Use this macro to check if Find and Bind Target plugin is included
#define EMBER_AF_PLUGIN_FIND_AND_BIND_TARGET
// User options for plugin Find and Bind Target
#define EMBER_AF_PLUGIN_FIND_AND_BIND_TARGET_COMMISSIONING_TIME 180

// Use this macro to check if Form and Join Library plugin is included
#define EMBER_AF_PLUGIN_FORM_AND_JOIN

// Use this macro to check if Gateway Support plugin is included
#define EMBER_AF_PLUGIN_GATEWAY
// User options for plugin Gateway Support
#define EMBER_AF_PLUGIN_GATEWAY_MAX_FDS 10
#define EMBER_AF_PLUGIN_GATEWAY_TCP_PORT_OFFSET 4900
#define EMBER_AF_PLUGIN_GATEWAY_MAX_WAIT_FOR_EVENT_TIMEOUT_MS 0xFFFFFFFF

// Use this macro to check if Heartbeat plugin is included
#define EMBER_AF_PLUGIN_HEARTBEAT
// User options for plugin Heartbeat
#define EMBER_AF_PLUGIN_HEARTBEAT_PERIOD_QS 1

// Use this macro to check if IAS Zone Client plugin is included
#define EMBER_AF_PLUGIN_IAS_ZONE_CLIENT
// User options for plugin IAS Zone Client
#define EMBER_AF_PLUGIN_IAS_ZONE_CLIENT_MAX_DEVICES 10

// Use this macro to check if Identify Cluster plugin is included
#define EMBER_AF_PLUGIN_IDENTIFY

// Use this macro to check if Identify Feedback plugin is included
#define EMBER_AF_PLUGIN_IDENTIFY_FEEDBACK
// User options for plugin Identify Feedback
#define EMBER_AF_PLUGIN_IDENTIFY_FEEDBACK_LED_FEEDBACK

// Use this macro to check if Interpan plugin is included
#define EMBER_AF_PLUGIN_INTERPAN
// User options for plugin Interpan
#define EMBER_AF_PLUGIN_INTERPAN_ALLOW_REQUIRED_SMART_ENERGY_MESSAGES
#define EMBER_AF_PLUGIN_INTERPAN_DELIVER_TO PRIMARY_ENDPOINT
#define EMBER_AF_PLUGIN_INTERPAN_DELIVER_TO_SPECIFIED_ENDPOINT_VALUE 1

// Use this macro to check if mbed TLS plugin is included
#define EMBER_AF_PLUGIN_MBEDTLS

// Use this macro to check if NCP Configuration plugin is included
#define EMBER_AF_PLUGIN_NCP_CONFIGURATION
// User options for plugin NCP Configuration
#define EMBER_BINDING_TABLE_SIZE 2
#define EMBER_SOURCE_ROUTE_TABLE_SIZE 7
#define EMBER_MAX_END_DEVICE_CHILDREN 6
#define EMBER_END_DEVICE_KEEP_ALIVE_SUPPORT_MODE EMBER_KEEP_ALIVE_SUPPORT_ALL
#define EMBER_END_DEVICE_POLL_TIMEOUT MINUTES_256
#define EMBER_END_DEVICE_POLL_TIMEOUT_SHIFT 6
#define EMBER_KEY_TABLE_SIZE 0
#define EMBER_ZLL_GROUP_ADDRESSES 0
#define EMBER_ZLL_RSSI_THRESHOLD -40
#define EMBER_TRANSIENT_KEY_TIMEOUT_S 300
#define EMBER_APS_UNICAST_MESSAGE_COUNT 10
#define EMBER_BROADCAST_TABLE_SIZE 15
#define EMBER_NEIGHBOR_TABLE_SIZE 16
#define EMBER_GP_PROXY_TABLE_SIZE 5
#define EMBER_GP_SINK_TABLE_SIZE 0

// Use this macro to check if Network Creator plugin is included
#define EMBER_AF_PLUGIN_NETWORK_CREATOR
// User options for plugin Network Creator
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_SCAN_DURATION 4
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_CHANNEL_MASK 0x02108800
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_CHANNEL_BEACONS_THRESHOLD 20
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_RADIO_POWER 3

// Use this macro to check if Network Creator Security plugin is included
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_SECURITY
// User options for plugin Network Creator Security
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_SECURITY_NETWORK_OPEN_TIME_S 300
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_SECURITY_TRUST_CENTER_SUPPORT
#define EMBER_AF_PLUGIN_NETWORK_CREATOR_SECURITY_ALLOW_HA_DEVICES_TO_STAY

// Use this macro to check if Network Find plugin is included
#define EMBER_AF_PLUGIN_NETWORK_FIND
#define EMBER_AF_DISABLE_FORM_AND_JOIN_TICK
// User options for plugin Network Find
#define EMBER_AF_PLUGIN_NETWORK_FIND_CHANNEL_MASK 0x0318C800
#define EMBER_AF_PLUGIN_NETWORK_FIND_CUT_OFF_VALUE -48
#define EMBER_AF_PLUGIN_NETWORK_FIND_RADIO_TX_POWER 3
#define EMBER_AF_PLUGIN_NETWORK_FIND_EXTENDED_PAN_ID { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
#define EMBER_AF_PLUGIN_NETWORK_FIND_DURATION 5
#define EMBER_AF_PLUGIN_NETWORK_FIND_JOINABLE_SCAN_TIMEOUT_MINUTES 1

// Use this macro to check if Price Common plugin is included
#define EMBER_AF_PLUGIN_PRICE_COMMON

// Use this macro to check if Scan Dispatch plugin is included
#define EMBER_AF_PLUGIN_SCAN_DISPATCH
// User options for plugin Scan Dispatch
#define EMBER_AF_PLUGIN_SCAN_DISPATCH_SCAN_QUEUE_SIZE 10

// Use this macro to check if EZSP Secure Protocol Stub plugin is included
#define EMBER_AF_PLUGIN_SECURE_EZSP_STUB

// Use this macro to check if Security Support plugin is included
#define EMBER_AF_PLUGIN_SECURITY_SUPPORT

// Use this macro to check if Simple Main plugin is included
#define EMBER_AF_PLUGIN_SIMPLE_MAIN

// Use this macro to check if Trust Center Network Key Update Broadcast plugin is included
#define EMBER_AF_PLUGIN_TRUST_CENTER_NWK_KEY_UPDATE_BROADCAST

// Use this macro to check if Unix Library plugin is included
#define EMBER_AF_PLUGIN_UNIX_LIBRARY
// User options for plugin Unix Library

// Use this macro to check if Unix Printf plugin is included
#define EMBER_AF_PLUGIN_UNIX_PRINTF

// Use this macro to check if HA Device Trust Center Link Key Update plugin is included
#define EMBER_AF_PLUGIN_UPDATE_HA_TC_LINK_KEY

// Use this macro to check if ZCL Framework Core plugin is included
#define EMBER_AF_PLUGIN_ZCL_FRAMEWORK_CORE
// User options for plugin ZCL Framework Core
#define EMBER_AF_PLUGIN_ZCL_FRAMEWORK_CORE_CLI_ENABLED
#define ZA_CLI_FULL

// Use this macro to check if ZLL Commissioning Common plugin is included
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_COMMON
// User options for plugin ZLL Commissioning Common
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_COMMON_PRIMARY_CHANNEL_MASK 0x02108800
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_COMMON_SCAN_SECONDARY_CHANNELS
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_COMMON_SECONDARY_CHANNEL_MASK 0x05EF7000
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_COMMON_RADIO_TX_POWER 3
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_COMMON_ZIGBEE3_SUPPORT

// Use this macro to check if ZLL Commissioning Server plugin is included
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_SERVER
// User options for plugin ZLL Commissioning Server
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_SERVER_RX_ON_AT_STARTUP_PERIOD 300
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_SERVER_DEFAULT_RADIO_CHANNEL 11
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_SERVER_STEALING_ALLOWED
#define EMBER_AF_PLUGIN_ZLL_COMMISSIONING_SERVER_REMOTE_RESET_ALLOWED

// Use this macro to check if ZLL Identify Server plugin is included
#define EMBER_AF_PLUGIN_ZLL_IDENTIFY_SERVER
// User options for plugin ZLL Identify Server
#define EMBER_AF_PLUGIN_ZLL_IDENTIFY_SERVER_EVENT_DELAY 1024
#define EMBER_AF_PLUGIN_ZLL_IDENTIFY_SERVER_BLINK_EVENTS 2
#define EMBER_AF_PLUGIN_ZLL_IDENTIFY_SERVER_BREATHE_EVENTS 4
#define EMBER_AF_PLUGIN_ZLL_IDENTIFY_SERVER_OKAY_EVENTS 6
#define EMBER_AF_PLUGIN_ZLL_IDENTIFY_SERVER_CHANNEL_CHANGE_EVENTS 8


// Generated API headers

// API ezsp-protocol from EZSP Common plugin
#define EMBER_AF_API_EZSP_PROTOCOL "../../../protocol/zigbee/app/util/ezsp/ezsp-protocol.h"

// API ezsp from EZSP Common plugin
#define EMBER_AF_API_EZSP "../../../protocol/zigbee/app/util/ezsp/ezsp.h"

// API find-and-bind-initiator from Find and Bind Initiator plugin
#define EMBER_AF_API_FIND_AND_BIND_INITIATOR "../../../protocol/zigbee/app/framework/plugin/find-and-bind-initiator/find-and-bind-initiator.h"

// API find-and-bind-target from Find and Bind Target plugin
#define EMBER_AF_API_FIND_AND_BIND_TARGET "../../../protocol/zigbee/app/framework/plugin/find-and-bind-target/find-and-bind-target.h"

// API network-creator from Network Creator plugin
#define EMBER_AF_API_NETWORK_CREATOR "../../../protocol/zigbee/app/framework/plugin/network-creator/network-creator.h"

// API network-creator-security from Network Creator Security plugin
#define EMBER_AF_API_NETWORK_CREATOR_SECURITY "../../../protocol/zigbee/app/framework/plugin/network-creator-security/network-creator-security.h"

// API scan-dispatch from Scan Dispatch plugin
#define EMBER_AF_API_SCAN_DISPATCH "../../../protocol/zigbee/app/framework/plugin/scan-dispatch/scan-dispatch.h"

// API ezsp-secure from EZSP Secure Protocol Stub plugin
#define EMBER_AF_API_EZSP_SECURE "../../../protocol/zigbee/app/util/ezsp/secure-ezsp-protocol.h"

// API crc from Unix Library plugin
#define EMBER_AF_API_CRC "../../../platform/base/hal/micro/crc.h"

// API endian from Unix Library plugin
#define EMBER_AF_API_ENDIAN "../../../platform/base/hal/micro/endian.h"

// API hal from Unix Library plugin
#define EMBER_AF_API_HAL "../../../platform/base/hal/hal.h"

// API random from Unix Library plugin
#define EMBER_AF_API_RANDOM "../../../platform/base/hal/micro/random.h"

// API system-timer from Unix Library plugin
#define EMBER_AF_API_SYSTEM_TIMER "../../../platform/base/hal/micro/system-timer.h"

// API command-interpreter2 from ZCL Framework Core plugin
#define EMBER_AF_API_COMMAND_INTERPRETER2 "../../../protocol/zigbee/app/util/serial/command-interpreter2.h"

// API zll-profile from ZLL Commissioning Common plugin
#define EMBER_AF_API_ZLL_PROFILE "../../../protocol/zigbee/app/framework/plugin/zll-commissioning-common/zll-commissioning.h"


// Custom macros
#ifdef APP_SERIAL
#undef APP_SERIAL
#endif
#define APP_SERIAL 1

#ifdef EMBER_ASSERT_SERIAL_PORT
#undef EMBER_ASSERT_SERIAL_PORT
#endif
#define EMBER_ASSERT_SERIAL_PORT 1

#ifdef EMBER_AF_BAUD_RATE
#undef EMBER_AF_BAUD_RATE
#endif
#define EMBER_AF_BAUD_RATE 19200

#ifdef EMBER_AF_SERIAL_PORT_INIT
#undef EMBER_AF_SERIAL_PORT_INIT
#endif
#define EMBER_AF_SERIAL_PORT_INIT() \
  do { \
    emberSerialInit(1, BAUD_19200, PARITY_NONE, 1); \
  } while (0)



#endif // SILABS_ZNET_CONFIG