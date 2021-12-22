/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_APP_IF_H__
#define __AYLA_APP_IF_H__

/*
 * Property requests to appd.
 */
struct appd_req {
	struct timer wait_timer; /* timer when request to appd expires */
	struct appd_req *next;	/* list linkage */
	struct server_req *req;	/* associated server request, if any */
	int req_id;		/* appd request ID */
};

extern int app_req_id;

void app_send_confirm_true(struct ops_devd_cmd *cmd);
void app_send_confirm_false(struct ops_devd_cmd *cmd, int dests,
			    const char *err);
/*
 * Send a DATA nak
 */
void app_send_nak(const char *err, int id);

/*
 * Send a DATA ACK
 */
void app_send_ack(int id);


/*
 * Send JSON object to appd
 */
int app_send_json(json_t *obj);

/*
 * Send a nak to devd. Give name, dests, err if that information needs to be
 * sent
 */
void app_send_nak_with_args(struct ops_devd_cmd *op_cmd);

/*
 * Send an echo failure to appd with the arguments given
 */
void app_send_echo_failure_with_args(struct ops_devd_cmd *op_cmd);

/*
 * Send NAK to appd for an operation
 */
void app_send_nak_for_inval_args(struct ops_devd_cmd *op_cmd);

/*
 * Initialize the app subsystem. This subsystem sends messages to appd
 */
void app_init(void);

/*
 * Send schedules to appd
 */
void app_send_sched(json_t *scheds, int dests);

/*
 * Register a handler for 'gateway' packets
 */
void app_register_gateway_handler(enum app_parse_rc
				(*gw_parser)(json_t *, int, json_t *));

/*
 * Add request to list.
 */
void app_req_add(struct appd_req *ar);

/*
 * Allocate new request.
 */
struct appd_req *app_req_alloc(void);

/*
 * Find and remove request from list.
 */
struct appd_req *app_req_delete(u16 req_id);

/*
 * Free all the pending requests
 */
void app_req_delete_all(void);


#endif /*  __AYLA_APP_IF_H__ */
