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
#include <endian.h>
#include <sys/queue.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/hex.h>
#include <ayla/hashmap.h>
#include <ayla/nameval.h>
#include <ayla/gateway_interface.h>

#include <app/props.h>
#include <app/gateway.h>

#include "bt_utils.h"

static DEF_NAMEVAL_TABLE(bt_gatt_flag_table, BT_GATT_FLAGS);

/*
 * Byte swap numbers of any size.  Memory pointed to by in and out may overlap.
 */
static void bt_byte_swap(void *out, const void *in, size_t size)
{
	u8 buf[size];

	/* Zero byte case */
	if (!size) {
		return;
	}
	/* Single byte case */
	if (size == 1) {
		*((u8 *)out) = *((u8 *)in);
		return;
	}
	/* Swap using temp buffer to allow overlapping src and dest */
	while (size--) {
		buf[size] = *((const u8 *)in++);
	}
	memcpy(out, buf, size);
}

/*****************************************
 * UUID Utilities
 *****************************************/

static void bt_uuid_expand(struct bt_uuid *uuid)
{
	u8 temp[] = BT_UUID_16_TO_128(uuid->val[0], uuid->val[1]);

	memcpy(uuid->val, temp, sizeof(uuid->val));
}

/*
 * Simple hash function for UUID keys.  This algorithm incorporates the 32-bit
 * golden ratio and was inspired by the Boost UUID library (www.boost.org).
 */
static size_t bt_uuid_hash(const void *key)
{
	const struct bt_uuid *uuid = (const struct bt_uuid *)key;
	size_t hash = 0;
	const u8 *byte;

	for (byte = uuid->val; byte < &uuid->val[sizeof(uuid->val)]; ++byte) {
		hash ^= (hash << 6) + (hash >> 2) + (size_t)*byte + 0x9e3779b9;
	}
	return hash;
}

/*
 * Key comparator function for UUID keys.
 */
static int bt_uuid_hash_compare(const void *a, const void *b)
{
	return bt_uuid_compare((const struct bt_uuid *)a,
	    (const struct bt_uuid *)b);
}

int bt_uuid_parse(struct bt_uuid *uuid, const char *string)
{
	const char *cp = string;
	size_t len = 0;

	ASSERT(uuid != NULL);
	ASSERT(string != NULL);

	while (*cp) {
		if (len >= sizeof(uuid->val)) {
			log_err("UUID too long: %s", string);
			return -1;
		}
		if (*cp == '-') {
			/* Ignore dashes in UUID */
			++cp;
			continue;
		}
		cp = hex_parse_byte(cp, uuid->val + len);
		if (!cp) {
			log_err("UUID contains invalid char: %s", string);
			return -1;
		}
		++len;
	}
	switch (len) {
	case BT_UUID_16_LEN:
		/* Always expand to 128-bit UUID */
		bt_uuid_expand(uuid);
		break;
	case BT_UUID_128_LEN:
		break;
	default:
		log_err("UUID is unsupported length: %zu", len);
		return -1;
	}
	return 0;
}

ssize_t bt_uuid_string_r(const struct bt_uuid *uuid,
	char *buf, size_t buf_size)
{
	/* XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
	static const u8 uuid_128_template[] = { 4, 2, 2, 2, 6 };
	const u8 *template = uuid_128_template;
	size_t len = 0;
	size_t i;
	ssize_t rc;

	ASSERT(uuid != NULL);
	ASSERT(buf != NULL);

	for (i = 0; i < sizeof(uuid->val); i += *template, ++template) {
		if (i > 0) {
			buf[len++] = '-';
		}
		rc = hex_string(buf + len, buf_size - len, uuid->val + i,
		    *template, false, 0);
		if (rc < 0) {
			log_err("UUID value corrupt");
			return -1;
		}
		len += rc;
	}
	return len;
}

const char *bt_uuid_string(const struct bt_uuid *uuid)
{
	static char buf[BT_UUID_128_STR_LEN + 1];

	ASSERT(uuid != NULL);

	if (bt_uuid_string_r(uuid, buf, sizeof(buf)) < 0) {
		return "";
	}
	return buf;
}

int bt_uuid_compare(const struct bt_uuid *uuid1, const struct bt_uuid *uuid2)
{
	ASSERT(uuid1 != NULL);
	ASSERT(uuid2 != NULL);

	return memcmp(uuid1->val, uuid2->val, sizeof(uuid1->val));
}

int bt_uuid_compare_string(const struct bt_uuid *uuid1, const char *uuid2)
{
	struct bt_uuid temp;

	ASSERT(uuid1 != NULL);
	ASSERT(uuid2 != NULL);

	if (bt_uuid_parse(&temp, uuid2) < 0) {
		return 1;
	}
	return bt_uuid_compare(uuid1, &temp);
}

bool bt_uuid_valid(const struct bt_uuid *uuid)
{
	const u8 *byte;

	ASSERT(uuid != NULL);

	for (byte = uuid->val; byte < &uuid->val[sizeof(uuid->val)]; ++byte) {
		if (*byte) {
			return true;
		}
	}
	return false;
}

/*****************************************
 * GATT characteristic flag Lookup
 *****************************************/

u32 bt_gatt_flag_parse(const char *str)
{
	int rc;

	ASSERT(str != NULL);

	rc = lookup_by_name(bt_gatt_flag_table, str);
	if (rc < 0) {
		return 0;
	}
	return BIT(rc);
}

/*****************************************
 * GATT Value Utilities
 *****************************************/

void bt_gatt_val_host_to_network(void *out, const void *in, size_t size)
{
	/* No swap needed, since network is little endian */
#if __BYTE_ORDER == __LITTLE_ENDIAN
	if (out != in) {
		memmove(out, in, size);
	}
	return;
#endif
	/* Otherwise, swap */
	bt_byte_swap(out, in, size);
}

void bt_gatt_val_cleanup(struct bt_gatt_val *val)
{
	if (!val) {
		return;
	}
	free(val->data);
	memset(val, 0, sizeof(*val));
}

static int bt_gatt_val_resize(struct bt_gatt_val *val, size_t new_size)
{
	void *new_data;

	new_data = realloc(val->data, new_size);
	if (!new_data) {
		log_err("malloc failed");
		return -1;
	}
	val->data = new_data;
	val->size = new_size;
	if (val->len > new_size) {
		val->len = new_size;
	}
	return 0;
}

int bt_gatt_val_set(struct bt_gatt_val *val, const void *data, size_t data_len)
{
	if (data_len > val->size) {
		if (bt_gatt_val_resize(val, data_len) < 0) {
			return -1;
		}
	}
	memcpy(val->data, data, data_len);
	val->len = data_len;
	return 0;
}

int bt_gatt_val_update(struct bt_gatt_val *val, const void *data,
	size_t data_len, size_t offset)
{
	size_t new_len = data_len + offset;

	if (new_len > val->size) {
		if (bt_gatt_val_resize(val, new_len) < 0) {
			return -1;
		}
	}
	if (offset > val->len) {
		/* Zero out sparse segment of buffer */
		memset((u8 *)val->data + val->len, 0, offset - val->len);
	}
	if (data) {
		memcpy((u8 *)val->data + offset, data, data_len);
	} else {
		memset((u8 *)val->data + offset, 0, data_len);
	}
	if (val->len < new_len) {
		val->len = new_len;
	}
	return 0;
}

int bt_gatt_val_set_string(struct bt_gatt_val *val, const char *string)
{
	return bt_gatt_val_set(val, string, strlen(string));
}

const char *bt_gatt_val_get_string(const struct bt_gatt_val *val)
{
	struct bt_gatt_val *mutable_val = (struct bt_gatt_val *)val;

	if (!val->len) {
		return "";
	}
	/* Add NULL termination for in-place access */
	if (val->len == val->size) {
		bt_gatt_val_update(mutable_val, "", 1, val->len);
		--mutable_val->len;
	} else {
		((u8 *)mutable_val->data)[val->len] = '\0';
	}
	return (const char *)val->data;
}

s32 bt_gatt_val_get_int32(const struct bt_gatt_val *val)
{
	s32 int_val;

	if (!val->len) {
		return 0;
	}
	switch (val->len) {
	case 1:
		int_val = *((u8 *)val->data);
		break;
	case 2:
		int_val = le16toh(*((u16 *)val->data));
		break;
	case 4:
		int_val = le32toh(*((u32 *)val->data));
		break;
	default:
		log_err("unsupported integer precision: %zu", val->len);
		return 0;
	}
	return int_val;
}

double bt_gatt_val_get_double(const struct bt_gatt_val *val)
{
	double double_val;

	if (!val->len) {
		return 0;
	}
	switch (val->len) {
	case 4:
		double_val = (float)le32toh(*((u32 *)val->data));
		break;
	case 8:
		double_val = (double)le64toh(*((u64 *)val->data));
		break;
	default:
		log_err("unsupported floating point precision: %zu", val->len);
		return 0;
	}
	return double_val;
}

/*****************************************
 * GATT service to Ayla template database
 *****************************************/

struct bt_gatt_db {
	struct hashmap services;
	struct hashmap characteristics;
};

struct bt_gatt_db_service {
	struct bt_uuid uuid;
	struct bt_gatt_db_template template;
};

struct bt_gatt_db_char {
	struct bt_uuid uuid;
	struct bt_gatt_db_prop_list props;
};

/* Declare type-specific hashmap interface for GATT database entries */
HASHMAP_FUNCS_CREATE(bt_gatt_db_service, const struct bt_uuid,
	struct bt_gatt_db_service)
HASHMAP_FUNCS_CREATE(bt_gatt_db_char, const struct bt_uuid,
	struct bt_gatt_db_char)

static struct bt_gatt_db gatt_db;

int bt_gatt_db_init(void)
{
	if (hashmap_init(&gatt_db.services, bt_uuid_hash,
	    bt_uuid_hash_compare, 10) < 0 ||
	    hashmap_init(&gatt_db.characteristics, bt_uuid_hash,
	    bt_uuid_hash_compare, 20) < 0) {
		return -1;
	}
	return 0;
}

void bt_gatt_db_cleanup(void)
{
	struct hashmap_iter *iter;
	struct bt_gatt_db_service *service_def;
	struct bt_gatt_db_char *char_def;
	struct bt_gatt_db_prop *prop_def;

	for (iter = hashmap_iter(&gatt_db.services); iter;
	    iter = hashmap_iter_remove(&gatt_db.services, iter)) {
		service_def = bt_gatt_db_service_hashmap_iter_get_data(iter);
		free(service_def);
	}
	for (iter = hashmap_iter(&gatt_db.characteristics); iter;
	    iter = hashmap_iter_remove(&gatt_db.characteristics, iter)) {
		char_def = bt_gatt_db_char_hashmap_iter_get_data(iter);
		while ((prop_def =
		    SLIST_FIRST(&char_def->props)) != NULL) {
			SLIST_REMOVE_HEAD(&char_def->props, entry);
			free(prop_def);
		}
		free(char_def);
	}
}

int bt_gatt_db_add_template(const char *service_uuid, const char *template_key,
	const char *template_version)
{
	struct bt_gatt_db_service *service_def;

	ASSERT(service_uuid != NULL && *service_uuid != '\0');
	ASSERT(template_key != NULL && *template_key != '\0');
	ASSERT(strlen(template_key) < GW_TEMPLATE_KEY_SIZE);

	service_def = (struct bt_gatt_db_service *)calloc(1,
	    sizeof(*service_def));
	if (!service_def) {
		log_err("malloc failed");
		return -1;
	}
	if (bt_uuid_parse(&service_def->uuid, service_uuid) < 0) {
		free(service_def);
		return -1;
	}
	if (bt_gatt_db_service_hashmap_put(&gatt_db.services,
	    &service_def->uuid, service_def) != service_def) {
		free(service_def);
		log_err("failed to add GATT database service entry: %s --> %s",
		    service_uuid, template_key);
		return -1;
	}
	service_def->template.key = template_key;
	service_def->template.version = template_version;
	return 0;
}

int bt_gatt_db_add_prop(const char *characteristic_uuid, const char *subdevice,
	const char *name, enum prop_type type, enum prop_direction dir,
	int (*val_set)(struct node *, struct node_prop *, struct bt_gatt_val *),
	int (*val_send)(struct node *, struct node_prop *,
	const struct bt_gatt_val *))
{
	struct bt_gatt_db_char *char_def;
	struct bt_gatt_db_prop *prop_def;
	struct bt_uuid uuid;

	ASSERT(characteristic_uuid != NULL && *characteristic_uuid != '\0');
	ASSERT(name != NULL && *name != '\0');
	ASSERT(strlen(name) < GW_PROPERTY_NAME_SIZE);

	if (bt_uuid_parse(&uuid, characteristic_uuid) < 0) {
		return -1;
	}
	prop_def = (struct bt_gatt_db_prop *)calloc(1, sizeof(*prop_def));
	if (!prop_def) {
		log_err("malloc failed");
		return -1;
	}
	if (!subdevice || !subdevice[0]) {
		/* Populate a default subdevice "00" if none is specified */
		subdevice = BT_GATT_SUBDEVICE_DEFAULT;
	}
	prop_def->subdevice = subdevice;
	prop_def->def.name = name;
	prop_def->def.type = type;
	prop_def->def.dir = dir;
	prop_def->val_set = val_set;
	prop_def->val_send = val_send;
	char_def = bt_gatt_db_char_hashmap_get(&gatt_db.characteristics, &uuid);
	if (!char_def) {
		char_def = (struct bt_gatt_db_char *)calloc(1,
		    sizeof(*char_def));
		if (!char_def) {
			log_err("malloc failed");
			free(prop_def);
			return -1;
		}
		char_def->uuid = uuid;
		SLIST_INIT(&char_def->props);
		if (bt_gatt_db_char_hashmap_put(&gatt_db.characteristics,
		    &char_def->uuid, char_def) != char_def) {
			free(char_def);
			free(prop_def);
			log_err("failed to add GATT database characteristic "
			    "entry: %s --> %s", characteristic_uuid, name);
			return -1;
		}
	}
	SLIST_INSERT_HEAD(&char_def->props, prop_def, entry);
	return 0;
}

struct bt_gatt_db_template *bt_gatt_db_lookup_template(
	const struct bt_uuid *service_uuid)
{
	struct bt_gatt_db_service *service_def;

	service_def = bt_gatt_db_service_hashmap_get(&gatt_db.services,
	    service_uuid);
	if (!service_def) {
		return NULL;
	}
	return &service_def->template;
}

struct bt_gatt_db_prop_list *bt_gatt_db_lookup_props(
	const struct bt_uuid *characteristic_uuid)
{
	struct bt_gatt_db_char *char_def;

	char_def = bt_gatt_db_char_hashmap_get(&gatt_db.characteristics,
	    characteristic_uuid);
	if (!char_def) {
		return NULL;
	}
	return &char_def->props;
}
