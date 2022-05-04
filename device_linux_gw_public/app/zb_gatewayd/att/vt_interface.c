/*
 * Copyright 2019 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "ember_stack_include.h"

#include "ember/af-structs.h"
#include "ember/attribute-id.h"
#include "ember/attribute-type.h"
#include "ember/att-storage.h"
#include "ember/callback.h"
#include "ember/cluster-id.h"
#include "ember/command-id.h"
#include "ember/enums.h"
#include "ember/print-cluster.h"


#include <ayla/log.h>
#include <ayla/assert.h>

#define u8 uint8_t

#include <app/props.h>
#include <ayla/gateway_interface.h>
#include <app/gateway.h>

#include "node.h"
#include "vt_interface.h"
#include "att_interface.h"
#include "att_interface_node.h"



/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_query_info_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result))
{
	ASSERT(node != NULL);
	log_info("%s: querying node info", node->addr);
	att_set_query_complete_cb(node, callback);
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_configure_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result))
{
	ASSERT(node != NULL);
	log_info("%s: configuring node", node->addr);
	att_set_config_complete_cb(node, callback);
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_prop_set_handler(struct node *node, struct node_prop *prop,
		void (*callback)(struct node *, struct node_prop *,
		enum node_network_result))
{
	uint16_t node_id;
	bool bool_value;
	int int_value;

	ASSERT(node != NULL);
	ASSERT(prop != NULL);

	if (!(node->online)) {
		log_info("node %s is offline", node->addr);
		if (callback) {
			callback(node, prop, NETWORK_OFFLINE);
		}
		return 0;
	}

	node_id = att_get_node_id(node);

	switch (prop->type) {
	case PROP_INTEGER:
		int_value = *(int *)(prop->val);
		log_debug("set node %s node_id 0x%04X int prop %s value %d",
		    node->addr, node_id, prop->name, int_value);
		break;
	case PROP_STRING:
		log_debug("set node %s node_id 0x%04X string prop %s",
		    node->addr, node_id, prop->name);
		break;
	case PROP_BOOLEAN:
		bool_value = *(bool *)(prop->val);
		log_debug("set node %s node_id 0x%04X bool prop %s, value %d",
		    node->addr, node_id, prop->name, bool_value);
		break;
	case PROP_DECIMAL:
		break;
	default:
		log_err("property type not supported: %s:%s:%s",
		    prop->subdevice->key, prop->template->key, prop->name);
		break;
	}

	if (callback) {
		callback(node, prop, NETWORK_SUCCESS);
	}
	return 0;
}


/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int vt_leave_handler(struct node *node,
		void (*callback)(struct node *, enum node_network_result))
{
	att_set_leave_complete_cb(node, callback);
	return 0;
}

/*
 * Save virtual node info
 */
json_t *vt_conf_save_handler(const struct node *node)
{
	return att_conf_save_handler(node);
}

/*
 * Restore virtual node info
 */
int vt_conf_loaded_handler(struct node *node, json_t *net_state_obj)
{
	return att_conf_loaded_handler(node, net_state_obj);
}

/*
 * Initializes the virtual node platform
 */
int vt_init(void)
{
	return 0;
}

/*
 * Start virtual node status update.
 */
int vt_start(void)
{
	/* Initialize protocol stack */
	if (vt_init() < 0) {
		log_err("vt initialization failed");
		return -1;
	}

	return 0;
}

/*
 * Cleanup on exit
 */
void vt_exit(void)
{
}

/*
 * Handle pending events
 */
void vt_poll(void)
{
}


/*
 * Add node to gateway node list.
 */
void vt_node_add(const char *mac_addr)
{
	att_node_add(mac_addr);
}

int vt_set_node_data(char *id, char *name, char *value)
{
	return att_set_node_data(id, name, value);
}
