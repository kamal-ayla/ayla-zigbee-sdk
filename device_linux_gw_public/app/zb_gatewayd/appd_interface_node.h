/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __APPD_INTERFACE_NODE_H__
#define __APPD_INTERFACE_NODE_H__



/*
 * Init appd interface.
 */
void appd_interface_init(void);

/*
 *  Appd interface exit.
 */
void appd_interface_exit(void);

/*
 * Get node_id by the node addr.
 */
uint16_t appd_get_node_id(struct node *zb_node);

/*
 * Set query handle complete callback
 */
void appd_set_query_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Set config handle complete callback
 */
void appd_set_config_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Set prop set handle complete callback
 */
void appd_set_prop_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result));

/*
 * Set leave handle complete callback
 */
void appd_set_leave_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Appd add node info when restore node from config file
 *//*
int appd_add_node_info(struct node *zb_node,
			uint8_t *node_eui, uint16_t node_id);*/

/*
 * Appd convert node info to json object when save node info to config file
 */
json_t *appd_conf_save_handler(const struct node *zb_node);

/*
 * Appd get node info from json object when restore node info from config file
 */
int appd_conf_loaded_handler(struct node *zb_node, json_t *info_obj);

/*
 * Update all nodes state
 */
void appd_update_all_node_state(void);

/*
 * Gateway bind prop handler
 * cmd format 1: bind,source node address,destination node address,cluster_id
 * cmd format 2: unbind,source node address,destination node address,cluster_id
 * format example 1: bind,00158D00006F95F1,00158D00006F9405,0x0006
 * format example 2: unbind,00158D00006F95F1,00158D00006F9405,0x0006
*/
int appd_gw_bind_prop_handler(const char *cmd, char *result, int len);

#endif /* __APPD_INTERFACE_NODE_H__ */

