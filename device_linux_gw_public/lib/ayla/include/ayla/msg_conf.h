/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __AYLA_MSG_CONF_H__
#define __AYLA_MSG_CONF_H__

enum msg_conf_priv {
	MSG_CONF_READ		= BIT(0),
	MSG_CONF_WRITE		= BIT(1),
	MSG_CONF_NOTIFY		= BIT(2) | MSG_CONF_READ,
	MSG_CONF_ALL	= MSG_CONF_READ | MSG_CONF_WRITE | MSG_CONF_NOTIFY
};

/*
 * Remote config access privilege table entry.  Path may end with a '*' to
 * specify a wildcard.	Wildcards in the middle of a path are not supported.
 */
struct msg_conf_privs {
	const char *path;		/* Config path for the entry */
	u8 priv_mask;			/* Bit mask of msg_conf_priv privs */
};

enum amsg_err;
struct amsg_endpoint;

/*
 * Initialize the remote config interface.
 * Register remote config message handlers, and assign the config privilege
 * table to a global state structure.  If the notify handler is passed in,
 * a config change handler is registered and change notifications will be
 * called based on NOTIFY permissions specified in the privilege table.
 */
void msg_conf_init(const struct msg_conf_privs *privs_table, size_t table_size,
	void (*notify)(const char *, const json_t *),
	enum amsg_err (*handle_factory_reset)(const struct amsg_endpoint *));

/*
 * Request the config JSON at the specified path.  val_handler will be invoked
 * if the requested value is received successfully.  This operation can be
 * performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_get(struct amsg_endpoint *endpoint, const char *path,
	void (*val_handler)(struct amsg_endpoint *, enum amsg_err, const char *,
	json_t *), bool synchronous);

/*
 * Set the config JSON at the specified path.  This operation can be
 * performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_set(struct amsg_endpoint *endpoint,
	const char *path, const json_t *val, bool synchronous);

/*
 * Delete the config JSON at the specified path. This operation can be
 * performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_delete(struct amsg_endpoint *endpoint, const char *path,
	bool synchronous);

/*
 * Request a factory reset.This operation can be performed synchronously,
 * or asynchronously.
 */
enum amsg_err msg_conf_factory_reset(struct amsg_endpoint *endpoint,
	bool synchronous);

/*
 * Register for config notifications on the specified path.
 * This operation can be performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_register(struct amsg_endpoint *endpoint,
	const char *path, void (*callback)(const char *, json_t *, void *),
	void *callback_arg, bool synchronous);

/*
 * Unregister for config notifications on the specified path.
 * This operation can be performed synchronously, or asynchronously.
 */
enum amsg_err msg_conf_unregister(struct amsg_endpoint *endpoint,
	const char *path, bool synchronous);

/*
 * Process notifications for this endpoint.  This would usually be called
 * by the 'notify' handler.  This operation can be performed synchronously,
 * or asynchronously.
 */
enum amsg_err msg_conf_notify_process(struct amsg_endpoint *endpoint,
	const char *path, const json_t *val, bool synchronous);

/*
 * Calculate config privileges for the specified path.
 */
u8 msg_conf_get_privs(const struct msg_conf_privs *privs_table,
	size_t table_size, const char *path);

#endif /* __AYLA_MSG_CONF_H__ */
