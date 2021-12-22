/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __BT_INTERFACE_H__
#define __BT_INTERFACE_H__

#define BT_RSSI_INVALID		MIN_S16

enum bt_dev_op_err_code {
	BT_DEV_OP_UNKNOWN_ERROR = -4,    /* operation faced unknown error */
	BT_DEV_OP_IN_PROGRESS = -3,      /* operation is in progress */
	BT_DEV_OP_NO_NODE = -2,          /* cannot find operation node */
	BT_DEV_OP_NO_DEVICE = -1,        /* cannot find operation device */
	BT_DEV_OP_CONNECT_SUCCESS = 0,   /* operation connect success */
	BT_DEV_OP_ADD_DONE = 1,          /* operation add node done */
	BT_DEV_OP_UPDATE_DONE = 2,       /* operation update node done */
	BT_DEV_OP_ALREADY_DONE = 3,      /* operation is already done */
};

/*
 * Structure to pass scan result information to the scan_update callback.
 */
struct bt_scan_result {
	const char *addr;
	const char *name;
	const char *type;
	s16 rssi;
};

/*
 * Callbacks used to handle events from the Bluetooth interface.
 */
struct bt_callbacks {
	void (*scan_update)(const struct bt_scan_result *);
	int (*passkey_request)(const char *, u32 *);
	int (*passkey_display)(const char *, u32);
	int (*passkey_display_clear)(const char *);
};

struct file_event_table;
struct timer;

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_query_info_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_configure_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_prop_set_handler(struct node *node, struct node_prop *prop,
    void (*callback)(struct node *, struct node_prop *,
    enum node_network_result));

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
int bt_leave_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result));

/*
 * Initialize the Bluetooth interface.
 */
int bt_init(struct file_event_table *file_events, struct timer_head *timers,
	const struct bt_callbacks *callbacks);

/*
 * Free resources associated with the Bluetooth interface.
 */
int bt_cleanup(void);

/*
 * Start the Bluetooth interface.
 */
int bt_start(struct bt_callbacks *callbacks);

/*
 * Stop the Bluetooth interface.
 */
int bt_stop(void);

/*
 * Enables or disables scanning for discoverable Bluetooth devices.
 */
int bt_discover(bool enable);

/*
 * Returns true if discovery is currently enabled.
 */
bool bt_discovery_running(void);

/*
 * Begins the pairing process with the specified Bluetooth device.
 */
enum bt_dev_op_err_code bt_node_connect(const char *addr,
	void (*callback)(const char *addr, enum node_network_result,
	    enum bt_dev_op_err_code, void *),
	void *arg);

/*
 * Cancels an ongoing pairing attempt.
 */
void bt_node_connect_cancel(const char *addr);

/*
 * Begins the disconnect process with the specified Bluetooth device.
 */
enum bt_dev_op_err_code bt_node_disconnect(const char *addr,
	void (*callback)(const char *, enum node_network_result, void *),
	void *arg);

/*
 * Check bulb node if got characteristic properties.
 */
bool bt_node_check_bulb_prop(struct node *nd);

#endif /* __BT_INTERFACE_H__ */
