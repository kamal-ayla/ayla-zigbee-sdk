/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __BT_UTILS_H__
#define __BT_UTILS_H__

#include <sys/queue.h>

#include <ayla/token_table.h>
#include <ayla/gateway_interface.h>
#include <app/props.h>
#include <app/gateway.h>

#include "node.h"

#define BT_GATT_SUBDEVICE_DEFAULT	"00"

#define BT_UUID_128_LEN		16
#define BT_UUID_16_LEN		2
#define BT_UUID_128_STR_LEN	36
#define BT_UUID_16_STR_LEN	4

#define BT_UUID_16_TO_128(byte0, byte1)				\
	{ 0x00, 0x00, (byte0), (byte1), 0x00, 0x00, 0x10, 0x00,	\
	0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb }

/*
 * 128-bit UUID used to identify Bluetooth resources.
 */
struct bt_uuid {
	u8 val[BT_UUID_128_LEN];
};

/*
 * Supported GATT characteristic flags
 */
#define BT_GATT_FLAGS(def)						\
	def(broadcast,			BT_GATT_BROADCAST)		\
	def(read,			BT_GATT_READ)			\
	def(write-without-response,	BT_GATT_WRITE_WITHOUT_RESP)	\
	def(write,			BT_GATT_WRITE)			\
	def(notify,			BT_GATT_NOTIFY)			\
	def(indicate,			BT_GATT_INDICATE)		\
	def(authenticated-signed-writes, BT_GATT_AUTH_SIGNED_WRITES)	\
	def(reliable-write,		BT_GATT_RELIABLE_WRITE)		\
	def(writable-auxiliaries,	BT_GATT_WRITABLE_AUX)		\
	def(encrypt-read,		BT_GATT_ENCRYPT_READ)		\
	def(encrypt-write,		BT_GATT_ENCRYPT_WRITE)		\
	def(encrypt-authenticated-read,	BT_GATT_ENCRYPT_AUTH_READ)	\
	def(encrypt-authenticated-write, BT_GATT_ENCRYPT_AUTH_WRITE)	\
	def(secure-read,		BT_GATT_SECURE_READ)		\
	def(secure-write,		BT_GATT_SECURE_WRITE)

DEF_ENUM(bt_gatt_flag, BT_GATT_FLAGS);

/*
 * Structure containing a variable sized GATT characteristic raw byte value.
 */
struct bt_gatt_val {
	size_t size;
	size_t len;
	void *data;
};

/*
 * Ayla definition for a template mapped to a GATT service.
 */
struct bt_gatt_db_template {
	const char *key;
	const char *version;
};

/*
 * Extended Ayla property definition for props mapped to a GATT characteristic.
 */
struct bt_gatt_db_prop {
	const char *subdevice;
	struct node_prop_def def;
	int (*val_set)(struct node *, struct node_prop *, struct bt_gatt_val *);
	int (*val_send)(struct node *, struct node_prop *,
		const struct bt_gatt_val *);
	SLIST_ENTRY(bt_gatt_db_prop) entry;
};
SLIST_HEAD(bt_gatt_db_prop_list, bt_gatt_db_prop);

/*****************************************
 * UUID Utilities
 *****************************************/

int bt_uuid_parse(struct bt_uuid *uuid, const char *string);

ssize_t bt_uuid_string_r(const struct bt_uuid *uuid,
	char *buf, size_t buf_size);

const char *bt_uuid_string(const struct bt_uuid *uuid);

int bt_uuid_compare(const struct bt_uuid *uuid1, const struct bt_uuid *uuid2);

int bt_uuid_compare_string(const struct bt_uuid *uuid1, const char *uuid2);

bool bt_uuid_valid(const struct bt_uuid *uuid);

/*****************************************
 * GATT characteristic flag Lookup
 *****************************************/

u32 bt_gatt_flag_parse(const char *str);

/*****************************************
 * GATT Value Utilities
 *****************************************/

void bt_gatt_val_host_to_network(void *out, const void *in, size_t size);

void bt_gatt_val_cleanup(struct bt_gatt_val *val);

int bt_gatt_val_set(struct bt_gatt_val *val, const void *data, size_t data_len);

int bt_gatt_val_update(struct bt_gatt_val *val, const void *data,
	size_t data_len, size_t offset);

int bt_gatt_val_set_string(struct bt_gatt_val *val, const char *string);

const char *bt_gatt_val_get_string(const struct bt_gatt_val *val);

s32 bt_gatt_val_get_int32(const struct bt_gatt_val *val);

double bt_gatt_val_get_double(const struct bt_gatt_val *val);

/*****************************************
 * GATT service to Ayla template database
 *****************************************/

#define BT_GATT_DB_PROP_LIST_FOREACH(prop, prop_list)	\
	SLIST_FOREACH(prop, prop_list, entry)

int bt_gatt_db_init(void);

void bt_gatt_db_cleanup(void);

int bt_gatt_db_add_template(const char *service_uuid, const char *template_key,
	const char *template_version);

int bt_gatt_db_add_prop(const char *characteristic_uuid, const char *subdevice,
	const char *name, enum prop_type type, enum prop_direction dir,
	int (*val_set)(struct node *, struct node_prop *, struct bt_gatt_val *),
	int (*val_send)(struct node *, struct node_prop *,
	const struct bt_gatt_val *));

struct bt_gatt_db_template *bt_gatt_db_lookup_template(
	const struct bt_uuid *service_uuid);

struct bt_gatt_db_prop_list *bt_gatt_db_lookup_props(
	const struct bt_uuid *characteristic_uuid);

#endif /* __BT_UTILS_H__ */
