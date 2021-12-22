/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __BT_GATT_H__
#define __BT_GATT_H__

#define BT_GATT_BULB_TEMPLATE	"bulb_rgb"

/*
 * Load GATT service and characteristic definitions and handlers into the
 * internal database.
 */
int bt_gatt_init(void);

/*
 * Free GATT service and characteristic definitions.
 */
void bt_gatt_cleanup(void);

#endif /* __BT_GATT_H__ */
