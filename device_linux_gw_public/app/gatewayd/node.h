/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __NODE_H__
#define __NODE_H__

#include <ayla/token_table.h>

struct timer_head;
struct confirm_info;

/*
 * Result statuses for network interface operations.
 */
#define NODE_NETWORK_RESULTS(def)			\
	def(SUCCESS,		NETWORK_SUCCESS)	\
	def(OFFLINE,		NETWORK_OFFLINE)	\
	def(UKNOWN,		NETWORK_UNKNOWN)	\
	def(UNSUPPORTED,	NETWORK_UNSUPPORTED)

DEF_ENUM(node_network_result, NODE_NETWORK_RESULTS);

/*
 * Generic node management states.
 */
#define NODE_STATES(def)				\
	def(JOINED,		NODE_JOINED)		\
	def(REMOVED,		NODE_REMOVED)		\
	def(READY,		NODE_READY)		\
	def(NET_QUERY,		NODE_NET_QUERY)		\
	def(NET_CONFIGURE,	NODE_NET_CONFIGURE)	\
	def(NET_FACTORY_RESET,	NODE_NET_FACTORY_RESET)	\
	def(NET_REMOVE,		NODE_NET_REMOVE)	\
	def(CLOUD_ADD,		NODE_CLOUD_ADD)		\
	def(CLOUD_REMOVE,	NODE_CLOUD_REMOVE)	\
	def(CLOUD_UPDATE,	NODE_CLOUD_UPDATE)

DEF_ENUM(node_state, NODE_STATES);

/*
 * Selector for node state slots.
 */
enum node_state_slot {
	STATE_SLOT_APP = 0,
	STATE_SLOT_NET,

	STATE_SLOT_COUNT	/* MUST be last */
};

/*
 * Generic node representation.  Maintains identity and current state.
 */
struct node {
	char addr[GW_NODE_ADDR_SIZE];		/* Unique address */
	char version[32];			/* Optional node SW version */
	char oem_model[GW_MAX_OEM_MODEL_SIZE];	/* Optional OEM model */
	enum gw_interface interface;		/* Node network type */
	enum gw_power power;			/* Node power source */
	bool online;				/* Current online state */
};

/*
 * Generic node subdevice.
 */
struct node_subdevice {
	char key[GW_SUBDEVICE_KEY_SIZE];
};

/*
 * Generic node template.
 */
struct node_template {
	char key[GW_TEMPLATE_KEY_SIZE];
	char version[GW_TEMPLATE_VER_SIZE];
};

/*
 * Generic node property state.  Caches the current value.
 */
struct node_prop {
	char name[GW_PROPERTY_NAME_SIZE];
	enum prop_type type;
	enum prop_direction dir;
	void *val;
	size_t val_size;
	struct node_subdevice *subdevice;
	struct node_template *template;
};

/*
 * Generic node property definition.
 */
struct node_prop_def {
	const char *name;
	enum prop_type type;
	enum prop_direction dir;
};

/*
 * Interface for generic node management to interact with the gateway
 * application.
 */
struct node_cloud_callbacks {
	/* Add node to the cloud */
	int (*node_add)(struct node *,
	    void (*)(struct node *, const struct confirm_info *));
	/* Remove node from the cloud */
	int (*node_remove)(struct node *,
	    void (*)(struct node *, const struct confirm_info *));
	/* Update node info in the cloud */
	int (*node_update_info)(struct node *,
	    void (*)(struct node *, const struct confirm_info *));
	/* Update node online status in the cloud */
	int (*node_conn_status)(struct node *,
	    void (*)(struct node *, const struct confirm_info *));
	/* Send or batch a node property value */
	int (*node_prop_send)(const struct node *, const struct node_prop *,
	    void (*)(struct node *, struct node_prop *,
	    const struct confirm_info *), bool batch_append);
	/* Send all batched property values for the node */
	int (*node_prop_batch_send)(void);
	/* Generate a JSON object to persist the node's application state */
	json_t *(*node_conf_save)(const struct node *);
	/* Restore the node's application state from persistent storage */
	int (*node_conf_loaded)(struct node *, json_t *);
};

/*
 * Interface for generic node management to interact with the network-specific
 * interface or network stack.
 */
struct node_network_callbacks {
	/* Query the node's capabilities and populate properties */
	int (*node_query_info)(struct node *,
	    void (*)(struct node *, enum node_network_result));
	/* Setup the node for management by the coordinator */
	int (*node_configure)(struct node *,
	    void (*)(struct node *, enum node_network_result));
	/* Send a new property value to the node */
	int (*node_prop_set)(struct node *, struct node_prop *,
	    void (*)(struct node *, struct node_prop *,
	    enum node_network_result));
	/* Restore the node to factory default state */
	int (*node_factory_reset)(struct node *,
	    void (*)(struct node *, enum node_network_result));
	/* Remove the node from the network */
	int (*node_leave)(struct node *,
	    void (*)(struct node *, enum node_network_result));
	/* Push a firmware update to the node */
	int (*node_ota_update)(struct node *, const char *, const char *,
	    void (*)(struct node *, enum node_network_result));
	/* Generate a JSON object to persist the node's network state */
	json_t *(*node_conf_save)(const struct node *);
	/* Restore the node's network state from persistent storage */
	int (*node_conf_loaded)(struct node *, json_t *);
};


/*
 * Set callbacks for generic node management to interact with the
 * gateway application.  Indicate an unsupported function by setting the
 * callback to NULL.
 */
void node_set_cloud_callbacks(const struct node_cloud_callbacks *callbacks);

/*
 * Set callbacks for generic node management to interact with the network
 * interface layer.  Indicate an unsupported function by setting the
 * callback to NULL.
 */
void node_set_network_callbacks(const struct node_network_callbacks *callbacks);

/*
 * Initialize the generic node management module.
 */
int node_mgmt_init(struct timer_head *timers);

/*
 * Free all resources allocated for node management.
 */
void node_mgmt_exit(void);

/*
 * Clear all node management state from memory.
 */
void node_mgmt_factory_reset(void);

/*
 * Immediately run the node management state machine for all nodes in
 * case any actions are pending.  This should be used to speed retries,
 * if any operations have recently failed and are now likely to succeed.
 */
void node_sync_all(void);

/*
 * Handle a node joined event from the network stack.
 */
struct node *node_joined(const char *addr, const char *model,
	enum gw_interface interface, enum gw_power power, const char *version);

/*
 * Handle a node left event from the network stack.
 */
int node_left(struct node *node);

/*
 * Handle a node connection status update from the network stack.
 */
int node_conn_status_changed(struct node *node, bool online);

/*
 * Handle updated node info.
 */
int node_info_changed(struct node *node, const char *version);

/*
 * Handle a cloud factory reset request.
 */
int node_factory_reset(struct node *node);

/*
 * Handle a cloud node remove request.
 */
int node_remove(struct node *node);

/*
 * Handle a property update.
 */
int node_prop_set(struct node *node, struct node_prop *prop,
	const void *val, size_t val_len);

/*
 * Apply an OTA image to a node.
 */
int node_ota_apply(struct node *node, const char *version,
	const char *image_path);

/*
 * Get the number of nodes currently managed on the network.
 */
size_t node_count(void);

/*
 * Find a node by address string.
 */
struct node *node_lookup(const char *addr);

/*
 * Iterate through all nodes and invoke the specified function for each one.
 * If func returns < 0, exit the loop and return the error code.  If func
 * returns > 0, exit the loop and indicate success.  Success return value is 0.
 */
int node_foreach(int (*func)(struct node *, void *), void *arg);

/*
 * Find a node property by name.  Subdevice and template are optional.
 */
struct node_prop *node_prop_lookup(struct node *node,
	const char *subdevice, const char *template, const char *name);

/*
 * Iterate through all props and invoke the specified function for each one.
 * If func returns < 0, exit the loop and return the error code.  If func
 * returns > 0, exit the loop and indicate success.  Success return value is 0.
 */
int node_prop_foreach(struct node *node,
	int (*func)(struct node *, struct node_prop *, void *),
	void *arg);

/*
 * Add a node property to track.  This function should be used
 * when a node first joins the network (in the NODE_NET_QUERY state).  If
 * called after the initial node setup has completed, apply the change
 * using node_info_changed().  Template version is optional.
 */
struct node_prop *node_prop_add(struct node *node,
	const char *subdevice, const char *template,
	const struct node_prop_def *prop_def, const char *template_version);

/*
 * Remove a node property that is currently being tracked.  This should
 * be called to free resources if it is determined that a certain property is
 * no longer needed.  This might occur if the gateway determines that a node
 * property is not used by the cloud.
 */
void node_prop_remove(struct node *node, struct node_prop *prop);

/*
 * Get or send an integer type property.
 */
int node_prop_integer_val(struct node_prop *prop);
int node_prop_integer_send(struct node *node,
	struct node_prop *prop, int val);

/*
 * Get or send a string type property.
 */
char *node_prop_string_val(struct node_prop *prop);
int node_prop_string_send(struct node *node,
	struct node_prop *prop, const char *val);

/*
 * Get or send a boolean type property.
 */
bool node_prop_boolean_val(struct node_prop *prop);
int node_prop_boolean_send(struct node *node,
	struct node_prop *prop, bool val);

/*
 * Get or send a decimal type property.
 */
double node_prop_decimal_val(struct node_prop *prop);
int node_prop_decimal_send(struct node *node,
	struct node_prop *prop, double val);

/*
 * Send the current value of a node property.
 */
int node_prop_send(struct node *node, struct node_prop *prop);

/*
 * Begin batching datapoints.
 */
void node_prop_batch_begin(struct node *node);

/*
 * Stop batching datapoints and send the batch.
 */
void node_prop_batch_end(struct node *node);

/*
 * Set and access node and property state slots.  These slots provide a safe
 * way for the application layer and the network interface layer to associate
 * additional state with node or node property entries.  If the node
 * is deleted, the supplied cleanup_func will be invoked automatically to
 * free any associated resources.  Set the ptr to NULL to clear a slot.
 */
void node_state_set(struct node *node, enum node_state_slot slot,
	void *ptr, void (*cleanup_func)(void *));
void *node_state_get(struct node *node, enum node_state_slot slot);
void node_prop_state_set(struct node_prop *prop, enum node_state_slot slot,
	void *ptr, void (*cleanup_func)(void *));
void *node_prop_state_get(struct node_prop *prop,
	enum node_state_slot slot);

/*
 * Helper function to populate a gw_node structure used by the generic gateway
 * framework to perform a node_add or node_update operation.
 */
int node_populate_gw_node(const struct node *node, struct gw_node *gw_node);

/*
 * Helper function to populate a gw_node_prop structure used by the
 * generic gateway framework to send a property.
 */
void node_populate_gw_prop(const struct node *node,
	const struct node_prop *prop, struct gw_node_prop *gw_prop);

/*
 * Set node all properties retry_send flag.
 */
int node_prop_send_all_set(struct node *node, int dir);

/*
 * Get a node state.
 */
enum node_state node_get_state(struct node *node);

#endif /* __NODE_H__ */
