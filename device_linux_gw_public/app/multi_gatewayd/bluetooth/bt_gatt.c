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

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/hex.h>
#include <ayla/gateway_interface.h>
#include <ayla/json_parser.h>
#include <ayla/json_interface.h>

#include <app/props.h>
#include <app/gateway.h>

#include "node.h"
#include "bt_utils.h"
#include "bt_gatt.h"


/*
 * GATT property definition table.
 */
struct bt_gatt_prop_table_entry {
	const char *name;
	enum prop_type type;
	enum prop_direction dir;
	int (*val_set)(struct node *, struct node_prop *, struct bt_gatt_val *);
	int (*val_send)(struct node *, struct node_prop *,
	    const struct bt_gatt_val *);
};

/*
 * Convenience function to load a table of properties into the GATT database.
 */
static int bt_gatt_add_prop_table(const char *characteristic_uuid,
	const char *subdevice, struct bt_gatt_prop_table_entry *table,
	size_t num_props)
{
	int rc = 0;

	for (; num_props; --num_props, ++table) {
		rc |= bt_gatt_db_add_prop(characteristic_uuid, subdevice,
		    table->name, table->type, table->dir,
		    table->val_set, table->val_send);
	}
	return rc;
}

/*
 * Generic send function for characteristics whose entire value will be sent
 * as a single property.
 */
static int bt_gatt_val_send_prop(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	s32 int_val;
	const char *str_val;
	double double_val;

	switch (prop->type) {
	case PROP_INTEGER:
		int_val = bt_gatt_val_get_int32(val);
		return node_prop_integer_send(node, prop, int_val);
	case PROP_STRING:
		str_val = bt_gatt_val_get_string(val);
		if (!str_val) {
			log_err("invalid string value");
			return -1;
		}
		return node_prop_string_send(node, prop, str_val);
	case PROP_BOOLEAN:
		return node_prop_boolean_send(node, prop,
		    bt_gatt_val_get_int32(val) ? true : false);
	case PROP_DECIMAL:
		double_val = bt_gatt_val_get_double(val);
		return node_prop_decimal_send(node, prop, double_val);
	default:
		log_err("property type not supported: %s:%s:%s",
		    prop->subdevice->key, prop->template->key, prop->name);
	}
	return -1;
}

/*
 * Send a characteristic as a hex string property value.
 */
static int bt_gatt_val_send_hex_string(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	char hex_str[val->len * 3];

	if (hex_string(hex_str, sizeof(hex_str), val->data, val->len,
	    true, '-') < 0) {
		return -1;
	}
	return node_prop_string_send(node, prop, hex_str);
}

static int bt_gatt_init_info(void)
{
	int rc = 0;
	const char *subdevice = "dev";

	rc |= bt_gatt_db_add_template("180a", "info", NULL);
	rc |= bt_gatt_db_add_prop("2a29", subdevice,
		"mfg_name", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("2a24", subdevice,
		"model_num", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("2a25", subdevice,
		"serial_num", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("2a27", subdevice,
		"hw_revision", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("2a26", subdevice,
		"fw_revision", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("2a28", subdevice,
		"sw_revision", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("2a23", subdevice,
		"system_id", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_hex_string);
	return rc;
}

static int bt_gatt_val_send_battery_level(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	if (bt_gatt_val_send_prop(node, prop, val) < 0) {
		return -1;
	}
	if (node->power != GP_BATTERY) {
		/* Update node attributes if battery_service is supported */
		node->power = GP_BATTERY;
		log_debug("%s node_info_changed version NULL", node->addr);
		node_info_changed(node, NULL);
	}
	return 0;
}

static int bt_gatt_init_battery(void)
{
	int rc = 0;
	const char *subdevice = "dev";

	rc |= bt_gatt_db_add_template("180f", "battery", NULL);
	rc |= bt_gatt_db_add_prop("2a19", subdevice,
		"level", PROP_INTEGER, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_battery_level);
	return rc;
}

static int bt_gatt_val_send_sensor_loc(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	const char *loc_strs[] = {
		"other",
		"chest",
		"wrist",
		"finger",
		"hand",
		"ear_lobe",
		"foot"
	};
	u8 loc;

	if (val->len != sizeof(loc)) {
		return -1;
	}
	loc = *(u8 *)val->data;
	if (loc >= ARRAY_LEN(loc_strs)) {
		return node_prop_string_send(node, prop, "unsupported");
	}
	return node_prop_string_send(node, prop, loc_strs[loc]);
}

static int bt_gatt_val_set_reset_energy_exp(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	bool reset = node_prop_boolean_val(prop);
	const u8 reset_val = 1;
	int rc;

	if (!reset) {
		return 1;
	}
	rc = bt_gatt_val_set(val, &reset_val, 1);
	node_prop_boolean_send(node, prop, false);
	return rc;
}

static int bt_gatt_init_heart(void)
{
	int rc = 0;
	const char *subdevice = NULL;

	rc |= bt_gatt_db_add_template("180d", "heart", NULL);
	/* TODO split heart rate characteristic into individual props */
	rc |= bt_gatt_db_add_prop("2a37", subdevice,
		"rate", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_hex_string);
	rc |= bt_gatt_db_add_prop("2a38", subdevice,
		"sensor_loc", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_sensor_loc);
	rc |= bt_gatt_db_add_prop("2a39", subdevice,
		"reset_energy_exp", PROP_BOOLEAN, PROP_TO_DEVICE,
		bt_gatt_val_set_reset_energy_exp, NULL);
	return rc;
}

static int bt_gatt_init_ayla(void)
{
	int rc = 0;
	const char *subdevice = "dev";

	rc |= bt_gatt_db_add_template("fe28", "ayla", NULL);
	rc |= bt_gatt_db_add_prop("00000001-fe28-435b-991a-f1b21bb9bcd0",
		subdevice, "unique_id", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("00000002-fe28-435b-991a-f1b21bb9bcd0",
		subdevice, "oem", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("00000003-fe28-435b-991a-f1b21bb9bcd0",
		subdevice, "oem_model", PROP_STRING, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	return rc;
}

static int bt_gatt_init_ayla_tstat(void)
{
	int rc = 0;
	const char *subdevice = NULL;

	rc |= bt_gatt_db_add_template("28e7b565-0215-46d7-a924-b8e7c48eab9b",
	    "gg_tstat", NULL);
	rc |= bt_gatt_db_add_prop("1950c6c9-6566-4608-8210-d712e3df95b0",
		subdevice, "ac_on", PROP_BOOLEAN, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("1950c6c9-6566-4608-8210-d712e3df95b1",
		subdevice, "heat_on", PROP_BOOLEAN, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("1950c6c9-6566-4608-8210-d712e3df95b2",
		subdevice, "local_temp", PROP_INTEGER, PROP_FROM_DEVICE,
		NULL, bt_gatt_val_send_prop);
	rc |= bt_gatt_db_add_prop("1950c6c9-6566-4608-8210-d712e3df95b3",
		subdevice, "temp_setpoint", PROP_INTEGER, PROP_TO_DEVICE,
		NULL, NULL);	/* TODO generic set handler */
	rc |= bt_gatt_db_add_prop("1950c6c9-6566-4608-8210-d712e3df95b4",
		subdevice, "vacation_mode", PROP_BOOLEAN, PROP_TO_DEVICE,
		NULL, NULL);	/* TODO generic set handler */
	return rc;
}

/*
Magic blue bulb value range
0 <= r/g/b <= 255
0 <= white <= 100
0 <= fade_rate <= 100
*/

/*
 * Values for the bulb power type.
 */
enum bt_gatt_bulb_power {
	BT_BULB_POWER_OFF			= 0,
	BT_BULB_POWER_ON			= 1
};

/*
 * Values for the bulb fade type.
 */
enum bt_gatt_bulb_fade {
	BT_BULB_MODE_FADE_RAINBOW		= 0,
	BT_BULB_MODE_FADE_RED			= 1,
	BT_BULB_MODE_FADE_GREEN			= 2,
	BT_BULB_MODE_FADE_BLUE			= 3,
	BT_BULB_MODE_FADE_YELLOW		= 4,
	BT_BULB_MODE_FADE_CYAN			= 5,
	BT_BULB_MODE_FADE_PURPLE		= 6,
	BT_BULB_MODE_FADE_WHITE			= 7,
	BT_BULB_MODE_FADE_RED_GREEN		= 8,
	BT_BULB_MODE_FADE_RED_BLUE		= 9,
	BT_BULB_MODE_FADE_GREEN_BLUE		= 10,
	BT_BULB_MODE_FADE_RAINBOW_STROBE	= 11,
	BT_BULB_MODE_FADE_RED_STROBE		= 12,
	BT_BULB_MODE_FADE_GREEN_STROBE		= 13,
	BT_BULB_MODE_FADE_BLUE_STROBE		= 14,
	BT_BULB_MODE_FADE_YELLOW_STROBE		= 15,
	BT_BULB_MODE_FADE_CYAN_STROBE		= 16,
	BT_BULB_MODE_FADE_PURPLE_STROBE		= 17,
	BT_BULB_MODE_FADE_WHITE_STROBE		= 18,
	BT_BULB_MODE_FADE_RAINBOW_HOP		= 19
};

/*
 * Values for the bulb mode type.
 */
enum bt_gatt_bulb_mode {
	BT_BULB_MODE_RGB			= 1,
	BT_BULB_MODE_WHITE			= 2,
	BT_BULB_MODE_FADE			= 3
};

/*
 * Check if the bulb mode property value is same as the parameter value.
 * if yes, return 0;
 * if no, return 1;
 * if not found property, return -1;
 */
static int bt_gatt_val_check_bulb_mode(struct node *node,
	struct node_prop *prop, enum bt_gatt_bulb_mode mode)
{
	struct node_prop *prop_mode;
	int mode_val;

	/* Check mode prop */
	prop_mode = node_prop_lookup(node, prop->subdevice->key,
	    prop->template->key, "mode");
	if (!prop_mode) {
		log_err("node %s no mode prop", node->addr);
		return -1;
	}
	mode_val = node_prop_integer_val(prop_mode);

	if (mode_val != mode) {
		log_debug("node %s mode %d, the parameter mode %d",
		    node->addr, mode_val, mode);
		return 1;
	}

	return 0;
}

/*
 * Demo set function that works for MagicBlue bulbs. These bulbs
 * advertise service UUID ffe5 with characteristic UUID ffe9.
 * This characteristic is write-only and
 * various values may be set for RGB, white, and fade modes.
 */
static int bt_gatt_val_set_bulb_mode(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	u8 val_solid[] = { 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA };
	u8 val_fade[] = { 0xBB, 0x00, 0x00, 0x44 };
	enum bt_gatt_bulb_mode mode;
	struct node_prop *prop_rgb, *prop_white;
	struct node_prop *prop_fade_rate, *prop_fade;
	int rgb, white, fade, fade_rate;

	mode = node_prop_integer_val(prop);
	switch (mode) {
	case BT_BULB_MODE_RGB:
		/* Get rgb prop value */
		prop_rgb = node_prop_lookup(node, prop->subdevice->key,
		    prop->template->key, "rgb");
		if (!prop_rgb) {
			log_err("node %s no rgb prop", node->addr);
			return -1;
		}
		rgb = node_prop_integer_val(prop_rgb);
		/* Set RGB values, 0xRRGGBB */
		val_solid[1] = ((rgb >> 16) & 0xFF);
		val_solid[2] = ((rgb >> 8) & 0xFF);
		val_solid[3] = ((rgb >> 0) & 0xFF);
		/* RGB mode select */
		val_solid[5] =  0xF0;
		if (bt_gatt_val_set(val, &val_solid, sizeof(val_solid)) < 0) {
			log_err("node %s prop %s bt_gatt_val_set error",
			    node->addr, prop->name);
			return -1;
		}
		break;
	case BT_BULB_MODE_WHITE:
		/* Get white prop value */
		prop_white = node_prop_lookup(node, prop->subdevice->key,
		    prop->template->key, "white");
		if (!prop_white) {
			log_err("node %s no white prop", node->addr);
			return -1;
		}
		white = node_prop_integer_val(prop_white);
		/* Set white value */
		val_solid[4] = (white & 0xFF);
		if (val_solid[4] > 100) {
			val_solid[4] = 100;
		}
		/* White mode select */
		val_solid[5] = 0x0F;
		if (bt_gatt_val_set(val, &val_solid, sizeof(val_solid)) < 0) {
			log_err("node %s prop %s bt_gatt_val_set error",
			    node->addr, prop->name);
			return -1;
		}
		break;
	case BT_BULB_MODE_FADE:
		/* Update fade prop */
		prop_fade = node_prop_lookup(node, prop->subdevice->key,
		    prop->template->key, "fade");
		if (!prop_fade) {
			log_err("node %s no fade prop", node->addr);
			return -1;
		}
		fade = node_prop_integer_val(prop_fade);

		/* Get fade_rate prop value */
		prop_fade_rate = node_prop_lookup(node, prop->subdevice->key,
		    prop->template->key, "fade_rate");
		if (!prop_fade_rate) {
			log_err("node %s no fade_rate prop", node->addr);
			return -1;
		}
		fade_rate = node_prop_integer_val(prop_fade_rate);

		/* Fade modes start at 0x25 */
		val_fade[1] = 0x25 + fade;
		/* Set fade rate value */
		val_fade[2] = (fade_rate & 0xFF);
		if (val_fade[2] > 100) {
			val_fade[2] = 100;
		}
		val_fade[2] = (100 - val_fade[2]);
		if (bt_gatt_val_set(val, &val_fade, sizeof(val_fade)) < 0) {
			log_err("node %s prop %s bt_gatt_val_set error",
			    node->addr, prop->name);
			return -1;
		}
		break;
	default:
		log_warn("unsupported bulb mode: %d", mode);
		return -1;
		break;
	}
	return 0;
}

/*
 * Set magic blue bulb on or off.
 */
static int bt_gatt_val_set_bulb_onoff(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	u8 val_onoff[] = { 0xCC, 0x003, 0x33 };
	bool onoff;

	onoff = node_prop_boolean_val(prop);
	if (onoff) {
		val_onoff[1] = 0x23; /* On */
	} else {
		val_onoff[1] = 0x24; /* Off */
	}
	if (bt_gatt_val_set(val, &val_onoff, sizeof(val_onoff)) < 0) {
		log_err("node %s prop %s bt_gatt_val_set error",
		    node->addr, prop->name);
		return -1;
	}

	return 0;
}

/*
 * Set magic blue bulb rgb color.
 */
static int bt_gatt_val_set_bulb_rgb(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	u8 val_solid[] = { 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA };
	int ret, rgb;

	/* mode must be rgb */
	ret = bt_gatt_val_check_bulb_mode(node, prop, BT_BULB_MODE_RGB);
	if (ret) {
		log_debug("node %s mode value not correct", node->addr);
		return ret;
	}

	rgb = node_prop_integer_val(prop);

	/* Set RGB values, RGB format: 0xRRGGBB */
	val_solid[1] = ((rgb >> 16) & 0xFF);
	val_solid[2] = ((rgb >> 8) & 0xFF);
	val_solid[3] = (rgb & 0xFF);
	/* RGB mode select */
	val_solid[5] =  0xF0;
	if (bt_gatt_val_set(val, &val_solid, sizeof(val_solid)) < 0) {
		log_err("node %s prop %s bt_gatt_val_set error",
		    node->addr, prop->name);
		return -1;
	}

	return 0;
}

/*
 * Set magic blue bulb white and white level.
 */
static int bt_gatt_val_set_bulb_white(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	u8 val_solid[] = { 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA };
	int ret, white;

	/* mode must be white */
	ret = bt_gatt_val_check_bulb_mode(node, prop, BT_BULB_MODE_WHITE);
	if (ret) {
		log_debug("node %s mode value not correct", node->addr);
		return ret;
	}

	white = node_prop_integer_val(prop);
	if ((white < 0) && (white > 100)) {
		log_err("node %s prop %s value %d error",
		    node->addr, prop->name, white);
		return -1;
	}

	/* Set white value */
	val_solid[4] = (white & 0xFF);
	/* White mode select */
	val_solid[5] = 0x0F;

	if (bt_gatt_val_set(val, &val_solid, sizeof(val_solid)) < 0) {
		log_err("node %s prop %s bt_gatt_val_set error",
		    node->addr, prop->name);
		return -1;
	}

	return 0;
}

/*
 * Set magic blue bulb fade.
 */
static int bt_gatt_val_set_bulb_fade(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	u8 val_fade[] = { 0xBB, 0x00, 0x00, 0x44 };
	struct node_prop *prop_fade_rate;
	int ret, fade, fade_rate;

	/* mode must be fade */
	ret = bt_gatt_val_check_bulb_mode(node, prop, BT_BULB_MODE_FADE);
	if (ret) {
		log_debug("node %s mode value not correct", node->addr);
		return ret;
	}

	fade = node_prop_integer_val(prop);
	if ((fade < 0) && (fade > BT_BULB_MODE_FADE_RAINBOW_HOP)) {
		log_err("node %s prop %s value %d error",
		    node->addr, prop->name, fade);
		return -1;
	}

	/* Get fade_rate prop value */
	prop_fade_rate = node_prop_lookup(node, prop->subdevice->key,
	    prop->template->key, "fade_rate");
	if (!prop_fade_rate) {
		log_err("node %s no fade_rate prop", node->addr);
		return -1;
	}
	fade_rate = node_prop_integer_val(prop_fade_rate);
	if ((fade_rate < 0) && (fade_rate > 100)) {
		log_err("node %s prop %s value %d error",
		    node->addr, prop->name, fade_rate);
		return -1;
	}

	/* Fade modes start at 0x25 */
	val_fade[1] = 0x25 + fade;
	/* Set fade rate value */
	val_fade[2] = (fade_rate & 0xFF);
	if (val_fade[2] > 100) {
		val_fade[2] = 100;
	}
	val_fade[2] = (100 - val_fade[2]);
	if (bt_gatt_val_set(val, &val_fade, sizeof(val_fade)) < 0) {
		log_err("node %s prop %s bt_gatt_val_set error",
		    node->addr, prop->name);
		return -1;
	}

	return 0;
}

/*
 * Set magic blue bulb fade rate.
 */
static int bt_gatt_val_set_bulb_fade_rate(struct node *node,
	struct node_prop *prop, struct bt_gatt_val *val)
{
	u8 val_fade[] = { 0xBB, 0x00, 0x00, 0x44 };
	struct node_prop *prop_fade;
	int ret, fade_rate, fade;

	/* mode must be fade */
	ret = bt_gatt_val_check_bulb_mode(node, prop, BT_BULB_MODE_FADE);
	if (ret) {
		log_debug("node %s mode value not correct", node->addr);
		return ret;
	}

	fade_rate = node_prop_integer_val(prop);
	if ((fade_rate < 0) && (fade_rate > 100)) {
		log_err("node %s prop %s value %d error",
		    node->addr, prop->name, fade_rate);
		return -1;
	}

	/* Get fade prop value */
	prop_fade = node_prop_lookup(node, prop->subdevice->key,
	    prop->template->key, "fade");
	if (!prop_fade) {
		log_err("node %s no fade prop", node->addr);
		return -1;
	}
	fade = node_prop_integer_val(prop_fade);
	if ((fade < 0) && (fade > BT_BULB_MODE_FADE_RAINBOW_HOP)) {
		log_err("node %s prop %s value %d error",
		    node->addr, prop->name, fade);
		return -1;
	}

	/* Fade modes start at 0x25 */
	val_fade[1] = 0x25 + fade;
	/* Set fade rate value */
	val_fade[2] = (fade_rate & 0xFF);
	if (val_fade[2] > 100) {
		val_fade[2] = 100;
	}
	val_fade[2] = (100 - val_fade[2]);
	if (bt_gatt_val_set(val, &val_fade, sizeof(val_fade)) < 0) {
		log_err("node %s prop %s bt_gatt_val_set error",
		    node->addr, prop->name);
		return -1;
	}

	return 0;
}

static int bt_gatt_init_bulb_rgb(void)
{
	int rc = 0;
	const char *subdevice = NULL;

	rc |= bt_gatt_db_add_template("ffe5", BT_GATT_BULB_TEMPLATE, "1.5");
	rc |= bt_gatt_db_add_prop("ffe9", subdevice,
		"mode", PROP_INTEGER, PROP_TO_DEVICE,
		bt_gatt_val_set_bulb_mode, NULL);
	rc |= bt_gatt_db_add_prop("ffe9", subdevice,
		"onoff", PROP_BOOLEAN, PROP_TO_DEVICE,
		bt_gatt_val_set_bulb_onoff, NULL);
	rc |= bt_gatt_db_add_prop("ffe9", subdevice,
		"rgb", PROP_INTEGER, PROP_TO_DEVICE,
		bt_gatt_val_set_bulb_rgb, NULL);
	rc |= bt_gatt_db_add_prop("ffe9", subdevice,
		"white", PROP_INTEGER, PROP_TO_DEVICE,
		bt_gatt_val_set_bulb_white, NULL);
	rc |= bt_gatt_db_add_prop("ffe9", subdevice,
		"fade", PROP_INTEGER, PROP_TO_DEVICE,
		bt_gatt_val_set_bulb_fade, NULL);
	rc |= bt_gatt_db_add_prop("ffe9", subdevice,
		"fade_rate", PROP_INTEGER, PROP_TO_DEVICE,
		bt_gatt_val_set_bulb_fade_rate, NULL);
	return rc;
}

/*
 * GrillRight sensor characteristic payload:
 *
 * 0:		flags, byte
 *			bit [0:1]: Control Mode, 2-bit enum
 *			bit [2:3]: Status, 2-bit enum
 * 1:		Meat, byte
 * 2:		Doneness, byte
 * 3:		Target Hours, byte
 * 4:		Target Minutes, byte
 * 5:		Target Seconds, byte
 * 6:		Current Hours, byte
 * 7:		Current Minutes, byte
 * 8:		Current Seconds, byte
 * 9:		Unknown
 * 10:11:	Target Temp, uint16, little endian, x10
 * 12:13:	Temp, uint16, little endian, x10, NO_SENSOR = 0x8FFF
 * 14:15:	Percent Done, uint16, little endian, INVALID = 0xFFFF
 */
enum bt_gatt_grillright_cmd_op {
	BT_GRILLRIGHT_CMD_OP_CONFIG		= 0x82,
	BT_GRILLRIGHT_CMD_OP_START		= 0x83,
	BT_GRILLRIGHT_CMD_OP_STOP		= 0x84
};
enum bt_gatt_grillright_control_mode {
	BT_GRILLRIGHT_CTL_OFF			= 0,
	BT_GRILLRIGHT_CTL_MEAT			= 1,
	BT_GRILLRIGHT_CTL_TEMP			= 2,
	BT_GRILLRIGHT_CTL_TIME			= 3,
};
enum bt_gatt_grillright_status {
	BT_GRILLRIGHT_STATUS_OFF		= 0,
	BT_GRILLRIGHT_STATUS_NORMAL		= 1,
	BT_GRILLRIGHT_STATUS_ALMOST_DONE	= 2,
	BT_GRILLRIGHT_STATUS_OVER_DONE		= 3,
};
enum bt_gatt_grillright_meat {
	BT_GRILLRIGHT_MEAT_INVALID		= 0,
	BT_GRILLRIGHT_MEAT_BEEF			= 1,
	BT_GRILLRIGHT_MEAT_VEAL			= 2,
	BT_GRILLRIGHT_MEAT_LAMB			= 3,
	BT_GRILLRIGHT_MEAT_PORK			= 4,
	BT_GRILLRIGHT_MEAT_CHICKEN		= 5,
	BT_GRILLRIGHT_MEAT_TURKEY		= 6,
	BT_GRILLRIGHT_MEAT_FISH			= 7,
	BT_GRILLRIGHT_MEAT_HAMBURGER		= 8
};
enum bt_gatt_grillright_done {
	BT_GRILLRIGHT_DONE_INVALID		= 0,
	BT_GRILLRIGHT_DONE_RARE			= 1,
	BT_GRILLRIGHT_DONE_MEDIUM_RARE		= 2,
	BT_GRILLRIGHT_DONE_MEDIUM		= 3,
	BT_GRILLRIGHT_DONE_MEDIUM_WELL		= 4,
	BT_GRILLRIGHT_DONE_WELL			= 5
};

struct bt_gatt_grillright_sensor_status {
	u8 flags;
	u8 meat;
	u8 doneness;
	u8 target_hrs;
	u8 target_mins;
	u8 target_secs;
	u8 current_hrs;
	u8 current_mins;
	u8 current_secs;
	u8 reserved1;
	u16 target_temp;
	u16 current_temp;
	u16 done_percent;
} PACKED;

static struct bt_gatt_grillright_sensor_status *bt_gatt_grillright_get_packet(
	const struct bt_gatt_val *val)
{
	if (val->len != sizeof(struct bt_gatt_grillright_sensor_status)) {
		log_warn("unexpected value size: %zu != %zu", val->len,
		    sizeof(struct bt_gatt_grillright_sensor_status));
		return NULL;
	}
	return (struct bt_gatt_grillright_sensor_status *)val->data;
}

static int bt_gatt_val_send_grillright_control_mode(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	enum bt_gatt_grillright_control_mode mode;

	if (!packet) {
		return -1;
	}
	mode = packet->flags & 0x3;
	return node_prop_integer_send(node, prop, mode);
}

static int bt_gatt_val_send_grillright_alarm(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	enum bt_gatt_grillright_status status;
	int alarm_val;

	if (!packet) {
		return -1;
	}
	status = (packet->flags >> 2) & 0x3;
	switch (status) {
	case BT_GRILLRIGHT_STATUS_ALMOST_DONE:
		alarm_val = 1;
		break;
	case BT_GRILLRIGHT_STATUS_OVER_DONE:
		alarm_val = 2;
		break;
	default:
		alarm_val = 0;
		break;
	}
	return node_prop_integer_send(node, prop, alarm_val);
}

static int bt_gatt_val_send_grillright_cooking(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	enum bt_gatt_grillright_status status;
	bool cooking;

	if (!packet) {
		return -1;
	}
	status = (packet->flags >> 2) & 0x3;
	cooking = (status != BT_GRILLRIGHT_STATUS_OFF);
	return node_prop_boolean_send(node, prop, cooking);
}

static int bt_gatt_val_send_grillright_time(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	char time[9];	/* HH:MM:SS */

	if (!packet) {
		return -1;
	}
	snprintf(time, sizeof(time), "%02hhu:%02hhu:%02hhu",
	    packet->current_hrs, packet->current_mins, packet->current_secs);
	return node_prop_string_send(node, prop, time);
}

static int bt_gatt_val_send_grillright_doneness(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);

	if (!packet) {
		return -1;
	}
	return node_prop_integer_send(node, prop, packet->doneness);
}

static int bt_gatt_val_send_grillright_meat(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);

	if (!packet) {
		return -1;
	}
	return node_prop_integer_send(node, prop, packet->meat);
}

static int bt_gatt_val_send_grillright_pct_done(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	u16 done_percent;

	if (!packet) {
		return -1;
	}
	done_percent = le16toh(packet->done_percent);
	if (done_percent == 0xFFFF) {
		/* Invalid percent value */
		done_percent = 0;
	}
	return node_prop_integer_send(node, prop, done_percent);
}

static int bt_gatt_val_send_grillright_target_temp(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	u16 temp;

	if (!packet) {
		return -1;
	}
	temp = le16toh(packet->target_temp);
	temp = (temp + 5) / 10;
	return node_prop_integer_send(node, prop, temp);
}

static int bt_gatt_val_send_grillright_temp(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	u16 temp_val;
	double temp;

	if (!packet) {
		return -1;
	}
	temp_val = le16toh(packet->current_temp);
	if (temp_val == 0x8FFF) {
		/* No sensor value */
		temp = 0;
	} else {
		temp = (double)temp_val / 10;
	}
	return node_prop_decimal_send(node, prop, temp);
}

static int bt_gatt_val_send_grillright_target_time(struct node *node,
	struct node_prop *prop, const struct bt_gatt_val *val)
{
	struct bt_gatt_grillright_sensor_status *packet =
	    bt_gatt_grillright_get_packet(val);
	char time[9];	/* HH:MM:SS */

	if (!packet) {
		return -1;
	}
	snprintf(time, sizeof(time), "%02hhu:%02hhu:%02hhu",
	    packet->target_hrs, packet->target_mins, packet->target_secs);
	return node_prop_string_send(node, prop, time);
}

static int bt_gatt_init_grillright(void)
{
	int rc = 0;

	struct bt_gatt_prop_table_entry sensor_props[] = {
		{ "ALARM", PROP_INTEGER, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_alarm },
		{ "CONTROL_MODE", PROP_INTEGER, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_control_mode },
		{ "COOKING", PROP_BOOLEAN, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_cooking },
		{ "TIME", PROP_STRING, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_time },
		{ "DONENESS", PROP_INTEGER, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_doneness },
		{ "MEAT", PROP_INTEGER, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_meat },
		{ "PCT_DONE", PROP_INTEGER, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_pct_done },
		{ "TARGET_TEMP", PROP_INTEGER, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_target_temp },
		{ "TARGET_TIME", PROP_STRING, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_target_time },
		{ "TEMP", PROP_DECIMAL, PROP_FROM_DEVICE,
			NULL, bt_gatt_val_send_grillright_temp }
	};

	rc |= bt_gatt_db_add_template("2899fe00-c277-48a8-91cb-b29ab0f01ac4",
		"grillrt", "1.2");
	/*
	 * TODO add support for command props.  Currently, just needed to work
	 * around bug in device where indications don't work on sensor
	 * characteristics until they are enabled for the control
	 * characteristic.
	 */
	/* Add prop for the control characteristic under subdevice "ctl" */
	rc |= bt_gatt_db_add_prop("28998e03-c277-48a8-91cb-b29ab0f01ac4",
		"ctl", "COMMAND", PROP_STRING, PROP_TO_DEVICE, NULL, NULL);
	/* Add props for sensor1 characteristic under subdevice "00" */
	rc |= bt_gatt_add_prop_table("28998e10-c277-48a8-91cb-b29ab0f01ac4",
		"00", sensor_props, ARRAY_LEN(sensor_props));
	/* Add props for sensor2 characteristic under subdevice "01" */
	rc |= bt_gatt_add_prop_table("28998e11-c277-48a8-91cb-b29ab0f01ac4",
		"01", sensor_props, ARRAY_LEN(sensor_props));
	return rc;
}

/*
 * Load GATT service and characteristic definitions and handlers into the
 * internal database.
 */
int bt_gatt_init(void)
{
	int rc;

	/* Initialize the GATT database */
	rc = bt_gatt_db_init();
	if (rc < 0) {
		return rc;
	}

	/* Device Information Service "info" template */
	rc |= bt_gatt_init_info();

	/* Battery Service "battery" template */
	rc |= bt_gatt_init_battery();

	/* Heart Rate Service "heart" template */
	rc |= bt_gatt_init_heart();

	/* Ayla Networks proprietary device info service */
	rc |= bt_gatt_init_ayla();

	/* Ayla Networks Thermostat simulator demo */
	rc |= bt_gatt_init_ayla_tstat();

	/* RGB smart bulb "bulb_rgb" template (for demo MagicBlue LED bulbs) */
	rc |= bt_gatt_init_bulb_rgb();

	/* BBQ thermometer "grillrt" template (for demo GrillRight devices) */
	rc |= bt_gatt_init_grillright();

	return rc;
}

/*
 * Free GATT service and characteristic definitions.
 */
void bt_gatt_cleanup(void)
{
	bt_gatt_db_cleanup();
}
