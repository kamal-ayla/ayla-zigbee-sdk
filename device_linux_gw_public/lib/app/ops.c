/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 *
 * This code is offered as an example without any guarantee or warranty.
 */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include <ayla/utypes.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/json_interface.h>

#include <app/msg_client.h>
#include <app/ops.h>
#include <app/props.h>
#include <app/data.h>

#include "props_internal.h"
#include "data_internal.h"
#include "msg_client_internal.h"
#include "ops_internal.h"


/*
 * Operations to be executed
 */
struct ops_cmd {
	STAILQ_ENTRY(ops_cmd) link;
	int (*op_handler)(void *arg, int *req_id, int confirm_needed);
	void *arg;
	/* the nak cb is called for a NAK received for ADS conn err */
	int (*nak_cb)(void *arg, int req_id, enum confirm_err err,
	    json_t *obj_j);
	/*
	 * If this function is defined and confirm flag is set to 1,
	 * this function will be called back. success = 1 means success,
	 * success = 0 means failure.
	 */
	int (*confirm_cb)(void *arg, enum confirm_status status,
	    enum confirm_err err, int dests);
	void (*free_arg_cb)(void *arg);
	int req_id;
};

static pthread_mutex_t ops_cmdq_lock = PTHREAD_MUTEX_INITIALIZER;
enum { OPS_SOCKET_SEND = 0, OPS_SOCKET_RECV };
static int ops_cmdq_sockets[2];
static STAILQ_HEAD(, ops_cmd) ops_cmdq;
static STAILQ_HEAD(, ops_cmd) ops_need_confirmq;
static bool ops_initialized;
static void (*ops_cloud_recovery_handler)(void);
static void (*ops_cloud_connectivity_handler)(bool connected);
static void (*ops_client_event_handler)(enum ops_client_event event);
static bool ops_app_ready;
static bool ops_cloud_up_once;


static void ops_free_op_cmd(struct ops_cmd *op_cmd)
{
	if (op_cmd && op_cmd->free_arg_cb) {
		op_cmd->free_arg_cb(op_cmd->arg);
	}
	free(op_cmd);
}

/*
 * Create a socket pair for multi-threaded applications to use to wake up the
 * main thread.
 */
static int ops_cmdq_socket(void)
{
	int socket_opts;

	if (ops_cmdq_sockets[OPS_SOCKET_SEND] > 0) {
		goto done;
	}
	if (socketpair(AF_LOCAL, SOCK_SEQPACKET, 0, ops_cmdq_sockets) < 0) {
		log_err("socketpair fail %m");
		return -1;
	}
	socket_opts = fcntl(ops_cmdq_sockets[OPS_SOCKET_RECV], F_GETFL, 0);
	fcntl(ops_cmdq_sockets[OPS_SOCKET_RECV], F_SETFL,
	    socket_opts | O_NONBLOCK);
done:
	return ops_cmdq_sockets[OPS_SOCKET_SEND];
}

static void ops_cloud_event_handler(bool up)
{
	/* Perform any actions needed for individual properties */
	prop_cloud_status_changed(up);
	/* Ops cloud recovery callback */
	if (up && ops_cloud_up_once) {
		if (ops_cloud_recovery_handler) {
			ops_cloud_recovery_handler();
		}
	}
	/* Ops cloud connectivity callback */
	if (ops_cloud_connectivity_handler) {
		ops_cloud_connectivity_handler(up);
	}
	/* Ops generic event handler */
	if (ops_client_event_handler) {
		if (up) {
			ops_client_event_handler(OPS_EVENT_CLOUD_UP);
		} else {
			ops_client_event_handler(OPS_EVENT_CLOUD_DOWN);
		}
	}
	/*
	 * Send/resend client listen command when cloud is connected.
	 * It is cleared in the client each time the cloud connection goes
	 * down, so it is reasserted here after all application-side recovery
	 * operations have completed.
	 */
	if (up && ops_app_ready) {
		msg_client_listen_enable();
	}
	if (up) {
		ops_cloud_up_once = true;
	}
}

static void ops_lan_event_handler(bool up)
{
	/* Ops generic event handler */
	if (ops_client_event_handler) {
		if (up) {
			ops_client_event_handler(OPS_EVENT_LAN_UP);
		} else {
			ops_client_event_handler(OPS_EVENT_LAN_DOWN);
		}
	}
}

static void ops_reg_event_handler(bool registered)
{
	if (ops_client_event_handler) {
		if (registered) {
			ops_client_event_handler(OPS_EVENT_REGISTERED);
		} else {
			ops_client_event_handler(OPS_EVENT_UNREGISTERED);
		}
	}
}

/*
 * Polling routine, call periodically to execute pending operations
 * (i.e. property sends)
 *
 * THIS FUNCTION IS DEPRECATED.  It is preferred to use the app library,
 * which is newer and calls this internally.
 */
int ops_poll(void)
{
	struct ops_cmd *op_cmd;
	int rc;

	if (!ops_initialized) {
		return -1;
	}
	while (1) {
		pthread_mutex_lock(&ops_cmdq_lock);
		if (STAILQ_EMPTY(&ops_cmdq)) {
			pthread_mutex_unlock(&ops_cmdq_lock);
			break;
		}
		op_cmd = STAILQ_FIRST(&ops_cmdq);
		STAILQ_REMOVE_HEAD(&ops_cmdq, link);
		pthread_mutex_unlock(&ops_cmdq_lock);
		rc = op_cmd->op_handler(op_cmd->arg, &op_cmd->req_id,
		    op_cmd->confirm_cb != NULL);
		if (op_cmd->confirm_cb)	{
			if (!rc) {
				/*
				 * Add op_cmd to the queue of op_cmds waiting
				 * for confirmation
				 */
				STAILQ_INSERT_TAIL(&ops_need_confirmq, op_cmd,
				    link);
				continue;
			}
			/* Failure */
			op_cmd->confirm_cb(op_cmd->arg, CONF_STAT_FAIL,
			    CONF_ERR_UNKWN, DEST_ADS);
		}
		ops_free_op_cmd(op_cmd);
	}
	return 0;
}

/*
 * Register a new operation to be executed at the next ops_poll
 */
int ops_add(int (*op_handler)(void *arg, int *req_id, int confirm_needed),
	    void *arg, int (*nak_cb)(void *arg, int req_id,
	    enum confirm_err err, json_t *obj_j),
	    int (*confirm_cb)(void *, enum confirm_status, enum confirm_err,
	    int), void (*free_arg)(void *arg))
{
	struct ops_cmd *op_cmd;
	bool notify;

	if (!ops_initialized) {
		log_err("ops_init must be called");
		return -1;
	}
	if (!op_handler) {
		log_err("handler cannot be NULL");
		return -1;
	}
	if (nak_cb && !confirm_cb) {
		log_err("cannot handle naks if confirm not needed");
		return -1;
	}
	op_cmd = calloc(1, sizeof(*op_cmd));
	REQUIRE(op_cmd, REQUIRE_MSG_ALLOCATION);
	op_cmd->op_handler = op_handler;
	op_cmd->arg = arg;
	op_cmd->nak_cb = nak_cb;
	op_cmd->confirm_cb = confirm_cb;
	op_cmd->free_arg_cb = free_arg;

	pthread_mutex_lock(&ops_cmdq_lock);
	notify = STAILQ_EMPTY(&ops_cmdq);
	STAILQ_INSERT_TAIL(&ops_cmdq, op_cmd, link);
	pthread_mutex_unlock(&ops_cmdq_lock);

	if (notify) {
		ops_notify();
	}
	return 0;
}

/*
 * Legacy ops initialization function.
 * Set multi_threaded flag to 1 if operations are going to be called
 * from multiple threads. If multi_threaded is set, pass in a pointer
 * to an integer which will get assigned a recv_socket that needs to
 * be polled using file_event_reg. Use ops_cmdq_notification as the
 * receive function for this socket.
 *
 * THIS FUNCTION IS DEPRECATED.  It is preferred to use the app library,
 * which is newer and initializes ops internally.
 */
void ops_init(int multi_threaded, int *recv_socket)
{
	/*
	 * Assert here to immediately identify cases where ops_init_internal
	 * was already called by the apps library, but the legacy init function
	 * is still being called by the application.
	 */
	ASSERT(!ops_initialized);

	STAILQ_INIT(&ops_cmdq);
	STAILQ_INIT(&ops_need_confirmq);

	/* Register for various client events */
	msg_client_set_cloud_event_callback(ops_cloud_event_handler);
	msg_client_set_lan_event_callback(ops_lan_event_handler);
	msg_client_set_registration_callback(ops_reg_event_handler);

	/* Setup a socket for waking up the main-loop */
	if (multi_threaded) {
		ASSERT(recv_socket != NULL);
		*recv_socket = ops_cmdq_socket();
	} else if (recv_socket) {
		*recv_socket = -1;
	}
	ops_initialized = true;
}

/*
 * Initialize the ops library.  This provides a command queue for
 * properties and other requests, and basic event handling for cloud events.
 *
 * This function is a preferred alternative to ops_init(), and is intended
 * to be called by a library to simplify application initialization.
 */
int ops_init_internal(struct file_event_table *file_events,
	struct timer_head *timers)
{
	ASSERT(file_events != NULL);
	/*
	 * Currently, timers are not required by this library, but that may
	 * change in the future.
	 * ASSERT(timers != NULL);
	 */

	if (ops_initialized) {
		return 0;
	}
	STAILQ_INIT(&ops_cmdq);
	STAILQ_INIT(&ops_need_confirmq);

	/* Register for various client events */
	msg_client_set_cloud_event_callback(ops_cloud_event_handler);
	msg_client_set_lan_event_callback(ops_lan_event_handler);
	msg_client_set_registration_callback(ops_reg_event_handler);

	/* Setup a socket for waking up the main-loop */
	if (ops_cmdq_socket() < 0) {
		return -1;
	}
	if (file_event_reg(file_events, ops_cmdq_sockets[OPS_SOCKET_RECV],
	    ops_cmdq_notification, NULL, NULL) < 0) {
		log_err("failed to register file event listener");
		return -1;
	}
	ops_initialized = true;
	return 0;
}

/*
 * Recv handler for multi-threaded receive socket (see ops_init).
 * Processes any queued commands.
 */
void ops_cmdq_notification(void *arg, int sock)
{
	char tmp[1];

	/* Drop all packets */
	while (recv(sock, tmp, sizeof(tmp), 0) > 0) {
		;
	}
	if (sock == ops_cmdq_sockets[OPS_SOCKET_RECV]) {
		ops_poll();
	}
}

/*
 * Create a socket event to wake up the main loop.
 */
void ops_notify(void)
{
	u8 byte = 1;

	if (ops_cmdq_sockets[OPS_SOCKET_SEND] > 0) {
		send(ops_cmdq_sockets[OPS_SOCKET_SEND], &byte, sizeof(byte), 0);
	}
}

/*
 * App should call this when its ready to receive updates from cloud.
 * Only needs to be called once. Cannot be undone.
 */
void ops_app_ready_for_cloud_updates(void)
{
	ops_app_ready = true;
	if (ops_cloud_up()) {
		msg_client_listen_enable();
	}
}

/*
 * Request the current registration token.  Resp_handler will be called
 * when the registration token is fetched.
 */
int ops_request_regtoken(void (*resp_handler)(const char *))
{
	ASSERT(resp_handler != NULL);

	/* We now cache a copy of the regtoken locally in msg_client */
	resp_handler(msg_client_regtoken());
	return 0;
}

/*
 * Return true when successfully connected to to ADS.
 */
bool ops_cloud_up(void)
{
	return msg_client_cloud_up();
}

/*
 * Return true when connected to one or more LAN clients.
 */
bool ops_lan_up(void)
{
	return msg_client_lan_up();
}

/*
 * Convert a given error to an enum confirm_err
 */
static enum confirm_err ops_err_str_to_confirm_err(const char *err)
{
	if (!err) {
		return CONF_ERR_NONE;
	}
	if (!strcmp(err, JINT_ERR_CONN_ERR)) {
		return CONF_ERR_CONN;
	}
	if (!strcmp(err, JINT_ERR_UNKWN_PROP)) {
		return CONF_ERR_APP;
	}
	return CONF_ERR_UNKWN;
}

/*
 * Helper function for processing confirmations from devd
 */
static int ops_confirm_process(int req_id, int dests, const char *err)
{
	struct ops_cmd *op_cmd;

	if (!ops_initialized) {
		return -1;
	}
	STAILQ_FOREACH(op_cmd, &ops_need_confirmq, link) {
		if (op_cmd->req_id == req_id) {
			break;
		}
	}
	if (!op_cmd) {
		log_debug("confirmation for unknown req_id %d", req_id);
		return -1;
	}
	STAILQ_REMOVE(&ops_need_confirmq, op_cmd, ops_cmd, link);

	if (op_cmd->confirm_cb) {
		op_cmd->confirm_cb(op_cmd->arg,
		    err ? CONF_STAT_FAIL : CONF_STAT_SUCCESS,
		    ops_err_str_to_confirm_err(err), dests);
	}
	ops_free_op_cmd(op_cmd);

	return 0;
}

static int ops_nak_echo_failure_process(json_t *arg, const char **opstr,
			int *dests, const char **err)
{
	if (opstr) {
		*opstr = json_get_string(arg, "op");
	}
	if (err) {
		*err = json_get_string(arg, "err");
		if (*err == NULL) {
			return -1;
		}
	}
	if (json_get_int(arg, "dests", dests) < 0) {
		return -1;
	}
	return 0;
}

/*
 * Process naks from devd
 */
int ops_nak_process(json_t *cmd, int req_id)
{
	struct ops_cmd *op_cmd;
	const char *err;
	const char *opstr;
	int dests;
	json_t *args;
	json_t *arg;

	args = json_object_get(cmd, "args");
	if (!args) {
		return -1;
	}
	arg = json_array_get(args, 0);
	if (!arg) {
		return -1;
	}
	if (ops_nak_echo_failure_process(arg, &opstr, &dests, &err)) {
		return -1;
	}
	log_debug("got nak for op %s, req_id = %d", opstr, req_id);
	if (!(dests & DEST_ADS)) {
		/* XXX
		 * ignore NAKs that are not for ADS
		 */
		log_debug("ignore nak");
		return 0;
	}
	STAILQ_FOREACH(op_cmd, &ops_need_confirmq, link) {
		if (op_cmd->req_id == req_id) {
			break;
		}
	}
	if (!op_cmd) {
		log_debug("nak for unknown req_id %d", req_id);
		return 0;
	}
	if (op_cmd->nak_cb) {
		op_cmd->nak_cb(op_cmd->arg, req_id,
		    ops_err_str_to_confirm_err(err), arg);
	}

	return 0;
}

/*
 * Process echo failures from devd
 */
int ops_echo_failure_process(json_t *cmd,
	int (*echo_fail_handler)(const char *echo_name,
	const json_t *arg))
{
	const char *err;
	const char *echo_name;
	json_t *args;
	json_t *arg;
	int dests;

	args = json_object_get(cmd, "args");
	if (!args) {
		return -1;
	}
	arg = json_array_get(args, 0);
	if (!arg) {
		return -1;
	}
	echo_name = json_get_string(arg, "name");
	if (!echo_name) {
		return -1;
	}
	log_debug("echo failure for %s", echo_name);
	if (ops_nak_echo_failure_process(arg, NULL, &dests, &err)) {
		return -1;
	}
	if (strcmp(err, JINT_ERR_CONN_ERR) || !(dests & DEST_ADS)) {
		/* XXX
		 * ignore echo failures that are not for ADS or are not
		 * for connection loss
		 */
		return 0;
	}

	if (echo_fail_handler(echo_name, arg)) {
		return -1;
	}
	return 0;
}

/*
 * Process true confirmation for a request id
 */
void ops_true_confirmation_process(int req_id)
{
	ops_confirm_process(req_id, 0, NULL);
}

/*
 * Process false confirmations for a request id
 */
void ops_false_confirmation_process(json_t *cmd, int req_id)
{
	json_t *args;
	json_t *arg;
	const char *err;
	int dests;

	args = json_object_get(cmd, "args");
	if (!json_is_array(args)) {
inval_args:
		log_warn("inval arg");
		return;
	}
	arg = json_array_get(args, 0);
	err = json_get_string(arg, "err");
	if (!err) {
		goto inval_args;
	}
	json_get_int(arg, "dests", &dests);

	ops_confirm_process(req_id, dests, err);
}

/*
 * Register a handler for being notified when connectivity to Ayla cloud
 * service resumes. When connectivity resumes, ops will handle
 * any properties that failed to send (invoking their ads_recovery_cb's),
 * then will call the ops_cloud_recovery_handler.
 */
void ops_set_cloud_recovery_handler(void (*handler)(void))
{
	ops_cloud_recovery_handler = handler;
}

/*
 * Register a Ayla cloud service connectivity up/down handler.
 */
void ops_set_cloud_connectivity_handler(void (*handler)(bool))
{
	ops_cloud_connectivity_handler = handler;
}

/*
 * Register a handler for client events.  This callback is invoked for all
 * events from the client, and uninteresting events may be safely ignored.
 */
void ops_set_client_event_handler(void (*handler)(enum ops_client_event event))
{
	ops_client_event_handler = handler;
}

/*
 * Create object for a property_ack
 */
json_t *ops_prop_ack_json_create(const char *proto, int req_id, int source,
				json_t *prop)
{
	json_t *root = jint_new_cmd(proto, data_ops[AD_PROP_ACK], req_id);
	json_t *cmd = json_object_get(root, "cmd");
	json_t *args = json_array();

	REQUIRE(args, REQUIRE_MSG_ALLOCATION);
	json_array_append(args, prop);
	json_object_set_new(cmd, "args", args);
	jint_source_set(cmd, source);

	return root;
}

/*
 * For simple operations
 */
static int ops_cmd_handler(void *arg, int *req_id, int confirm_needed)
{
	json_t *root = arg;
	json_t *cmd_j;

	if (confirm_needed) {
		cmd_j = json_object_get(root, "cmd");
		jint_confirm_set(cmd_j, confirm_needed);
		if (req_id) {
			json_get_int(cmd_j, "id", req_id);
		}
	}
	return data_send_json(root);
}

/*
 * Free handler for ops commands
 */
static void ops_cmd_free(void *arg)
{
	json_t *root = arg;

	json_decref(root);
}

/*
 * Confirmation handler
 */
static int ops_ack_confirmation_handler(void *arg, enum confirm_status status,
	enum confirm_err err, int dests)
{
	json_t *obj = arg;
	json_t *arg_arr_j;
	json_t *cmd_j;
	json_t *arg_obj_j;
	json_t *op_args_arr_j;
	const char *protocol;
	const char *propname;

	if (status != CONF_STAT_FAIL) {
		return 0;
	}
	if ((err != CONF_ERR_CONN) || !(dests & DEST_ADS)) {
		/*
		 * ignore ack send failures that are not for ADS or are not
		 * for connection loss
		 */
		return 0;
	}
	/* an ACK failure to ADS is like an echo failure to ADS */
	/* XXX An ACK failure should NOT prompt a gratuitous property echo */
	obj = json_object_get(obj, "cmd");
	protocol = json_get_string(obj, "proto");
	obj = json_object_get(obj, "args");
	obj = json_array_get(obj, 0);
	obj = json_object_get(obj, "property");
	if (!obj) {
bad_obj:
		log_warn("bad obj");
		return 0;
	}
	propname = json_get_string(obj, "name");
	if (!obj || !propname) {
		goto bad_obj;
	}
	cmd_j = json_object();
	json_object_set_new(cmd_j, "op",
	    json_string(data_ops[AD_ECHO_FAILURE]));
	arg_arr_j = json_array();
	arg_obj_j = json_object();
	json_object_set_new(arg_obj_j, "name", json_string(propname));
	json_object_set_new(arg_obj_j, "dests", json_integer(dests));
	json_object_set_new(arg_obj_j, "err", json_string(JINT_ERR_CONN_ERR));
	op_args_arr_j = json_array();
	json_array_append(op_args_arr_j, obj);
	json_object_set_new(arg_obj_j, "op_args", op_args_arr_j);
	json_array_append_new(arg_arr_j, arg_obj_j);
	json_object_set_new(cmd_j, "args", arg_arr_j);
	data_cmd_parse(cmd_j, protocol, 0);
	json_decref(cmd_j);

	return 0;
}

/*
 * Send acknowledgment for property update. Should be called as a result
 * of a call to the set handler with an ack_arg. *status* needs to be set to
 * 0 for a success and any other value for failure.
 * *ack_message* can be any integer that the oem desires to store in the cloud
 * to represent the result of the command.
 */
void ops_prop_ack_send(void *ack_arg, int status, int ack_message)
{
	json_t *root;
	json_t *obj;

	if (!ack_arg) {
		/* if ack_arg is NULL, no ack is needed */
		return;
	}
	root = (json_t *)ack_arg;
	obj = json_object_get(root, "cmd");
	obj = json_object_get(obj, "args");
	obj = json_array_get(obj, 0);
	obj = json_object_get(obj, "property");
	if (!obj) {
		log_warn("bad arg");
		return;
	}
	json_object_set_new(obj, "status", json_integer(status));
	json_object_set_new(obj, "ack_message", json_integer(ack_message));
	ops_add(ops_cmd_handler, root, NULL, ops_ack_confirmation_handler,
	    ops_cmd_free);
}

/*
 * Return the current system time in milliseconds
 */
unsigned long long ops_get_system_time_ms(void)
{
	struct timeval timestamp;

	gettimeofday(&timestamp, NULL);
	return (unsigned long long)(timestamp.tv_sec) * 1000 +
	    timestamp.tv_usec / 1000;
}
