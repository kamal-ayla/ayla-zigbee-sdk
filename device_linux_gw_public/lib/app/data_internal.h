/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_DATA_INTERNAL_H__
#define __LIB_APP_DATA_INTERNAL_H__

struct prop_batch_sent_list;

struct data_obj {
	enum prop_type type;
	size_t val_len;
	union {
		u8 bool_val;
		int int_val;
		double rval;
		const char *str_val;
		char *decoded_val;
	} val;
};


/*
 * Initialize the data client.  This provides a messaging interface for
 * sending and receiving properties and related data with devd.
 *
 * This function is a preferred alternative to data_client_init(),
 * and is intended to be called by a library to simplify application
 * initialization.
 */
int data_client_init_internal(struct file_event_table *file_events,
	const char *socket_path);

/*
 * Execute a prop command. Sends, receives, etc.
 */
int data_execute_prop_cmd(const struct prop_cmd *pcmd, int *pcmd_req_id);

/*
 * Send command to devd
 */
int data_send_cmd(const char *proto, const char *op, json_t *args, int req_id,
		int *pcmd_req_id, const struct op_options *opts);

/*
 * Create a json object based on the value type
 */
json_t *data_type_to_json_obj(const void *val, size_t val_len,
	enum prop_type type);


/*
 * Set the handler function for schedules incoming from devd
 */
void data_set_schedule_handler(void (*sched_handler)(const char *,
		const void *, size_t, json_t *));

/*
 * Send json through socket
 */
int data_send_json(json_t *obj);

/*
 * Execute a batch cmd
 */
int data_execute_batch_cmd(const struct prop_batch_sent_list *batch_sent_list,
			    int *pcmd_req_id);

/*
 * Get the value inside a json object based on the expected type
 */
int data_json_to_value(struct data_obj *obj, json_t *val_j, enum prop_type type,
			const void **dataobj_val);

/*
 * Parse 'cmd' object
 */
enum app_parse_rc data_cmd_parse(json_t *cmd, const char *protocol,
				int recv_request_id);

/*
 * set reconnect to devd timer.
 */
void app_set_reconnect(void);

#endif /* __LIB_APP_DATA_INTERNAL_H__ */
