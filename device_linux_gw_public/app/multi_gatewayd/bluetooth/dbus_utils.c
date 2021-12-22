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

#include <dbus/dbus.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>

#include "dbus_utils.h"


/*****************************************
 * D-Bus message print funcs for debugging
 *****************************************/

void dbus_utils_msg_print_iter(const char *func, const DBusMessageIter *iter,
	unsigned indent)
{
	DBusMessageIter msg_iter;
	DBusMessageIter val_iter;
	int arg_type;
	const char *str;
	char *signature;
	enum log_level level = LOG_AYLA_DEBUG;
	unsigned i = 0;

	if (!func) {
		func = __func__;
	}
	if (!log_debug_enabled()) {
		level = LOG_AYLA_INFO;
	}
	if (!iter) {
		log_base(func, level, "iter is NULL");
		return;
	}
	msg_iter = *iter;
	while ((arg_type = dbus_message_iter_get_arg_type(&msg_iter))
	    != DBUS_TYPE_INVALID) {
		++i;
		switch (arg_type) {
		case DBUS_TYPE_STRUCT:
		case DBUS_TYPE_ARRAY:
		case DBUS_TYPE_DICT_ENTRY:
			signature = dbus_message_iter_get_signature(&msg_iter);
			log_base(func, level,
			    "%*s[%u:%c]: %s %s", indent, "", i, (char)arg_type,
			    arg_type == DBUS_TYPE_DICT_ENTRY ? "dict" :
			    arg_type == DBUS_TYPE_ARRAY ? "array" : "struct",
			    signature);
			dbus_free(signature);
			dbus_message_iter_recurse(&msg_iter, &val_iter);
			dbus_utils_msg_print_iter(func, &val_iter, indent + 3);
			break;
		case DBUS_TYPE_VARIANT:
			log_base(func, level,
			    "%*s[%u:%c]: variant", indent, "", i,
			    (char)arg_type);
			dbus_message_iter_recurse(&msg_iter, &val_iter);
			dbus_utils_msg_print_iter(func, &val_iter, indent + 3);
			break;
		default:
			str = dbus_utils_val_str(&msg_iter);
			if (str) {
				log_base(func, level,
				    "%*s[%u:%c] %s", indent, "", i,
				    (char)arg_type, str);
			} else {
				log_base(func, level,
				    "%*s[%u:%c]: type not supported",
				    indent, "", i, (char)arg_type);
			}
			break;
		}
		dbus_message_iter_next(&msg_iter);
	}
}

void dbus_utils_msg_print(const char *func, DBusMessage *msg)
{
	DBusMessageIter iter;

	ASSERT(msg != NULL);
	log_base(func, LOG_AYLA_INFO, "%s: %s -> %s %s[%s]::%s %s",
	    dbus_message_type_to_string(dbus_message_get_type(msg)),
	    dbus_message_get_sender(msg),
	    dbus_message_get_destination(msg),
	    dbus_message_get_interface(msg),
	    dbus_message_get_path(msg),
	    dbus_message_get_member(msg),
	    dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR ?
	    dbus_message_get_error_name(msg) : "");

	dbus_message_iter_init(msg, &iter);
	dbus_utils_msg_print_iter(func, &iter, 3);
}

/*****************************************
 * D-Bus message value parsing
 *****************************************/

static int dbus_utils_parse_val(DBusMessageIter *iter,
	int *type, DBusBasicValue *val)
{
	DBusMessageIter child_iter;
	int arg_type;

	ASSERT(type != NULL);
	ASSERT(val != NULL);

	if (!iter) {
		return -1;
	}
	arg_type = dbus_message_iter_get_arg_type(iter);
	while (arg_type == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(iter, &child_iter);
		arg_type = dbus_message_iter_get_arg_type(&child_iter);
		if (arg_type == DBUS_TYPE_INVALID) {
			return -1;	/* Incomplete varient structure */
		}
		iter = &child_iter;
	}
	*type = arg_type;
	switch (arg_type) {
	case DBUS_TYPE_UNIX_FD:
	case DBUS_TYPE_STRUCT:
	case DBUS_TYPE_ARRAY:
	case DBUS_TYPE_DICT_ENTRY:
		/* FDs and containers not supported with this func */
		return -1;
	default:
		break;
	}
	memset(val, 0, sizeof(*val));
	dbus_message_iter_get_basic(iter, val);
	return 0;
}

int dbus_utils_parse_bool(DBusMessageIter *iter, bool *val)
{
	int type;
	DBusBasicValue value;

	ASSERT(val != NULL);

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return -1;
	}
	switch (type) {
	case DBUS_TYPE_BOOLEAN:
	case DBUS_TYPE_BYTE:
	case DBUS_TYPE_INT16:
	case DBUS_TYPE_INT32:
	case DBUS_TYPE_UINT16:
	case DBUS_TYPE_UINT32:
		*val = value.bool_val ? true : false;
		break;
	default:
		return -1;
	}
	return 0;
}

int dbus_utils_parse_int(DBusMessageIter *iter, s32 *val)
{
	int type;
	DBusBasicValue value;

	ASSERT(val != NULL);

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return -1;
	}
	switch (type) {
	case DBUS_TYPE_INT16:
		*val = value.i16;
		break;
	case DBUS_TYPE_INT32:
		*val = value.i32;
		break;
	default:
		return -1;
	}
	return 0;
}

int dbus_utils_parse_int64(DBusMessageIter *iter, s64 *val)
{
	int type;
	DBusBasicValue value;

	ASSERT(val != NULL);

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return -1;
	}
	switch (type) {
	case DBUS_TYPE_INT16:
		*val = value.i16;
		break;
	case DBUS_TYPE_INT32:
		*val = value.i32;
		break;
	case DBUS_TYPE_INT64:
		*val = value.i64;
		break;
	default:
		return -1;
	}
	return 0;
}

int dbus_utils_parse_uint(DBusMessageIter *iter, u32 *val)
{
	int type;
	DBusBasicValue value;

	ASSERT(val != NULL);

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return -1;
	}
	switch (type) {
	case DBUS_TYPE_BYTE:
		*val = value.byt;
		break;
	case DBUS_TYPE_UINT16:
		*val = value.u16;
		break;
	case DBUS_TYPE_UINT32:
		*val = value.u32;
		break;
	default:
		return -1;
	}
	return 0;
}

int dbus_utils_parse_uint64(DBusMessageIter *iter, u64 *val)
{
	int type;
	DBusBasicValue value;

	ASSERT(val != NULL);

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return -1;
	}
	switch (type) {
	case DBUS_TYPE_BYTE:
		*val = value.byt;
		break;
	case DBUS_TYPE_UINT16:
		*val = value.u16;
		break;
	case DBUS_TYPE_UINT32:
		*val = value.u32;
		break;
	case DBUS_TYPE_UINT64:
		*val = value.u64;
		break;
	default:
		return -1;
	}
	return 0;
}

int dbus_utils_parse_double(DBusMessageIter *iter, double *val)
{
	int type;
	DBusBasicValue value;

	ASSERT(val != NULL);

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return -1;
	}
	switch (type) {
	case DBUS_TYPE_DOUBLE:
		*val = value.dbl;
		break;
	default:
		return -1;
	}
	return 0;
}

const char *dbus_utils_parse_string(DBusMessageIter *iter)
{
	int type;
	DBusBasicValue value;

	if (dbus_utils_parse_val(iter, &type, &value) < 0) {
		return NULL;
	}
	switch (type) {
	case DBUS_TYPE_STRING:
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_SIGNATURE:
		return value.str;
	default:
		break;
	}
	return NULL;
}

ssize_t dbus_utils_val_str_copy(char *buf, size_t buf_size,
	DBusMessageIter *iter)
{
	DBusMessageIter val_iter;
	bool bool_val;
	s64 int_val;
	u64 uint_val;
	double double_val;
	const char *str_val;
	size_t len;

	ASSERT(buf != NULL);

	if (!iter) {
		return -1;
	}
restart:
	switch (dbus_message_iter_get_arg_type(iter)) {
	case DBUS_TYPE_BYTE:
		if (dbus_utils_parse_uint64(iter, &uint_val) < 0) {
			goto error;
		}
		len = snprintf(buf, buf_size, "%02llx",
		    (long long unsigned)uint_val);
		break;
	case DBUS_TYPE_BOOLEAN:
		if (dbus_utils_parse_bool(iter, &bool_val) < 0) {
			goto error;
		}
		len = snprintf(buf, buf_size, "%s",
		    bool_val ? "true" : "false");
		break;
	case DBUS_TYPE_INT16:
	case DBUS_TYPE_INT32:
	case DBUS_TYPE_INT64:
		if (dbus_utils_parse_int64(iter, &int_val) < 0) {
			goto error;
		}
		len = snprintf(buf, buf_size, "%lld",
		    (long long int)int_val);
		break;
	case DBUS_TYPE_UINT16:
	case DBUS_TYPE_UINT32:
	case DBUS_TYPE_UINT64:
		if (dbus_utils_parse_uint64(iter, &uint_val) < 0) {
			goto error;
		}
		len = snprintf(buf, buf_size, "%llu",
		    (long long unsigned)uint_val);
		break;
	case DBUS_TYPE_DOUBLE:
		if (dbus_utils_parse_double(iter, &double_val) < 0) {
			goto error;
		}
		len = snprintf(buf, buf_size, "%f", double_val);
		break;
	case DBUS_TYPE_STRING:
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_SIGNATURE:
		str_val = dbus_utils_parse_string(iter);
		if (!str_val) {
			goto error;
		}
		len = snprintf(buf, buf_size, "%s", str_val);
		break;
	case DBUS_TYPE_VARIANT:
		dbus_message_iter_recurse(iter, &val_iter);
		iter = &val_iter;
		goto restart;
	default:
		goto error;
	}
	if (len >= buf_size) {
		goto error;
	}
	return len;
error:
	return -1;
}

const char *dbus_utils_val_str(DBusMessageIter *iter)
{
	static char buf[32];
	DBusMessageIter val_iter;

	if (!iter) {
		return NULL;
	}
restart:
	switch (dbus_message_iter_get_arg_type(iter)) {
	case DBUS_TYPE_STRING:
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_SIGNATURE:
		return dbus_utils_parse_string(iter);
	case DBUS_TYPE_VARIANT:
		dbus_message_iter_recurse(iter, &val_iter);
		iter = &val_iter;
		goto restart;
	default:
		if (dbus_utils_val_str_copy(buf, sizeof(buf), iter) < 0) {
			return NULL;
		}
		return buf;
	}
}

const char *dbus_utils_parse_dict(DBusMessageIter *iter,
	DBusMessageIter *val_iter)
{
	const char *name;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_DICT_ENTRY) {
		log_err("invalid dictionary type");
		return NULL;
	}
	/* Enter the prop dict entry */
	dbus_message_iter_recurse(iter, val_iter);
	name = dbus_utils_parse_string(val_iter);
	if (!name) {
		log_err("missing dictionary key");
		return NULL;
	}
	dbus_message_iter_next(val_iter);
	if (dbus_message_iter_get_arg_type(val_iter) == DBUS_TYPE_INVALID) {
		log_err("no dictionary value for %s", name);
		return NULL;
	}
	return name;
}

/*****************************************
 * D-Bus message creation helpers
 *****************************************/

void dbus_utils_append_variant(DBusMessageIter *iter, int type,
	const void *val)
{
	DBusMessageIter val_iter;
	char sig[2] = { type, '\0' };

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, sig,
	    &val_iter);
	dbus_message_iter_append_basic(&val_iter, type, val);
	dbus_message_iter_close_container(iter, &val_iter);
}

DBusMessage *bt_utils_create_msg_prop_set(const char *service,
	const char *path, const char *interface,
	const char *name, int type, const void *val)
{
	DBusMessage *msg;
	DBusMessageIter iter;
	 dbus_bool_t dbus_bool;

	msg = dbus_message_new_method_call(service, path,
	    "org.freedesktop.DBus.Properties", "Set");
	if (!msg) {
		log_err("message allocation failed");
		return NULL;
	}
	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);
	switch (type) {
	case DBUS_TYPE_STRING:
		/* String functions take a pointer to the string */
		dbus_utils_append_variant(&iter, type, &val);
		break;
	case DBUS_TYPE_BOOLEAN:
		/* D-Bus booleans are > 1 byte */
		dbus_bool = *(bool *)val;
		dbus_utils_append_variant(&iter, type, &dbus_bool);
		break;
	default:
		dbus_utils_append_variant(&iter, type, val);
		break;
	}
	return msg;
}

/*****************************************
 * D-Bus message filter and rule formatting utils
 *****************************************/

static int dbus_utils_match_rule_append(char *buf, size_t buf_size,
	size_t *offset, const char *key, const char *value)
{
	ASSERT(*offset < buf_size);

	*offset += snprintf(buf + *offset, buf_size - *offset, "%s%s='%s'",
	    *offset ? "," : "", key, value);
	return *offset < buf_size ? 0 : -1;
}

bool dbus_utils_msg_filter_eval(const struct dbus_client_msg_filter *filter,
	DBusMessage *msg)
{
	const char *val;

	if (filter->type != DBUS_MSG_TYPE_INVALID &&
	    filter->type != dbus_message_get_type(msg)) {
		/*log_debug("msg type mismatch");*/
		return false;
	}
	if (filter->sender) {
		val = dbus_message_get_sender(msg);
		if (!val || strcmp(filter->sender, val)) {
			/*log_debug("sender mismatch: %s", val);*/
			return false;
		}
	}
	if (filter->interface) {
		val = dbus_message_get_interface(msg);
		if (val) {
			if (strcmp(filter->interface, val)) {
				/*log_debug("interface mismatch: %s", val);*/
				return false;
			}
		} else if (dbus_message_get_type(msg) !=
		    DBUS_MSG_TYPE_METHOD_CALL) {
			/* Interface is optional for method calls */
			/*log_debug("sender missing on non-method");*/
			return false;
		}
	}
	if (filter->member) {
		val = dbus_message_get_member(msg);
		if (!val || strcmp(filter->member, val)) {
			/*log_debug("member mismatch: %s", val);*/
			return false;
		}
	}
	if (filter->path) {
		val = dbus_message_get_path(msg);
		/* Match entire path, or the prefix specified by the filter */
		if (!val || strcmp(filter->path, val)) {
			/*log_debug("path mismatch: %s", filter->path);*/
			return false;
		}
	}
	if (filter->destination) {
		val = dbus_message_get_destination(msg);
		if (!val || strcmp(filter->destination, val)) {
			/*log_debug("destination mismatch: %s", val);*/
			return false;
		}
	}
	/*log_debug("filter match");*/
	return true;
}

int dbus_utils_match_rule_create(char *buf, size_t buf_size,
	const struct dbus_client_msg_filter *filter, DBusConnection *conn)
{
	size_t offset = 0;
	const char *unique_name;

	if (filter->type != DBUS_MSG_TYPE_INVALID) {
		if (dbus_utils_match_rule_append(buf, buf_size, &offset,
		    "type", dbus_message_type_to_string(filter->type)) < 0) {
			goto rule_overflow;
		}
	}
	if (filter->sender) {
		if (dbus_utils_match_rule_append(buf, buf_size, &offset,
		    "sender", filter->sender) < 0) {
			goto rule_overflow;
		}
	}
	if (filter->interface) {
		if (dbus_utils_match_rule_append(buf, buf_size, &offset,
		    "interface", filter->interface) < 0) {
			goto rule_overflow;
		}
	}
	if (filter->member) {
		if (dbus_utils_match_rule_append(buf, buf_size, &offset,
		    "member", filter->member) < 0) {
			goto rule_overflow;
		}
	}
	if (filter->path) {
		if (dbus_utils_match_rule_append(buf, buf_size, &offset,
		    "path", filter->path) < 0) {
			goto rule_overflow;
		}
	}
	if (filter->destination) {
		if (dbus_utils_match_rule_append(buf, buf_size, &offset,
		    "destination", filter->destination) < 0) {
			goto rule_overflow;
		}
		/*
		 * Attempting to subscribe to non-broadcast messages not
		 * addressed to us requires "evesdropping".
		 */
		unique_name = dbus_bus_get_unique_name(conn);
		if (unique_name && strcmp(filter->destination, unique_name)) {
			if (dbus_utils_match_rule_append(buf, buf_size,
			    &offset, "eavesdrop", "true") < 0) {
				goto rule_overflow;
			}
		}
	}
	if (!offset) {
		buf[0] = '\0';
	}
	return 0;
rule_overflow:
	log_err("match rule exceeded max length: %zu", buf_size - 1);
	return -1;
}

/*****************************************
 * D-Bus message UUIDs variant parsing
 *****************************************/
int dbus_utils_parse_uuid_variant(DBusMessageIter *iter,
	char **uuids, int uuid_max)
{
	DBusMessageIter child_iter;
	int arg_type;
	const char *str_val;
	int count = 0;

	ASSERT(uuids != NULL);
	if (!iter) {
		return -1;
	}

	arg_type = dbus_message_iter_get_arg_type(iter);
	if (arg_type != DBUS_TYPE_VARIANT) {
		return -1;
	}

	dbus_message_iter_recurse(iter, &child_iter);
	arg_type = dbus_message_iter_get_arg_type(&child_iter);
	if (arg_type != DBUS_TYPE_ARRAY) {
		return -1;
	}

	iter = &child_iter;
	dbus_message_iter_recurse(iter, &child_iter);
	arg_type = dbus_message_iter_get_arg_type(&child_iter);

	while (arg_type == DBUS_TYPE_STRING) {
		str_val = dbus_utils_parse_string(&child_iter);
		if (str_val) {
			uuids[count] = strdup(str_val);
			log_debug("uuids[%d]: %s", count, uuids[count]);
			count++;
			if (count >= uuid_max) {
				return count;
			}
		}
		dbus_message_iter_next(&child_iter);
		arg_type = dbus_message_iter_get_arg_type(&child_iter);
	}

	return count;
}

