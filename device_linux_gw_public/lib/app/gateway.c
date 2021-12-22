/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 *
 * This code is offered as an example without any guarantee or warranty.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <sys/queue.h>

#include <ayla/utypes.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/gateway_interface.h>
#include <ayla/assert.h>
#include <ayla/base64.h>
#include <ayla/log.h>
#include <ayla/file_io.h>
#include <ayla/conf_io.h>

#include <app/ops.h>
#include <app/props.h>
#include <app/gateway.h>

#include "ops_internal.h"
#include "props_internal.h"
#include "data_internal.h"
#include "sched_internal.h"
#include "gateway_internal.h"

#define GATEWAY_SUBSYSTEM_ID	2
#define GATEWAY_SCHEDULES	"gateway_schedules"

struct gw_node_prop_nonconst {
	char *addr;
	char *subdevice_key;
	char *template_key;
	char *name;
};

struct gw_node_prop_dp_nonconst {
	struct gw_node_prop_nonconst *prop;
	enum prop_type type;
	json_t *val_j;
	size_t val_len;
};

struct gw_node_ota_info_nonconst {
	char *addr;
	char *version;
	char *save_location;
};

/*
 * Gateway operations to be executed
 */
struct gw_cmd {
	enum ayla_gateway_op op;
	json_t *data;
	int req_id;
	void *arg;		/* arg used for various internal functions */
	void (*free_arg_handler)(void *arg); /* handler for freeing *arg* */
	enum gw_confirm_arg_type arg_type;
	struct op_options opts;
};

static int gw_node_prop_batch_sent_counter;

static int (*gw_node_conn_get_handler)(const char *addr);
static int (*gw_confirm_handler)(enum ayla_gateway_op, enum gw_confirm_arg_type,
				const void *, const struct op_options *opts,
				const struct confirm_info *);
static int (*gw_node_prop_get_handler)(struct gw_node_prop *, int,
				const char *);
static int (*gw_node_prop_set_handler)(struct gw_node_prop *, enum prop_type,
					const void *, size_t,
					const struct op_args *);
static int gw_app_manages_acks;
static void (*gw_node_rst_handler)(const char *addr, void *cookie);
static void (*gw_node_ota_handler)(const char *addr, const char *ver,
					void *cookie);
static void (*gw_node_reg_handler)(const char *addr, bool stat,
					void *cookie);
static int (*gw_cloud_fail_handler)(enum ayla_gateway_op,
	enum gw_confirm_arg_type, const void *, const struct op_options *);

/*
 * Property table is a dynamic array of pointers to props.
 * The app can add new properties to this.
 * Properties cannot be deleted yet.
 */
struct gw_state {
	struct gw_node_prop_entry **table;
	unsigned int count;
	bool gw_initialized;
};

static struct gw_state gw_state;

/*
 * Create a JSON object representation of a subdevice list
 */
static int gw_subdev_array(struct gw_node_subdevice *devq, json_t **subdev_arr)
{
	struct gw_subdevice_template *template;
	struct gw_template_prop *property;
	json_t *template_arr_j;
	json_t *props_arr_j;
	json_t *subdev_obj_j;
	json_t *template_obj_j;
	json_t *property_obj_j;
	json_t *subdev_arr_j;

	if (!devq) {
		return -1;
	}
	subdev_arr_j = json_array();
	REQUIRE(subdev_arr_j, REQUIRE_MSG_ALLOCATION);
	for (; devq != NULL; devq = devq->link.le_next) {
		if (devq->subdevice_key[0] == '\0') {
			log_warn("subdevice must have a key");
			return -1;
		}
		subdev_obj_j = json_object();
		REQUIRE(subdev_obj_j, REQUIRE_MSG_ALLOCATION);
		if (json_object_set_new(subdev_obj_j, "subdevice_key",
		    json_string(devq->subdevice_key))) {
			REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
		}
		if (devq->submeta && json_object_set_new(subdev_obj_j,
		    "submeta", json_string(devq->submeta))) {
			REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
		}
		if (LIST_EMPTY(&devq->templates)) {
			goto skip_templates;
		}
		template_arr_j = json_array();
		REQUIRE(template_arr_j, REQUIRE_MSG_ALLOCATION);
		for (template = devq->templates.lh_first; template != NULL;
		    template = template->link.le_next) {
			template_obj_j = json_object();
			REQUIRE(template_obj_j, REQUIRE_MSG_ALLOCATION);
			if (json_object_set_new(template_obj_j, "template_key",
			    json_string(template->template_key))) {
				REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
			}
			if (template->template_ver[0] != '\0' &&
			    json_object_set_new(template_obj_j, "version",
			    json_string(template->template_ver))) {
				REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
			}
			if (LIST_EMPTY(&template->props)) {
				goto skip_props;
			}
			props_arr_j = json_array();
			REQUIRE(props_arr_j, REQUIRE_MSG_ALLOCATION);
			for (property = template->props.lh_first;
			    property != NULL;
			    property = property->link.le_next) {
				property_obj_j = json_object();
				REQUIRE(property_obj_j, REQUIRE_MSG_ALLOCATION);
				if (json_object_set_new(property_obj_j, "name",
				    json_string(property->name))) {
					REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
				}
				if (property->propmeta &&
				    json_object_set_new(property_obj_j,
				    "propmeta",
				    json_string(property->propmeta))) {
					REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
				}
				json_array_append_new(props_arr_j,
				    property_obj_j);
			}
			if (json_object_set_new(template_obj_j, "properties",
			    props_arr_j)) {
				REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
			}
skip_props:
			json_array_append_new(template_arr_j, template_obj_j);
		}
		if (json_object_set_new(subdev_obj_j, "templates",
		    template_arr_j)) {
			REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
		}
skip_templates:
		json_array_append_new(subdev_arr_j, subdev_obj_j);
	}
	*subdev_arr = subdev_arr_j;

	return 0;
}

/*
 * Create a JSON object representation of the node. The json structure is
 * dynamically allocated. It must be freed using json_decref (see libjansson)
 */
int gw_node_to_json(struct gw_node *node, json_t **node_json)
{
	json_t *data_j;
	json_t *subdev_j;
	json_t *node_j;

	if (!node->initialized) {
		log_warn("node not initialized");
		return -1;
	}
	if (node->addr[0] == '\0') {
		log_warn("node must have an addr");
		return -1;
	}
	if (LIST_EMPTY(&node->subdevices)) {
		log_warn("node must have at least one sub device");
		return -1;
	}
	data_j = json_object();
	REQUIRE(data_j, REQUIRE_MSG_ALLOCATION);
	node_j = json_object();
	REQUIRE(node_j, REQUIRE_MSG_ALLOCATION);
	if (json_object_set_new(data_j, "address",
	    json_string(node->addr))) {
		REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
	}
	if (node->oem_model) {
		if (strlen(node->oem_model) > GW_MAX_OEM_MODEL_SIZE) {
			log_warn("node oem model too long");
			return -1;
		}
		if (json_object_set_new(data_j, "oem_model",
		    json_string(node->oem_model))) {
			REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
		}
	}
	if (node->sw_version && json_object_set_new(data_j, "sw_version",
	    json_string(node->sw_version))) {
		REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
	}
	if (node->nodemeta && json_object_set_new(data_j, "nodemeta",
	    json_string(node->nodemeta))) {
		REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
	}
	if (json_object_set_new(data_j, "interface",
	    json_integer(node->interface))) {
		REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
	}
	if (json_object_set_new(data_j, "power",
	    json_integer(node->power))) {
		REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
	}
	if (gw_subdev_array(node->subdevices.lh_first, &subdev_j)) {
		log_warn("couldn't make subdevice array");
		return -1;
	}
	if (json_object_set_new(data_j, "subdevices", subdev_j)) {
		REQUIRE_FAILED(REQUIRE_MSG_ALLOCATION);
	}
	json_object_set_new(node_j, "node", data_j);
	if (node_json) {
		*node_json = node_j;
	}
	return 0;
}

/*
 * Free subdevices, templates, and props of a node. If the free_strs flag is set
 * to 1, then the oem_model, sw_version, and any metadata will also be freed.
 */
int gw_node_free(struct gw_node *node, int free_strs)
{
	struct gw_node_subdevice *subdev;
	struct gw_subdevice_template *template;
	struct gw_template_prop *prop;

	if (!node) {
		return 0;
	}
	node->initialized = 0;
	if (free_strs) {
		free(node->nodemeta);
		free(node->oem_model);
		free(node->sw_version);
	}
	while ((subdev = LIST_FIRST(&node->subdevices)) != NULL) {
		while ((template = LIST_FIRST(&subdev->templates)) != NULL) {
			while ((prop = LIST_FIRST(&template->props)) != NULL) {
				LIST_REMOVE(prop, link);
				if (free_strs) {
					free(prop->propmeta);
				}
				free(prop);
			}
			LIST_REMOVE(template, link);
			free(template);
		}
		if (free_strs) {
			free(subdev->submeta);
		}
		LIST_REMOVE(subdev, link);
		free(subdev);
	}


	return 0;
}

/*
 * Create a subdevice list based on a json array
 */
static int gw_json_to_subdev_array(struct gw_node *node,
	struct gw_node_subdevice *subdevq, json_t *subdev_arr_j)
{
	struct gw_node_subdevice *subdev;
	struct gw_subdevice_template *template;
	struct gw_template_prop *prop;
	json_t *subdev_j;
	json_t *subsubdev_arr_j;
	json_t *templates_arr_j;
	json_t *template_j;
	json_t *props_arr_j;
	json_t *prop_j;
	const char *str;
	const char *ver;
	int i;
	int j;
	int k;
	int rc = 0;

	for (i = 0; i < json_array_size(subdev_arr_j); i++) {
		subdev_j = json_array_get(subdev_arr_j, i);
		str = json_get_string(subdev_j, "subdevice_key");
		subdev = gw_subdev_add(node, str);
		if (!subdev) {
			continue;
		}
		subdev->submeta = json_get_string_dup(subdev_j, "submeta");
		templates_arr_j = json_object_get(subdev_j, "templates");
		for (j = 0; j < json_array_size(templates_arr_j); j++) {
			template_j = json_array_get(templates_arr_j, j);
			str = json_get_string(template_j, "template_key");
			ver = json_get_string(template_j, "version");
			template = gw_template_add(subdev, str, ver);
			if (!template) {
				continue;
			}
			props_arr_j = json_object_get(template_j, "properties");
			for (k = 0; k < json_array_size(props_arr_j); k++) {
				prop_j = json_array_get(props_arr_j, k);
				str = json_get_string(prop_j, "name");
				prop = gw_prop_add(template, str);
				if (!prop) {
					continue;
				}
				prop->propmeta = json_get_string_dup(prop_j,
				    "propmeta");
			}
		}
		subsubdev_arr_j = json_object_get(subdev_j, "subdevices");
		if (subsubdev_arr_j && json_is_array(subsubdev_arr_j)) {
			rc = gw_json_to_subdev_array(NULL, subdev,
			    subsubdev_arr_j);
			if (rc) {
				return rc;
			}
		}
	}

	return rc;
}

/*
 * Populate a gw_node object from the JSON representation of the node. The
 * oem_model, sw_version, and metadatas are all dynamically allocated. The
 * application can free the gw_node structure by calling gw_node_free with
 * the "free_strs" flag set to 1.
 */
int gw_json_to_node(struct gw_node *node, json_t *data)
{
	json_t *subdev_arr_j;
	u8 value;
	json_t *node_j;
	const char *addr;
	char *old_oem_model;

	if (!data || !node) {
		return -1;
	}
	node_j = json_object_get(data, "node");
	if (!node_j) {
		log_warn("missing node object");
		return -1;
	}
	addr = json_get_string(node_j, "address");
	if (!addr || !strlen(addr)) {
		log_warn("node must have an addr");
		return -1;
	}
	if (gw_node_init(node, addr)) {
		return -1;
	}
	subdev_arr_j = json_object_get(node_j, "subdevices");
	if (!json_is_array(subdev_arr_j) || !json_array_size(subdev_arr_j))  {
		log_warn("node must have subdevices");
		return -1;
	}
	if (json_get_uint8(node_j, "interface", &value) < 0) {
		log_warn("node must have an interface");
		return -1;
	}
	node->interface = (enum gw_interface)value;
	if (json_get_uint8(node_j, "power", &value) < 0) {
		log_warn("node must have power");
		return -1;
	}
	node->power = (enum gw_power)value;
	node->oem_model = json_get_string_dup(node_j, "oem_model");
	if (!node->oem_model) {
		/* XXX: we used to call this manuf_model */
		node->oem_model = json_get_string_dup(node_j, "manuf_model");
	}
	if (!node->oem_model) {
		/* XXX: we used to call this model */
		node->oem_model = json_get_string_dup(node_j, "model");
	}
	if (node->oem_model &&
	    (strlen(node->oem_model) > GW_MAX_OEM_MODEL_SIZE)) {
		log_warn("node oem model %s too long, truncating...",
		    node->oem_model);
		old_oem_model = node->oem_model;
		node->oem_model = malloc(GW_MAX_OEM_MODEL_SIZE);
		snprintf(node->oem_model, GW_MAX_OEM_MODEL_SIZE, "%s",
		    old_oem_model);
		free(old_oem_model);
	}
	node->sw_version = json_get_string_dup(node_j, "sw_version");
	node->nodemeta = json_get_string_dup(node_j, "nodemeta");

	if (gw_json_to_subdev_array(node, NULL, subdev_arr_j)) {
		log_warn("couldn't make subdevice array");
		return -1;
	}

	return 0;
}

/*
 * Allocate a new subdevice for gw_node and return pointer to it.
 */
struct gw_node_subdevice *gw_subdev_add(struct gw_node *node, const char *key)
{
	struct gw_node_subdevice *subdev;

	if (!node->initialized) {
		log_warn("node not initialized");
		return NULL;
	}
	if (!key || !strlen(key) || strlen(key) > GW_SUBDEVICE_KEY_SIZE - 1) {
		log_warn("bad key for subdevice");
		return NULL;
	}
	subdev = calloc(1, sizeof(*subdev));
	REQUIRE(subdev, REQUIRE_MSG_ALLOCATION);
	LIST_INIT(&subdev->templates);
	LIST_INSERT_HEAD(&node->subdevices, subdev, link);
	snprintf(subdev->subdevice_key, sizeof(subdev->subdevice_key), "%s",
	    key);
	return subdev;
}

/*
 * Allocate a new template for gw_node_subdevice and return pointer to it.
 * 'key' is required. Optionally also give a "version" of the template.
 */
struct gw_subdevice_template *gw_template_add(struct gw_node_subdevice *subdev,
				const char *key, const char *ver)
{
	struct gw_subdevice_template *template;

	if (!key || !strlen(key) || strlen(key) > GW_TEMPLATE_KEY_SIZE - 1) {
		log_warn("invalid key for template");
		return NULL;
	}
	if (ver && (strlen(ver) >= GW_TEMPLATE_VER_SIZE)) {
		log_warn("ver for template too long");
		return NULL;
	}
	template = calloc(1, sizeof(*template));
	REQUIRE(template, REQUIRE_MSG_ALLOCATION);
	LIST_INIT(&template->props);
	LIST_INSERT_HEAD(&subdev->templates, template, link);
	snprintf(template->template_key, sizeof(template->template_key), "%s",
	    key);
	if (ver) {
		snprintf(template->template_ver, sizeof(template->template_ver),
		    "%s", ver);
	}
	return template;
}

/*
 * Allocate a new property for gw_subdevice_template and return pointer to it.
 * By default, every property of a template is added to the node. However, if
 * the application only wants to use a subset of the properties, it can use
 * this function to list the properties from the template that are to be used.
 */
struct gw_template_prop *gw_prop_add(struct gw_subdevice_template *template,
					const char *name)
{
	struct gw_template_prop *prop;

	if (!name || !strlen(name) ||
	    strlen(name) > GW_PROPERTY_NAME_SIZE - 1) {
		log_warn("bad name for prop");
		return NULL;
	}
	prop = calloc(1, sizeof(*prop));
	REQUIRE(prop, REQUIRE_MSG_ALLOCATION);
	LIST_INSERT_HEAD(&template->props, prop, link);
	snprintf(prop->name, sizeof(prop->name), "%s", name);
	return prop;
}

/*
 * Initialize a struct gw_node with a given address.
 */
int gw_node_init(struct gw_node *node, const char *addr)
{
	if (!node) {
		log_warn("invalid node");
		return -1;
	}
	if (!addr || (strlen(addr) > GW_NODE_ADDR_SIZE)) {
		log_warn("bad address for node");
		return -1;
	}
	memset(node, 0, sizeof(*node));
	LIST_INIT(&node->subdevices);
	snprintf(node->addr, sizeof(node->addr), "%s", addr);
	node->initialized = 1;

	return 0;
}

/*
 * Handler for batch operations
 */
static int gw_node_prop_batch_handler(void *arg, int *req_id,
				int confirm_needed)
{
	struct gw_node_prop_batch_sent_list *batch_sent_list = arg;
	struct gw_node_prop_batch_entry *batch_entry;
	struct gw_state *gstate = &gw_state;
	json_t *args;
	json_t *dp_info;
	json_t *batch_dp;
	u8 confirm_opt_bkup;
	int request_id;
	int rc;

	if (!gstate->gw_initialized) {
		return -1;
	}

	args = json_array();
	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	STAILQ_FOREACH(batch_entry, &batch_sent_list->batch_list->batchq,
	    link) {
		dp_info = json_object_get(batch_entry->gw_cmd->data,
		    "property");
		batch_dp = json_object();
		REQUIRE(batch_dp, REQUIRE_MSG_ALLOCATION);
		json_object_set(batch_dp, "property", dp_info);
		json_object_set_new(batch_dp, "batch_id",
		    json_integer(batch_entry->entry_id));
		json_array_append_new(args, batch_dp);
	}
	/*
	 * If confirm_needed is set, that overrides opts.confirm
	 */
	confirm_opt_bkup = batch_sent_list->opts.confirm;
	batch_sent_list->opts.confirm = confirm_needed;
	rc = data_send_cmd(JINT_PROTO_GATEWAY, gateway_ops[AG_PROP_BATCH_SEND],
	    args, 0, &request_id, &batch_sent_list->opts);
	batch_sent_list->opts.confirm = confirm_opt_bkup;
	if (req_id) {
		*req_id = request_id;
	}
	batch_sent_list->sent_req_id = request_id;
	if (rc) {
		log_warn("batch_send handler failed");
		return -1;
	}
	return 0;
}

/*
 * Handle queued gateway operations
 */
static int gw_cmd_handler(void *arg, int *req_id, int confirm_needed)
{
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd = arg;
	json_t *args;
	u8 confirm_opt_bkup;
	int request_id;
	int rc;

	if (!gstate->gw_initialized) {
		return -1;
	}

	if (!json_is_array(gw_cmd->data)) {
		args = json_array();
		REQUIRE(args, REQUIRE_MSG_ALLOCATION);
		json_array_append(args, gw_cmd->data);
	} else {
		args = json_incref(gw_cmd->data);
	}
	/*
	 * If confirm_needed is set, that overrides opts.confirm
	 */
	confirm_opt_bkup = gw_cmd->opts.confirm;
	gw_cmd->opts.confirm = confirm_needed;
	rc = data_send_cmd(JINT_PROTO_GATEWAY, gateway_ops[gw_cmd->op], args,
	    gw_cmd->req_id, &request_id, &gw_cmd->opts);
	gw_cmd->opts.confirm = confirm_opt_bkup;
	if (req_id) {
		*req_id = request_id;
	}
	if (rc) {
		log_warn("handler failed");
		return -1;
	}
	return 0;
}

/*
 * Free the components of a gw_node_prop_dp_nonconst
 */
static void gw_node_prop_dp_free(void *arg)
{
	struct gw_node_prop_dp_nonconst *dp = arg;
	struct gw_node_prop_nonconst *prop;

	if (!dp) {
		return;
	}
	prop = dp->prop;
	if (!prop) {
		return;
	}
	free(prop->addr);
	free(prop->subdevice_key);
	free(prop->template_key);
	free(prop->name);
	free(prop);
	dp->prop = NULL;
	free(dp);
}

/*
 * Free the components of a gw_node_ota_info
 */
static void gw_node_ota_info_free(void *arg)
{
	struct gw_node_ota_info_nonconst *ota_info = arg;

	free(ota_info->addr);
	free(ota_info->version);
	free(ota_info->save_location);
	free(ota_info);
}

/*
 * Free gw_cmd
 */
static void gw_cmd_free(void *arg)
{
	struct gw_cmd *gw_cmd = arg;

	if (!gw_cmd) {
		return;
	}
	json_decref(gw_cmd->data);
	if (gw_cmd->free_arg_handler) {
		gw_cmd->free_arg_handler(gw_cmd->arg);
	}
	prop_metadata_free(gw_cmd->opts.metadata);
	free(gw_cmd);
}

/*
 * Mark ADS failure for a node property
 */
static void gw_node_ads_failure(struct gw_node_prop_entry *entry,
	struct gw_node_prop_dp *node_prop_dp, struct op_options *opts)
{
	struct op_options opts_bkup;

	if (!node_prop_dp) {
		return;
	}
	if (!opts) {
		memset(&opts_bkup, 0, sizeof(opts_bkup));
		opts_bkup.dev_time_ms = ops_get_system_time_ms();
		opts = &opts_bkup;
	}
	if (entry) {
		entry->ads_failure = 1;
	}
	if (entry && entry->ads_failure_cb) {
		entry->ads_failure_cb(AG_PROP_SEND, CAT_NODEPROP_DP,
		    node_prop_dp, opts);
	} else if (gw_cloud_fail_handler) {
		gw_cloud_fail_handler(AG_PROP_SEND, CAT_NODEPROP_DP,
		    node_prop_dp, opts);
	}
}

/*
 * Calls confirm or ads failure handlers for CAT_NODEPROP_DP gw_cmds
 */
static int gw_nodeprop_dp_handle(struct gw_cmd *gw_cmd,
	enum confirm_status status, enum confirm_err err,
	int dests, int for_confirm)
{
	struct confirm_info confirm_info = {.status = status, .err = err,
	    .dests = dests};
	struct gw_node_prop_dp_nonconst *orig_dp =
	    (struct gw_node_prop_dp_nonconst *)gw_cmd->arg;
	struct gw_node_prop_dp prop_info = {.type = orig_dp->type,
	    .val_len = orig_dp->val_len};
	struct gw_node_prop prop;
	struct gw_node_prop_entry *entry;
	struct data_obj dataobj;
	const void *dataobj_val;

	if (!gw_cmd || !gw_cmd->opts.confirm) {
		return 0;
	}
	prop.addr = orig_dp->prop->addr;
	prop.subdevice_key = orig_dp->prop->subdevice_key;
	prop.template_key = orig_dp->prop->template_key;
	prop.name = orig_dp->prop->name;
	prop_info.prop = &prop;
	if (data_json_to_value(&dataobj, orig_dp->val_j,
	    orig_dp->type, &dataobj_val)) {
		return 0;
	}
	prop_info.val = dataobj_val;
	entry = gw_node_prop_lookup(prop_info.prop);
	if (for_confirm) {
		if (entry && entry->confirm_cb) {
			entry->confirm_cb(gw_cmd->op, gw_cmd->arg_type,
			    &prop_info, &gw_cmd->opts, &confirm_info);
		} else if (gw_confirm_handler) {
			gw_confirm_handler(gw_cmd->op, gw_cmd->arg_type,
			    &prop_info, &gw_cmd->opts, &confirm_info);
		}
	} else {
		gw_node_ads_failure(entry, &prop_info, &gw_cmd->opts);
	}
	if (orig_dp->type == PROP_BLOB) {
		free(dataobj.val.decoded_val);
	}

	return 0;
}

/*
 * Confirmation handler for batch sends
 */
static int gw_batch_confirm_cb(void *arg, enum confirm_status status,
	enum confirm_err err, int dests)
{
	struct confirm_info confirm_info = {.status = status, .err = err,
	    .dests = dests};
	struct gw_node_prop_batch_sent_list *batch_sent_list = arg;
	struct gw_node_prop_batch_entry *entry;
	struct gw_cmd *gw_cmd;
	u8 has_naks = 0; /* 1 if any naks were recvd for the batch send */

	if (!batch_sent_list) {
		return 0;
	}
	/*
	 * Go through every datapoint to see which need confirmation
	 */
	STAILQ_FOREACH(entry, &batch_sent_list->batch_list->batchq, link) {
		gw_cmd = entry->gw_cmd;
		if (gw_cmd->arg_type != CAT_NODEPROP_DP) {
			log_warn("bad arg type");
			continue;
		}
		if (status == CONF_STAT_SUCCESS) {
			if (entry->recvd_nak) {
				/* this entry failed, send confirmation fail */
				has_naks = 1;
				confirm_info.status = CONF_STAT_FAIL;
				confirm_info.err = CONF_ERR_CONN;
			}
			gw_nodeprop_dp_handle(entry->gw_cmd,
			    confirm_info.status, confirm_info.err, DEST_ADS, 1);
			confirm_info.status = status;
			confirm_info.err = err;
		} else {
			gw_nodeprop_dp_handle(entry->gw_cmd,
			    CONF_STAT_FAIL, err, dests, 1);
		}
	}
	if (batch_sent_list->opts.confirm && gw_confirm_handler) {
		if (status == CONF_STAT_SUCCESS && has_naks) {
			/* if one of the entries failed, mark it partial */
			confirm_info.status = CONF_STAT_PARTIAL_SUCCESS;
		} else {
			confirm_info.status = status;
		}
		if (confirm_info.status == CONF_STAT_SUCCESS) {
			confirm_info.dests =
			    prop_set_confirm_dests_mask(batch_sent_list->
			    opts.dests);
		}
		gw_confirm_handler(AG_PROP_BATCH_SEND, CAT_BATCH_ID,
		    &batch_sent_list->batch_id, &batch_sent_list->opts,
		    &confirm_info);
	}
	return 0;
}

/*
 * Process NAK for a batch entry
 */
static int gw_batch_nak_cb(void *arg, int req_id, enum confirm_err err,
			json_t *obj_j)
{
	struct gw_node_prop_batch_sent_list *batch_sent_list = arg;
	struct gw_node_prop_batch_entry *entry;
	json_t *op_args;
	int batch_id;

	if (!batch_sent_list) {
		return 0;
	}
	op_args = json_object_get(obj_j, "op_args");
	if (!op_args || json_get_int(op_args, "batch_id", &batch_id) < 0) {
		/*
		 * Individual batch_id is not included in the nak if the
		 * entire batch failed.
		 */
		batch_id = -1;
	}
	/* Process failure for all failed entries */
	STAILQ_FOREACH(entry, &batch_sent_list->batch_list->batchq, link) {
		if ((batch_id == -1 || entry->entry_id == batch_id) &&
		    entry->gw_cmd->arg_type == CAT_NODEPROP_DP) {
			entry->recvd_nak = 1;
			/* ADS failure only if this is a conn err */
			if (err == CONF_ERR_CONN) {
				gw_nodeprop_dp_handle(entry->gw_cmd,
				    CONF_STAT_FAIL, err, DEST_ADS, 0);
			}
			if (entry->entry_id == batch_id) {
				/*
				 * In the case of a partial batch failure,
				 * an individual nak will be sent for each
				 * failed property.
				 */
				break;
			}
		}
	}
	if (batch_id != -1 && !entry) {
		log_debug("received nak for unknown batch_id: %d", batch_id);
		return -1;
	}

	return 0;
}

/*
 * Call gw_confirm_handler once a node ota has been successfully downloaded or
 * discarded.
 */
static void gw_node_ota_confirm(struct gw_cmd *gw_cmd,
	const struct confirm_info *confirm_info)
{
	struct gw_node_ota_info_nonconst *info_nonconst =
	    (struct gw_node_ota_info_nonconst *)gw_cmd->arg;
	struct gw_node_ota_info node_ota_info = {.addr = info_nonconst->addr,
	    .version = info_nonconst->version,
	    .save_location = info_nonconst->save_location};

	if (!gw_confirm_handler || !gw_node_ota_handler) {
		return;
	}
	gw_confirm_handler(gw_cmd->op, gw_cmd->arg_type, &node_ota_info,
	    &gw_cmd->opts, confirm_info);
}

/*
 * Confirmation handler. Calls the appropriate confirm handler if defined
 */
static int gw_confirm_cb(void *arg, enum confirm_status status,
	enum confirm_err err, int dests)
{
	struct confirm_info confirm_info = {.status = status, .err = err,
	    .dests = dests};
	struct gw_cmd *gw_cmd = arg;

	if (!gw_cmd || !gw_cmd->opts.confirm) {
		return 0;
	}
	if (status == CONF_STAT_SUCCESS) {
		dests = prop_set_confirm_dests_mask(gw_cmd->opts.dests);
		confirm_info.dests = dests;
	}
	if (gw_cmd->arg_type == CAT_NODEPROP_DP) {
		gw_nodeprop_dp_handle(gw_cmd, status, err, dests, 1);
	} else if (gw_cmd->arg_type == CAT_NODE_OTA_INFO) {
		gw_node_ota_confirm(gw_cmd, &confirm_info);
	} else if (gw_confirm_handler) {
		gw_confirm_handler(gw_cmd->op, gw_cmd->arg_type, gw_cmd->arg,
		    &gw_cmd->opts, &confirm_info);
	}

	return 0;
}

/*
 * Setup confirm information in gw_cmd (used for node_add and conn_status)
 */
static void gw_setup_addr_confirmation(const char *addr,
	struct gw_cmd *gw_cmd, const struct op_options *opts)
{
	gw_cmd->arg = strdup(addr);
	gw_cmd->arg_type = CAT_ADDR;
	gw_cmd->free_arg_handler = free;
	if (opts) {
		memcpy(&gw_cmd->opts, opts, sizeof(gw_cmd->opts));
	}
	if (!gw_cmd->opts.dev_time_ms) {
		gw_cmd->opts.dev_time_ms = ops_get_system_time_ms();
	}
}

/*
 * Delete all schedules for a node
 */
static void gw_delete_all_scheds(const char *addr)
{
	json_t *scheds;
	json_t *sched;
	json_t *sched_arg;
	const char *sched_addr;
	int deleted = 0;
	int i;
	int rc;

	scheds = sched_get_json_form_of_scheds(GATEWAY_SUBSYSTEM_ID);
	for (i = 0; i < json_array_size(scheds); i++) {
		sched = json_array_get(scheds, i);
		sched_arg = json_object_get(sched, "arg");
		sched_addr = json_get_string(sched_arg, "address");
		if (!strcmp(sched_addr, addr)) {
			sched_remove_schedule(GATEWAY_SUBSYSTEM_ID,
			    json_get_string(sched, "name"));
			json_array_remove(scheds, i);
			i--;
			deleted = 1;
		}
	}
	if (deleted) {
		rc = conf_set(GATEWAY_SCHEDULES, scheds);
		if (rc < 0) {
			log_err("failed to set %s", GATEWAY_SCHEDULES);
		} else if (!rc) {
			conf_save();
		}
	}
	json_decref(scheds);

}

/*
 * Process NAK for a gateway packet
 */
static int gw_nak_cb(void *arg, int req_id, enum confirm_err err,
			    json_t *obj_j)
{
	struct gw_cmd *gw_cmd = arg;

	if (!gw_cmd) {
		return 0;
	}
	if (err != CONF_ERR_CONN) {
		/* don't call ads failure if this isn't due to conn err */
		return 0;
	}
	if (gw_cmd->arg_type == CAT_NODEPROP_DP) {
		gw_nodeprop_dp_handle(gw_cmd, CONF_STAT_FAIL, CONF_ERR_CONN,
		    DEST_ADS, 0);
	} else if (gw_cloud_fail_handler) {
		gw_cloud_fail_handler(gw_cmd->op, gw_cmd->arg_type,
		    gw_cmd->arg, &gw_cmd->opts);
	}
	return 0;
}

/*
 * Helper function for node adds/updates
 */
static int gw_node_add_update_helper(struct gw_node *node, json_t **node_info_j,
	const struct op_options *opts, enum ayla_gateway_op op)
{
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd;
	int rc;

	if (!gstate->gw_initialized) {
		log_warn("gw not initialized");
		return -1;
	}
	if (!node->initialized) {
		log_warn("node not initialized");
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = op;
	rc = gw_node_to_json(node, &gw_cmd->data);
	if (rc) {
		log_warn("couldn't create json object");
		return -1;
	}
	if (node_info_j) {
		*node_info_j = json_incref(gw_cmd->data);
	}
	if (op == AG_NODE_ADD) {
		/* so we don't fire old scheds */
		gw_delete_all_scheds(node->addr);
	}
	gw_setup_addr_confirmation(node->addr, gw_cmd, opts);
	ops_add(gw_cmd_handler, gw_cmd, gw_nak_cb, gw_confirm_cb, gw_cmd_free);

	return 0;
}

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
	const struct op_options *opts)
{
	return gw_node_add_update_helper(node, node_info_j, opts, AG_NODE_ADD);
}


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
	const struct op_options *opts)
{
	return gw_node_add_update_helper(node, node_info_j, opts,
	    AG_NODE_UPDATE);
}

/*
 * Remove a node that has been added to the gateway.
 * The "confirm" option can only be used if the handler function has been
 * set using *gw_confirm_handler_set*.
 */
int gw_node_remove(const char *addr, const struct op_options *opts)
{
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd;
	json_t *info_j;

	if (!gstate->gw_initialized) {
		log_warn("gw not initialized");
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	info_j = json_object();
	REQUIRE(info_j, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(info_j, "address", json_string(addr));
	gw_cmd->data = info_j;
	gw_setup_addr_confirmation(addr, gw_cmd, opts);
	gw_cmd->op = AG_NODE_REMOVE;
	ops_add(gw_cmd_handler, gw_cmd, gw_nak_cb, gw_confirm_cb, gw_cmd_free);

	return 0;
}

/*
 * Check if two gw_node_prop's are equal. A NULL is treated as a wildcard in
 * prop1. prop2 cannot have wildcards.
 */
static int gw_node_prop_cmp(const struct gw_node_prop *prop1,
			     const struct gw_node_prop *prop2)
{
	if (!prop2->addr || (prop1->addr && strcmp(prop1->addr, prop2->addr))) {
		return 1;
	}
	if (!prop2->subdevice_key || (prop1->subdevice_key &&
	    strcmp(prop1->subdevice_key, prop2->subdevice_key))) {
		return 1;
	}
	if (!prop2->template_key || (prop1->template_key &&
	    strcmp(prop1->template_key, prop2->template_key))) {
		return 1;
	}
	if (!prop2->name || (prop1->name && strcmp(prop1->name, prop2->name))) {
		return 1;
	}

	return 0;
}

/*
 * Set the connection status for a node. "status" should be 1 or 0.
 */
int gw_node_conn_status_send(const char *addr, u8 status,
	const struct op_options *opts)
{
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd;
	json_t *status_info_j;

	if (!gstate->gw_initialized) {
		log_warn("gw not initialized");
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	status_info_j = json_object();
	REQUIRE(status_info_j, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(status_info_j, "address", json_string(addr));
	json_object_set_new(status_info_j, "status",
	    status ? json_true() : json_false());
	gw_cmd->data = status_info_j;
	gw_setup_addr_confirmation(addr, gw_cmd, opts);
	gw_cmd->op = AG_CONN_STATUS;
	ops_add(gw_cmd_handler, gw_cmd, gw_nak_cb, gw_confirm_cb, gw_cmd_free);

	return 0;
}

/*
 * Copy gw_node_prop structures
 */
static void gw_node_prop_copy(struct gw_node_prop_nonconst *dest,
	const struct gw_node_prop *src)
{
	if (!src || !dest) {
		return;
	}
	free(dest->addr);
	free(dest->subdevice_key);
	free(dest->template_key);
	free(dest->name);
	memset(dest, 0, sizeof(*dest));
	if (src->addr) {
		dest->addr = strdup(src->addr);
	}
	if (src->subdevice_key) {
		dest->subdevice_key = strdup(src->subdevice_key);
	}
	if (src->template_key) {
		dest->template_key = strdup(src->template_key);
	}
	if (src->name) {
		dest->name = strdup(src->name);
	}
}

/*
 * Check for gw_node_prop validity. gw_node_prop structs coming from appd
 * must have all values filled in.
 */
static int gw_node_prop_is_valid(const struct gw_node_prop *prop)
{
	if (!prop || !prop->addr || !prop->subdevice_key ||
	    !prop->template_key || !prop->name) {
		return 0;
	}
	return 1;
}

/*
 * Converts a gw_node_prop to a prop_obj json structure
 */
static json_t *gw_node_prop_to_prop_obj(const struct gw_node_prop *prop)
{
	json_t *prop_obj;
	json_t *prop_info;

	if (!gw_node_prop_is_valid(prop)) {
		log_warn("invalid gw_node_prop");
		return NULL;
	}
	prop_obj = json_object();
	REQUIRE(prop_obj, REQUIRE_MSG_ALLOCATION);
	prop_info = json_object();
	REQUIRE(prop_info, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(prop_info, "address", json_string(prop->addr));
	json_object_set_new(prop_info, "subdevice_key",
	    json_string(prop->subdevice_key));
	json_object_set_new(prop_info, "template_key",
	    json_string(prop->template_key));
	json_object_set_new(prop_info, "name", json_string(prop->name));
	json_object_set_new(prop_obj, "property", prop_info);

	return prop_obj;
}

/*
 * Request a node property value from the cloud
 */
int gw_node_prop_request(struct gw_node_prop *prop)
{
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd;
	json_t *prop_obj;

	if (!gstate->gw_initialized) {
		log_warn("gw not initialized");
		return -1;
	}
	prop_obj = gw_node_prop_to_prop_obj(prop);
	if (!prop_obj) {
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = AG_PROP_REQ;
	gw_cmd->data = prop_obj;
	ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);

	return 0;
}

/*
 * Helper function for request all and request all to-device props
 */
static int gw_node_prop_request_many_helper(enum ayla_gateway_op op,
					const char *addr)
{
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd;

	if (!gstate->gw_initialized) {
		log_warn("gw not initialized");
		return -1;
	}
	if (!addr) {
		log_warn("bad addr");
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = op;
	gw_cmd->data = json_object();
	REQUIRE(gw_cmd->data, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(gw_cmd->data, "address", json_string(addr));
	ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);

	return 0;
}

/*
 * Request values of all node properties from the
 * cloud
 */
int gw_node_prop_request_all(const char *addr)
{
	return gw_node_prop_request_many_helper(AG_PROP_REQ_ALL, addr);
}

/*
 * Request values of all to-device node properties
 * from the cloud. This function could be useful in making sure the device is in
 * sync with the cloud at bootup.
 */
int gw_node_prop_request_to_dev(const char *addr)
{
	return gw_node_prop_request_many_helper(AG_PROP_REQ_TO_DEV, addr);
}

/*
 * Return a new gw_cmd object for a property send/resp operation
 */
static int gw_cmd_create(const struct gw_node_prop *prop, enum prop_type type,
	const void *val, size_t val_len, int req_id,
	const struct op_options *opts, struct gw_cmd **gw_cmd_ptr)
{
	struct gw_node_prop_dp_nonconst *prop_dp_blk;
	struct gw_state *gstate = &gw_state;
	struct gw_cmd *gw_cmd;
	json_t *prop_obj;
	json_t *prop_data;
	json_t *val_obj;

	if (!gstate->gw_initialized) {
		log_warn("gw not initialized");
		return -1;
	}
	if (!val) {
		return -1;
	}
	if (type == PROP_STRING) {
		val_len = strlen((char *)val);
	} else if (!val_len) {
		return -1;
	}
	val_obj = data_type_to_json_obj(val, val_len, type);
	if (!val_obj) {
		return -1;
	}
	prop_obj = gw_node_prop_to_prop_obj(prop);
	if (!prop_obj) {
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = req_id ? AG_PROP_RESP : AG_PROP_SEND;
	gw_cmd->req_id = req_id;
	gw_cmd->data = prop_obj;
	prop_data = json_object_get(prop_obj, "property");
	json_object_set_new(prop_data, "base_type",
	    json_string(data_types[type]));

	json_object_set_new(prop_data, "value", val_obj);
	if (gw_cmd->op == AG_PROP_SEND && opts) {
		gw_cmd->opts = *opts;
		if (opts->metadata && opts->metadata->num_entries) {
			/* Duplicate metadata for use by confirm callback */
			gw_cmd->opts.metadata =
			    prop_metadata_dup(opts->metadata);
			/* Add metadata to the property JSON object */
			json_object_set_new(prop_data, "metadata",
			    prop_metadata_to_json(opts->metadata));
		}
	}
	if (!gw_cmd->opts.dev_time_ms) {
		gw_cmd->opts.dev_time_ms = ops_get_system_time_ms();
	}
	json_object_set_new(prop_data, "dev_time_ms",
	    json_integer(gw_cmd->opts.dev_time_ms));
	if (gw_cmd->op == AG_PROP_SEND) {
		/*
		 * make a copy of the property information so we can
		 * use it on the confirm callback.
		 */
		prop_dp_blk =
		    calloc(1, sizeof(struct gw_node_prop_dp_nonconst));
		prop_dp_blk->prop =
		    calloc(1, sizeof(struct gw_node_prop_nonconst));
		gw_node_prop_copy(prop_dp_blk->prop, prop);
		prop_dp_blk->type = type;
		prop_dp_blk->val_j = val_obj;
		prop_dp_blk->val_len = val_len;
		gw_cmd->arg = prop_dp_blk;
		gw_cmd->arg_type = CAT_NODEPROP_DP;
		gw_cmd->free_arg_handler = gw_node_prop_dp_free;
	}
	*gw_cmd_ptr = gw_cmd;

	return 0;
}

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
	const struct op_options *opts)
{
	struct gw_cmd *gw_cmd;
	int rc;

	log_debug("%s::%s:%s:%s", prop->addr, prop->subdevice_key,
	    prop->template_key, prop->name);
	rc = gw_cmd_create(prop, type, val, val_len, req_id, opts, &gw_cmd);
	if (rc) {
		return rc;
	}
	if (gw_cmd->op == AG_PROP_SEND) {
		ops_add(gw_cmd_handler, gw_cmd, gw_nak_cb, gw_confirm_cb,
		    gw_cmd_free);
	} else {
		ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);
	}

	return 0;
}

/*
 * Helper function to free the prop_batch_list
 */
static void gw_node_prop_batch_list_free_helper(struct gw_node_prop_batch_list
						*list)
{
	struct gw_node_prop_batch_entry *entry;

	if (!list) {
		return;
	}
	while ((entry = STAILQ_FIRST(&list->batchq)) != NULL) {
		STAILQ_REMOVE_HEAD(&list->batchq, link);
		gw_cmd_free(entry->gw_cmd);
		free(entry);
	}
	free(list);
}

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
 * batch updates for the cloud. In this case, you can call prop_arg_send for LAN
 * clients and prop_batch_send for just the cloud (by setting op_options).
 */
struct gw_node_prop_batch_list *gw_node_prop_batch_append(
		struct gw_node_prop_batch_list *list,
		const struct gw_node_prop *prop,
		enum prop_type type, const void *val, size_t val_len,
		const struct op_options *opts)
{
	struct gw_node_prop_batch_list *new_list;
	struct gw_node_prop_batch_entry *new_entry;
	struct gw_node_prop_batch_entry *entry;
	struct gw_node_prop_batch_entry *entry_before = NULL;
	int rc;
	int new_list_alloced = 0;
	struct gw_cmd *gw_cmd;

	if (type == PROP_FILE) {
		/* batching is not allowed for FILE properties */
		return NULL;
	}
	if (!list) {
		/* create a new queue */
		new_list = calloc(1, sizeof(*new_list));
		STAILQ_INIT(&new_list->batchq);
		list = new_list;
		new_list_alloced = 1;
	} else if (list->sent) {
		log_warn("cannot modify list after it's sent");
		return NULL;
	}
	rc = gw_cmd_create(prop, type, val, val_len, 0, opts, &gw_cmd);
	if (rc) {
		if (new_list_alloced) {
			gw_node_prop_batch_list_free_helper(new_list);
		}
		return NULL;
	}
	/*
	 * Insert the new update in timestamp order
	 */
	STAILQ_FOREACH(entry, &list->batchq, link) {
		if (entry->gw_cmd->opts.dev_time_ms >
		    gw_cmd->opts.dev_time_ms) {
			break;
		}
		entry_before = entry;
	}
	new_entry = calloc(1, sizeof(*new_entry));
	new_entry->entry_id = ++list->batchq_len;
	new_entry->gw_cmd = gw_cmd;
	if (entry_before) {
		STAILQ_INSERT_AFTER(&list->batchq, entry_before, new_entry,
		   link);
	} else {
		STAILQ_INSERT_HEAD(&list->batchq, new_entry, link);
	}
	log_debug("%s::%s:%s:%s appended to batch", prop->addr,
	    prop->subdevice_key, prop->template_key, prop->name);

	return list;
}

/*
 * Free the gw_node_prop_batch_list. Can be used to abort/free a batch before
 * being sent. This function must not be called after the batch has already been
 * sent. It should also not be called twice. To be safe, the lib takes away the
 * application's pointer to the batch by setting *list_ptr to NULL.
 */
void gw_node_prop_batch_list_free(struct gw_node_prop_batch_list **list_ptr)
{
	struct gw_node_prop_batch_list *list;

	if (!list_ptr) {
		return;
	}
	list = *list_ptr;
	if (!list) {
		return;
	}
	if (list->sent) {
		log_warn("app should not free a sent list");
		return;
	}
	gw_node_prop_batch_list_free_helper(list);
	*list_ptr = NULL;
}

/*
 * Free the prop_batch_sent_list
 */
static void gw_node_prop_batch_sent_list_free(void *arg)
{
	struct gw_node_prop_batch_sent_list *batch_sent_list = arg;

	gw_node_prop_batch_list_free_helper(batch_sent_list->batch_list);
	free(batch_sent_list);
}

/*
 * Send a node batch list. The application SHOULD NOT modify the batch list
 * after its sent (i.e. calling append on the list again). To be safe, the lib
 * takes away the application's pointer to the list by setting *list_ptr to
 * NULL. The application has op_options available to set *dests* for the batch,
 * etc. The *batch_id* arg can be optionally given to store the batch # assigned
 * for this batch. On success, 0 is returned.
 */
int gw_node_prop_batch_send(struct gw_node_prop_batch_list **list_ptr,
		const struct op_options *opts, int *batch_id)
{
	struct gw_node_prop_batch_sent_list *batch_sent_list;
	struct gw_node_prop_batch_list *list;

	if (!list_ptr) {
		return -1;
	}
	list = *list_ptr;
	if (!list) {
		return -1;
	}
	if (STAILQ_EMPTY(&list->batchq)) {
		free(list);
		*list_ptr = NULL;
		return -1;
	}
	list->sent = 1;
	batch_sent_list = calloc(1, sizeof(*batch_sent_list));
	batch_sent_list->batch_list = list;
	batch_sent_list->batch_id = ++gw_node_prop_batch_sent_counter;
	if (batch_id) {
		*batch_id = batch_sent_list->batch_id;
	}
	if (opts) {
		memcpy(&batch_sent_list->opts, opts,
		    sizeof(batch_sent_list->opts));
	}
	if (!batch_sent_list->opts.dev_time_ms) {
		batch_sent_list->opts.dev_time_ms = ops_get_system_time_ms();
	}
	ops_add(gw_node_prop_batch_handler, batch_sent_list, gw_batch_nak_cb,
	    gw_batch_confirm_cb, gw_node_prop_batch_sent_list_free);
	*list_ptr = NULL;
	return 0;
}

/*
 * Takes a json object and fills in a gw_node_prop structure
 */
static void gw_json_to_node_prop(json_t *data,
    struct gw_node_prop *node_prop)
{
	if (!node_prop || !data) {
		return;
	}
	node_prop->addr = json_get_string(data, "address");
	node_prop->subdevice_key = json_get_string(data, "subdevice_key");
	node_prop->template_key = json_get_string(data, "template_key");
	node_prop->name = json_get_string(data, "name");
}

/*
 * Send a GATEWAY nak
 */
static void gw_send_nak(const char *err, int id)
{
	jint_send_nak(JINT_PROTO_GATEWAY, err, id);
}

/*
 * Handle update to a node property received from the cloud or a mobile app
 */
int gw_node_datapoint_set(struct gw_node_prop_entry *entry,
	struct gw_node_prop *node_prop, enum prop_type type, const void *val,
	size_t len, const struct op_args *args)
{
	struct gw_node_prop_dp node_prop_dp = {.prop = node_prop, .type = type,
	    .val = val, .val_len = len};
	int rc = -1;

	if (entry && entry->set) {
		rc = entry->set(node_prop, type, val, len, args);
	} else if (gw_node_prop_set_handler) {
		rc = gw_node_prop_set_handler(node_prop, type, val, len, args);
	}
	if (args->source != SOURCE_ADS && !ops_cloud_up()) {
		/* property update came from a LAN client and ADS is not up */
		gw_node_ads_failure(entry, &node_prop_dp, NULL);
	}

	return rc;
}

/*
 * Process a node property update
 */
static int gw_prop_update_process(json_t *cmd, int req_id)
{
	struct gw_node_prop_entry *entry;
	struct gw_node_prop node_prop;
	enum prop_type type;
	const char *type_str;
	int i;
	json_t *args;
	json_t *arg;
	json_t *val;
	json_t *propobj;
	json_t *metadata_j;
	const char *propmeta;
	struct data_obj dataobj;
	const void *dataobj_val;
	int rc;
	int ack_req;
	void *ack_arg = NULL;
	struct op_args op_args;
	json_t *opts_obj;
	int ack_managed;

	memset(&op_args, 0, sizeof(op_args));
	opts_obj = json_object_get(cmd, "opts");
	if (opts_obj) {
		json_get_uint8(opts_obj, "source", &op_args.source);
	}
	args = json_object_get(cmd, "args");
	if (!args || !json_is_array(args)) {
inval_args:
		gw_send_nak(JINT_ERR_INVAL_ARGS, req_id);
		return -1;
	}
	for (i = 0; i < json_array_size(args); i++) {
		arg = json_array_get(args, i);
		if (!json_is_object(arg)) {
			goto inval_args;
		}
		propobj = json_object_get(arg, "property");
		if (!propobj) {
			goto inval_args;
		}
		gw_json_to_node_prop(propobj, &node_prop);
		type_str = json_get_string(propobj, "base_type");
		if (!type_str) {
			gw_send_nak(JINT_ERR_INVAL_TYPE, req_id);
			continue;
		}
		type = (enum prop_type)jint_get_data_type(type_str);
		val = json_object_get(propobj, "value");
		if (!val) {
bad_value:
			if (ack_arg) {
				json_decref(ack_arg);
			}
			gw_send_nak(JINT_ERR_BAD_VAL, req_id);
			continue;
		}
		rc = -1;
		ack_arg = NULL;
		op_args.ack_arg = NULL;
		ack_managed = 0;
		ack_req = !!json_get_string(propobj, "id"); /* explicit ack */
		metadata_j = json_object_get(propobj, "metadata");
		if (json_is_object(metadata_j)) {
			propmeta = json_get_string(metadata_j, "propmeta");
			if (propmeta) {
				op_args.propmeta = propmeta;
			}
		}
		if (ack_req) {
			ack_arg = ops_prop_ack_json_create(JINT_PROTO_GATEWAY,
			    req_id, op_args.source, arg);
		}
		entry = gw_node_prop_lookup(&node_prop);
		if (entry && entry->set) {
			if (entry->type && entry->type != type) {
				gw_send_nak(JINT_ERR_INVAL_TYPE, req_id);
				if (ack_arg) {
					json_decref(ack_arg);
				}
				continue;
			}
			if (entry->app_manages_acks) {
				ack_managed = 1;
				op_args.ack_arg = ack_arg;
			}
			if (entry->pass_jsonobj) {
				rc = gw_node_datapoint_set(entry, &node_prop,
				    type, val, sizeof(json_t *), &op_args);
				goto ack_and_continue;
			}
			if (json_is_null(val)) {
				if (entry->reject_null) {
					goto bad_value;
				}
				rc = gw_node_datapoint_set(entry, &node_prop,
				    type, NULL, 0, &op_args);
				goto ack_and_continue;
			}
		} else if (gw_node_prop_set_handler) {
			if (gw_app_manages_acks) {
				ack_managed = 1;
				op_args.ack_arg = ack_arg;
			}
			if (json_is_null(val)) {
				rc = gw_node_datapoint_set(entry, &node_prop,
				    type, NULL, 0, &op_args);
				goto ack_and_continue;
			}
		} else {
			/* skip over props that don't have set defiend */
			continue;
		}
		if (data_json_to_value(&dataobj, val, type, &dataobj_val)) {
			goto bad_value;
		}
		rc = gw_node_datapoint_set(entry, &node_prop, type, dataobj_val,
		    dataobj.val_len, &op_args);
		if (type == PROP_BLOB) {
			free(dataobj.val.decoded_val);
		}
ack_and_continue:
		if (ack_arg && !ack_managed) {
			ops_prop_ack_send(ack_arg, rc, 0);
		}
	}

	return 0;
}

/*
 * Fire a scheduled event for a property
 */
static void gw_fire_schedule(char *name, enum prop_type type, void *val,
				size_t val_len, json_t *arg)
{
	int (*set_handler)(struct gw_node_prop *, enum prop_type,
	    const void *, size_t, const struct op_args *);
	int (*response_handler)(struct gw_node_prop *, int,
		const char *) = gw_node_prop_get_handler;
	struct gw_node_prop node_prop;
	struct gw_node_prop *prop = &node_prop;
	struct gw_node_prop_entry *entry;
	json_t *root;

	prop->addr = json_get_string(arg, "address");
	gateway_break_up_node_prop_name(name, &prop->subdevice_key,
	    &prop->template_key, &prop->name);

	entry = gw_node_prop_lookup(prop);
	if (entry && entry->set) {
		if (entry->type && entry->type != type) {
bad_sched:
			log_warn("unable to fire scheduled event for %s", name);
			return;
		}
		if (entry->pass_jsonobj) {
			switch (type) {
			case PROP_INTEGER:
			case PROP_BOOLEAN:
				root = json_integer(*((int *)val));
				break;
			case PROP_STRING:
				root = json_stringn((char *)val, val_len);
				break;
			default:
				log_warn("schedule event for %s is "
				    "unsupported type: %d", name, type);
				return;
			}
			entry->set(prop, type, root, sizeof(json_t *), NULL);
			if (entry->get) {
				response_handler = entry->get;
			}
			json_decref(root);
			return;
		}
		set_handler = entry->set;
	} else if (gw_node_prop_set_handler) {
		set_handler = gw_node_prop_set_handler;
	} else {
		goto bad_sched;
	}
	set_handler(prop, type, val, val_len, NULL);
	if (response_handler) {
		/* echo the property update to the cloud */
		response_handler(prop, 0, NULL);
	}
}

/*
 * Load gateway schedules
 */
static int gw_process_schedobj(json_t *sched, json_t *arg)
{
	const char *name;
	const char *value;
	const char *addr;

	name = json_get_string(sched, "name");
	addr = json_get_string(arg, "address");
	value = json_get_string(sched, "value");
	if (!name || !addr || !value) {
		log_warn("bad gateway_schedule");
		return -1;
	}
	sched_add_new_schedule(GATEWAY_SUBSYSTEM_ID, name, value, arg,
	    gw_fire_schedule);

	return 0;
}

/*
 * Handle an echo failure
 */
static int gw_echo_failure_process(const char *echo_name, const json_t *arg)
{
	struct gw_node_prop_entry *entry;
	struct gw_node_prop_dp node_prop_dp;
	struct gw_node_prop node_prop;
	json_t *op_args_j;
	json_t *op_arg_j;

	op_args_j = json_object_get(arg, "op_args");
	if (!json_is_array(op_args_j) || !json_array_size(op_args_j)) {
		log_warn("missing op args");
		return 0;
	}
	memset(&node_prop_dp, 0, sizeof(node_prop_dp));
	op_arg_j = json_array_get(op_args_j, 0);
	gw_json_to_node_prop(op_arg_j, &node_prop);
	node_prop_dp.prop = &node_prop;
	entry = gw_node_prop_lookup(&node_prop);
	gw_node_ads_failure(entry, &node_prop_dp, NULL);
	return 0;
}

/*
 * Handle a gateway operation
 */
enum app_parse_rc gw_cmd_parse(json_t *cmd, int recv_id)
{
	char sched_name[GW_NODE_ADDR_SIZE + PROP_NAME_LEN + 3];
	const char *opstr = json_get_string(cmd, "op");
	struct gw_node_prop_entry *entry;
	struct gw_cmd *gw_cmd;
	struct gw_node_prop node_prop;
	enum ayla_gateway_op op;
	json_t *args;
	json_t *arg;
	json_t *addresses_j;
	json_t *address_j;
	json_t *schedobj;
	const char *str;
	const char *data;
	json_t *status_resp_j;
	json_t *args_resp_j;
	json_t *prop_j;
	int i;
	int status;
	json_t *data_j, *reg_j;
	bool bstat;
	char *str_dbg;

	str_dbg = json_dumps(cmd, JSON_COMPACT);
	log_debug("%s", str_dbg);
	free(str_dbg);

	if (!opstr) {
		gw_send_nak(JINT_ERR_OP, recv_id);
		return APR_DONE;
	}
	op = gateway_op_get(opstr);
	if (op == AG_ACK) {
		return APR_DONE;
	}
	args = json_object_get(cmd, "args");
	if (op != AG_CONFIRM_TRUE && !json_is_array(args)) {
err:
		gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
		return APR_ERR;
	}
	switch (op) {
	case AG_CONN_STATUS_REQ:
		if (!gw_node_conn_get_handler) {
			break;
		}
		arg = json_array_get(args, 0);
		addresses_j = json_object_get(arg, "addresses");
		if (!json_is_array(addresses_j)) {
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		args_resp_j = json_array();
		REQUIRE(args_resp_j, REQUIRE_MSG_ALLOCATION);
		for (i = 0; i < json_array_size(addresses_j); i++) {
			address_j = json_array_get(addresses_j, i);
			if (!json_is_string(address_j)) {
				json_decref(args_resp_j);
				goto err;
			}
			str = json_string_value(address_j);
			status = gw_node_conn_get_handler(str);
			status_resp_j = json_object();
			REQUIRE(status_resp_j, REQUIRE_MSG_ALLOCATION);
			if (!status) {
				json_object_set(status_resp_j, "status",
				    json_false());
			} else if (status == 1) {
				json_object_set(status_resp_j, "status",
				    json_true());
			} else {
				log_warn("couldn't find addr %s", str);
				json_decref(status_resp_j);
				continue;
			}
			json_object_set(status_resp_j, "address", address_j);
			json_array_append_new(args_resp_j, status_resp_j);
		}
		gw_cmd = calloc(1, sizeof(*gw_cmd));
		REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
		gw_cmd->op = AG_CONN_STATUS_RESP;
		gw_cmd->data = args_resp_j;
		gw_cmd->req_id = recv_id;
		ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);
		break;
	case AG_PROP_REQ:
		/* can only handle one prop request at a time */
		arg = json_array_get(args, 0);
		if (!arg) {
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		prop_j = json_object_get(arg, "property");
		if (!prop_j) {
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		data  = json_get_string(prop_j, "data");
		if (!data || data[0] == '\0' || !strcmp(data, "none")) {
			data = NULL;
		}
		gw_json_to_node_prop(prop_j, &node_prop);
		entry = gw_node_prop_lookup(&node_prop);
		if (entry && entry->get) {
			status = entry->get(&node_prop, recv_id, data);
		} else if (gw_node_prop_get_handler) {
			status = gw_node_prop_get_handler(&node_prop,
			    recv_id, data);
		} else {
			log_warn("missing response function for prop");
			status = -1;
		}
		if (!status) {
			/* app will respond using gw_node_prop_send */
			break;
		}
		json_object_set_new(cmd, "op",
		    json_string(gateway_ops[AG_PROP_RESP]));
		json_object_set_new(prop_j, "status",
		    json_string(JINT_ERR_UNKWN_PROP));
		gw_cmd = calloc(1, sizeof(*gw_cmd));
		REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
		gw_cmd->op = AG_PROP_RESP;
		gw_cmd->data = json_incref(arg);
		gw_cmd->req_id = recv_id;
		ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);
		break;
	case AG_PROP_UPDATE:
		gw_prop_update_process(cmd, recv_id);
		break;
	case AG_NODE_FACTORY_RST:
	case AG_NODE_OTA:
		arg = json_array_get(args, 0);
		str = json_get_string(arg, "address");
		if (!arg || !str) {
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		json_incref(arg);
		if (op == AG_NODE_FACTORY_RST) {
			if (!gw_node_rst_handler) {
				log_warn("missing fact rst handler");
				/*
				 * no function to hadle node factory reset
				 * automatically mark it failed
				 */
				gw_node_rst_cb(str, arg, 0, 0);
				break;
			}
			gw_node_rst_handler(str, arg);
		} else {
			/* node ota */
			if (!gw_node_ota_handler) {
				log_warn("missing node ota handler");
				/*
				 * no function to hadle node factory reset
				 * automatically discard it
				 */
				gw_node_ota_cb(str, arg, 0, 0);
				break;
			}
			data = json_get_string(arg, "version");
			gw_node_ota_handler(str, data, arg);
		}
		break;
	case AG_NODE_REG:
		arg = json_array_get(args, 0);
		str = json_get_string(arg, "address");
		if (!arg || !str) {
			log_warn("missing node reg address info");
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		data_j = json_object_get(arg, "cmd_data");
		if (!data_j) {
			log_warn("missing node reg cmd_data info");
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		reg_j = json_object_get(data_j, "registration");
		if (!reg_j) {
			log_warn("missing node reg registration info");
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		if (json_get_bool(reg_j, "status", &bstat)) {
			log_warn("missing node reg status");
			gw_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
			return APR_ERR;
		}
		json_incref(arg);
		if (!gw_node_reg_handler) {
			log_warn("missing node reg handler");
			/*
			 * no function to hadle node register statuc change,
			 * automatically mark it failed
			 */
			gw_node_reg_cb(str, arg, 0, 0);
			break;
		}
		gw_node_reg_handler(str, bstat, arg);
		break;
	case AG_SCHED_UPDATE:
		for (i = 0; i < json_array_size(args); i++) {
			arg = json_array_get(args, i);
			schedobj = json_object_get(arg, "schedule");
			if (!schedobj) {
				goto err;
			}
			str = json_get_string(schedobj, "address");
			if (!str) {
				goto err;
			}
			snprintf(sched_name, sizeof(sched_name), "%s_%s",
			    str, json_get_string(schedobj, "name"));
			data = json_get_string(schedobj, "value");
			address_j = json_object();
			REQUIRE(address_j, REQUIRE_MSG_ALLOCATION);
			json_object_set(address_j, "address",
			    json_object_get(schedobj, "address"));
			sched_add_new_schedule(GATEWAY_SUBSYSTEM_ID, sched_name,
			    data, address_j, gw_fire_schedule);
			json_decref(address_j);
		}
		schedobj = sched_get_json_form_of_scheds(GATEWAY_SUBSYSTEM_ID);
		if (!conf_set_new(GATEWAY_SCHEDULES, schedobj)) {
			conf_save();
		}
		break;
	case AG_ECHO_FAILURE:
		if (ops_echo_failure_process(cmd, gw_echo_failure_process)) {
			goto err;
		}
		break;
	default:
		log_err("can't process opcode %d", op);
		gw_send_nak(JINT_ERR_OP, recv_id);
		return APR_ERR;
	}

	return APR_DONE;
}

/*
 * Load gateway schedules
 */
static int gw_schedules_set(json_t *scheds)
{
	json_t *arg;
	json_t *sched;
	int i;

	if (!json_is_array(scheds)) {
		return 0;
	}
	for (i = 0; i < json_array_size(scheds); i++) {
		sched = json_array_get(scheds, i);
		arg = json_object_get(sched, "arg");
		gw_process_schedobj(sched, arg);
	}
	return 0;
}

/*
 * Use this function to send back the result of a node factory reset. The
 * *cookie* argument should be the same as what was passed into the
 * gw_node_rst_handler. *success* = 0 means that the factory reset failed.
 * *msg_code* can be any integer that the oem desires to store in the cloud to
 * represent the result of the command.
 */
int gw_node_rst_cb(const char *addr, void *cookie, u8 success,
			int msg_code)
{
	struct gw_cmd *gw_cmd;
	json_t *cmd_j;
	const char *cmd_addr;

	if (!addr || !cookie) {
		return -1;
	}
	cmd_j = (json_t *)cookie;
	cmd_addr = json_get_string(cmd_j, "address");
	if (!cmd_addr || strcmp(cmd_addr, addr)) {
		/* safety check to make sure *cookie* is correct */
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = AG_NODE_RST_RESULT;
	gw_cmd->data = cmd_j;
	success = success != 0;
	if (success) {
		gw_delete_all_scheds(addr);
	}
	json_object_set_new(cmd_j, "success", json_integer(success));
	json_object_set_new(cmd_j, "msg_code", json_integer(msg_code));
	ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);

	return 0;
}

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
			const struct op_options *opts)
{
	struct gw_node_ota_info_nonconst *ota_info;
	char *save_realpath = NULL;
	struct gw_cmd *gw_cmd;
	json_t *cmd_j;
	const char *cmd_addr;
	const char *version;

	if (!addr || !cookie) {
		return -1;
	}
	cmd_j = (json_t *)cookie;
	cmd_addr = json_get_string(cmd_j, "address");
	if (!cmd_addr || strcmp(cmd_addr, addr)) {
		/* safety check to make sure *cookie* is correct */
		return -1;
	}
	if (save_location) {
		if (file_touch(save_location) < 0) {
			/* not a writable save path */
			log_warn("%s not writable", save_location);
			return -1;
		} else {
			save_realpath = realpath(save_location, NULL);
		}
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = AG_NODE_OTA_RESULT;
	gw_cmd->data = cmd_j;
	gw_cmd->opts.confirm = opts ? opts->confirm : 1;
	json_object_set_new(cmd_j, "save_location",
	    save_realpath ? json_string(save_realpath) : json_null());
	free(save_realpath);
	ota_info = calloc(1, sizeof(*ota_info));
	ota_info->addr = strdup(addr);
	version = json_get_string(cmd_j, "version");
	if (version) {
		ota_info->version = strdup(version);
	}
	ota_info->save_location = save_location ? strdup(save_location) : NULL;
	gw_cmd->arg = ota_info;
	gw_cmd->arg_type = CAT_NODE_OTA_INFO;
	gw_cmd->free_arg_handler = gw_node_ota_info_free;
	ops_add(gw_cmd_handler, gw_cmd, NULL, gw_confirm_cb, gw_cmd_free);

	return 0;
}

/*
 * Use this function to send back the result of a node register status. The
 * *cookie* argument should be the same as what was passed into the
 * gw_node_reg_handler. *success* = 0 means that register status sent failed.
 * *msg_code* can be any integer that the oem desires to store in the cloud to
 * represent the result of the command.
 */
int gw_node_reg_cb(const char *addr, void *cookie, u8 success, int msg_code)
{
	struct gw_cmd *gw_cmd;
	json_t *cmd_j;
	const char *cmd_addr;

	if (!addr || !cookie) {
		return -1;
	}
	cmd_j = (json_t *)cookie;
	cmd_addr = json_get_string(cmd_j, "address");
	if (!cmd_addr || strcmp(cmd_addr, addr)) {
		/* safety check to make sure *cookie* is correct */
		return -1;
	}
	gw_cmd = calloc(1, sizeof(*gw_cmd));
	REQUIRE(gw_cmd, REQUIRE_MSG_ALLOCATION);
	gw_cmd->op = AG_NODE_REG_RESULT;
	gw_cmd->data = cmd_j;
	success = success != 0;
	json_object_set_new(cmd_j, "success", json_integer(success));
	json_object_set_new(cmd_j, "msg_code", json_integer(msg_code));
	ops_add(gw_cmd_handler, gw_cmd, NULL, NULL, gw_cmd_free);

	return 0;
}

/*
 * Register a handler for confirmation callbacks for cases when *confirm* was
 * set when calling gateway_node_add, gateway_prop_send, etc. Use the "op" to
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
    const struct op_options *opts, const struct confirm_info *confirm_info))
{
	gw_confirm_handler = handler;
}

/*
 * Register a handler for responding to node property requests. The response
 * MUST be sent using the *gw_node_prop_send* function with the same *req_id*.
 * The *arg* parameter may (optionally) contain additional
 * information about the request. Should return -1 if no datapoint or if no
 * property exists for the given gw_node_prop.
 */
void gw_node_prop_get_handler_set(int (*handler)(struct gw_node_prop *prop,
				int req_id, const char *arg))
{
	gw_node_prop_get_handler = handler;
}

/*
 * Register a handler for responding to connection status requests.
 * This function sets the handler for connection status requests coming from
 * devd, i.e., connection status requests made by the service/mobile app .
 * The *addr* is the address of the node. The handler should return a 0 if the
 * node is offline, 1 if it’s online, and -1 if the node cannot be found.
 * The application should maintain an online/offline state for all of its nodes
 * as this can be queried anytime.
 */
void gw_node_conn_get_handler_set(int (*handler)(const char *addr))
{
	gw_node_conn_get_handler = handler;
}

/*
 * Register a handler for accepting node property updates and responses.
 * The 'args' parameter will be NULL if no additional args exist.
 * Please see the definition of op_args to see what different
 * args can be passed in. Note that the 'args' structure is on the stack,
 * If the app sets the *app_manages_acks* to 1, then the app must take care of
 * acks by calling "ops_prop_ack_send" with the args->ack_arg. The application
 * can ignore the contents of ack_arg. If the app sets the *app_manages_acks* to
 * 0, then the library will automatically take care of acks by using the return
 * value of this handler. It'll consider a 0 return code as a success and
 * failure otherwise.
 */
void gw_node_prop_set_handler_set(int (*handler)(struct gw_node_prop *prop,
		enum prop_type type, const void *val, size_t val_len,
		const struct op_args *args), int app_manages_acks)
{
	gw_node_prop_set_handler = handler;
	gw_app_manages_acks = app_manages_acks;
}

/*
 * Register a handler for processing node factory resets.
 * The *addr* represents the node that needs to be factory reset.
 * Response to the node factor reset should be given by calling
 * gw_node_rst_cb with the *cookie*
 */
void gw_node_rst_handler_set(void (*handler)(const char *addr,
				void *cookie))
{
	gw_node_rst_handler = handler;
}


/*
 * Register a handler for processing a pending Node OTA command from
 * the cloud. The *addr* represents the node that the OTA is pending for and the
 * version is the one assigned to the OTA when it was uploaded to the Ayla
 * service.
 * Response to the Node OTA should be given by calling gw_node_ota_cb with the
 * *cookie*
 */
void gw_node_ota_handler_set(void (*handler)(const char *addr, const char *ver,
				void *cookie))
{
	gw_node_ota_handler = handler;
}

/*
 * Register a handler for processing a pending Node register status from
 * the cloud. The *addr* represents the node that registered status changed.
 * The *stat* represents the node registered status.
 * Response to the Node register status should be given by calling
 * gw_node_reg_cb with the *cookie*
 */
void gw_node_reg_handler_set(void (*handler)(const char *addr, bool stat,
				void *cookie))
{
	gw_node_reg_handler = handler;
}

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
		const struct op_options *opts))
{
	gw_cloud_fail_handler = handler;
}

/*
 * Initialize the gateway block.
 */
void gw_initialize(void)
{
	struct gw_state *gstate = &gw_state;

	if (gstate->gw_initialized) {
		return;
	}
	conf_register(GATEWAY_SCHEDULES, gw_schedules_set, NULL);
	gstate->gw_initialized = true;
}

/*
 * Add an array of node_prop entries to the lookup table.
 */
int gw_node_prop_add(struct gw_node_prop_entry *prop, unsigned int count)
{
	struct gw_state *gstate = &gw_state;
	struct gw_node_prop_entry **new_table;
	int i;

	if (!prop) {
		return -1;
	}
	new_table = realloc(gstate->table,
	    sizeof(*prop) * (gstate->count + count));
	if (!new_table) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		new_table[gstate->count++] = prop++;
	}
	gstate->table = new_table;
	return 0;
}

/*
 * Return a pointer to the first matching gw_node_prop_entry in the table.
 * Returns NULL if there are no matches.
 */
struct gw_node_prop_entry *gw_node_prop_lookup(const struct gw_node_prop *prop)
{
	struct gw_state *gstate = &gw_state;
	struct gw_node_prop_entry **entry_ptr;
	struct gw_node_prop_entry *entry;

	if (!gstate->gw_initialized) {
		return NULL;
	}
	for (entry_ptr = gstate->table;
	    entry_ptr < &gstate->table[gstate->count]; entry_ptr++) {
		entry = *entry_ptr;
		if (entry && !gw_node_prop_cmp(&entry->prop, prop)) {
			return entry;
		}
	}
	return NULL;
}

/*
 * Return a pointer to the first matching gw_node_prop_entry in the table
 * with the given nickname.
 * Returns NULL if there are no matches.
 */
struct gw_node_prop_entry *gw_node_prop_lookup_by_nickname(const char *n)
{
	struct gw_state *gstate = &gw_state;
	struct gw_node_prop_entry **entry_ptr;
	struct gw_node_prop_entry *entry;

	if (!gstate->gw_initialized) {
		return NULL;
	}
	for (entry_ptr = gstate->table;
	    entry_ptr < &gstate->table[gstate->count]; entry_ptr++) {
		entry = *entry_ptr;
		if (entry && entry->nickname && !strcmp(entry->nickname, n)) {
			return entry;
		}
	}
	return NULL;
}
