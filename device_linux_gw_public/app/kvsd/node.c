/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/queue.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/timer.h>
#include <ayla/hashmap.h>
#include <ayla/nameval.h>
#include <ayla/conf_io.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/gateway_interface.h>

#include <app/ops.h>
#include <app/props.h>
#include <app/gateway.h>

#include "gateway.h"
#include "node.h"

/* Retry delay if a node management operation failed */
#define NODE_STEP_OP_RETRY_MS		60000

DEF_NAME_TABLE(node_state_names, NODE_STATES);
DEF_NAMEVAL_TABLE(node_state_table, NODE_STATES);

DEF_NAME_TABLE(node_network_result_names, NODE_NETWORK_RESULTS);

/*
 * State used to pass a node_foreach handler to the appropriate callback.
 */
struct node_foreach_state {
	int (*func)(struct node *, void *);
	void *arg;
};

/*
 * Slots used to associate additional state with a node.
 */
struct state_slot {
	void *ptr;
	void (*cleanup)(void *);
};

/*
 * Structure with a node subdevice and internal management state.
 */
struct node_subdevice_entry {
	struct node_subdevice subdevice;
	SLIST_HEAD(, node_template_entry) templates;
	SLIST_ENTRY(node_subdevice_entry) list_entry;
};

/*
 * Structure with a node template and internal management state.
 */
struct node_template_entry {
	struct node_template template;
	SLIST_HEAD(, node_prop_entry) props;
	SLIST_ENTRY(node_template_entry) list_entry;
};

/*
 * Structure with a node property and internal management state.
 */
struct node_prop_entry {
	struct node_prop prop;		/* Node property state */
	bool val_synced;		/* Gateway and cloud are in sync */
	bool retry_send;		/* Resend datapoint to cloud */
	bool retry_set;			/* Resend datapoint to network device */
	struct state_slot state[STATE_SLOT_COUNT];	/* Prop state slots */
	SLIST_ENTRY(node_prop_entry) list_entry;
};

/*
 * Structure with a generic node and internal management state.
 */
struct node_entry {
	struct node node;		/* Node state */
	enum node_state state;		/* Node management state */
	bool op_pending;		/* Node management op is in progress */

	bool update;			/* Node info has been updated */
	bool remove;			/* Cloud removed node */
	bool factory_reset;		/* Cloud requested node factory reset */
	bool reconfigure;		/* Node setup has changed */
	bool left;			/* Node left the network */

	bool retry_send_conn_status;	/* Schedule online status resend */
	bool retry_send_props;		/* Schedule node property resend */
	bool retry_set_props;		/* Schedule node property set retry */

	unsigned batch_prop_sends;	/* Batch outgoing prop values */

	struct timer step_timer;	/* Schedule next management step */
	struct state_slot slots[STATE_SLOT_COUNT];	/* Node state slots */
	SLIST_HEAD(, node_subdevice_entry) subdevices;	/* Node property tree */
};

/*
 * Generic node management state.
 */
struct node_mgmt_state {
	struct hashmap nodes;
	struct node_cloud_callbacks cloud;
	struct node_network_callbacks network;
	struct timer_head *timers;
};

static struct node_mgmt_state mgmt_state;

/* Generate type-specific hashmap functions for node entries */
HASHMAP_FUNCS_CREATE(node_entry, const char, struct node_entry)

/*
 * Forward declarations
 */
static void node_step_timeout(struct timer *timer);
static void node_step(struct node_entry *entry);


/*
 * Cleanup and clear a state slot.
 */
static void node_state_slot_clear(struct state_slot *slot)
{
	if (slot->cleanup) {
		slot->cleanup(slot->ptr);
		slot->cleanup = NULL;
	}
	slot->ptr = NULL;
}

/*
 * Cleanup and clear all available state slots.
 */
static void node_state_slot_clear_all(struct state_slot *slots)
{
	unsigned i;

	for (i = 0; i < STATE_SLOT_COUNT; ++i) {
		node_state_slot_clear(slots + i);
	}
}

/*
 * Assign a state pointer and optional cleanup callback to a state slot.
 */
static void node_state_slot_set(struct state_slot *slot,
	void *ptr, void (*cleanup_func)(void *))
{
	node_state_slot_clear(slot);
	slot->ptr = ptr;
	slot->cleanup = cleanup_func;
}

/*
 * Allocate a data buffer for a node property.
 */
static void *node_prop_val_alloc(struct node_prop *prop)
{
	if (prop->val) {
		log_warn("prop already allocated");
		return prop->val;
	}
	switch (prop->type) {
	case PROP_INTEGER:
		prop->val_size = sizeof(int);
		prop->val = calloc(1, prop->val_size);
		break;
	case PROP_STRING:
		prop->val_size = PROP_STRING_LEN + 1;
		prop->val = calloc(prop->val_size, sizeof(char));
		break;
	case PROP_BOOLEAN:
		prop->val_size = sizeof(bool);
		prop->val = calloc(1, prop->val_size);
		break;
	case PROP_DECIMAL:
		prop->val_size = sizeof(double);
		prop->val = calloc(1, prop->val_size);
		break;
	default:
		log_err("unsupported prop type");
		prop->val_size = 0;
		return NULL;
	}
	if (!prop->val) {
		log_err("malloc failed");
		prop->val_size = 0;
		return NULL;
	}
	return prop->val;
}

static void node_prop_entry_cleanup(struct node_prop_entry *entry)
{
	node_state_slot_clear_all(entry->state);
	free(entry->prop.val);
}

static void node_entry_init(struct node_entry *entry, const char *addr,
	const char *model, enum gw_interface interface, enum gw_power power,
	const char *version)
{
	struct node *node = &entry->node;

	/* Initialize public node structure */
	snprintf(node->addr, sizeof(node->addr), "%s", addr);
	snprintf(node->oem_model, sizeof(node->oem_model), "%s", model);
	if (version) {
		snprintf(node->version, sizeof(node->version), "%s", version);
	}
	node->interface = interface;
	node->power = power;
	/* Initialize timer for node state transitions */
	timer_init(&entry->step_timer, node_step_timeout);
	/* Initialize property tree */
	SLIST_INIT(&entry->subdevices);
}

static void node_entry_cleanup(struct node_entry *entry)
{
	struct node_subdevice_entry *s;
	struct node_template_entry *t;
	struct node_prop_entry *p;

	/* Free allocated property data */
	while ((s = SLIST_FIRST(&entry->subdevices)) != NULL) {
		SLIST_REMOVE_HEAD(&entry->subdevices, list_entry);
		while ((t = SLIST_FIRST(&s->templates)) != NULL) {
			SLIST_REMOVE_HEAD(&s->templates, list_entry);
			while ((p = SLIST_FIRST(&t->props)) != NULL) {
				SLIST_REMOVE_HEAD(&t->props, list_entry);
				node_prop_entry_cleanup(p);
				free(p);
			}
			free(t);
		}
		free(s);
	}
}

/*
 * Hashmap iterator callback to invoke the node_foreach handler
 */
static int node_foreach_callback(const char *addr, struct node_entry *entry,
	void *arg)
{
	struct node_foreach_state *s = (struct node_foreach_state *)arg;

	return s->func(&entry->node, s->arg);
}

/*
 * Create a node and add it to the managed node list.
 */
static struct node_entry *node_entry_add(const char *addr, const char *model,
	enum gw_interface interface, enum gw_power power, const char *version)
{
	struct node_entry *entry;
	struct node_entry *entry_data;

	/* Create new node entry */
	entry = (struct node_entry *)calloc(1, sizeof(*entry));
	if (!entry) {
		log_err("malloc failed");
		return NULL;
	}
	node_entry_init(entry, addr, model, interface, power, version);
	/* Insert node entry into managed node map */
	entry_data = node_entry_hashmap_put(&mgmt_state.nodes,
	     entry->node.addr, entry);
	if (entry_data != entry) {
		log_err("cannot add duplicate node: %s", addr);
		node_entry_cleanup(entry);
		free(entry);
		return NULL;
	}
	return entry;
}

/*
 * Remove a node from the managed node list and free its resources.
 */
static void node_entry_delete(struct node_entry *entry)
{
	timer_cancel(mgmt_state.timers, &entry->step_timer);
	node_entry_hashmap_remove(&mgmt_state.nodes, entry->node.addr);
	while (entry->batch_prop_sends > 0) {
		/* Send any batched datapoints before deleting node */
		node_prop_batch_end(&entry->node);
	}
	node_state_slot_clear_all(entry->slots);
	node_entry_cleanup(entry);
	free(entry);
}

/*
 * Hashmap foreach function for removing all nodes.
 */
static int node_entry_delete_foreach(const char *addr, struct node_entry *entry,
	void *arg)
{
	node_entry_delete(entry);
	return 0;
}

/*
 * Schedule the node management state machine to run with a delay.
 */
static void node_step_delayed(struct node_entry *entry, u32 delay_ms)
{
	u64 cur_delay = timer_delay_get_ms(&entry->step_timer);

	if (!cur_delay || cur_delay > delay_ms) {
		timer_set(mgmt_state.timers, &entry->step_timer, delay_ms);
	}
}

/*
 * Run the node management state machine as soon as possible.
 */
static void node_step(struct node_entry *entry)
{
	timer_set(mgmt_state.timers, &entry->step_timer, 0);
}

/*
 * Next state logic for node management state machine.
 */
static void node_op_complete(struct node_entry *entry)
{
	enum node_state last_state = entry->state;

	if (!entry->op_pending) {
		log_err("%s: no operation in progress", entry->node.addr);
		return;
	}
	switch (entry->state) {
	case NODE_NET_QUERY:
		if (entry->left) {
			/* Skip net operation if node left network */
			entry->state = NODE_REMOVED;
			break;
		}
		entry->state = NODE_CLOUD_ADD;
		break;
	case NODE_NET_CONFIGURE:
	case NODE_NET_FACTORY_RESET:
	case NODE_CLOUD_UPDATE:
		entry->state = NODE_READY;
		break;
	case NODE_CLOUD_ADD:
		if (entry->left) {
			/* Skip net operation if node left network */
			entry->state = NODE_CLOUD_REMOVE;
			break;
		}
		entry->state = NODE_NET_CONFIGURE;
		break;
	case NODE_NET_REMOVE:
	case NODE_CLOUD_REMOVE:
		entry->state = NODE_REMOVED;
		break;
	default:
		ASSERT_NOTREACHED();
	}
	log_debug("%s: state change %s --> %s", entry->node.addr,
	    node_state_names[last_state], node_state_names[entry->state]);
	entry->op_pending = false;
	conf_save();	/* Save management state to config */
	node_step(entry);
}

/*
 * Management operation failed; try again later.
 */
static void node_op_retry(struct node_entry *entry)
{
	if (!entry->op_pending) {
		log_err("%s: no operation in progress", entry->node.addr);
		return;
	}
	entry->op_pending = false;
	node_step_delayed(entry, NODE_STEP_OP_RETRY_MS);
}

/*
 * Cloud operation confirmation callback.
 */
static void node_cloud_op_complete(struct node *node,
	const struct confirm_info *info)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	if (!info) {
		log_warn("%s: operation %s not handled", node->addr,
		    node_state_names[entry->state]);
		node_op_complete(entry);
		return;
	}
	switch (info->err) {
	case CONF_ERR_NONE:
		node_op_complete(entry);
		break;
	case CONF_ERR_CONN:
		log_err("%s: connectivity error on %s",
		    node->addr, node_state_names[entry->state]);
		node_op_retry(entry);
		break;
	case CONF_ERR_APP:
		log_err("%s: application error on %s",
		    node->addr, node_state_names[entry->state]);
		node_op_complete(entry);	/* Don't retry */
		break;
	case CONF_ERR_UNKWN:
		log_err("%s: unknown error on %s",
		    node->addr, node_state_names[entry->state]);
		node_op_complete(entry);	/* Don't retry */
		if (entry->state == NODE_CLOUD_ADD) {
			/*
			 * Remove node if unrecoverable error returned on
			 * cloud add, because node will be non-functional.
			 */
			node_remove(node);
		}
		break;
	}
}

/*
 * Network interface operation callback.
 */
static void node_network_op_complete(struct node *node,
	enum node_network_result result)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	switch (result) {
	case NETWORK_UNSUPPORTED:
		log_warn("%s: %s skipped due to %s status",
		    node->addr, node_state_names[entry->state],
		    node_network_result_names[result]);
		    /* no break */
	case NETWORK_SUCCESS:
		node_op_complete(entry);
		break;
	case NETWORK_OFFLINE:
		log_debug("%s: %s deferred due to %s status",
		    node->addr, node_state_names[entry->state],
		    node_network_result_names[result]);
		node_op_retry(entry);
		break;
	case NETWORK_UNKNOWN:
		log_err("%s: %s failed with %s status",
		    node->addr, node_state_names[entry->state],
		    node_network_result_names[result]);
		/*
		 * Network stack indicated the node is non-existent,
		 * so assume it left the network.
		 */
		node_op_complete(entry);
		node_left(node);
		break;
	}
}

/*
 * Connection status send confirmation callback.
 */
static void node_send_conn_status_complete(struct node *node,
	const struct confirm_info *info)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	if (!info) {
		log_warn("%s: conn_status send not handled", node->addr);
		return;
	}
	if (info->err == CONF_ERR_NONE) {
		return;
	}
	if (info->err == CONF_ERR_CONN) {
		log_warn("%s: sending conn_status failed due to connectivity "
		    "error", node->addr);
		entry->retry_send_conn_status = true;
		node_step_delayed(entry, NODE_STEP_OP_RETRY_MS);
		return;
	}
	log_err("%s: sending conn_status failed", node->addr);
}

/*
 * Send connection status to the cloud.
 */
static int node_conn_status_send(struct node_entry *entry)
{
	if (!mgmt_state.cloud.node_conn_status) {
		node_send_conn_status_complete(&entry->node, NULL);
		return -1;
	}
	if (entry->state != NODE_READY) {
		/* Defer send until node management operation completes */
		entry->retry_send_conn_status = true;
		return 0;
	}
	if (mgmt_state.cloud.node_conn_status(&entry->node,
	    node_send_conn_status_complete) < 0) {
		/* Failed to initiate send, try later */
		entry->retry_send_conn_status = true;
		node_step_delayed(entry, NODE_STEP_OP_RETRY_MS);
		return -1;
	}
	entry->retry_send_conn_status = false;
	return 0;
}

/*
 * Property send confirmation callback.  A retry send flag is set if the send
 * failed due to a connection error.
 */
static void node_prop_send_complete(struct node *node, struct node_prop *prop,
	const struct confirm_info *info)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	if (!info) {
		log_warn("%s: prop %s send not handled", node->addr,
		    prop->name);
		return;
	}
	if (info->err == CONF_ERR_NONE) {
		return;
	}
	if (info->err == CONF_ERR_CONN) {
		log_warn("%s: sending prop %s failed due to connectivity error",
		    node->addr, prop->name);
		entry->retry_send_props = true;
		prop_entry->retry_send = true;
		node_step_delayed(entry, NODE_STEP_OP_RETRY_MS);
		return;
	}
	log_err("%s: sending prop %s failed", node->addr, prop->name);
}

/*
 * Send (or batch) a node property.  If a filter was supplied as the optional
 * argument, only send if the filter returned true.  This function is meant
 * to be used by node_prop_foreach().
 */
static int node_prop_send_filtered(struct node *node, struct node_prop *prop,
	void *arg)
{
	bool (*filter)(const struct node_prop_entry *) = arg;
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	/* Apply optional send filter */
	if (filter && !filter(prop_entry)) {
		return 0;
	}
	return node_prop_send(node, prop);
}

/*
 * Filter to be used with a node_prop_send_filtered() to select
 * only properties that failed to send.
 */
static bool node_prop_filter_retry_send(
	const struct node_prop_entry *prop_entry)
{
	return prop_entry->retry_send;
}

/*
 * Send all node properties with a retry_send flag set.  This function should
 * ONLY be called if the node indicated there were properties to be re-sent.
 */
static int node_prop_retry_send_all(struct node_entry *entry)
{
	int rc;

	ASSERT(entry->retry_send_props == true);

	if (!mgmt_state.cloud.node_prop_send) {
		log_warn("%s: prop send not handled", entry->node.addr);
		return -1;
	}
	entry->retry_send_props = false;
	/* Perform resend in a single batch */
	node_prop_batch_begin(&entry->node);
	rc = node_prop_foreach(&entry->node, node_prop_send_filtered,
	    node_prop_filter_retry_send);
	node_prop_batch_end(&entry->node);
	if (rc < 0) {
		node_step_delayed(entry, NODE_STEP_OP_RETRY_MS);
		return -1;
	}
	return 0;
}

/*
 * Network interface property set operation callback.  A retry set flag is set
 * if the set failed due to the node being offline.
 */
static void node_prop_set_complete(struct node *node, struct node_prop *prop,
	enum node_network_result result)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	switch (result) {
	case NETWORK_SUCCESS:
		prop_entry->retry_set = false;
		break;
	case NETWORK_OFFLINE:
		/* Schedule retry when node comes online */
		entry->retry_set_props = true;
		prop_entry->retry_set = true;
		log_debug("%s: property %s set deferred, node %s",
		    node->addr, prop->name, node_network_result_names[result]);
		break;
	case NETWORK_UNKNOWN:
	case NETWORK_UNSUPPORTED:
		log_warn("%s: property %s set failed with %s status",
		    node->addr, prop->name, node_network_result_names[result]);
		break;
	}
}

/*
 * Set a node property value on the network device.  If a filter was supplied
 * as the optional argument, only set if the filter returned true.  This
 * function is meant to be used by node_prop_foreach().
 */
static int node_prop_set_network_filtered(struct node *node,
	struct node_prop *prop, void *arg)
{
	bool (*filter)(const struct node_prop *) = arg;

	/* Apply optional send filter */
	if (filter && !filter(prop)) {
		return 0;
	}
	if (!mgmt_state.network.node_prop_set) {
		return 0;
	}
	/* Push the latest property value to the network interface */
	if (mgmt_state.network.node_prop_set(node, prop,
	    node_prop_set_complete) < 0) {
		node_prop_set_complete(node, prop, NETWORK_UNSUPPORTED);
		return -1;
	}
	return 0;
}

/*
 * Filter to be used with a node_prop_set_filtered() to select
 * only properties that failed to set.
 */
static bool node_prop_filter_retry_set(const struct node_prop_entry *prop_entry)
{
	return prop_entry->retry_set;
}

/*
 * Set all node properties with a retry_set flag set.  This function should
 * ONLY be called if the node indicated there were properties to be re-set.
 */
static int node_prop_retry_set_network_all(struct node_entry *entry)
{
	ASSERT(entry->retry_set_props == true);

	if (!mgmt_state.network.node_prop_set) {
		log_warn("%s: prop set not handled", entry->node.addr);
		return -1;
	}
	entry->retry_set_props = false;
	return node_prop_foreach(&entry->node, node_prop_set_network_filtered,
	    node_prop_filter_retry_set);
}

static void node_network_query(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_NET_QUERY);

	entry->op_pending = true;

	if (!mgmt_state.network.node_query_info ||
	    mgmt_state.network.node_query_info(&entry->node,
	    node_network_op_complete) < 0) {
		node_network_op_complete(&entry->node, NETWORK_UNSUPPORTED);
	}
}

static void node_network_configure(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_NET_CONFIGURE);

	entry->op_pending = true;

	if (!mgmt_state.network.node_configure ||
	    mgmt_state.network.node_configure(&entry->node,
	    node_network_op_complete) < 0) {
		node_network_op_complete(&entry->node, NETWORK_UNSUPPORTED);
	}
}

static void node_network_factory_reset(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_NET_FACTORY_RESET);

	entry->op_pending = true;

	if (!mgmt_state.network.node_factory_reset ||
	    mgmt_state.network.node_factory_reset(&entry->node,
	    node_network_op_complete) < 0) {
		node_network_op_complete(&entry->node, NETWORK_UNSUPPORTED);
	}
}

static void node_network_remove(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_NET_REMOVE);

	entry->op_pending = true;

	if (!mgmt_state.network.node_leave ||
	    mgmt_state.network.node_leave(&entry->node,
	    node_network_op_complete) < 0) {
		node_network_op_complete(&entry->node,
		    NETWORK_UNSUPPORTED);
	}
}

static void node_cloud_add(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_CLOUD_ADD);

	entry->op_pending = true;

	if (!mgmt_state.cloud.node_add ||
	    mgmt_state.cloud.node_add(&entry->node,
	    node_cloud_op_complete) < 0) {
		node_cloud_op_complete(&entry->node, NULL);
	}
}

static void node_cloud_remove(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_CLOUD_REMOVE);

	entry->op_pending = true;

	if (!mgmt_state.cloud.node_remove ||
	    mgmt_state.cloud.node_remove(&entry->node,
	    node_cloud_op_complete) < 0) {
		node_cloud_op_complete(&entry->node, NULL);
	}
}

static void node_cloud_update(struct node_entry *entry)
{
	ASSERT(!entry->op_pending);
	ASSERT(entry->state == NODE_CLOUD_UPDATE);

	entry->op_pending = true;

	if (!mgmt_state.cloud.node_update_info ||
	    mgmt_state.cloud.node_update_info(&entry->node,
	    node_cloud_op_complete) < 0) {
		node_cloud_op_complete(&entry->node, NULL);
	}
}

/*
 * Execute node management action(s) based on the current node state.
 */
static void node_step_handler(struct node_entry *entry)
{
	enum node_state state;

	/* Only initiate a new node action if no node ops in progress */
	if (entry->op_pending) {
		return;
	}
	do {
		state = entry->state;
		switch (entry->state) {
		case NODE_JOINED:
			if (entry->left) {
				/* Skip join flow if node immediately left */
				entry->state = NODE_REMOVED;
				break;
			}
			/* Start node management sequence */
			entry->state = NODE_NET_QUERY;
			/* Persist the node in gateway config */
			conf_save();
			break;
		case NODE_REMOVED:
			/* All ops relating to node deletion have completed */
			log_debug("%s: deleted", entry->node.addr);
			node_entry_delete(entry);
			/* Update gateway config */
			conf_save();
			return;
		case NODE_READY:
			timer_cancel(mgmt_state.timers, &entry->step_timer);
			/* Check for event flags requiring an action */
			if (entry->left) {
				entry->left = false;
				entry->state = NODE_CLOUD_REMOVE;
				break;
			}
			if (entry->factory_reset) {
				entry->factory_reset = false;
				entry->state = NODE_NET_FACTORY_RESET;
				break;
			}
			if (entry->remove) {
				entry->remove = false;
				entry->state = NODE_NET_REMOVE;
				break;
			}
			if (entry->update) {
				entry->update = false;
				entry->state = NODE_CLOUD_UPDATE;
				break;
			}
			if (entry->reconfigure) {
				entry->reconfigure = false;
				entry->state = NODE_NET_CONFIGURE;
			}
			break;
		case NODE_NET_QUERY:
			/* Ask network stack to populate node info and props */
			log_debug("%s: network query", entry->node.addr);
			node_network_query(entry);
			break;
		case NODE_NET_CONFIGURE:
			/* Clear reconfigure flag if set before first config */
			entry->reconfigure = false;
			/* Configure node to monitor relevant props */
			log_debug("%s: network configure", entry->node.addr);
			node_network_configure(entry);
			break;
		case NODE_NET_FACTORY_RESET:
			/* Reset node to defaults */
			log_debug("%s: network factory reset",
			    entry->node.addr);
			node_network_factory_reset(entry);
			break;
		case NODE_NET_REMOVE:
			/* Ask network stack to disconnect the node */
			log_debug("%s: network leave", entry->node.addr);
			node_network_remove(entry);
			break;
		case NODE_CLOUD_ADD:
			/* Push node info to the cloud */
			log_debug("%s: cloud add", entry->node.addr);
			node_cloud_add(entry);
			break;
		case NODE_CLOUD_REMOVE:
			/* Indicate to the cloud that node left the network */
			log_debug("%s: cloud remove", entry->node.addr);
			node_cloud_remove(entry);
			break;
		case NODE_CLOUD_UPDATE:
			/* Push updated node info to the cloud */
			log_debug("%s: cloud update", entry->node.addr);
			node_cloud_update(entry);
			break;
		}
	} while (state != entry->state);
	/*
	 * Connection status and prop resends may be run concurrently with
	 * the above management operations, but should not be started until
	 * all management tasks are complete.
	 */
	if (entry->state != NODE_READY) {
		return;
	}
	if (entry->retry_send_conn_status) {
		log_debug("%s: cloud retry send conn status", entry->node.addr);
		node_conn_status_send(entry);
	}
	if (entry->retry_send_props) {
		log_debug("%s: cloud retry send props", entry->node.addr);
		node_prop_retry_send_all(entry);
	}
	if (entry->retry_set_props && entry->node.online) {
		log_debug("%s: network retry set props", entry->node.addr);
		node_prop_retry_set_network_all(entry);
	}
}

static void node_step_timeout(struct timer *timer)
{
	struct node_entry *entry =
	    CONTAINER_OF(struct node_entry, step_timer, timer);

	node_step_handler(entry);
}

static int node_step_foreach_handler(const char *addr, struct node_entry *entry,
	void *arg)
{
	/* Cancel the step timer since the step handler is called directly */
	timer_cancel(mgmt_state.timers, &entry->step_timer);
	node_step_handler(entry);
	return 0;
}

/*
 * Helper function to parse a JSON structure representing a generic node's
 * state and add the managed node list.
 */
static int node_conf_set_node(json_t *node_obj)
{
	json_t *obj;
	json_t *subdevice_arr;
	json_t *template_arr;
	json_t *props_arr;
	json_t *subdevice_obj;
	json_t *template_obj;
	json_t *props_obj;
	json_t *net_state_obj;
	json_t *cloud_state_obj;
	int i, j, k;
	struct node_entry *entry = NULL;
	struct node_prop *prop;
	const char *str;
	int num;
	const char *addr;
	const char *version;
	const char *oem_model;
	const char *subdevice_key;
	const char *template_key;
	const char *template_version;
	struct node_prop_def prop_def;
	enum gw_interface interface;
	enum gw_power power;

	addr = json_get_string(node_obj, "address");
	if (!addr) {
		log_err("node does not have valid address");
		goto invalid;
	}
	/*
	 * Do not reload an existing node.
	 * Assume the loaded version is up to date.
	 */
	if (node_lookup(addr)) {
		return 0;
	}
	version = json_get_string(node_obj, "version");
	oem_model = json_get_string(node_obj, "oem_model");
	obj = json_object_get(node_obj, "interface");
	interface = json_integer_value(obj);
	obj = json_object_get(node_obj, "power");
	power = json_integer_value(obj);

	entry = node_entry_add(addr, oem_model, interface, power, version);
	if (!entry) {
		goto invalid;
	}
	str = json_get_string(node_obj, "management_state");
	if (str) {
		num = lookup_by_name(node_state_table, str);
		if (num == -1) {
			log_warn("%s: invalid management state: %s", addr, str);
			entry->state = NODE_READY;
		} else {
			entry->state = num;
		}
	} else {
		entry->state = NODE_READY;
	}
	net_state_obj = json_object_get(node_obj, "network_state");
	cloud_state_obj = json_object_get(node_obj, "cloud_state");
	log_debug("%s: loaded, state %s", addr, node_state_names[entry->state]);
	subdevice_arr = json_object_get(node_obj, "subdevices");
	if (!json_is_array(subdevice_arr)) {
		log_err("%s: missing subdevices array", addr);
		goto invalid;
	}
	json_array_foreach(subdevice_arr, i, subdevice_obj) {
		subdevice_key = json_get_string(subdevice_obj, "key");
		if (!subdevice_key) {
			log_err("%s: missing subdevice key", addr);
			continue;
		}
		template_arr = json_object_get(subdevice_obj, "templates");
		if (!json_is_array(template_arr)) {
			log_err("%s: missing template array", addr);
			continue;
		}
		json_array_foreach(template_arr, j, template_obj) {
			template_key = json_get_string(template_obj, "key");
			if (!template_key) {
				log_err("%s: missing template key", addr);
				continue;
			}
			props_arr = json_object_get(template_obj, "properties");
			if (!json_is_array(props_arr)) {
				log_err("%s: missing properties array", addr);
				continue;
			}
			template_version = json_get_string(template_obj,
			    "version");
			json_array_foreach(props_arr, k, props_obj) {
				prop_def.name = json_get_string(props_obj,
				    "name");
				if (!prop_def.name) {
					log_err("%s: invalid prop name", addr);
					continue;
				}
				obj = json_object_get(props_obj, "type");
				if (!json_is_integer(obj)) {
					log_err("%s: invalid prop type", addr);
					continue;
				}
				prop_def.type = json_integer_value(obj);
				obj = json_object_get(props_obj, "from-device");
				prop_def.dir = json_boolean_value(obj) ?
				    PROP_FROM_DEVICE : PROP_TO_DEVICE;
				prop = node_prop_add(&entry->node,
				    subdevice_key, template_key, &prop_def,
				    template_version);
				if (!prop) {
					continue;
				}
				log_debug2("%s: loaded property %s:%s:%s", addr,
				    subdevice_key, template_key, prop_def.name);
			}
		}
	}
	/* Indicate to the network interface that a node has been restored */
	if (mgmt_state.network.node_conf_loaded) {
		mgmt_state.network.node_conf_loaded(&entry->node,
		    net_state_obj);
	}
	/* Indicate to the cloud interface that a node has been restored */
	if (mgmt_state.cloud.node_conf_loaded) {
		mgmt_state.cloud.node_conf_loaded(&entry->node,
		    cloud_state_obj);
	}
	/* Start the node management state machine */
	node_step(entry);
	return 0;
invalid:
	if (entry) {
		node_entry_delete(entry);
	}
	return -1;
}

/*
 * Helper function to populate a JSON structure representing a generic node's
 * state.  A JSON array is passed in via the arg parameter.  When this
 * JSON structure is passed to node_conf_set_node, the node info, property
 * tree, and management state should be restored to the identical state.
 */
static int node_conf_get_node(struct node *node, void *arg)
{
	json_t *nodes_arr = (json_t *)arg;
	json_t *node_obj;
	json_t *subdevice_obj;
	json_t *template_obj;
	json_t *prop_obj;
	json_t *subdevice_arr;
	json_t *template_arr;
	json_t *props_arr;
	json_t *cloud_state_obj = NULL;
	json_t *net_state_obj = NULL;
	struct node_entry *n = CONTAINER_OF(struct node_entry, node, node);
	struct node_subdevice_entry *s;
	struct node_template_entry *t;
	struct node_prop_entry *p;

	/* Request state to persist from the cloud interface */
	if (mgmt_state.cloud.node_conf_save) {
		cloud_state_obj = mgmt_state.cloud.node_conf_save(node);
	}
	/* Request state to persist from the network interface */
	if (mgmt_state.network.node_conf_save) {
		net_state_obj = mgmt_state.network.node_conf_save(node);
	}

	node_obj = json_object();
	subdevice_arr = json_array();
	json_array_append_new(nodes_arr, node_obj);
	json_object_set_new(node_obj, "address", json_string(node->addr));
	json_object_set_new(node_obj, "version", json_string(node->version));
	json_object_set_new(node_obj, "oem_model",
	    json_string(node->oem_model));
	json_object_set_new(node_obj, "interface",
	    json_integer(node->interface));
	json_object_set_new(node_obj, "power", json_integer(node->power));
	json_object_set_new(node_obj, "management_state",
	    json_string(lookup_by_val(node_state_table, n->state)));
	if (cloud_state_obj) {
		json_object_set_new(node_obj, "cloud_state", cloud_state_obj);
	}
	if (net_state_obj) {
		json_object_set_new(node_obj, "network_state", net_state_obj);
	}
	json_object_set_new(node_obj, "subdevices", subdevice_arr);

	/* Populate JSON structure from the node's property tree */
	SLIST_FOREACH(s, &n->subdevices, list_entry) {
		subdevice_obj = json_object();
		template_arr = json_array();
		json_array_append_new(subdevice_arr, subdevice_obj);
		json_object_set_new(subdevice_obj, "key",
		    json_string(s->subdevice.key));
		json_object_set_new(subdevice_obj, "templates", template_arr);
		SLIST_FOREACH(t, &s->templates, list_entry) {
			template_obj = json_object();
			props_arr = json_array();
			json_array_append_new(template_arr, template_obj);
			json_object_set_new(template_obj, "key",
			    json_string(t->template.key));
			if (t->template.version[0]) {
				json_object_set_new(template_obj, "version",
				    json_string(t->template.version));
			}
			json_object_set_new(template_obj, "properties",
			    props_arr);
			SLIST_FOREACH(p, &t->props, list_entry) {
				prop_obj = json_object();
				json_array_append_new(props_arr, prop_obj);
				json_object_set_new(prop_obj, "name",
				    json_string(p->prop.name));
				json_object_set_new(prop_obj, "type",
				    json_integer(p->prop.type));
				json_object_set_new(prop_obj, "from-device",
				    json_boolean(p->prop.dir ==
				    PROP_FROM_DEVICE));
			}
		}
	}
	return 0;
}

/*
 * Nodes config set handler.  This is invoked any time conf_load() is called.
 * Load the nodes config JSON array and restore all saved nodes.
 */
static int node_conf_set(json_t *nodes_arr)
{
	size_t i;
	json_t *node_obj;

	if (!json_is_array(nodes_arr)) {
		return -1;
	}
	json_array_foreach(nodes_arr, i, node_obj) {
		node_conf_set_node(node_obj);
	}
	return 0;
}

/*
 * Nodes config get handler.  This is invoked any time conf_save() is called.
 * Create a nodes config JSON array from the current list of managed nodes.
 * An example of the structure is shown below:
 *
 *"nodes" [
 *	{
 *		"address": "",
 *		"version": "",
 *		"oem_model": "",
 *		"interface": I,
 *		"power": P,
 *		"state": "READY",
 *		"subdevices": [
 *			{
 *				"key": "",
 *				"templates": [
 *					{
 *						"key": "",
 *						"version": "",
 *						"properties": [
 *							{
 *								"name": "",
 *								"type": T,
 *								"from-device":
 *									false,
 *								"
 *							}
 *						]
 *					}
 *				]
 *			}
 *		]
 *	}
 *]
 */
static json_t *node_conf_get(void)
{
	json_t *nodes;

	nodes = json_array();
	node_foreach(node_conf_get_node, nodes);
	return nodes;
}

/*
 * Set callbacks for generic node management to interact with the
 * gateway application.  Indicate an unsupported function by setting the
 * callback to NULL.
 */
void node_set_cloud_callbacks(const struct node_cloud_callbacks *callbacks)
{
	mgmt_state.cloud = *callbacks;
}

/*
 * Set callbacks for generic node management to interact with the network
 * interface layer.  Indicate an unsupported function by setting the
 * callback to NULL.
 */
void node_set_network_callbacks(const struct node_network_callbacks *callbacks)
{
	mgmt_state.network = *callbacks;
}

/*
 * Initialize the generic node management module.
 */
int node_mgmt_init(struct timer_head *timers)
{
	mgmt_state.timers = timers;

	hashmap_init(&mgmt_state.nodes, hashmap_hash_string,
	    hashmap_compare_string, 32);

	return conf_register("nodes", node_conf_set, node_conf_get);
}

/*
 * Free all resources allocated for node management.
 */
void node_mgmt_exit(void)
{
	/* Get each entry from the hashmap and remove it */
	node_entry_hashmap_foreach(&mgmt_state.nodes, node_entry_delete_foreach,
	    NULL);
	hashmap_destroy(&mgmt_state.nodes);
}

/*
 * Clear all node management state from memory.
 */
void node_mgmt_factory_reset(void)
{
	struct node_entry *entry;
	struct hashmap_iter *iter;

	/* Mark all nodes for removal */
	for (iter = hashmap_iter(&mgmt_state.nodes); iter;
	    iter = hashmap_iter_next(&mgmt_state.nodes, iter)) {
		entry = node_entry_hashmap_iter_get_data(iter);
		node_remove(&entry->node);
	}
	/* Immediately step management state machine to expedite removal */
	node_sync_all();
}

/*
 * Immediately run the node management state machine for all nodes in
 * case any actions are pending.  This should be used to speed retries,
 * if any operations have recently failed and are now likely to succeed.
 */
void node_sync_all(void)
{
	node_entry_hashmap_foreach(&mgmt_state.nodes,
	    node_step_foreach_handler, NULL);
}

/*
 * Handle a node joined event from the network stack.
 */
struct node *node_joined(const char *addr, const char *model,
	enum gw_interface interface, enum gw_power power, const char *version)
{
	struct node_entry *entry;
	struct node *node;

	/*
	 * If node is already being tracked, this might be a duplicate join
	 * event, or it might have left unannounced, so just update and
	 * reconfigure it.
	 */
	node = node_lookup(addr);
	if (node) {
		log_debug("%s: is already being tracked", node->addr);
		snprintf(node->oem_model, sizeof(node->oem_model), "%s", model);
		node->interface = interface;
		node->power = power;
		node_info_changed(node, version);
		return node;
	}

	/* Create new node entry */
	entry = node_entry_add(addr, model, interface, power, version);
	if (!entry) {
		return NULL;
	}
	entry->state = NODE_JOINED;
	/* Start the node management state machine */
	node_step(entry);
	return &entry->node;
}

/*
 * Handle a node left event from the network stack.
 */
int node_left(struct node *node)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	entry->left = true;
	node_step(entry);
	return 0;
}

/*
 * Handle a node connection status update from the network stack.
 */
int node_conn_status_changed(struct node *node, bool online)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	if (online != node->online) {
		node->online = online;
		/*
		 * Node came online, so run state machine in case a
		 * pending operation failed due to the node being offline.
		 */
		if (online) {
			node_step(entry);
		}
	}
	return node_conn_status_send(entry);
}

/*
 * Handle updated node info.
 */
int node_info_changed(struct node *node, const char *version)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	if (version) {
		snprintf(node->version, sizeof(node->version), "%s", version);
	}
	entry->update = true;
	entry->reconfigure = true;	/* Reconfigure after update */
	node_step(entry);
	return 0;
}

/*
 * Handle a cloud factory reset request.
 */
int node_factory_reset(struct node *node)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	entry->factory_reset = true;
	node_step(entry);
	return 0;
}

/*
 * Handle a cloud node remove request.
 */
int node_remove(struct node *node)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	entry->remove = true;
	node_step(entry);
	return 0;
}

/*
 * Handle a property update.
 */
int node_prop_set(struct node *node, struct node_prop *prop,
	const void *val, size_t val_len)
{
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	if (prop->dir != PROP_TO_DEVICE) {
		log_err("%s: property %s is read-only",
		    node->addr, prop->name);
		return -1;
	}
	if (val_len > prop->val_size) {
		log_err("%s: oversized value for property %s",
		    node->addr, prop->name);
		return -1;
	}
	switch (prop->type) {
	case PROP_INTEGER:
		*((int *)prop->val) = *((int *)val);
		break;
	case PROP_STRING:
		if (val_len >= prop->val_size) {
			log_err("%s: string value for property %s exceeds "
			    "%zu byte max", node->addr, prop->name,
			    prop->val_size - 1);
			return -1;
		}
		memcpy(prop->val, val, val_len);
		((char *)prop->val)[val_len] = '\0';
	break;
	case PROP_BOOLEAN:
		*((bool *)prop->val) = *((bool *)val) ? true : false;
		break;
	case PROP_DECIMAL:
		*((double *)prop->val) = *((double *)val);
		break;
	default:
		log_err("%s: property %s type %d is unsupported",
		    node->addr, prop->name, prop->type);
		return -1;
	}
	/* Mark the property as synchronized with the cloud */
	prop_entry->val_synced = true;
	/* Push the latest property value to the network interface */
	if (!mgmt_state.network.node_prop_set ||
	    mgmt_state.network.node_prop_set(node, prop,
	    node_prop_set_complete) < 0) {
		node_prop_set_complete(node, prop, NETWORK_UNSUPPORTED);
		return -1;
	}
	return 0;
}

/*
 * Apply an OTA image to a node.
 */
int node_ota_apply(struct node *node, const char *version,
	const char *image_path)
{
	if (!mgmt_state.network.node_ota_update) {
		log_warn("%s: OTA update not handled", node->addr);
		return -1;
	}
	return mgmt_state.network.node_ota_update(node, version,
	    image_path, NULL);
}

/*
 * Get the number of nodes currently managed on the network.
 */
size_t node_count(void)
{
	return hashmap_size(&mgmt_state.nodes);
}

/*
 * Find a node by address string.
 */
struct node *node_lookup(const char *addr)
{
	struct node_entry *entry;

	entry = node_entry_hashmap_get(&mgmt_state.nodes, addr);
	if (!entry) {
		return NULL;
	}
	return &entry->node;
}

/*
 * Iterate through all nodes and invoke the specified function for each one.
 * If func returns < 0, exit the loop and return the error code.  If func
 * returns > 0, exit the loop and indicate success.  Success return value is 0.
 */
int node_foreach(int (*func)(struct node *, void *), void *arg)
{
	struct node_foreach_state s = { func, arg };

	/* node_entry_hashmap_foreach() is safe for node removals */
	return node_entry_hashmap_foreach(&mgmt_state.nodes,
	    node_foreach_callback, &s);
}

/*
 * Find a node property by name.  Subdevice and template are optional.
 */
struct node_prop *node_prop_lookup(struct node *node,
	const char *subdevice, const char *template, const char *name)
{
	struct node_entry *n = CONTAINER_OF(struct node_entry, node, node);
	struct node_subdevice_entry *s;
	struct node_template_entry *t;
	struct node_prop_entry *p;

	SLIST_FOREACH(s, &n->subdevices, list_entry) {
		if (subdevice && strcmp(subdevice, s->subdevice.key)) {
			continue;
		}
		SLIST_FOREACH(t, &s->templates, list_entry) {
			if (template && strcmp(template, t->template.key)) {
				continue;
			}
			SLIST_FOREACH(p, &t->props, list_entry) {
				if (!strcmp(name, p->prop.name)) {
					return &p->prop;
				}
			}
		}
	}
	return NULL;
}

/*
 * Iterate through all props and invoke the specified function for each one.
 * If func returns < 0, exit the loop and return the error code.  If func
 * returns > 0, exit the loop and indicate success.  Success return value is 0.
 */
int node_prop_foreach(struct node *node,
	int (*func)(struct node *, struct node_prop *, void *),
	void *arg)
{
	struct node_entry *n = CONTAINER_OF(struct node_entry, node, node);
	struct node_subdevice_entry *s, *s_next;
	struct node_template_entry *t, *t_next;
	struct node_prop_entry *p, *p_next;
	int rc;

	/* Perform safe traversal, in case a property is deleted */
	s = SLIST_FIRST(&n->subdevices);
	while (s) {
		s_next = SLIST_NEXT(s, list_entry);
		t = SLIST_FIRST(&s->templates);
		while (t) {
			t_next = SLIST_NEXT(t, list_entry);
			p = SLIST_FIRST(&t->props);
			while (p) {
				p_next = SLIST_NEXT(p, list_entry);
				rc = func(node, &p->prop, arg);
				if (rc < 0) {
					return rc;
				}
				if (rc > 0) {
					return 0;
				}
				p = p_next;
			}
			t = t_next;
		}
		s = s_next;
	}
	return 0;
}

/*
 * Add a node property to track.  This function should be used
 * when a node first joins the network (in the NODE_NET_QUERY state).  If
 * called after the initial node setup has completed, apply the change
 * using node_info_changed().  Template version is optional.
 */
struct node_prop *node_prop_add(struct node *node,
	const char *subdevice, const char *template,
	const struct node_prop_def *prop_def, const char *template_version)
{
	struct node_entry *n = CONTAINER_OF(struct node_entry, node, node);
	struct node_subdevice_entry *s;
	struct node_template_entry *t;
	struct node_prop_entry *p;

	if (!template_version) {
		template_version = "";
	}
	SLIST_FOREACH(s, &n->subdevices, list_entry) {
		if (!strcmp(subdevice, s->subdevice.key)) {
			break;
		}
	}
	if (!s) {
		s = (struct node_subdevice_entry *)calloc(1, sizeof(*s));
		if (!s) {
			log_err("malloc failed");
			return NULL;
		}
		snprintf(s->subdevice.key, sizeof(s->subdevice.key), "%s",
		    subdevice);
		SLIST_INIT(&s->templates);
		SLIST_INSERT_HEAD(&n->subdevices, s, list_entry);
	}
	SLIST_FOREACH(t, &s->templates, list_entry) {
		if (!strcmp(template, t->template.key)) {
			break;
		}
	}
	if (!t) {
		t = (struct node_template_entry *)calloc(1, sizeof(*t));
		if (!t) {
			log_err("malloc failed");
			return NULL;
		}
		snprintf(t->template.key, sizeof(t->template.key), "%s",
		    template);
		snprintf(t->template.version, sizeof(t->template.version), "%s",
		    template_version);
		SLIST_INIT(&t->props);
		SLIST_INSERT_HEAD(&s->templates, t, list_entry);
	}
	/* Check for duplicate property */
	SLIST_FOREACH(p, &t->props, list_entry) {
		if (!strcmp(prop_def->name, p->prop.name)) {
			break;
		}
	}
	if (p) {
		if (p->prop.type == prop_def->type &&
		    p->prop.dir == prop_def->dir) {
			/* Same property already added */
			return &p->prop;
		}
		log_err("%s: cannot redefine existing property %s",
		    node->addr, p->prop.name);
		return NULL;
	}
	p = (struct node_prop_entry *)calloc(1, sizeof(*p));
	if (!p) {
		log_err("malloc failed");
		return NULL;
	}
	snprintf(p->prop.name, sizeof(p->prop.name), "%s", prop_def->name);
	p->prop.type = prop_def->type;
	p->prop.dir = prop_def->dir;
	p->prop.subdevice = &s->subdevice;
	p->prop.template = &t->template;
	if (!node_prop_val_alloc(&p->prop)) {
		free(p);
		return NULL;
	}
	SLIST_INSERT_HEAD(&t->props, p, list_entry);
	/* Check for template version mismatch */
	if (strcmp(template_version, t->template.version)) {
		log_warn("overriding template %s:%s version: %s --> %s",
		    s->subdevice.key, t->template.key, t->template.version,
		    template_version);
		snprintf(t->template.version, sizeof(t->template.version), "%s",
		    template_version);
	}
	return &p->prop;
}

/*
 * Remove a node property that is currently being tracked.  This should
 * be called to free resources if it is determined that a certain property is
 * no longer needed.  This might occur if the gateway determines that a node
 * property is not used by the cloud.
 */
void node_prop_remove(struct node *node, struct node_prop *prop)
{
	struct node_entry *n = CONTAINER_OF(struct node_entry, node, node);
	struct node_prop_entry *p = CONTAINER_OF(
	    struct node_prop_entry, prop, prop);
	struct node_template_entry *t = CONTAINER_OF(
	    struct node_template_entry, template, prop->template);
	struct node_subdevice_entry *s = CONTAINER_OF(
	    struct node_subdevice_entry, subdevice, prop->subdevice);

	/* Check if this is the only property in the template*/
	if (p == SLIST_FIRST(&t->props)) {
		/* Check if this is the only template in the subdevice */
		if (t == SLIST_FIRST(&s->templates)) {
			SLIST_REMOVE(&n->subdevices, s, node_subdevice_entry,
			    list_entry);
			free(s);
		} else {
			SLIST_REMOVE(&s->templates, t, node_template_entry,
			    list_entry);
		}
		free(t);
	} else {
		SLIST_REMOVE(&t->props, p, node_prop_entry, list_entry);
	}
	node_prop_entry_cleanup(p);
	free(p);
}

int node_prop_integer_val(struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	return *((int *)prop->val);
}

int node_prop_integer_send(struct node *node,
	struct node_prop *prop, int val)
{
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	ASSERT(prop->type == PROP_INTEGER);
	if (prop_entry->val_synced && *((int *)prop->val) == val) {
		return 0;
	}
	*((int *)prop->val) = val;
	return node_prop_send(node, prop);
}

char *node_prop_string_val(struct node_prop *prop)
{
	ASSERT(prop->type == PROP_STRING);
	return prop->val;
}

int node_prop_string_send(struct node *node,
	struct node_prop *prop, const char *val)
{
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	ASSERT(prop->type == PROP_STRING);
	if (prop_entry->val_synced && !strcmp((const char *)prop->val, val)) {
		return 0;
	}
	snprintf((char *)prop->val, prop->val_size, "%s", val);
	return node_prop_send(node, prop);
}

bool node_prop_boolean_val(struct node_prop *prop)
{
	ASSERT(prop->type == PROP_BOOLEAN);
	return *((bool *)prop->val);
}

int node_prop_boolean_send(struct node *node,
	struct node_prop *prop, bool val)
{
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	ASSERT(prop->type == PROP_BOOLEAN);
	if (prop_entry->val_synced && *((bool *)prop->val) == val) {
		return 0;
	}
	*((bool *)prop->val) = val;
	return node_prop_send(node, prop);
}

double node_prop_decimal_val(struct node_prop *prop)
{
	ASSERT(prop->type == PROP_DECIMAL);
	return *((double *)prop->val);
}

int node_prop_decimal_send(struct node *node,
	struct node_prop *prop, double val)
{
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	ASSERT(prop->type == PROP_DECIMAL);
	if (prop_entry->val_synced && *((double *)prop->val) == val) {
		return 0;
	}
	*((double *)prop->val) = val;
	return node_prop_send(node, prop);
}

/*
 * Send (or batch) a node property value.
 */
int node_prop_send(struct node *node, struct node_prop *prop)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	if (!mgmt_state.cloud.node_prop_send) {
		node_prop_send_complete(node, prop, NULL);
		return -1;
	}
	/* Mark the property as synchronized with the cloud */
	prop_entry->val_synced = true;
	if (entry->state != NODE_READY) {
		/* Defer send until node management operation completes */
		log_debug("%s: deferring send of %s; current state is %s",
		    node->addr, prop->name, node_state_names[entry->state]);
		entry->retry_send_props = true;
		prop_entry->retry_send = true;
		return 0;
	}
	if (mgmt_state.cloud.node_prop_send(node, prop,
	    node_prop_send_complete, entry->batch_prop_sends > 0) < 0) {
		entry->retry_send_props = true;
		prop_entry->retry_send = true;
		node_step_delayed(entry, NODE_STEP_OP_RETRY_MS);
		return -1;
	}
	prop_entry->retry_send = false;
	return 0;
}

/*
 * Begin batching datapoints.
 */
void node_prop_batch_begin(struct node *node)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	if (mgmt_state.cloud.node_prop_batch_send) {
		/* Use counter to support multiple batch calls */
		++entry->batch_prop_sends;
	}
}

/*
 * Stop batching datapoints and send the batch.
 */
void node_prop_batch_end(struct node *node)
{
	struct node_entry *entry = CONTAINER_OF(struct node_entry, node, node);

	if (mgmt_state.cloud.node_prop_batch_send &&
	    entry->batch_prop_sends > 0) {
		/* Only send batch after final batch_end call */
		if (--entry->batch_prop_sends == 0) {
			mgmt_state.cloud.node_prop_batch_send();
		}
	}
}

void node_state_set(struct node *node, enum node_state_slot slot,
	void *ptr, void (*cleanup_func)(void *))
{
	struct state_slot *state =
	    &CONTAINER_OF(struct node_entry, node, node)->slots[slot];

	node_state_slot_set(state, ptr, cleanup_func);
}

void *node_state_get(struct node *node, enum node_state_slot slot)
{
	return CONTAINER_OF(struct node_entry, node, node)->slots[slot].ptr;
}

void node_prop_state_set(struct node_prop *prop, enum node_state_slot slot,
	void *ptr, void (*cleanup_func)(void *))
{
	struct state_slot *state =
	    &CONTAINER_OF(struct node_prop_entry, prop, prop)->state[slot];

	node_state_slot_set(state, ptr, cleanup_func);
}

void *node_prop_state_get(struct node_prop *prop,
	enum node_state_slot slot)
{
	return CONTAINER_OF(struct node_prop_entry, prop,
	    prop)->state[slot].ptr;
}

/*
 * Helper function to populate a gw_node structure used by the generic gateway
 * framework to perform a node_add or node_update operation.
 */
int node_populate_gw_node(const struct node *node, struct gw_node *gw_node)
{
	struct node_entry *n = CONTAINER_OF(struct node_entry, node, node);
	struct gw_node_subdevice *subdevice;
	struct gw_subdevice_template *template;
	struct node_subdevice_entry *s;
	struct node_template_entry *t;
	struct node_prop_entry *p;

	gw_node_init(gw_node, node->addr);
	gw_node->oem_model = node->oem_model[0] ?
	    (char *)node->oem_model : NULL;
	gw_node->sw_version = node->version[0] ?
	    (char *)node->version : NULL;
	gw_node->interface = node->interface;
	gw_node->power = node->power;
	/* Populate gw_node structure from the node's property tree */
	SLIST_FOREACH(s, &n->subdevices, list_entry) {
		subdevice = gw_subdev_add(gw_node, s->subdevice.key);
		if (!subdevice) {
			goto error;
		}
		SLIST_FOREACH(t, &s->templates, list_entry) {
			template = gw_template_add(subdevice, t->template.key,
			    t->template.version[0] ?
			    t->template.version : NULL);
			if (!template) {
				goto error;
			}
			SLIST_FOREACH(p, &t->props, list_entry) {
				gw_prop_add(template, p->prop.name);
			}
		}
	}
	return 0;
error:
	gw_node_free(gw_node, 0);
	return -1;
}

/*
 * Helper function to populate a gw_node_prop structure used by the
 * generic gateway framework to send a property.
 */
void node_populate_gw_prop(const struct node *node,
	const struct node_prop *prop, struct gw_node_prop *gw_prop)
{
	gw_prop->addr = node->addr;
	gw_prop->subdevice_key = prop->subdevice->key;
	gw_prop->template_key = prop->template->key;
	gw_prop->name = prop->name;
}

/*
 * Update node property retry send flag. This function is meant
 * to be used by node_prop_foreach().
 */
static int node_prop_send_update(struct node *node, struct node_prop *prop,
	void *arg)
{
	int dir = *(int *)arg;
	struct node_prop_entry *prop_entry =
	    CONTAINER_OF(struct node_prop_entry, prop, prop);

	if (dir > PROP_FROM_DEVICE) {
		prop_entry->retry_send = true;
	} else if (dir == prop->dir) {
		prop_entry->retry_send = true;
	}

	return 0;
}

/*
 * Set node all properties retry_send flag.
 */
int node_prop_send_all_set(struct node *node, int dir)
{
	struct node_entry *entry;

	ASSERT(node);
	entry = CONTAINER_OF(struct node_entry, node, node);

	if (!mgmt_state.cloud.node_prop_send) {
		log_warn("%s: prop send not handled", node->addr);
		return -1;
	}

	log_debug("node %s send all set dir %d", node->addr, dir);

	/* Update node prop retry send flag */
	entry->retry_send_props = true;
	node_prop_foreach(node, node_prop_send_update, &dir);

	/* Active node next step operation */
	node_step(entry);

	return 0;
}

/*
 * Get a node state.
 */
enum node_state node_get_state(struct node *node)
{
	struct node_entry *entry;
	ASSERT(node);
	entry = CONTAINER_OF(struct node_entry, node, node);
	return entry->state;
}

