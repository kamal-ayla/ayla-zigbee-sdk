/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_GATEWAY_H__
#define __LIB_APP_GATEWAY_H__

#define GW_NODE_ADDR_SIZE	28
#define GW_SUBDEVICE_KEY_SIZE	20	/* can be increased to a max of 201 */
#define GW_TEMPLATE_KEY_SIZE	9
#define GW_TEMPLATE_VER_SIZE	10
#define GW_PROPERTY_NAME_SIZE	28
#define GW_MAX_OEM_MODEL_SIZE	62

#include <sys/queue.h>
#include <jansson.h>

struct gw_template_prop {
	LIST_ENTRY(gw_template_prop) link;
	/* name of prop must match the name in cloud */
	char name[GW_PROPERTY_NAME_SIZE];
	char *propmeta;	/* (optional) metadata of property */
};

struct gw_subdevice_template {
	LIST_ENTRY(gw_subdevice_template) link;
	LIST_HEAD(, gw_template_prop) props; /* (optional) list of props */
	/*
	 * must match the template key and version given when creating template
	 * in developer website.
	 */
	char template_key[GW_TEMPLATE_KEY_SIZE];
	char template_ver[GW_TEMPLATE_VER_SIZE];
};

struct gw_node_subdevice {
	LIST_ENTRY(gw_node_subdevice) link;
	LIST_HEAD(, gw_subdevice_template) templates; /* (optional) */
	/* must be unique across the subdevices of a node */
	char subdevice_key[GW_SUBDEVICE_KEY_SIZE];
	char *submeta; /* (optional) metadata of subdevice */
};

enum gw_interface {
	GI_WIFI = 0,
	GI_ZIGBEE,
	GI_ZWAVE,
	GI_BLE,
};

enum gw_power {
	GP_MAINS = 0,
	GP_BATTERY,
};

/*
 * A node is made up of subdevices which can have one or more templates.
 * Each template is made up of one or more properties.
 */
struct gw_node {
	/* must be unique across the nodes of gw */
	char addr[GW_NODE_ADDR_SIZE];

	/* (optional) used for Node OTA. Max Len: GW_MAX_OEM_MODEL_SIZE */
	char *oem_model;

	char *sw_version;   /* (optional) used for Node OTA */
	char *nodemeta;	    /* (optional) node metadata */
	LIST_HEAD(, gw_node_subdevice) subdevices;	/* must have min 1 */

	/*
	 * Interface type of the node
	 * 0 = Wifi
	 * 1 = Zigbee
	 * 2 = Z-Wave
	 * 3 = BLE
	 */
	enum gw_interface interface;

	/*
	 * 0 = Mains Powered
	 * 1 = Battery Powered
	 */
	enum gw_power power;

	/* must be initialized by calling gw_node_init */
	u8 initialized:1;
};

struct gw_node_prop {
	const char *addr;
	const char *subdevice_key;
	const char *template_key;
	const char *name;
};

struct gw_node_prop_dp {
	const struct gw_node_prop *prop;
	const enum prop_type type;
	const void *val;
	const size_t val_len;
};

struct gw_node_ota_info {
	const char *addr;
	const char *version;
	const char *save_location;
};

enum gw_confirm_arg_type {
	CAT_NODEPROP_DP = 0,	/* *arg* is of type struct gw_node_prop_dp */
	CAT_ADDR,		/* *arg* is a char * representing node addr */
	CAT_BATCH_ID,		/* *arg* is a int * of the batch_id */
	CAT_NODE_OTA_INFO,	/* *arg* is of type struct gw_node_ota_info */
};

/*
 * Definition of a batch entry. Used internally by Ayla
 */
struct gw_node_prop_batch_entry {
	STAILQ_ENTRY(gw_node_prop_batch_entry) link;
	int entry_id;
	struct gw_cmd *gw_cmd;
	u8 recvd_nak;	/* recvd nak for this entry */
};

STAILQ_HEAD(gw_node_prop_batchq, gw_node_prop_batch_entry);

/*
 * Definition of a batch list
 */
struct gw_node_prop_batch_list {
	struct gw_node_prop_batchq batchq;
	int batchq_len;
	u8 sent;	/* already sent. should not be modified after this */
};

/*
 * Initialize the gateway block.
 */
void gw_initialize(void);

/*
 * Add node for the gateway. Helper functions like gw_node_init and
 * and gw_subdev_add should be used to build the gw_node structure. If the
 * *node_info_j* parameter is given, it will be set to the JSON representation
 * of "node". The application can use the JSON to do things like persisting the
 * information about nodes that have been added. The JSON structure is
 * dynamically allocated. It can be freed using *json_decref* (see libjansson)
 * Please see the description of "prop_arg_send" in props.h for a
 * description of op_options. The "confirm" option can only be used if the
 * handler function has been set using *gw_confirm_handler_set*.
 */
int gw_node_add(struct gw_node *node, json_t **node_info_j,
		const struct op_options *opts);

/*
 * Update template/property information of a node on the gateway. This funciton
 * is useful for times when the templates/props of a node have changed (for ex.
 * due to a Node OTA) and you want the new feature set to get reflected in the
 * cloud. This function should only be called after the node has been added to
 * the gateway (through gw_node_add). Helper functions like gw_node_init and and
 * gw_subdev_add should be used to build the gw_node structure. If the
 * *node_info_j* parameter is given, it will be set to the JSON representation
 * of "node". The application can use the JSON to do things like persisting the
 * information about the node. The JSON structure is dynamically allocated. It
 * can be freed using *json_decref* (see libjansson) Please see the description
 * of "prop_arg_send" in props.h for a description of op_options. The "confirm
 * option can only be used if the handler function has been set using
 * *gw_confirm_handler_set*.
 */
int gw_node_update(struct gw_node *node, json_t **node_info_j,
	const struct op_options *opts);

/*
 * Initialize a struct gw_node with a given address.
 */
int gw_node_init(struct gw_node *node, const char *addr);

/*
 * Create a JSON object representation of the node. The json structure is
 * dynamically allocated. It must be freed using json_decref (see libjansson)
 */
int gw_node_to_json(struct gw_node *node, json_t **node_json);

/*
 * Allocate a new subdevice for gw_node and return pointer to it.
 */
struct gw_node_subdevice *gw_subdev_add(struct gw_node *node, const char *key);

/*
 * Allocate a new template for gw_node_subdevice and return pointer to it.
 * 'key' is required. Optionally also give a "version" of the template.
 */
struct gw_subdevice_template *gw_template_add(struct gw_node_subdevice *subdev,
				const char *key, const char *ver);

/*
 * Allocate a new property for gw_subdevice_template and return pointer to it.
 * By default, every property of a template is added to the node. However, if
 * the application only wants to use a subset of the properties, it can use
 * this function to list the properties from the template that are to be used.
 */
struct gw_template_prop *gw_prop_add(struct gw_subdevice_template *template,
					const char *name);

/*
 * Populate a gw_node object from the JSON representation of the node. The
 * manuf_model, sw_version, and metadatas are all dynamically allocated. The
 * application can free the gw_node structure by calling gw_node_free with
 * the "free_strs" flag set to 1.
 */
int gw_json_to_node(struct gw_node *node, json_t *data);

/*
 * Free subdevices, templates, and props of a node. If the free_strs flag is set
 * to 1, then the manuf_model, sw_version, and any metadata will also be freed.
 */
int gw_node_free(struct gw_node *node, int free_strs);

/*
 * Remove a node that has been added to the gateway.
 * The "confirm" option can only be used if the handler function has been
 * set using *gw_confirm_handler_set*.
 */
int gw_node_remove(const char *addr, const struct op_options *opts);

/*
 * Set the connection status for a node. "status" should be 1 or 0.
 */
int gw_node_conn_status_send(const char *addr, u8 status,
	const struct op_options *opts);

/*
 * Request a node property value from the cloud
 */
int gw_node_prop_request(struct gw_node_prop *prop);

/*
 * Request values of all node properties from the
 * cloud
 */
int gw_node_prop_request_all(const char *addr);

/*
 * Request values of all to-device node properties
 * from the cloud. This function could be useful in making sure the device is in
 * sync with the cloud at bootup.
 */
int gw_node_prop_request_to_dev(const char *addr);

/*
 * Send a property update for a node to cloud and mobile apps (LAN).
 * Applications should set req_id to 0 UNLESS they are responding to property
 * requests in which case *req_id* MUST be set to the value passed to the
 * application by the library. Please see *gw_prop_respond_handler* for details.
 * Please see description of *prop_arg_send* in props.h to see what op_options
 * are available. This function returns 0 if the property was successfully
 * scheduled to be sent.
 */
int gw_node_prop_send(const struct gw_node_prop *prop, enum prop_type type,
	const void *val, size_t val_len, int req_id,
	const struct op_options *opts);

/*
 * Same as gw_node_prop_send but the property is put in a queue to be sent
 * later. If the given list is set to NULL, a new batch list is created and can
 * be used in subsequent calls. Otherwise, the new update is appended to the
 * given list. Properties of type PROP_FILE cannot be batched. A *NULL* will
 * be returned if there was a failure appending to the batch. The batch can be
 * sent by calling *prop_batch_send*. The only opts allowed in this call are
 * *dev_time_ms* and *confirm*. See *prop_arg_send* for details on what the
 * options mean. Note that this function allows the application to have the
 * flexibility of having multiple ongoing batches if it desires. It may be
 * desirable to send the property updates to LAN clients right away and only
 * batch updates for the cloud. In this case, you can call gw_node_prop_send for
 * LAN clients and gw_node_prop_batch_send for just the cloud (by setting
 * op_options).
 */
struct gw_node_prop_batch_list *gw_node_prop_batch_append(
		struct gw_node_prop_batch_list *list,
		const struct gw_node_prop *prop,
		enum prop_type type, const void *val, size_t val_len,
		const struct op_options *opts);

/*
 * Send a node batch list. The application SHOULD NOT modify the batch list
 * after its sent (i.e. calling append on the list again). To be safe, the lib
 * takes away the application's pointer to the list by setting *list_ptr to
 * NULL. The application has op_options available to set *dests* for the batch,
 * etc. The *batch_id* arg can be optionally given to store the batch # assigned
 * for this batch. On success, 0 is returned.
 */
int gw_node_prop_batch_send(struct gw_node_prop_batch_list **list_ptr,
		const struct op_options *opts, int *batch_id);

/*
 * Free the gw_node_prop_batch_list. Can be used to abort/free a batch before
 * being sent. This function must not be called after the batch has already been
 * sent. It should also not be called twice. To be safe, the lib takes away the
 * application's pointer to the batch by setting *list_ptr to NULL.
 */
void gw_node_prop_batch_list_free(struct gw_node_prop_batch_list **list_ptr);

/*
 * Register a handler for confirmation callbacks for cases when *confirm* was
 * set when calling gw_node_add, gw_node_prop_send, etc. Use the "op" to
 * determine what operation the confirmation is for. Use the "type" to determine
 * what the "arg" should be casted to. The confirm_info->status tells you if the
 * operation succeeded. If the operation failed, confirm_info->err tells you
 * what the error was. Usually the application only cares if a property update
 * failed to each the cloud service (not mobile apps in LAN mode) and the lib
 * uses gw_cloud_fail_handler to provide this information. So the explicit
 * confirmation option is generally not needed.
 */
void gw_confirm_handler_set(int (*handler)(enum ayla_gateway_op op,
    enum gw_confirm_arg_type type, const void *arg,
    const struct op_options *opts, const struct confirm_info *confirm_info));

/*
 * Register a handler for responding to node property requests. The response
 * MUST be sent using the *gw_node_prop_send* function with the same *req_id*.
 * The *arg* parameter may (optionally) contain additional
 * information about the request. Should return -1 if no datapoint or if no
 * property exists for the given gw_node_prop.
 */
void gw_node_prop_get_handler_set(int (*handler)(struct gw_node_prop *prop,
				int req_id, const char *arg));

/*
 * Register a handler for responding to connection status requests.
 * This function sets the handler for connection status requests coming from
 * devd, i.e., connection status requests made by the service/mobile app .
 * The *addr* is the address of the node. The handler should return a 0 if the
 * node is offline, 1 if it’s online, and -1 if the node cannot be found.
 * The application should maintain an online/offline state for all of its nodes
 * as this can be queried anytime.
 */
void gw_node_conn_get_handler_set(int (*handler)(const char *addr));

/*
 * Register a handler for accepting node property updates and responses.
 * The 'args' parameter will be NULL if no additional args exist.
 * Please see the definition of op_args to see what different
 * args can be passed in. Note that the 'args' structure is on the stack.
 * If the app sets the *app_manages_acks* to 1, then the app must take care of
 * acks by calling "ops_prop_ack_send" with the args->ack_arg. The application
 * can ignore the contents of ack_arg. If the app sets the *app_manages_acks* to
 * 0, then the library will automatically take care of acks by using the return
 * value of this handler. It'll consider a 0 return code as a success and
 * failure otherwise.
 */
void gw_node_prop_set_handler_set(int (*handler)(struct gw_node_prop *prop,
		enum prop_type type, const void *val, size_t val_len,
		const struct op_args *args), int app_manages_acks);

/*
 * Register a handler for processing node factory resets.
 * The *addr* represents the node that needs to be factory reset.
 * Response to the node factor reset should be given by calling
 * gw_node_rst_cb with the *cookie*
 */
void gw_node_rst_handler_set(void (*handler)(const char *addr, void *cookie));

/*
 * Register a handler for processing a pending Node OTA command from
 * the cloud. The *addr* represents the node that the OTA is pending for and the
 * version is the one assigned to the OTA when it was uploaded to the Ayla
 * service.
 * Response to the Node OTA should be given by calling gw_node_ota_cb with the
 * *cookie*
 */
void gw_node_ota_handler_set(void (*handler)(const char *addr, const char *ver,
				void *cookie));

/*
 * Register a handler for processing a pending Node register status from
 * the cloud. The *addr* represents the node that registered status changed.
 * The *stat* represents the node registered status.
 * Response to the Node register status should be given by calling
 * gw_node_reg_cb with the *cookie*
 */
void gw_node_reg_handler_set(void (*handler)(const char *addr, bool stat,
				void *cookie));

/*
 * Register a handler for being notified that a node_add, conn_status update,
 * or node prop_send failed to reach the cloud (due to connectivity loss).
 * The application should set this handler and use the given information to keep
 * track of what will need to be done when cloud connectivity resumes.
 * Depending on the application, it might be ok to simply ignore these failures.
 * The application can use the ops_set_cloud_recovery_handler to get notified
 * when cloud connectivity resumes and can resend the updates at this time.
 * Use the "op" to determine what operation the confirmation is for. Use the
 * "type" to determine what the "arg" should be casted to. If type is
 * CAT_NODEPROP_DP and the *val* inside *arg* is NULL, the failure is for the
 * current value of the property. The return value of this handler is
 * currently unused.
 */
void gw_cloud_fail_handler_set(int (*handler)(enum ayla_gateway_op op,
		enum gw_confirm_arg_type type, const void *arg,
		const struct op_options *opts));

/*
 * Use this function to send back the result of a node factory reset. The
 * *cookie* argument should be the same as what was passed into the
 * gw_node_rst_handler. *success* = 0 means that the factory reset failed.
 * *msg_code* can be any integer that the oem desires to store in the cloud to
 * represent the result of the command.
 */
int gw_node_rst_cb(const char *addr, void *cookie, u8 success,
			int msg_code);

/*
 * Use this function to respond back to a Node OTA command (see
 * gw_node_ota_handler). The function should be given the node addr, the
 * *cookie* that was passed to the gw_node_ota_handler, a *save_location"
 * filepath where the OTA should be saved to, and *opts*. If the application
 * wants to discard the OTA, it should pass a NULL as the *save_location*. Until
 * the applciation uses this function to accept or discard the OTA, the command
 * will remain pending in the cloud. In order to know when the OTA has
 * downloaded and is ready for the application to use, the *confirm* flag of
 * *opts* should be set to 1 and the gw_confirm_handler should be used. For this
 * operation, the default for *confirm* is 1.
 */
int gw_node_ota_cb(const char *addr, void *cookie, const char *save_location,
			const struct op_options *opts);
/*
 * Use this function to send back the result of a node register status. The
 * *cookie* argument should be the same as what was passed into the
 * gw_node_reg_handler. *success* = 0 means that register status sent failed.
 * *msg_code* can be any integer that the oem desires to store in the cloud to
 * represent the result of the command.
 */
int gw_node_reg_cb(const char *addr, void *cookie, u8 success, int msg_code);

#endif /* __LIB_APP_GATEWAY_H__ */
