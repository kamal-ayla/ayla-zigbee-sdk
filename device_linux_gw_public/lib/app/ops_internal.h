 /*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_OPS_INTERNAL_H__
#define __LIB_APP_OPS_INTERNAL_H__

struct file_event_table;
struct timer_head;

/*
 * Initialize the ops library.  This provides a command queue for
 * properties and other requests, and basic event handling for cloud events.
 *
 * This function is a preferred alternative to ops_init(), and is intended
 * to be called by a library to simplify application initialization.
 */
int ops_init_internal(struct file_event_table *file_events,
	struct timer_head *timers);
/*
 * Register a new operation to be executed at the next ops_poll
 */
int ops_add(int (*op_handler)(void *arg, int *req_id, int confirm_needed),
	    void *arg, int (*nak_cb)(void *arg, int req_id,
	    enum confirm_err err, json_t *obj_j),
	    int (*confirm_cb)(void *, enum confirm_status, enum confirm_err,
	    int), void (*free_arg)(void *arg));

/*
 * Create a socket event to wake up the main loop.
 */
void ops_notify(void);

/*
 * Process true confirmation for a request id
 */
void ops_true_confirmation_process(int req_id);

/*
 * Process false confirmations for a request id
 */
void ops_false_confirmation_process(json_t *cmd, int req_id);

/*
 * Create object for a property_ack
 */
json_t *ops_prop_ack_json_create(const char *proto, int req_id, int source,
				json_t *prop);

/*
 * Process naks from devd
 */
int ops_nak_process(json_t *cmd, int req_id);

/*
 * Process echo failures from devd
 */
int ops_echo_failure_process(json_t *cmd,
	int (*echo_fail_handler)(const char *echo_name,
	const json_t *arg));

#endif /*  __LIB_APP_OPS_INTERNAL_H__ */
