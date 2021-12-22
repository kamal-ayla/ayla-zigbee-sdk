/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_OPS_H__
#define __LIB_APP_OPS_H__

/*
 * Events from the client.
 */
enum ops_client_event {
	OPS_EVENT_CLOUD_DOWN,		/* The connection to ADS is down */
	OPS_EVENT_CLOUD_UP,		/* The connection to ADS is up */
	OPS_EVENT_LAN_DOWN,		/* No LAN clients are available */
	OPS_EVENT_LAN_UP,		/* The >= 1 LAN clients are available */
	OPS_EVENT_UNREGISTERED,		/* Device was registered to a user */
	OPS_EVENT_REGISTERED		/* Device was unregistered */
};

/*
 * Options available for operations. Please see the developer's guide
 * to see what options are available for each operation.
 */
struct op_options {
	/*
	 * Timestamp in UTC milliseconds. Used for property sends. If no
	 * timestamp is given, the current system time is used
	 */
	unsigned long long dev_time_ms;

	/*
	 * Pointer to metadata to send with the operation.  This is supported
	 * for property datapoint sends.  If the operation does not support
	 * metadata, it will be ignored.
	 */
	struct prop_metadata *metadata;

	/*
	 * dest mask. If set to 0 (default), the operation is executed on all
	 * available destinations (cloud + mobile app in LAN). Otherwise, this
	 * can be used as a bit mask. Bit 0 represents the cloud, bit 1 is
	 * LAN client #1, bit 2 is LAN client #2, and so on. Thus, to execute
	 * the operation only on the cloud, the mask should be set to 1. To
	 * execute the operation only on mobile apps, the mask should be set to
	 * DEST_LAN_APPS.
	 */
	u8 dests;

	u8 confirm:1;	/* set to 1 if a confirmation is needed */
	u8 echo:1;	/* set to 1 if property update is an echo */

	/*
	 * Pointer to a user-defined, opaque argument that may be used by
	 * the confirmation handler.
	 */
	void *arg;
};

/*
 * Op Args. Operations that can be passed into prop_set
 */
struct op_args {
	/*
	 * If *ack_arg* is not null, it means this property needs an explicit
	 * acknowledgment (set by the template in the cloud)
	 * If the app sets the "app_manages_acks" to 0, then the Ayla library
	 * will automatically take care of acks by using the return value of the
	 * function. It'll consider a 0 return code as a success and failure
	 * otherwise.
	 * If the app sets the "app_manages_acks" to 1, then the app
	 * must take care of acks by calling "ops_prop_ack_send" with the given
	 * ack_arg. The application can ignore the contents of ack_arg.
	 * The 'ops_prop_ack_send' function will take care of freeing any
	 * allocated memory in *ack_arg*.
	 */
	void *ack_arg;

	/*
	 * The "propmeta" metadata for this property.
	 */
	const char *propmeta;

	/*
	 * If this is a property update, the following will tell you the source
	 * of the update. i.e. 1 = DEST_ADS, 2 = LAN App 1, 3 = LAN App 2....
	 */
	u8 source;
};

/*
 * Polling routine, call periodically to execute pending operations
 * (i.e. property sends)
 *
 * THIS FUNCTION IS DEPRECATED.  Use the app library, which is newer and calls
 * this internally.
 */
int ops_poll(void);

/*
 * Legacy ops initialization function.
 * Set multi_threaded flag to 1 if operations are going to be called
 * from multiple threads. If multi_threaded is set, pass in a pointer
 * to an integer which will get assigned a recv_socket that needs to
 * be polled using file_event_reg. Use ops_cmdq_notification as the
 * receive function for this socket.
 *
 * THIS FUNCTION IS DEPRECATED.  Use the app library, which is newer and
 * initializes ops internally.
 */
void ops_init(int multi_threaded, int *recv_sockets);

/*
 * Recv handler for multi-threaded receive socket (see ops_init).
 * Processes any queued commands.
 */
void ops_cmdq_notification(void *arg, int sock);

/*
 * App should call this when its ready to receive updates from cloud.
 * Only needs to be called once. Cannot be undone.
 */
void ops_app_ready_for_cloud_updates(void);

/*
 * Register a handler for being notified when connectivity to Ayla cloud
 * service resumes. When connectivity resumes, ops will handle
 * any properties that failed to send (invoking their ads_recovery_cb's),
 * then will call the ops_cloud_recovery_handler.
 */
void ops_set_cloud_recovery_handler(void (*handler)(void));

/*
 * Register a Ayla cloud service connectivity up/down handler.
 */
void ops_set_cloud_connectivity_handler(void (*handler)(bool));

/*
 * Register a handler for client events.  This callback is invoked for all
 * events from the client, and uninteresting events may be safely ignored.
 */
void ops_set_client_event_handler(void (*handler)(enum ops_client_event event));

/*
 * Send acknowledgment for property update. Should be called as a result
 * of a call to the set handler with an ack_arg operation arg. *status* needs to
 * be set to 0 for a success and any other value for failure.
 * *ack_message* can be any integer that the oem desires to store in the cloud
 * to represent the result of the command.
 */
void ops_prop_ack_send(void *ack_arg, int status, int ack_message);

/*
 * Request the current registration token.  Resp_handler will be called
 * when the registration token is fetched.
 */
int ops_request_regtoken(void (*resp_handler)(const char *));

/*
 * Return true when successfully connected to to ADS.
 */
bool ops_cloud_up(void);

/*
 * Return true when connected to one or more LAN clients.
 */
bool ops_lan_up(void);

/*
 * Return the current system time in milliseconds
 */
unsigned long long ops_get_system_time_ms(void);

#endif /*  __LIB_APP_OPS_H__ */
