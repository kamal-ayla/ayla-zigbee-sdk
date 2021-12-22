/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __DBUS_UTILS_H__
#define __DBUS_UTILS_H__

/*
 * D-Bus message types.
 * These must not change because they map to constants defined by the protocol.
 */
enum dbus_msg_type {
	DBUS_MSG_TYPE_INVALID		= DBUS_MESSAGE_TYPE_INVALID,
	DBUS_MSG_TYPE_METHOD_CALL	= DBUS_MESSAGE_TYPE_METHOD_CALL,
	DBUS_MSG_TYPE_METHOD_RETURN	= DBUS_MESSAGE_TYPE_METHOD_RETURN,
	DBUS_MSG_TYPE_ERROR		= DBUS_MESSAGE_TYPE_ERROR,
	DBUS_MSG_TYPE_SIGNAL		= DBUS_MESSAGE_TYPE_SIGNAL
};

/*
 * Definition for filtering received D-Bus messages.  Unset fields are
 * considered wildcards.
 */
struct dbus_client_msg_filter {
	enum dbus_msg_type type;
	const char *sender;
	const char *interface;
	const char *member;
	const char *path;
	const char *destination;
};


/*****************************************
 * D-Bus message print funcs for debugging
 *****************************************/

void dbus_utils_msg_print_iter(const char *func, const DBusMessageIter *iter,
	unsigned indent);

void dbus_utils_msg_print(const char *func, DBusMessage *msg);

/*****************************************
 * D-Bus message value parsing
 *****************************************/

int dbus_utils_parse_bool(DBusMessageIter *iter, bool *val);

int dbus_utils_parse_int(DBusMessageIter *iter, s32 *val);

int dbus_utils_parse_int64(DBusMessageIter *iter, s64 *val);

int dbus_utils_parse_uint(DBusMessageIter *iter, u32 *val);

int dbus_utils_parse_uint64(DBusMessageIter *iter, u64 *val);

int dbus_utils_parse_double(DBusMessageIter *iter, double *val);

const char *dbus_utils_parse_string(DBusMessageIter *iter);

ssize_t dbus_utils_val_str_copy(char *buf, size_t buf_size,
	DBusMessageIter *iter);

const char *dbus_utils_val_str(DBusMessageIter *iter);

const char *dbus_utils_parse_dict(DBusMessageIter *iter,
	DBusMessageIter *val_iter);

/*****************************************
 * D-Bus message creation helpers
 *****************************************/

void dbus_utils_append_variant(DBusMessageIter *iter, int type,
	const void *val);

DBusMessage *bt_utils_create_msg_prop_set(const char *service,
	const char *path, const char *interface,
	const char *name, int type, const void *val);

/*****************************************
 * D-Bus message filter and rule formatting utils
 *****************************************/

bool dbus_utils_msg_filter_eval(const struct dbus_client_msg_filter *filter,
	DBusMessage *msg);


int dbus_utils_match_rule_create(char *buf, size_t buf_size,
	const struct dbus_client_msg_filter *filter, DBusConnection *conn);

/*****************************************
 * D-Bus message UUIDs variant parsing
 *****************************************/
int dbus_utils_parse_uuid_variant(DBusMessageIter *iter,
	char **uuids, int uuid_max);

#endif /* __DBUS_UTILS_H__ */
