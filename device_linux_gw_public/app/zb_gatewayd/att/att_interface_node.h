/*
 * Copyright 2019 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __ATT_INTERFACE_NODE_H__
#define __ATT_INTERFACE_NODE_H__

/*
 * Get node_id by the node addr.
 */
uint16_t att_get_node_id(struct node *zb_node);

/*
 * Set query handle complete callback
 */
void att_set_query_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Set config handle complete callback
 */
void att_set_config_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Set prop set handle complete callback
 */
/*void att_set_prop_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result));*/

/*
 * Set leave handle complete callback
 */
void att_set_leave_complete_cb(struct node *zb_node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Appd convert node info to json object when save node info to config file
 */
json_t *att_conf_save_handler(const struct node *zb_node);

/*
 * Appd get node info from json object when restore node info from config file
 */
int att_conf_loaded_handler(struct node *zb_node, json_t *info_obj);


#endif /* __ATT_INTERFACE_NODE_H__ */

