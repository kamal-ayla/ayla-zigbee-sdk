/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_PROPS_H__
#define __LIB_APP_PROPS_H__

#include <sys/queue.h>
#include <ayla/ayla_interface.h>

#define PROP_NAME_LEN		27	/* max name len in bytes */
#define PROP_STRING_LEN		1024	/* max string prop len in bytes */

/* max message prop len in bytes */
#define PROP_MSG_LEN                (512 * 1024)

struct op_args;
struct op_options;

/* Expose temp directory used for file properties */
extern const char *prop_temp_dir;

/*
 * Property data types.  These map to the ATLV type codes used by the
 * communication interface.
 */
enum prop_type {
	PROP_INTEGER	= ATLV_INT,
	PROP_STRING	= ATLV_UTF8,
	PROP_BOOLEAN	= ATLV_BOOL,
	PROP_DECIMAL	= ATLV_FLOAT,
	PROP_BLOB	= ATLV_BIN,
	PROP_MESSAGE    = ATLV_MSG_BIN,
	PROP_FILE	= ATLV_FILE
};

/*
 * Property direction.  From-device properties may not be set by an
 * external source, but all properties may be sent or echoed to the
 * cloud and/or connected LAN-mode devices.
 */
enum prop_direction {
	PROP_TO_DEVICE,
	PROP_FROM_DEVICE
};

/*
 * Confirmation status (used for confirm_cbs)
 */
enum confirm_status {
	CONF_STAT_SUCCESS,	/* all dests were successful*/
	CONF_STAT_FAIL,		/* some dests failed */
	/* some of the items failed (i.e. batch sends) */
	CONF_STAT_PARTIAL_SUCCESS,
};

/*
 * Confirmation errors (used for confirm_cbs)
 */
enum confirm_err {
	CONF_ERR_NONE,		/* no error */
	CONF_ERR_CONN,		/* connection error */
	CONF_ERR_APP,		/* app level err (i.e. template mismatch) */
	CONF_ERR_UNKWN,		/* unknown error */
};

/*
 * Forward declaration of metadata structure.
 */
struct prop_metadata;

/*
 * Information struct for confirmations (used for confirm_cbs)
 */
struct confirm_info {
	enum confirm_status status;
	enum confirm_err err;
	u8 dests;	/* bit mask (BIT 0 = ADS, BIT 1 = LAN 1...) */
};

/*
 * Property table entry.
 */
struct prop {
	const char *name;	/* property name */
	enum prop_type type; /* type to be converted to/from */

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
	int (*set)(struct prop *, const void *val, size_t len,
	    const struct op_args *args);

	/*
	 * Function in app to call when property value is ready to be sent.
	 * The req_id should be set to 0 unless this is a response to a property
	 * request (see "get")
	 */
	int (*send)(struct prop *, int req_id, const struct op_options *opts);

	/*
	 * Set the handler the library can call to request the latest value of
	 * this property. This is used by mobile app when it wants to request
	 * the latest value of a property in LAN mode. The *arg* may contain
	 * additional information about the request (depending on the mobile
	 * app implementation). The function *MUST* send the property with the
	 * given req_id to respond back to the request. If this handler is not
	 * set, the *send* function will be automatically called instead. The
	 * return value of this function is currently unused.
	 */
	int (*get)(struct prop *, int req_id, const void *arg);

	/*
	 * (Optional) If this function is defined, the library will call it
	 * whenever an update to the Ayla cloud service for this property fails
	 * due to cloud connectivity loss. This function will usually be called
	 * in response to methods such as prop_arg_send, prop_val_send, and
	 * prop_batch_send. Regardless if this function is defined, the lib
	 * uses the *ads_failure* flag as a mechanism to flag any properties
	 * for which a cloud send failure occurred (see description below).
	 * The val and len represent the value of the property and *opts* is
	 * additional info given by application when it made the *send* call
	 * (i.e. timestamp of the update). If *val* is set to NULL, it means the
	 * failure is for the current value of the property. The return value of
	 * this function is currently unused.
	 */
	int (*ads_failure_cb)(struct prop *, const void *val, size_t len,
	    const struct op_options *opts);

	/*
	 * If this function is defined, the library will call it whenever
	 * ads_failure is set to 1 (see description below) and connection to
	 * cloud is re-established. This function can be used to re-send the
	 * property to Ayla cloud, drop the datapoint, etc. If this handler is
	 * not set, the *send* function will be automatically called instead.
	 * The return value of this function is currently unused.
	 */
	int (*ads_recovery_cb)(struct prop *);

	/*
	 * When using *send* functions such as prop_arg_send and
	 * prop_batch_append, the application has the ability to specify an
	 * op_options.confirm flag. If this function is defined and the confirm
	 * flag was set to 1, the library will provide this function with
	 * explicit confirmation regarding the result of the *send* call. The
	 * confirm_info->status tells you if the operation succeeded. If
	 * the operation failed, confirm_info->err tells you what the error was.
	 * Usually the application only cares if a property update failed to
	 * reach the cloud service (not mobile apps in LAN mode) and the lib
	 * uses ads_failure_cb to provide this information. So the explicit
	 * confirmation option is generally not needed.
	 */
	int (*confirm_cb)(struct prop *, const void *val, size_t len,
	    const struct op_options *opts,
	    const struct confirm_info *confirm_info);

	void *arg;		/* context for get / set */
	size_t buflen;		/* max len value */
	size_t len;		/* length or max len value desired */
	u8 fmt_flags;		/* formatting flags (use is TBD) */

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
	 * property update that needs acknowledgment based on the result of the
	 * *set* function. This flag is only applicable if your cloud template
	 * contains properties that need explicit acknowledgments.
	 */
	 u8 app_manages_acks:1;
};

enum err_t {
	ERR_OK	= 0,	/* No error */
	ERR_MEM	= -1,	/* Mem issue */
	ERR_VAL	= -2,	/* Illegal value */
	ERR_ARG	= -3,	/* Illegal arg */
	ERR_TYPE = -4,	/* Illegal type */
};

/*
 * Definition of a batch entry. Used internally by Ayla
 */
struct prop_batch_entry {
	STAILQ_ENTRY(prop_batch_entry) link;
	int entry_id;
	struct prop_cmd *pcmd;
	u8 recvd_nak;	/* recvd nak for this entry */
};

STAILQ_HEAD(prop_batch_queue, prop_batch_entry);

/*
 * Definition of a batch list
 */
struct prop_batch_list {
	struct prop_batch_queue batchq;
	int batchq_len;
	u8 sent;	/* already sent. should not be modified after this */
};

/*
 * Initialize the prop block.
 */
void prop_initialize(void);

/*
 * Add a list of properties to the library's property lookup table. This table
 * should match the property list in the device's cloud template.
 */
int prop_add(struct prop *props, unsigned int count);

/*
 * Lookup a property table entry by name.
 *
 * Returns NULL if not found.
 */
struct prop *prop_lookup(const char *name);

/*
 * This function can be used as the "send" function in the 'struct prop'
 * defined above. This function sends prop->arg to cloud and mobile apps (LAN).
 * Applications should set req_id to 0 UNLESS they are responding to property
 * requests in which case *req_id* MUST be set to the value passed to the
 * application by the library. Please see *get* in prop struct for details.
 * The function can return the following values:
 *    ERR_OK: Successfully scheduled to be sent
 *    ERR_MEM: Memory Allocation failure
 *    ERR_VAL: prop->val is NULL or val_len is 0 (except for PROP_STRING)
 *    ERR_ARG: Incorrect parameter
 *    ERR_TYPE: Invalid property type
 *
 * The op_options provide further features on send operations. Most applications
 * don't need to use anything other than the defaults in which case a NULL can
 * be passed in. Here are the features that op_options give:
 * 1) The application can timestamp the update by providing *dev_time_ms*. If no
 * timestamp is given, the current system time will be used.
 * 2) The *dests* flag can be used to specify where the property will be sent.
 * The application can use this to send to cloud only or mobile apps only. By
 * default, all available dests will be sent to.
 * 3) The *confirm* flag can be used to ask the library to give a
 * confirmation that the property update has been delivered. See "confirm_cb"
 * function handler for more details above.
 */
enum err_t prop_arg_send(struct prop *prop, int req_id,
			const struct op_options *opts);

/*
 * Send a value for a property. This is similar to *prop_arg_send* except it
 * allows you to give a value instead of just using what's pointed to by
 * prop->arg. Recommended option is to use "prop_arg_send".
 */
enum err_t prop_val_send(struct prop *prop, int req_id, const void *val,
			size_t val_len, const struct op_options *opts);

/*
 * Send property by looking it up by name. Default op_options will be used.
 */
enum err_t prop_send_by_name(const char *name);

/*
 * Send property by prop pointer. Default op_options will be used.
 */
enum err_t prop_send(struct prop *prop);

/*
 * This function can be used as the "set" function in the 'struct prop'
 * defined above. Basic function for handling incoming property updates. This
 * function sets the object pointed to prop->arg with *val*.
 * *val* is a pointer to the data and len is the size of data.
 */
int prop_arg_set(struct prop *prop, const void *val, size_t len,
		const struct op_args *args);

/*
 * Basic function for sending a file property datapoint from a file at the
 * specified path.
 */
int prop_file_send(struct prop *prop, int req_id, const char *path,
		const struct op_options *opts);

/*
 * Basic function for retrieving file properties from the cloud.
 *
 * XXX Assumes that the app wants the file stored in the path pointed to by
 * prop->arg.
 */
int prop_file_set(struct prop *prop, const void *val, size_t len,
		const struct op_args *args);

/*
 * Same as prop_arg_send but the property is put in a queue to be sent later.
 * If the given list is set to NULL, a new batch list is created and can be
 * used in subsequent calls. Otherwise, the new update is appended to the given
 * list. Properties of type PROP_FILE cannot be batched. A *NULL* will
 * be returned if there was a failure appending to the batch. The batch can be
 * sent by calling *prop_batch_send*. The only opts allowed in this call are
 * *dev_time_ms* and *confirm*. See *prop_arg_send* for details on what the
 * options mean. Note that this function allows the application to have the
 * flexibility of having multiple ongoing batches if it desires. It may be
 * desirable to send the property updates to LAN clients right away and only
 * batch updates for the cloud. In this case, you can call prop_arg_send for LAN
 * clients and prop_batch_send for just the cloud (by setting op_options).
 */
struct prop_batch_list *prop_arg_batch_append(struct prop_batch_list *list,
	struct prop *prop, const struct op_options *opts);

/*
 * Same as *prop_val_send* except in "batch mode". See descriptions of
 * *prop_arg_batch_append* and *prop_val_send*.
 */
struct prop_batch_list *prop_val_batch_append(struct prop_batch_list *list,
			struct prop *prop, void *val, size_t val_len,
			const struct op_options *opts);

/*
 * Send a batch list. The application SHOULD NOT modify the batch list
 * after its sent (i.e. calling append on the list again). To be safe, the lib
 * takes away the application's pointer to the list by setting *list_ptr to
 * NULL. The application has op_options available to set *dests* for the batch,
 * etc. Note that if the application wants explicit confirmation for the op, it
 * must first use *prop_batch_confirm_handler_set*. The *batch_id* arg can be
 * optionally given to store the batch # assigned for this batch. This can be
 * useful if using *prop_batch_confirm_handler_set*. On success, 0 is returned.
 */
int prop_batch_send(struct prop_batch_list **list_ptr,
		const struct op_options *opts, int *batch_id);

/*
 * Free the prop_batch_list. Can be used to abort/free a batch before being
 * sent. This function must not be called after the batch has already been sent.
 * It should also not be called twice. To be safe, the lib takes away the
 * application's pointer to the batch by setting *list_ptr to NULL.
 */
void prop_batch_list_free(struct prop_batch_list **list_ptr);

/*
 * (Optional) Set the confirmation handler for prop batch sends. This handler
 * will be called when a batch is sent with the confirm option set to 1. The
 * *batch_id* will be the same one given in *prop_batch_send*.
 */
void prop_batch_confirm_handler_set(int (*handler)(int batch_id,
	const struct op_options *opts,
	const struct confirm_info *confirm_info));

/*
 * Allocate an empty prop_metadata structure with a capacity of
 * PROP_METADATA_MAX_ENTRIES.
 */
struct prop_metadata *prop_metadata_alloc(void);

/*
 * Free a prop_metadata structure.
 */
void prop_metadata_free(struct prop_metadata *metadata);

/*
 * Add a new key/value pair to a prop_metadata_list structure.
 * Use prop_metadata_free() to free this structure once it has been passed
 * to a send function or is no longer needed.
 */
enum err_t prop_metadata_add(struct prop_metadata *metadata,
	const char *key, const char *val);

/*
 * Add a new key/value pair to a prop_metadata_list structure using a printf-
 * style formatted value.
 * Use prop_metadata_free() to free this structure once it has been passed
 * to a send function or is no longer needed.
 */
enum err_t prop_metadata_addf(struct prop_metadata *metadata,
	const char *key, const char *fmt, ...)
	__attribute__  ((format (printf, 3, 4)));

/*
 * Clear all key/value pairs added to a prop_metadata structure.  This allows
 * it to be reused, as an alternative to allocating a new one.
 */
void prop_metadata_clear(struct prop_metadata *metadata);

/*
 * Request value of a property from the cloud using a pointer to the struct.
 */
enum err_t prop_request(struct prop *prop);

/*
 * Request value of a property from the cloud using property's name.
 */
enum err_t prop_request_by_name(const char *name);

/*
 * Request values of all properties from the service. This function could be
 * useful in making sure the device is in sync with the cloud at bootup.
 */
enum err_t prop_request_all(void);

/*
 * Request values of all to-device properties from the service. This function
 * could be useful in making sure the device is in sync with the cloud at
 * bootup.
 */
enum err_t prop_request_to_dev(void);

/*
 * Send all from-device properties.  Property structs currently do not indicate
 * direction, so this function assumes that from-device properties do not have
 * a set handler defined.  If batch argument is true, all non-file props
 * will be sent in a single batch.
 */
enum err_t prop_send_from_dev(bool batch);

/*
 * Delete a property schedule. Should not be used except for the Zigbee
 * solution.
 */
void prop_schedule_delete(const char *prefix);

/*
 * Helper function for converting value to string format for printing. Given
 * a value of a certain type, this function will return a string representation
 * of the value allocated in a static buffer.  The returned value should be
 * used or copied to a different location before calling the function again.
 */
const char *prop_val_to_str(const void *val, enum prop_type type);

#endif /*  __LIB_APP_PROPS_H__ */
