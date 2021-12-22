/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __GATEWAY_INTERNAL_H__
#define __GATEWAY_INTERNAL_H__

/*
 * Definition of a batch list sent to devd
 */
struct gw_node_prop_batch_sent_list {
	STAILQ_ENTRY(gw_node_prop_batch_sent_list) link;
	struct gw_node_prop_batch_list *batch_list;
	int batch_id;
	int sent_req_id;
	struct op_options opts;
};

/*
 * [Will be supported TBD] The functions and structs below have not been
 * fully implemented.
 */

/*
 * Node property table entry.
 */
struct gw_node_prop_entry {
	struct gw_node_prop prop; /* specify NULL for wildcards */

	/*
	 * This function is called when an  update for this prop is recvd. The
	 * 'args' parameter will be NULL if no additional args exist. Please see
	 * the definition of op_args to see what different args can be passed
	 * in. Note that the 'args' structure is on the stack. If the app sets
	 * the *app_manages_acks* to 1, then the app must take care of
	 * acks by calling "ops_prop_ack_send" with the args->ack_arg. The
	 * application can ignore the contents of ack_arg. A zero return value
	 * of this function indicates a successful update of the property. Any
	 * other value indicates a failure.
	 */
	int (*set)(struct gw_node_prop *, enum prop_type type, const void *val,
	    size_t len, const struct op_args *args);

	/*
	 * Function in app to call when property value is ready to be sent.
	 * The req_id should be set to 0 unless this is a response to a property
	 * request (see "response_handler")
	 */
	int (*send)(struct gw_node_prop *, enum prop_type type, const void *val,
	    size_t len, int req_id, const struct op_options *opts);

	/*
	 * (Optional) Function handler for gateway library to use so it can
	 * request an app to send the latest value of a gw_node_prop. The
	 * response should be sent using the *send* function with the same
	 * *req_id*. The *arg* parameter may (optionally) contain additional
	 * information about the request. Should return -1 if no datapoint
	 * exists for the given gw_node_prop.
	 */
	int (*get)(struct gw_node_prop *, int req_id, const char *arg);

	/*
	 * (Optional) If this function is defined, the library will call it
	 * whenever an update to the Ayla cloud service for this property fails
	 * due to cloud connectivity loss. This function will usually be called
	 * in response to gw_node_prop_send. Regardless if this function is
	 * defined, the lib uses the *ads_failure* flag as a mechanism to flag
	 * any properties for which a cloud send failure occured.
	 * The args->val and  represent the value of the property and *opts* is
	 * additional info given by application when it made the *send* call
	 * (i.e. timestamp of the update). If *val* is set to NULL, it means the
	 * failure is for the current value of the property. The return value of
	 * this function is currently unused.
	 */
	int (*ads_failure_cb)(enum ayla_gateway_op op,
	    enum gw_confirm_arg_type type, const void *arg,
	    const struct op_options *opts);

	/*
	 * When using *send* functions such as gw_node_prop_send and
	 * gw_node_prop_batch_append, the application has the ability to specify
	 * an op_options.confirm flag. If this function is defined and the
	 * confirm flag was set to 1, the library will provide this function
	 * with explicit confirmation regarding the result of the *send* call.
	 * The confirm_info->status tells you if the operation succeeded. If
	 * the operation failed, confirm_info->err tells you what the error was.
	 * Usually the application only cares if a property update failed to
	 * reach the cloud service (not mobile apps in LAN mode) and the lib
	 * uses ads_failure_cb to provide this information. So the explicit
	 * confirmation option is generally not needed.
	 */
	int (*confirm_cb)(enum ayla_gateway_op op,
	    enum gw_confirm_arg_type type, const void *arg,
	    const struct op_options *opts,
	    const struct confirm_info *confirm_info);

	/*
	 * (Optional) shortcut to get this entry through the
	 * gw_node_prop_lookup_by_nickname function
	 */
	const char *nickname;

	enum prop_type type; /* (Optional) property type */

	/* 1 if app doesn't want to receive NULL vals in its *set* handler */
	u8 reject_null:1;

	/*
	 * 1 if post/echo to Ayla cloud failed. Usually it is important to the
	 * application that the cloud is in-sync with the state of the device
	 * at all times. If cloud connectivity is ever lost (due to internet
	 * loss for example), updates that happen on the device in LAN mode
	 * as well as any *send* calls made by the application will not reach
	 * the Ayla cloud. To solve this issue, the library marks any properties
	 * where this failure occurs using this flag. Then, when cloud
	 * connectivity is resumed, the library will call *ads_recovery_cb* (if
	 * defined) or the *send* function. This will make sure that the
	 * cloud service is back in-sync with the state of the device.
	 */
	u8 ads_failure:1;

	/*
	 * 1 if app wants it's *set* handler to be called with the *val* as
	 * a JSON object (instead of being the prop->type object).
	 */
	u8 pass_jsonobj:1;

	/*
	 * 1 if the app doesn't want the library to automatically ack any
	 * property update that needs acknowledgement based on the result of the
	 * *set* function. This flag is only applicable if your cloud template
	 * contains properties that need explicit acknowledgements.
	 */
	 u8 app_manages_acks:1;
};

/*
 * Add an array of node_prop entries to the lookup table.
 */
int gw_node_prop_add(struct gw_node_prop_entry *prop, unsigned int count);

/*
 * Return a pointer to the first matching gw_node_prop_entry in the table.
 * Returns NULL if there are no matches.
 */
struct gw_node_prop_entry *gw_node_prop_lookup(const struct gw_node_prop *prop);

/*
 * Return a pointer to the first matching gw_node_prop_entry in the table
 * with the given nickname.
 * Returns NULL if there are no matches.
 */
struct gw_node_prop_entry *gw_node_prop_lookup_by_nickname(const char *n);

/*
 * Handle a gateway operation
 */
enum app_parse_rc gw_cmd_parse(json_t *cmd, int recv_id);

#endif /* __GATEWAY_INTERNAL_H__ */
