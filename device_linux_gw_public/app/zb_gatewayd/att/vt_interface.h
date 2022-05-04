/*
 * Copyright 2017 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __VT_INTERFACE_H__
#define __VT_INTERFACE_H__

#include "att_interface.h"

/********************
 * Gateway controls
 ********************/

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_query_info_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_configure_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_prop_set_handler(struct node *node, struct node_prop *prop,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_leave_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result));

/*
 * Restore virtual node info
 */
int vt_conf_loaded_handler(struct node *node, json_t *net_state_obj);

/*
 * Save virtual node info
 */
json_t *vt_conf_save_handler(const struct node *node);

/*
 * Initializes the virtual node platform
 */
int vt_init(void);

/*
 * Start virtual node status update.
 */
int vt_start(void);

/*
 * Cleanup on exit
 */
void vt_exit(void);

/*
 * Handle pending events
 */
void vt_poll(void);


/*
 * Add node to gateway node list.
 */
void vt_node_add(const char *mac_addr);

int  vt_set_node_data(char *id, char *name, char *value);

#endif /* __VT_INTERFACE_H__ */

