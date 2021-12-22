/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __ZB_INTERFACE_H__
#define __ZB_INTERFACE_H__


/* XXX not declared in af.h */
/*
 * Initialize the network co-processor (NCP)
 */
void emAfResetAndInitNCP(void);


/********************
 * Gateway controls
 ********************/

/*
 * Send simple descriptor request to node
 */
int zb_send_simple_request(uint16_t node_id);

/*
 * Send power descriptor request to node
 */
int zb_send_power_request(uint16_t node_id);

/*
 * Send leave request to node
 */
int zb_send_leave_request(uint16_t node_id);

/*
 * Send IEEE address request to node
 */
int zb_send_ieee_addr_request(uint16_t node_id);

/*
 * Send power source request to node
 */
int zb_send_power_source_request(uint16_t node_id);

/*
 * Send model identifier request to node
 */
int zb_send_model_identifier_request(uint16_t node_id);

/*
 * Send read zone state request to node
 */
int zb_send_read_zone_state_request(uint16_t node_id);

/*
 * Send write CIE address request to node
 */
int zb_send_write_cie_request(uint16_t node_id);

/*
 * Send enroll response to node
 */
int zb_send_enroll_response(uint16_t node_id,
			uint8_t resp_code, uint8_t zone_id);

/*
 * Send status change notification response to node
 */
int zb_send_notification_response(uint16_t node_id);

/*
 * Send match response to node
 */
int zb_send_match_response(uint16_t node_id, uint8_t *content, uint8_t length);

/*
 * Send bind request to node
 */
int zb_send_bind_request(uint16_t node_id, uint8_t *src_eui,
			uint16_t cluster_id, uint8_t *dst_eui);

/*
 * Send unbind request to node
 */
int zb_send_unbind_request(uint16_t node_id, uint8_t *src_eui,
			uint16_t cluster_id, uint8_t *dst_eui);

/*
 * Send thermostat bind request to node
 */
int zb_thermostat_bind_request(uint16_t node_id, uint8_t *src_eui,
			uint16_t cluster_id);

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int zb_query_info_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int zb_configure_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int zb_prop_set_handler(struct node *node, struct node_prop *prop,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int zb_leave_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Restore ZigBee node info
 */
int zb_conf_loaded_handler(struct node *node, json_t *net_state_obj);

/*
 * Save ZigBee node info
 */
json_t *zb_conf_save_handler(const struct node *node);

/*
 * Initializes the zigbee platform
 */
int zb_init(void);

/*
 * Start ZigBee protocol status update.
 */
int zb_start(void);

/*
 * Cleanup on exit
 */
void zb_exit(void);

/*
 * Handle pending events
 */
void zb_poll(void);

/*
 * Permit node join network
 */
int zb_permit_join(uint8_t duration, bool broadcast);

/*
 * Gateway bind prop handler
 * cmd format: source node address,destination node address,cluster_id
 * format example: 00158D00006F95F1,00158D00006F9405,0x0006
*/
int zb_gw_bind_prop_handler(const char *cmd, char *result, int len);

#endif /* __ZB_INTERFACE_H__ */

