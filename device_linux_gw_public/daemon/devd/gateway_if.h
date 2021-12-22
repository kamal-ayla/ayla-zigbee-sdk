/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_GATEWAY_IF_H__
#define __AYLA_GATEWAY_IF_H__

/*
 * Send a GATEWAY nak
 */
void gateway_send_nak(const char *err, int id);

/*
 * Send a GATEWAY ACK
 */
void gateway_send_ack(int id);

/*
 * Handle appd's response to a reach status request
 */
void gateway_handle_conn_status_resp(json_t *cmd, int req_id);

/*
 * Send request to appd for connection status of nodes
 */
void gateway_conn_status_get(struct server_req *req);

/*
 * Send request to appd for property value of a node
 */
void gateway_node_property_get(struct server_req *req);

/*
 * Handle appd's response to node property request
 */
void gateway_handle_property_resp(json_t *cmd, int req_id);

/*
 * Node property updates from Cloud/LAN
 */
void gateway_process_node_update(struct device_state *dev, json_t *update_j,
	int source);

/*
 * Send response to a GET node prop request to appd
 */
void gateway_send_prop_resp(void *arg, json_t *props);

/*
 * Node schedules from cloud/LAN
 */
void gateway_process_node_scheds(struct device_state *dev, json_t *scheds_j,
	int source);

/*
 * Process Node Factory Reset Command
 */
void gateway_reset_node_put(struct server_req *req);

/*
 * Process Node OTA Command
 */
void gateway_node_ota_put(struct server_req *req);

/*
 * Process Node Register Status Change Command
 */
void gateway_node_reg_put(struct server_req *req, const char *dsn);

#endif /*  __AYLA_GATEWAY_IF_H__ */

