/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/buffer.h>
#include <ayla/hashmap.h>
#include <ayla/parse.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>
#include <ayla/msg_conf.h>


/* ID used for the registration list associated with each endpoint */
static int msg_conf_reg_list_id;

/* ID used for the notification handler map associated with each endpoint */
static int msg_conf_notify_map_id;

struct msg_conf_notify_handler {
	void (*callback)(const char *, json_t *, void *arg);
	void *callback_arg;
	char path[0];
};

HASHMAP_FUNCS_CREATE(notify, const char, struct msg_conf_notify_handler)

struct msg_conf_reg {
	LIST_ENTRY(msg_conf_reg) entry;
	char path[0];
};
LIST_HEAD(msg_conf_reg_list, msg_conf_reg);

static struct {
	const struct msg_conf_privs *privs_table;
	size_t privs_table_size;
	void (*notification_sender)(const char *, const json_t *);
	enum amsg_err (*factory_reset)(const struct amsg_endpoint *);
} state;


/*
 * Return true if the path fully matches the pattern, or if it matches
 * a pattern wildcard.
 */
static bool msg_conf_path_match(const char *pattern, const char *path)
{
	/* Check for exact match */
	for (; *pattern == *path; ++pattern, ++path) {
		if (*pattern == '\0') {
			return true;
		}
	}
	/* Check for sub-object wildcard if path ends with parent object */
	if (*path == '\0' && *pattern == '/') {
		++pattern;
	}
	/* Check for wildcard at the end of pattern */
	if (pattern[0] == '*' && pattern[1] == '\0') {
		return true;
	}
	return false;
}

/*
 * Attempt to match the child pattern specified to a parent path.  If the
 * pattern is a sub-object of path, return the base path.  If there is a
 * wildcard object name, set the wildcard pointer to point to it.
 * This function returns pointers to a static string buffer which will
 * be overwritten on subsequent calls.
 */
static const char *msg_conf_get_child_path(const char *pattern,
	const char *path, const char **wildcard)
{
	static char path_buf[200];
	const char *match = pattern;
	char *star;
	char *slash;
	size_t len;

	if (wildcard) {
		*wildcard = NULL;
	}
	/* Match entire path*/
	for (; *match == *path; ++match, ++path) {
		if (*path == '\0') {
			return NULL;
		}
	}
	/* Path is not parent */
	if (*path != '\0' || *match != '/' || match[1] == '\0') {
		return NULL;
	}
	/* Remove leading slash */
	++match;
	/* Check for wildcard at end of path */
	star = strrchr(match, '*');
	if (!star || star[1] != '\0') {
		/* No wildcard, so entire pattern should be valid */
		return pattern;
	}
	slash = strrchr(match, '/');
	if (!slash) {
		/* No full sub-objects past path */
		slash = (char *)(match - 1);
	}
	/* Put path to deepest full object and wildcard path_buf */
	len = star - pattern;
	if (len >= sizeof(path_buf)) {
		log_err("path_buf too small");
		return NULL;
	}
	memcpy(path_buf, pattern, len);
	path_buf[slash - pattern] = '\0';	/* Terminate base_path */
	/* Set wildcard if it includes a name prefix for sub-objects */
	if (wildcard && slash[1] != '*') {
		*wildcard = &path_buf[slash - pattern + 1];
		path_buf[len] = '\0';		/* Terminate wildcard */
	}
	return path_buf;
}

/*
 * Send a config message.
 */
static enum amsg_err msg_conf_msg_send(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type,
	const char *path, json_t *val, bool synchronous,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	json_t *, void *), void *resp_arg, uint32_t timeout_ms)
{
	json_t *msg;
	enum amsg_err err;

	ASSERT(path != NULL);

	msg = json_object();
	json_object_set_new(msg, "name", json_string(path));
	if (val) {
		json_object_set(msg, "val", val);
	}
	if (synchronous) {
		err = msg_send_json_sync(endpoint, interface, type, msg,
		    resp_handler, resp_arg, timeout_ms);
	} else {
		err = msg_send_json(endpoint, interface, type, msg,
		    resp_handler, resp_arg, timeout_ms);
	}
	json_decref(msg);
	return err;
}

/*
 * Handle a MSG_CONFIG_VALUE response for msg_conf_get().
 */
static void msg_conf_get_resp_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, json_t *json, void *arg)
{
	void (*val_handler)(struct amsg_endpoint *, enum amsg_err, const char *,
	    json_t *) = arg;
	const char *path = NULL;
	json_t *val = NULL;

	if (err != AMSG_ERR_NONE) {
		goto error;
	}
	path = json_get_string(json, "name");
	if (!path || !path[0]) {
		goto error;
	}
	val = json_object_get(json, "val");
error:
	val_handler(endpoint, err, path, val);
}

/*
 * Handle a request to set a configuration item.
 */
static enum amsg_err msg_conf_get_handler(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info, struct amsg_resp_info *resp_info)
{
	json_t *msg_obj;
	json_t *val_obj;
	const char *path;
	u8 priv_mask;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	path = json_get_string(msg_obj, "name");
	if (!path || !path[0]) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	priv_mask = msg_conf_get_privs(state.privs_table,
	    state.privs_table_size, path);
	if (!(priv_mask & MSG_CONF_READ)) {
		err = AMSG_ERR_PRIVS;
		goto error;
	}
	val_obj = conf_get(path);
	if (!val_obj) {
		err = AMSG_ERR_APPLICATION;
		goto error;
	}
	json_object_set(msg_obj, "val", val_obj);
	if (resp_info) {
		err = msg_send_json_resp(&resp_info,
		    MSG_INTERFACE_CONFIG, MSG_CONFIG_VALUE_RESP, msg_obj);
	} else if (endpoint) {
		err = msg_send_json(endpoint,
		    MSG_INTERFACE_CONFIG, MSG_CONFIG_VALUE_RESP, msg_obj,
		    NULL, NULL, 0);
	} else {
		err = AMSG_ERR_APPLICATION;
	}
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a request to set a configuration item.
 */
static enum amsg_err msg_conf_set_handler(const struct amsg_msg_info *info)
{
	json_t *msg_obj;
	json_t *val_obj;
	const char *path;
	u8 priv_mask;
	int rc;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	path = json_get_string(msg_obj, "name");
	if (!path || !path[0]) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	priv_mask = msg_conf_get_privs(state.privs_table,
	    state.privs_table_size, path);
	if (!(priv_mask & MSG_CONF_WRITE)) {
		err = AMSG_ERR_PRIVS;
		goto error;
	}
	val_obj = json_object_get(msg_obj, "val");
	if (!val_obj) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	rc = conf_set(path, val_obj);
	if (rc < 0) {
		err = AMSG_ERR_APPLICATION;
		goto error;
	}
	if (!rc) {
		if (conf_apply() < 0 || conf_save() < 0) {
			err = AMSG_ERR_APPLICATION;
			goto error;
		}
	}
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a request to remove a configuration item.
 */
static enum amsg_err msg_conf_delete_handler(const struct amsg_msg_info *info)
{
	json_t *msg_obj;
	const char *path;
	u8 priv_mask;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	path = json_get_string(msg_obj, "name");
	if (!path || !path[0]) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	priv_mask = msg_conf_get_privs(state.privs_table,
	    state.privs_table_size, path);
	if (!(priv_mask & MSG_CONF_WRITE)) {
		err = AMSG_ERR_PRIVS;
		goto error;
	}
	if (conf_delete(path) < 0) {
		/* No need to return an error if the object was non-existent */
		goto error;
	}
	if (conf_apply() < 0 || conf_save() < 0) {
		err = AMSG_ERR_APPLICATION;
		goto error;
	}
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Free all memory associated with a config registration list.
 */
static void msg_conf_reg_list_free(void *data)
{
	struct msg_conf_reg_list *reg_list = (struct msg_conf_reg_list *)data;
	struct msg_conf_reg *reg;

	if (!reg_list) {
		return;
	}
	while ((reg = LIST_FIRST(reg_list)) != NULL) {
		LIST_REMOVE(reg, entry);
		free(reg);
	}
	free(reg_list);
}

/*
 * Free all memory associated with a config notification handler map.
 */
static void msg_conf_notify_map_free(void *data)
{
	struct hashmap *handlers = (struct hashmap *)data;
	struct hashmap_iter *iter;

	if (!data) {
		return;
	}
	for (iter = hashmap_iter(handlers); iter;
	    iter = hashmap_iter_next(handlers, iter)) {
		free(notify_hashmap_iter_get_data(iter));
	}
	hashmap_destroy(handlers);
	free(handlers);
}

/*
 * Send a config notification message.
 */
static enum amsg_err msg_conf_notify(struct amsg_endpoint *endpoint,
	const char *path, const json_t *val, bool synchronous)
{
	return msg_conf_msg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_NOTIFY, path, (json_t *)val, synchronous,
	    NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Handle a config registration message.
 */
static enum amsg_err msg_conf_reg_handler(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	json_t *msg_obj;
	json_t *val;
	const char *path;
	u8 priv_mask;
	struct msg_conf_reg_list *reg_list;
	struct msg_conf_reg *reg;
	size_t path_len;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	path = json_get_string(msg_obj, "name");
	if (!path || !path[0]) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	/* Wildcards are forbidden in registration paths */
	ASSERT(strchr(path, '*') == NULL);
	priv_mask = msg_conf_get_privs(state.privs_table,
	    state.privs_table_size, path);
	/* All notify-capable config has read permissions */
	if (!(priv_mask & MSG_CONF_READ)) {
		err = AMSG_ERR_PRIVS;
		goto error;
	}
	reg_list = (struct msg_conf_reg_list *)amsg_get_user_data(endpoint,
	    msg_conf_reg_list_id);
	if (reg_list) {
		/* Check for duplicates */
		LIST_FOREACH(reg, reg_list, entry) {
			if (!strcmp(path, reg->path)) {
				err = AMSG_ERR_APPLICATION;
				goto error;
			}
		}
	} else {
		/* Allocate new registration list */
		reg_list = (struct msg_conf_reg_list *)malloc(
		    sizeof(*reg_list));
		if (!reg_list) {
			log_err("malloc failed");
			err = AMSG_ERR_MEM;
			goto error;
		}
		LIST_INIT(reg_list);
		if (!amsg_set_user_data(endpoint, msg_conf_reg_list_id,
		    reg_list, msg_conf_reg_list_free)) {
			log_err("failed to set endpoint user data");
			err = AMSG_ERR_MEM;
			goto error;
		}
	}
	/* Add new registration entry */
	path_len = strlen(path);
	reg = (struct msg_conf_reg *)malloc(sizeof(*reg) + path_len + 1);
	if (!reg) {
		log_err("malloc failed");
		err = AMSG_ERR_MEM;
		goto error;
	}
	strcpy(reg->path, path);
	/* Ensure registered path does not end in a slash */
	if (reg->path[path_len - 1] == '/') {
		reg->path[path_len - 1] = '\0';
	}
	LIST_INSERT_HEAD(reg_list, reg, entry);

	log_debug("path=%s", reg->path);

	/* Immediately notify with current value, if available */
	val = conf_get(reg->path);
	if (val) {
		msg_conf_notify(endpoint, reg->path, val, false);
	}
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Handle a config unregister message.
 */
static enum amsg_err msg_conf_unreg_handler(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	json_t *msg_obj;
	const char *path;
	struct msg_conf_reg_list *reg_list;
	struct msg_conf_reg *reg;
	enum amsg_err err = AMSG_ERR_NONE;

	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	path = json_get_string(msg_obj, "name");
	if (!path || !path[0]) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	reg_list = (struct msg_conf_reg_list *)amsg_get_user_data(endpoint,
	    msg_conf_reg_list_id);
	if (!reg_list) {
		err = AMSG_ERR_APPLICATION;
		goto error;
	}
	LIST_FOREACH(reg, reg_list, entry) {
		if (!strcmp(path, reg->path)) {
			break;
		}
	}
	if (!reg) {
		err = AMSG_ERR_APPLICATION;
		goto error;
	}
	LIST_REMOVE(reg, entry);
	free(reg);

	log_debug("path=%s", path);
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Response handler for config registration message.  Removes the notification
 * callback state if registration failed.
 */
static void msg_conf_register_resp_handler(struct amsg_endpoint *endpoint,
	enum amsg_err err, json_t *msg, void *arg)
{
	struct hashmap *handlers;
	struct msg_conf_notify_handler *handler =
	    (struct msg_conf_notify_handler *)arg;

	if (err == AMSG_ERR_NONE) {
		/* Nothing to do if successful */
		log_debug("registration on %s successful", handler->path);
		return;
	}
	log_err("registration on %s failed: %s", handler->path,
	    amsg_err_string(err));
	handlers = (struct hashmap *)amsg_get_user_data(endpoint,
	    msg_conf_notify_map_id);
	if (handlers) {
		notify_hashmap_remove(handlers, handler->path);
	} else {
		log_err("failed to get endpoint user data");
	}
	free(handler);
}

/*
 * Handle an incoming config notification message.
 */
static enum amsg_err msg_conf_notification(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info)
{
	json_t *msg_obj;
	json_t *val_obj;
	const char *path;
	struct hashmap *handlers;
	struct msg_conf_notify_handler *handler;
	enum amsg_err err = AMSG_ERR_NONE;

	handlers = (struct hashmap *)amsg_get_user_data(endpoint,
	    msg_conf_notify_map_id);
	if (!handlers) {
		return AMSG_ERR_PRIVS;
	}
	msg_obj = msg_parse_json(info);
	if (!msg_obj) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	path = json_get_string(msg_obj, "name");
	if (!path) {
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	val_obj = json_object_get(msg_obj, "val");
	handler = notify_hashmap_get(handlers, path);
	if (!handler) {
		err = AMSG_ERR_PRIVS;
		goto error;
	}
	handler->callback(handler->path, val_obj, handler->callback_arg);
error:
	json_decref(msg_obj);
	return err;
}

/*
 * Config change handler.  Matches path to defined config NOTIFY permissions
 * and invokes notification_sender as appropriate.
 */
static void msg_conf_changed(const char *path, const json_t *val_obj)
{
	const struct msg_conf_privs *p;
	u8 priv_mask;
	const char *base_path;
	const char *wildcard;
	size_t path_len;
	size_t wildcard_len;
	const char *key;
	char path_buf[200];
	json_t *child;
	json_t *subobj;

	ASSERT(state.notification_sender != NULL);

	/* Notify if path has notify permissions */
	priv_mask = msg_conf_get_privs(state.privs_table,
	    state.privs_table_size, path);
	if (priv_mask & MSG_CONF_NOTIFY) {
		state.notification_sender(path, val_obj);
		return;
	}
	/*
	 * The path does not have notify permissions, so look for sub-items
	 * with notify permissions and notify at their level.
	 */
	for (p = state.privs_table;
	    p < &state.privs_table[state.privs_table_size]; ++p) {
		if (!(p->priv_mask & MSG_CONF_NOTIFY)) {
			continue;
		}
		base_path = msg_conf_get_child_path(p->path, path, &wildcard);
		if (!base_path) {
			continue;
		}
		child = conf_get(base_path);
		if (!child) {
			continue;
		}
		if (!wildcard) {
			/* Pattern is full match or includes all sub-objects */
			state.notification_sender(base_path, child);
			continue;
		}
		path_len = snprintf(path_buf, sizeof(path_buf), "%s/",
		    base_path);
		wildcard_len = strlen(wildcard);
		/* Iterate sub-objects and notify on wildcard matches */
		json_object_foreach(child, key, subobj) {
			if (!strncmp(wildcard, key, wildcard_len)) {
				snprintf(path_buf + path_len,
				    sizeof(path_buf) - path_len, "%s", key);
				state.notification_sender(path_buf, subobj);
			}
		}
	}
}

/*
 * Handle an incoming config factory reset message.
 */
static enum amsg_err msg_conf_factory_reset_handler(
	struct amsg_endpoint *endpoint)
{
	if (!state.factory_reset) {
		return AMSG_ERR_PRIVS;
	}
	return state.factory_reset(endpoint);
}

/*
 * Message interface handler for the Config interface.
 */
static enum amsg_err msg_conf_config_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == MSG_INTERFACE_CONFIG);

	switch (info->type) {
	case MSG_CONFIG_VALUE_REQ:
		return msg_conf_get_handler(endpoint, info, resp_info);
	case MSG_CONFIG_VALUE_SET:
		return msg_conf_set_handler(info);
	case MSG_CONFIG_VALUE_DELETE:
		return msg_conf_delete_handler(info);
	case MSG_CONFIG_REG:
		return msg_conf_reg_handler(endpoint, info);
	case MSG_CONFIG_UNREG:
		return msg_conf_unreg_handler(endpoint, info);
	case MSG_CONFIG_NOTIFY:
		return msg_conf_notification(endpoint, info);
	case MSG_CONFIG_FACTORY_RESET:
		return msg_conf_factory_reset_handler(endpoint);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Initialize the remote config interface.
 * Register remote config message handlers, and assign the config privilege
 * table to a global state structure.  If the notify handler is passed in,
 * a config change handler is registered and change notifications will be
 * called based on NOTIFY permissions specified in the privilege table.
 */
void msg_conf_init(const struct msg_conf_privs *privs_table, size_t table_size,
	void (*notify)(const char *, const json_t *),
	enum amsg_err (*handle_factory_reset)(const struct amsg_endpoint *))
{
	msg_conf_reg_list_id = msg_generate_unique_id();
	msg_conf_notify_map_id = msg_generate_unique_id();
	state.privs_table = privs_table;
	state.privs_table_size = table_size;
	state.notification_sender = notify;
	state.factory_reset = handle_factory_reset;
	if (notify) {
		/* Cannot notify without notification privs */
		ASSERT(privs_table != NULL);
		conf_set_change_callback(msg_conf_changed);
	}
	amsg_set_interface_handler(MSG_INTERFACE_CONFIG,
	    msg_conf_config_handler);
}

/*
 * Request the config JSON at the specified path.  val_handler will be invoked
 * if the requested value is received successfully.  This operation can be
 * performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_get(struct amsg_endpoint *endpoint, const char *path,
	void (*val_handler)(struct amsg_endpoint *, enum amsg_err, const char *,
	json_t *), bool synchronous)
{
	ASSERT(val_handler != NULL);

	return msg_conf_msg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_VALUE_REQ, path, NULL, synchronous,
	    msg_conf_get_resp_handler, val_handler, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Set the config JSON at the specified path.  This operation can be
 * performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_set(struct amsg_endpoint *endpoint,
	const char *path, const json_t *val, bool synchronous)
{
	return msg_conf_msg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_VALUE_SET, path, (json_t *)val, synchronous,
	    NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Delete the config JSON at the specified path. This operation can be
 * performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_delete(struct amsg_endpoint *endpoint, const char *path,
	bool synchronous)
{
	return msg_conf_msg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_VALUE_DELETE, path, NULL, synchronous,
	    NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Request a factory reset.This operation can be performed synchronously,
 * or asynchronously.
 */
enum amsg_err msg_conf_factory_reset(struct amsg_endpoint *endpoint,
	bool synchronous)
{
	ASSERT(endpoint != NULL);

	if (synchronous) {
		return amsg_send_sync(endpoint, MSG_INTERFACE_CONFIG,
		    MSG_CONFIG_FACTORY_RESET, NULL, 0, NULL, NULL,
		    MSG_TIMEOUT_DEFAULT_MS);
	}
	return amsg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_FACTORY_RESET, NULL, 0, NULL, NULL,
	    MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Register for config notifications on the specified path.
 * This operation can be performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_register(struct amsg_endpoint *endpoint,
	const char *path, void (*callback)(const char *, json_t *, void *),
	void *callback_arg, bool synchronous)
{
	struct hashmap *handlers;
	struct msg_conf_notify_handler *handler;
	struct msg_conf_notify_handler *handler_entry;
	size_t path_len;
	enum amsg_err err;

	ASSERT(path != NULL);
	ASSERT(path[0] != '\0');
	ASSERT(callback != NULL);

	handlers = (struct hashmap *)amsg_get_user_data(endpoint,
	    msg_conf_notify_map_id);
	if (!handlers) {
		handlers = (struct hashmap *)malloc(sizeof(*handlers));
		if (!handlers) {
			log_err("malloc failed");
			return AMSG_ERR_MEM;
		}
		hashmap_init(handlers, hashmap_hash_string,
		    hashmap_compare_string, 20);
		if (!amsg_set_user_data(endpoint, msg_conf_notify_map_id,
		    handlers, msg_conf_notify_map_free)) {
			log_err("failed to set endpoint user data");
			return AMSG_ERR_MEM;
		}
	}
	path_len = strlen(path);
	ASSERT(path[path_len - 1] != '/');	/* No trailing slashes */
	ASSERT(strchr(path, '*') == NULL);	/* No wildcards */
	handler = (struct msg_conf_notify_handler *)malloc(sizeof(*handler) +
	    path_len + 1);
	if (!handler) {
		log_err("malloc failed");
		return AMSG_ERR_MEM;
	}
	handler->callback = callback;
	handler->callback_arg = callback_arg;
	strcpy(handler->path, path);
	handler_entry = notify_hashmap_put(handlers, handler->path, handler);
	if (!handler_entry) {
		log_err("failed to register notification handler for %s", path);
		free(handler);
		return AMSG_ERR_MEM;
	}
	if (handler_entry != handler) {
		free(handler);
		/* Do not override a non-matching handler for this path */
		if (handler_entry->callback != callback ||
		    handler_entry->callback_arg != callback_arg) {
			log_err("notification handler already registered "
			    "for %s", path);
			return AMSG_ERR_APPLICATION;
		}
	}
	err = msg_conf_msg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_REG, path, NULL, synchronous,
	    msg_conf_register_resp_handler, handler_entry,
	    MSG_TIMEOUT_DEFAULT_MS);
	if (err != AMSG_ERR_NONE) {
		msg_conf_register_resp_handler(endpoint, err, NULL,
		    handler_entry);
	}
	return err;
}

/*
 * Unregister for config notifications on the specified path.
 * This operation can be performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_unregister(struct amsg_endpoint *endpoint,
	const char *path, bool synchronous)
{
	struct hashmap *handlers;
	struct msg_conf_notify_handler *handler;

	handlers = (struct hashmap *)amsg_get_user_data(endpoint,
	    msg_conf_notify_map_id);
	if (!handlers) {
		log_err("failed to get endpoint user data");
		return AMSG_ERR_APPLICATION;
	}
	handler = notify_hashmap_remove(handlers, path);
	if (!handler) {
		log_err("no registration for %s", path);
		return AMSG_ERR_APPLICATION;
	}
	free(handler);

	return msg_conf_msg_send(endpoint, MSG_INTERFACE_CONFIG,
	    MSG_CONFIG_UNREG, path, NULL, synchronous,
	    NULL, NULL, MSG_TIMEOUT_DEFAULT_MS);
}

/*
 * Process notifications for this endpoint.  This would usually be called
 * by the 'notify' handler.  This operation can be performed synchronously,
 * or asynchronously.
 */
enum amsg_err msg_conf_notify_process(struct amsg_endpoint *endpoint,
	const char *path, const json_t *val, bool synchronous)
{
	struct msg_conf_reg_list *reg_list;
	struct msg_conf_reg *reg;
	const char *change_path;
	const char *reg_path;

	reg_list = (struct msg_conf_reg_list *)amsg_get_user_data(endpoint,
	    msg_conf_reg_list_id);
	if (!reg_list) {
		/* No registrations */
		return AMSG_ERR_NONE;
	}

	/* Search config registrations for a match on this change */
	LIST_FOREACH(reg, reg_list, entry) {
		change_path = path;
		reg_path = reg->path;
		/* Check for exact match */
		for (; *reg_path == *change_path; ++reg_path, ++change_path) {
			if (*reg_path == '\0') {
				goto notify;
			}
		}
		/* Check the end of either path (partial match) */
		if (*change_path == '\0' ||
		    (*change_path == '/' && *reg_path == '\0')) {
			/* Get full registered config object */
			val = conf_get(reg->path);
			goto notify;
		}
	}
	/* No matching registrations */
	return AMSG_ERR_NONE;
notify:
	log_debug("%s: %s%s", reg->path, path, val ? "" : " (deleted)");
	return msg_conf_notify(endpoint, reg->path, val, synchronous);
}

/*
 * Calculate config privileges for the specified path.
 */
u8 msg_conf_get_privs(const struct msg_conf_privs *privs_table,
	size_t table_size, const char *path)
{
	const struct msg_conf_privs *p;
	u8 priv_mask = 0;

	/* No privileges */
	if (!privs_table) {
		return 0;
	}
	/* Reject empty paths */
	if (!path || *path == '\0') {
		return 0;
	}
	/* Combine privs for all path matches */
	for (p = privs_table; p < &privs_table[table_size]; ++p) {
		if (msg_conf_path_match(p->path, path)) {
			priv_mask |= p->priv_mask;
		}
	}
	return priv_mask;
}
