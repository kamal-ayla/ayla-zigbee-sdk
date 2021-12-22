/*
 * Copyright 2015-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/file_event.h>
#include <ayla/timer.h>
#include <ayla/socket.h>
#include <ayla/time_utils.h>
#include <ayla/log.h>

#include <ayla/amsg_protocol.h>
#include <ayla/amsg.h>


/* Arbitrary maximum message size to limit unrealistic memory allocation */
#define AMSG_MSG_SIZE_MAX	8000000		/* 8 MB */

/* XXX Workaround for TAILQ_INSERT_TAIL bug */
#define	_TAILQ_INSERT_TAIL(head, elm, field) do {	\
	if (TAILQ_EMPTY(head)) {			\
		TAILQ_INSERT_HEAD(head, elm, field);	\
	} else {					\
		TAILQ_INSERT_TAIL(head, elm, field);	\
	}						\
} while (0)

DEF_NAME_TABLE(amsg_types_basic_names, AMSG_TYPES_INTERNAL);

/*
 * Asynchronous reply handling queue entry.
 */
struct amsg_pending_resp {
	struct amsg_endpoint *endpoint;
	void (*handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *);
	void *arg;
	uint16_t sequence_num;
	struct timer timer;
	TAILQ_ENTRY(amsg_pending_resp) entry;
};

/*
 * Structure containing information needed to send a response to an
 * incoming message.  A pointer to this structure is passed into
 * message handler callbacks and can be passed to amsg_send_resp().
 * If deferred response is desired, use the amsg_get_async_resp_info()
 * function to prepare a reply context that can be used by
 * amsg_send_resp() when the application is ready to send its reply.
 */
struct amsg_resp_info {
	struct amsg_endpoint *endpoint;
	uint8_t flags;
	uint8_t interface;
	uint8_t type;
	uint16_t sequence_num;
	int sync_sock;
	bool async_resp;
	TAILQ_ENTRY(amsg_resp_info) entry;
};

/*
 * List entry for storing user-defined data.
 */
struct amsg_user_data_entry {
	int id;
	void *data;
	void (*free_data)(void *);
	LIST_ENTRY(amsg_user_data_entry) entry;
};

/*
 * Reply context for asynchronous ping messages.
 */
struct amsg_ping_async_info {
	void (*callback)(enum amsg_err, uint32_t);
	uint64_t ping_time;
};

/*
 * Global lookup table of interface handlers.
 * Trades 1K of memory for constant time lookup. A maximum of 256 interfaces
 * are supported.
 */
static enum amsg_err (*amsg_interface_handlers[0xff])(
	struct amsg_endpoint *,
	const struct amsg_msg_info *,
	struct amsg_resp_info *);

/*
 * Return the user data entry at the specified ID, or NULL.
 */
static struct amsg_user_data_entry *amsg_user_data_get_entry(
	const struct amsg_endpoint *endpoint, int id)
{
	struct amsg_user_data_entry *entry;

	LIST_FOREACH(entry, &endpoint->user_data, entry) {
		if (entry->id == id) {
			return entry;
		}
	}
	return NULL;
}

/*
 * Remove and free all user data associated with an endpoint.
 */
static void amsg_user_data_free_all(struct amsg_endpoint *endpoint)
{
	struct amsg_user_data_entry *entry;

	while ((entry = LIST_FIRST(&endpoint->user_data)) != NULL) {
		LIST_REMOVE(entry, entry);
		if (entry->free_data) {
			entry->free_data(entry->data);
		}
		free(entry);
	}
}

/*
 * Handle default response messages.  Optionally return the interface and type.
 */
static enum amsg_err amsg_handler_default_resp(const struct amsg_msg_info *info,
	uint8_t *interface, uint8_t *type)
{
	struct amsg_msg_default_resp *msg =
	    (struct amsg_msg_default_resp *)info->payload;

	if (!(info->flags & AMSG_FLAGS_RESPONSE)) {
		/* Only supported as a response message */
		return AMSG_ERR_TYPE_UNSUPPORTED;
	}
#ifdef AMSG_DEBUG
	log_debug("%s: [%hhu:%hhu] err=\"%s\" seq#=%hu",
	    amsg_types_basic_names[info->type], msg->interface, msg->type,
	    amsg_err_string(msg->err), info->sequence_num);
#endif
	if (interface) {
		*interface = msg->interface;
	}
	if (type) {
		*type = msg->type;
	}
	return msg->err;
}

/*
 * Message handler for an incoming PING message.  Immediately replies
 * with a PING_RESP.
 */
static enum amsg_err amsg_handler_ping(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info, struct amsg_resp_info *resp_info)
{
#ifdef AMSG_DEBUG
	log_debug("received PING [%hu]", info->sequence_num);
#endif
	return amsg_send_resp(&resp_info,
	    AMSG_INTERFACE_INTERNAL, AMSG_TYPE_PING_RESP, NULL, 0);
}

/*
 * Response handler for PING messages.  Used for both synchronous and
 * asynchronous ping sends.
 */
static void amsg_resp_handler_ping(struct amsg_endpoint *endpoint,
	enum amsg_err err, const struct amsg_msg_info *info, void *resp_arg)
{
	struct amsg_ping_async_info *async_info =
	    (struct amsg_ping_async_info *)resp_arg;

	/* Synchronous call does not provide an arg */
	if (!async_info) {
		return;
	}
	/* Calculate turn-around time in milliseconds */
	async_info->callback(err, time_mtime_ms() - async_info->ping_time);
	free(async_info);
}

/*
 * Message interface handler for the internal interface.
 */
static enum amsg_err amsg_interface_handler_internal(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	ASSERT(info->interface == AMSG_INTERFACE_INTERNAL);

	switch (info->type) {
	case AMSG_TYPE_DEFAULT_RESP:
		return amsg_handler_default_resp(info, NULL, NULL);
	case AMSG_TYPE_PING:
		return amsg_handler_ping(endpoint, info, resp_info);
	default:
		break;
	}
	return AMSG_ERR_TYPE_UNSUPPORTED;
}

/*
 * Add the internal interface (required).
 */
static void amsg_init_handlers(void)
{
	amsg_interface_handlers[AMSG_INTERFACE_INTERNAL] =
	    amsg_interface_handler_internal;
}

/*
 * Message handler for all messages.  Dispatches handling to the
 * appropriate interface handler.
 */
static enum amsg_err amsg_handler(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info, struct amsg_resp_info *resp_info)
{
	enum amsg_err (*handler)(struct amsg_endpoint *,
	    const struct amsg_msg_info *, struct amsg_resp_info *);

	handler = amsg_interface_handlers[info->interface];
	if (!handler) {
		return AMSG_ERR_INTERFACE_UNSUPPORTED;
	}
	return handler(endpoint, info, resp_info);
}

/*
 * Invoke the response callback with an error code if a message recipient
 * did not reply in time.
 */
static void amsg_async_reply_timeout(struct timer *timer)
{
	struct amsg_pending_resp *pending_resp =
	    CONTAINER_OF(struct amsg_pending_resp, timer, timer);

	TAILQ_REMOVE(&pending_resp->endpoint->pending_resp_queue, pending_resp,
	    entry);
	if (pending_resp->handler) {
		pending_resp->handler(pending_resp->endpoint,
		    AMSG_ERR_TIMED_OUT, NULL, pending_resp->arg);
	}
	free(pending_resp);
}

/*
 * Send a message to the specified endpoint.  For outgoing synchronous
 * messages, a synchronous reply socket file descriptor can be sent to the
 * recipient by specifying a valid sync_sock parameter.  For synchronous
 * response messages, specify the reply socket file descriptor received from
 * the sender.  Asynchronous transactions do not need this, so set it to -1.
 */
static enum amsg_err amsg_send_internal(struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info, int sync_sock)
{
	int sock;
	struct msghdr sock_msg;
	struct iovec iov[2];
	struct cmsghdr *ctrl_msg;
	int *sock_ptr;
	uint8_t ctrl_data[CMSG_SPACE(sizeof(sock))];
	struct amsg_header header;
	enum amsg_err err = AMSG_ERR_NONE;
	bool sync_msg = (info->flags & AMSG_FLAGS_SYNC) && sync_sock > -1;
	bool msg_data = (info->payload && info->payload_size);

	/* Select appropriate destination socket */
	if (sync_msg && (info->flags & AMSG_FLAGS_RESPONSE)) {
		sock = sync_sock;
	} else if (endpoint->sock < 0) {
		log_err("socket not connected");
		err = AMSG_ERR_SOCKET;
		goto error;
	} else {
		sock = endpoint->sock;
	}
	/* Populate message header */
	amsg_populate_header(&header, info);
	/* Configure socket message */
	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	if (msg_data) {
		iov[1].iov_base = info->payload;
		iov[1].iov_len = info->payload_size;
	}
	memset(&sock_msg, 0, sizeof(sock_msg));
	sock_msg.msg_iov = iov;
	sock_msg.msg_iovlen = msg_data ? 2 : 1;
	/* Send reply socket FD as ancillary data, if needed */
	if (sync_msg && !(info->flags & AMSG_FLAGS_RESPONSE)) {
		sock_msg.msg_control = ctrl_data;
		sock_msg.msg_controllen = sizeof(ctrl_data);
		ctrl_msg = CMSG_FIRSTHDR(&sock_msg);
		ctrl_msg->cmsg_level = SOL_SOCKET;
		ctrl_msg->cmsg_type = SCM_RIGHTS;
		ctrl_msg->cmsg_len = CMSG_LEN(sizeof(sock));
		sock_ptr = (int *)CMSG_DATA(ctrl_msg);
		*sock_ptr = sync_sock;
#ifdef AMSG_DEBUG
		log_debug("sync_sock=%d", sync_sock);
#endif
	}
	/* Send message (suppress SIGPIPE) */
	if (sendmsg(sock, &sock_msg, MSG_NOSIGNAL) < 0) {
		log_err("send failed: %m");
		if (errno == EPIPE) {
			err = AMSG_ERR_DISCONNECTED;
		} else {
			err = AMSG_ERR_SOCKET;
		}
		goto error;
	}
#ifdef AMSG_DEBUG
	AMSG_DEBUG_PRINT_MSG_INFO("sent", *info);
#endif
error:
	if (err == AMSG_ERR_DISCONNECTED) {
		/* Disconnect handler can clean up endpoint state */
		if (endpoint->event_handler) {
			endpoint->event_handler(endpoint,
			    AMSG_ENDPOINT_DISCONNECT);
		}
	}
	return err;
}

/*
 * Receive an incoming message and populate the info structure.
 * On success, this function may allocate the incoming message payload buffer.
 * If the value pointed to by payload_malloced is set to true,
 * free info->payload when done handling the message.
 */
static enum amsg_err amsg_receive_internal(struct amsg_endpoint *endpoint,
	struct amsg_msg_info *info, bool *payload_mallocd, int *sync_sock)
{
	int sock;
	struct msghdr sock_msg;
	struct iovec iov[2];
	struct cmsghdr *ctrl_msg = NULL;
	int *sock_ptr;
	uint8_t ctrl_data[CMSG_SPACE(sizeof(*sock_ptr))] = { 0 };
	struct amsg_header header;
	enum amsg_err err = AMSG_ERR_NONE;
	size_t msg_len;
	ssize_t recv_len;

	/* Clear flag until payload is allocated */
	*payload_mallocd = false;

	/* Receive on sync reply sock if specified and valid */
	sock = (sync_sock && *sync_sock > -1) ? *sync_sock : endpoint->sock;

	/* Configure socket message */
	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	memset(&sock_msg, 0, sizeof(sock_msg));
	sock_msg.msg_iov = iov;
	sock_msg.msg_iovlen = 1;
	if (sync_sock && *sync_sock == -1) {
		/* Configure sock_msg to receive a sync reply socket */
		sock_msg.msg_control = ctrl_data;
		sock_msg.msg_controllen = sizeof(ctrl_data);
		ctrl_msg = CMSG_FIRSTHDR(&sock_msg);
	}

	/* Start by reading the header without clearing data from socket */
	recv_len = recvmsg(sock, &sock_msg, MSG_PEEK);
	if (recv_len < 0) {
		log_err("socket error: %m");
		err = AMSG_ERR_SOCKET;
		goto error;
	}
	if (!recv_len) {
		err = AMSG_ERR_DISCONNECTED;
		goto disconnected;
	}
	if (recv_len != sizeof(header)) {
		log_err("received incomplete header");
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	msg_len = recv_len;
	/* Parse the message header */
	err = amsg_parse_header(&header, msg_len, info);
	if (err != AMSG_ERR_NONE) {
		goto error;
	}
	/* Sync message ancillary data handling */
	if ((info->flags & AMSG_FLAGS_SYNC) &&
	    !(info->flags & AMSG_FLAGS_RESPONSE)) {
		if (ctrl_msg &&
		    ctrl_msg->cmsg_len == CMSG_LEN(sizeof(*sock_ptr)) &&
		    ctrl_msg->cmsg_level == SOL_SOCKET &&
		    ctrl_msg->cmsg_type == SCM_RIGHTS) {
			sock_ptr = (int *)CMSG_DATA(ctrl_msg);
			if (!sync_sock) {
				log_err("received unexpected sync sock");
				close(*sock_ptr);
				err = AMSG_ERR_SOCKET;
				goto error;
			}
			/* Incoming sync msgs should include a reply socket */
			*sync_sock = *sock_ptr;
#ifdef AMSG_DEBUG
			log_debug("sync_sock=%d", *sync_sock);
#endif
			/* Clear control msg to avoid receiving another FD */
			sock_msg.msg_control = NULL;
			sock_msg.msg_controllen = 0;
		} else {
			log_err("missing control msg with sync reply socket");
			err = AMSG_ERR_SOCKET;
			goto error;
		}
	}
	/* Don't need to allocate buffer for payload if there isn't one */
	if (!info->payload_size) {
		/* Clear buffered socket data */
		recvmsg(sock, &sock_msg, 0);
		goto done;
	}
	msg_len += info->payload_size;
	if (msg_len > AMSG_MSG_SIZE_MAX) {
		log_warn("%zu exceeded max message size: %u bytes", msg_len,
		    AMSG_MSG_SIZE_MAX);
		err = AMSG_ERR_MEM;
		goto error;
	}
	info->payload = malloc(info->payload_size);
	if (!info->payload) {
		err = AMSG_ERR_MEM;
		goto error;
	}
	*payload_mallocd = true;
	/* Fetch payload IOV */
	iov[1].iov_base = info->payload;
	iov[1].iov_len = info->payload_size;
	sock_msg.msg_iovlen = 2;
	recv_len = recvmsg(sock, &sock_msg, 0);
	if (recv_len < 0) {
		log_err("socket error: %m");
		err = AMSG_ERR_SOCKET;
		goto error;
	}
	if (!recv_len) {
		err = AMSG_ERR_DISCONNECTED;
		goto disconnected;
	}
	if (recv_len != msg_len) {
		log_err("received %zd payload bytes, expected %zu",
		    recv_len, msg_len);
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	goto done;
error:
	/* Clear any buffered socket data */
	recv(sock, &header, 1, 0);
disconnected:
	if (*payload_mallocd) {
		free(info->payload);
		info->payload = NULL;
		*payload_mallocd = false;
	}
	if (err == AMSG_ERR_DISCONNECTED) {
		/* Disconnect handler can clean up endpoint state */
		if (endpoint->event_handler) {
			endpoint->event_handler(endpoint,
			    AMSG_ENDPOINT_DISCONNECT);
		}
	}
done:
	return err;
}

/*
 * Receive and handle an incoming message.
 */
static void amsg_recv(void *arg, int sock)
{
	struct amsg_endpoint *endpoint = (struct amsg_endpoint *)arg;
	struct amsg_msg_info info;
	struct amsg_resp_info resp_info;
	struct amsg_resp_info *resp_info_ptr = &resp_info;
	bool payload_mallocd;
	void *payload_ptr = NULL;
	struct amsg_pending_resp *pending_resp = NULL;
	enum amsg_err err;

	ASSERT(endpoint->sock == sock);

	memset(&info, 0, sizeof(info));
	memset(&resp_info, 0, sizeof(resp_info));
	resp_info.sync_sock = -1;

	err = amsg_receive_internal(endpoint, &info, &payload_mallocd,
	    &resp_info.sync_sock);
	if (err != AMSG_ERR_NONE) {
		goto done;
	}
	if (payload_mallocd) {
		payload_ptr = info.payload;
	}
	/* Handle an async message reply */
	if (info.flags & AMSG_FLAGS_RESPONSE) {
		if (info.flags & AMSG_FLAGS_SYNC) {
			log_warn("received unexpected sync response");
			goto done;
		}
#ifdef AMSG_DEBUG
		AMSG_DEBUG_PRINT_MSG_INFO("received async response", info);
#endif
		/* Lookup reply entry by sequence number */
		TAILQ_FOREACH(pending_resp, &endpoint->pending_resp_queue,
		    entry) {
			if (pending_resp->sequence_num == info.sequence_num) {
				break;
			}
		}
		if (!pending_resp) {
			/* Default to standard message handler */
			err = amsg_handler(endpoint, &info, NULL);
#ifdef AMSG_DEBUG
			if (err != AMSG_ERR_NONE) {
				log_debug("response ignored: %s",
				    amsg_err_string(err));
			}
#endif
			goto done;
		}
		/* Custom reply handler found */
		if (endpoint->control->timers) {
			timer_cancel(endpoint->control->timers,
			    &pending_resp->timer);
		}
		TAILQ_REMOVE(&endpoint->pending_resp_queue, pending_resp,
		    entry);
		if (info.interface == AMSG_INTERFACE_INTERNAL &&
		    info.type == AMSG_TYPE_DEFAULT_RESP) {
			/* Extract default resp details and update msg_info */
			err = amsg_handler_default_resp(&info,
			    &info.interface, &info.type);
			info.payload = NULL;
			info.payload_size = 0;
			pending_resp->handler(endpoint, err, &info,
			    pending_resp->arg);
		} else {
			pending_resp->handler(endpoint, AMSG_ERR_NONE,
			    &info, pending_resp->arg);
		}
		goto done;
	}
	/* Handle a new message */
#ifdef AMSG_DEBUG
	AMSG_DEBUG_PRINT_MSG_INFO("received", info);
#endif
	if (info.flags & AMSG_FLAGS_RESPONSE_REQUESTED) {
		/* Populate message-specific response info */
		resp_info.endpoint = endpoint;
		resp_info.flags = info.flags;
		resp_info.interface = info.interface;
		resp_info.type = info.type;
		resp_info.sequence_num = info.sequence_num;
		/* Handler may modify resp_info to indicate response status */
		err = amsg_handler(endpoint, &info, &resp_info);
		if ((resp_info.flags & AMSG_FLAGS_RESPONSE_REQUESTED) &&
		    (!resp_info.async_resp || err != AMSG_ERR_NONE)) {
			/*
			 * Handler did not send a response and either did not
			 * indicate async response handling was scheduled, or
			 * indicated a failure handling the message.  In this
			 * case, send a default response here.
			 */
			resp_info.async_resp = false;
			amsg_send_default_resp(&resp_info_ptr, err);
		}
	} else {
		amsg_handler(endpoint, &info, NULL);
	}
done:
	if (resp_info.sync_sock > -1 && !resp_info.async_resp) {
		/* Sync reply socket was created but not used */
		close(resp_info.sync_sock);
	}
	free(payload_ptr);
	free(pending_resp);
}

/*
 * Free resources associated with a received message that requested a response.
 * This is called after sending the response, or if the response was canceled.
 */
static void amsg_response_cleanup(struct amsg_resp_info *resp_info)
{
	if (!resp_info) {
		return;
	}
	/* Indicate the reply context has been used */
	resp_info->flags &= ~AMSG_FLAGS_RESPONSE_REQUESTED;
	/* Free resources used for sync responses */
	if (resp_info->sync_sock > -1) {
		close(resp_info->sync_sock);
		resp_info->sync_sock = -1;
	}
	if (resp_info->endpoint && resp_info->async_resp) {
		/* Async response info is allocated and tracked in a queue */
		TAILQ_REMOVE(&resp_info->endpoint->resp_info_queue, resp_info,
		    entry);
		resp_info->endpoint = NULL;
	}
}

/*
 * Initialize an unconnected endpoint.
 */
static int amsg_endpoint_init(struct amsg_endpoint *endpoint,
	struct amsg_thread_control *control,
	int (*event_handler)(struct amsg_endpoint *, enum amsg_endpoint_event),
	void *event_data)
{
	endpoint->sock = -1;
	endpoint->control = control;
	endpoint->event_handler = event_handler;
	endpoint->event_data = event_data;
	endpoint->sequence_num = 0;
	LIST_INIT(&endpoint->user_data);
	TAILQ_INIT(&endpoint->pending_resp_queue);
	TAILQ_INIT(&endpoint->resp_info_queue);
	return 0;
}

/*
 * Generic disconnect and cleanup routine for an endpoint.  Causes any
 * pending reply callbacks to be invoked with an error code.
 */
static int amsg_endpoint_disconnect(struct amsg_endpoint *endpoint)
{
	struct amsg_pending_resp *pending_resp;
	struct amsg_resp_info *resp_info;

	if (endpoint->sock <= 0) {
		return 0;
	}
	close(endpoint->sock);
	endpoint->sock = -1;
	endpoint->sequence_num = 0;
	/* Clear any pending reply entries */
	while ((pending_resp = TAILQ_FIRST(&endpoint->pending_resp_queue))
	    != NULL) {
		if (endpoint->control->timers) {
			timer_cancel(endpoint->control->timers,
			    &pending_resp->timer);
		}
		TAILQ_REMOVE(&endpoint->pending_resp_queue, pending_resp,
		    entry);
		if (pending_resp->handler) {
			pending_resp->handler(endpoint, AMSG_ERR_DISCONNECTED,
			    NULL, pending_resp->arg);
		}
		free(pending_resp);
	}
	/* Clear any outstanding asynchronous response contexts */
	while ((resp_info = TAILQ_FIRST(&endpoint->resp_info_queue)) != NULL) {
		/* Cleanup function removes entry from queue and clears it */
		amsg_response_cleanup(resp_info);
	}
	return 0;
}

/*
 * Increment and return an endpoint's sequence number.  0 is not a valid
 * sequence.
 */
static uint16_t amsg_endpoint_next_sequence(struct amsg_endpoint *endpoint)
{
	if (endpoint->sock <= 0) {
		log_err("endpoint not connected");
		return 0;
	}
	++endpoint->sequence_num;
	/* Check for wrap */
	if (!endpoint->sequence_num) {
		++endpoint->sequence_num;
	}
	return endpoint->sequence_num;
}

/*
 * Endpoint connection event handler for a client.
 */
static int amsg_client_event(struct amsg_endpoint *endpoint,
	    enum amsg_endpoint_event event)
{
	struct amsg_client *state = (struct amsg_client *)endpoint->event_data;

	switch (event) {
	case AMSG_ENDPOINT_DISCONNECT:
#ifdef AMSG_DEBUG
		log_debug("disconnect: fd %d", endpoint->sock);
#endif
		if (endpoint->control->fd_events) {
			/* Unregister async message handler */
			if (file_event_unreg(endpoint->control->fd_events,
			    endpoint->sock, amsg_recv, NULL, endpoint) < 0) {
				log_warn("file event unregistration failed: "
				    "fd %d", endpoint->sock);
			}
		}
		amsg_endpoint_disconnect(endpoint);
		break;
	case AMSG_ENDPOINT_CONNECT:
		if (endpoint->control->fd_events) {
			/* Register async message handler */
			if (file_event_reg(endpoint->control->fd_events,
			    endpoint->sock, amsg_recv, NULL, endpoint) < 0) {
				log_err("file event registration failed: fd %d",
				    endpoint->sock);
				amsg_endpoint_disconnect(endpoint);
				return -1;
			}
		}
#ifdef AMSG_DEBUG
		log_debug("connected: fd %d", endpoint->sock);
#endif
		break;
	}
	/* Invoke client's event handler */
	if (state->event_callback) {
		return state->event_callback(endpoint, event);
	}

	return 0;
}

/*
 * Endpoint connection event handler for a server.
 */
static int amsg_server_session_event(struct amsg_endpoint *endpoint,
	    enum amsg_endpoint_event event)
{
	struct amsg_server *state = (struct amsg_server *)endpoint->event_data;
	struct amsg_server_session *session;

	switch (event) {
	case AMSG_ENDPOINT_DISCONNECT:
		session = CONTAINER_OF(struct amsg_server_session, endpoint,
		    endpoint);
#ifdef AMSG_DEBUG
		log_debug("session removed: fd %d", endpoint->sock);
#endif
		/* Unregister async message handler */
		if (file_event_unreg(endpoint->control->fd_events,
		    endpoint->sock, amsg_recv, NULL, endpoint) < 0) {
			log_warn("file event unregistration failed: fd %d",
			    endpoint->sock);
		}
		amsg_endpoint_disconnect(endpoint);
		LIST_REMOVE(session, list_entry);
		--state->num_sessions;
		/* Invoke server's client event handler before freeing */
		if (state->session_event_callback) {
			state->session_event_callback(endpoint, event);
		}
		/* Clear user data before freeing the session */
		amsg_user_data_free_all(endpoint);
		free(session);
		break;
	case AMSG_ENDPOINT_CONNECT:
		/* Create new session */
		session = (struct amsg_server_session *)malloc(sizeof(
		    struct amsg_server_session));
		if (!session) {
			log_err("malloc failed");
			amsg_endpoint_disconnect(endpoint);
			return -1;
		}
		session->endpoint = *endpoint;
		endpoint = &session->endpoint;
		/* Register async message handler */
		if (file_event_reg(endpoint->control->fd_events,
		    endpoint->sock, amsg_recv, NULL, endpoint) < 0) {
			log_err("file event registration failed: fd %d",
			    endpoint->sock);
			amsg_endpoint_disconnect(endpoint);
			free(session);
			return -1;
		}
		LIST_INSERT_HEAD(&state->session_list, session, list_entry);
		++state->num_sessions;
#ifdef AMSG_DEBUG
		log_debug("session added: fd %d", endpoint->sock);
#endif
		/* Invoke server's client event handler */
		if (state->session_event_callback) {
			state->session_event_callback(endpoint, event);
		}
		break;
	}
	return 0;
}

/*
 * Server session accept callback.  Initializes the session endpoint and
 * passes it to the server endpoint connection event handler to create the
 * session.
 */
static void amsg_server_accept(void *arg, int fd)
{
	struct amsg_server *state = (struct amsg_server *)arg;
	struct amsg_endpoint endpoint;
	int sock;

	sock = socket_accept(fd);
	if (sock == -1) {
		log_err("failed to accept client: fd %d",
		    state->server.sock);
		return;
	}
	if (state->max_sessions && state->num_sessions >= state->max_sessions) {
		log_warn("maximum number of sessions active (%zu): "
		    "rejecting client", state->max_sessions);
		close(sock);
		return;
	}
	amsg_endpoint_init(&endpoint, &state->control,
	    amsg_server_session_event, state);
	endpoint.sock = sock;
	endpoint.event_handler(&endpoint, AMSG_ENDPOINT_CONNECT);
}

/*
 * Initialize a message client.  Must be called prior to invoking other
 * functions.  fd_events and timers are optional arguments for clients.
 * The fd_events parameter may be set to NULL if asynchronous message handling
 * is not required.  The timers parameter may be set to null if asynchronous
 * send timeouts are not required.
 */
int amsg_client_init(struct amsg_client *state,
	struct file_event_table *fd_events, struct timer_head *timers)
{
	ASSERT(state != NULL);

	state->control.fd_events = fd_events;
	state->control.timers = timers;
	state->event_callback = NULL;
	if (amsg_endpoint_init(&state->endpoint, &state->control,
	    amsg_client_event, state) < 0) {
		return -1;
	}
	/* Register handlers for internal messages (required) */
	amsg_init_handlers();
	return 0;
}

/*
 * Cleanup resources associated with a message client.
 */
void amsg_client_cleanup(struct amsg_client *state)
{
	ASSERT(state != NULL);

	amsg_client_disconnect(state);
	amsg_user_data_free_all(&state->endpoint);
}

/*
 * Connect to a message server with the specified socket path.
 */
int amsg_client_connect(struct amsg_client *state, const char *path)
{
	ASSERT(state != NULL);
	ASSERT(path != NULL && *path != '\0');

	if (state->endpoint.sock != -1) {
		log_warn("already connected");
		return -1;
	}
	state->endpoint.sock = socket_connect(path, SOCK_SEQPACKET);
	if (state->endpoint.sock == -1) {
		log_err("connect failed: %s", path);
		return -1;
	}
	return state->endpoint.event_handler(
	    &state->endpoint, AMSG_ENDPOINT_CONNECT);
}

/*
 * Disconnect from a server.
 */
int amsg_client_disconnect(struct amsg_client *state)
{
	ASSERT(state != NULL);

	amsg_disconnect(&state->endpoint);
	return 0;
}

/*
 * Register a connection event callback to be called after the connection
 * is established or disconnected.
 */
void amsg_client_set_event_callback(struct amsg_client *state,
	int (*callback)(struct amsg_endpoint *, enum amsg_endpoint_event))
{
	ASSERT(state != NULL);

	state->event_callback = callback;
}

/*
 * Initialize a message server.  Must be called prior to invoking other
 * functions.  The timers parameter may be set to null if asynchronous send
 * timeouts are not required.
 */
int amsg_server_init(struct amsg_server *state,
	struct file_event_table *fd_events, struct timer_head *timers)
{
	ASSERT(state != NULL);
	ASSERT(fd_events != NULL);

	state->control.fd_events = fd_events;
	state->control.timers = timers;
	state->session_event_callback = NULL;
	if (amsg_endpoint_init(&state->server, &state->control,
	    NULL, NULL) < 0) {
		return -1;
	}
	state->max_sessions = 0;
	state->num_sessions = 0;
	LIST_INIT(&state->session_list);
	/* Register handlers for internal messages (required) */
	amsg_init_handlers();
	return 0;
}

/*
 * Cleanup resources associated with a message server.
 */
void amsg_server_cleanup(struct amsg_server *state)
{
	ASSERT(state != NULL);

	amsg_server_stop(state);
	amsg_user_data_free_all(&state->server);
}

/*
 * Create a server socket with the file mode specified, bind it to path, and
 * begin accepting connections from message clients.
 */
int amsg_server_start(struct amsg_server *state, const char *path, int mode)
{
	ASSERT(state != NULL);
	ASSERT(path != NULL && *path != '\0');

	if (state->server.sock != -1) {
		log_warn("already started");
		return -1;
	}
	/* Create server socket and bind to path */
	state->server.sock = socket_bind(path, SOCK_SEQPACKET, mode);
	if (state->server.sock == -1) {
		log_err("server bind failed: %s", path);
		goto error;
	}
	/* Server socket handling is asynchronous, so set non-blocking */
	if (fcntl(state->server.sock, F_SETFL, O_NONBLOCK) < 0) {
		log_err("fcntl failed: %m");
		goto error;
	}
	/* Listen for incoming client connections */
	if (listen(state->server.sock,
	    state->max_sessions ? state->max_sessions : 32) < 0) {
		log_err("listen failed: %m");
		goto error;
	}
	/* Register accept listener */
	if (file_event_reg(state->control.fd_events,
	    state->server.sock, amsg_server_accept, NULL, state) < 0) {
		log_err("file event registration failed: fd %d",
		    state->server.sock);
		goto error;
	}
	log_debug("fd %d bound to %s", state->server.sock, path);
	return 0;
error:
	amsg_server_stop(state);
	return -1;
}

/*
 * Disconnect all clients and shutdown the message server.
 */
int amsg_server_stop(struct amsg_server *state)
{
	ASSERT(state != NULL);

	if (state->server.sock <= 0) {
		return 0;
	}
	/* Disconnect all clients */
	amsg_server_session_disconnect_all(state);
	/* Unregister accept listener */
	file_event_unreg(state->control.fd_events, state->server.sock,
	    amsg_server_accept, NULL, state);
	/* Close server socket */
	return amsg_endpoint_disconnect(&state->server);
}

/*
 * Disconnect all connected clients.
 */
void amsg_server_session_disconnect_all(struct amsg_server *state)
{
	struct amsg_server_session *session;

	ASSERT(state != NULL);

	while ((session = LIST_FIRST(&state->session_list)) != NULL) {
		/* Disconnect handler removes list entry */
		amsg_disconnect(&session->endpoint);
	}
}

/*
 * Iterate through all connected clients and call func for each one.  Set arg
 * to pass user data to func.  If func returns non-zero, iteration will stop
 * immediately.  A negative value returned by func is considered an error and
 * is returned by this function.  It is safe to disconnect a client during
 * iteration.
 */
int amsg_server_session_foreach(struct amsg_server *state,
	int (*func)(struct amsg_endpoint *, void *), void *arg)
{
	struct amsg_server_session *session;
	struct amsg_server_session *session_next;
	int rc;

	ASSERT(state != NULL);
	ASSERT(func != NULL);

	session = LIST_FIRST(&state->session_list);
	while (session) {
		session_next = LIST_NEXT(session, list_entry);
		rc = func(&session->endpoint, arg);
		if (rc < 0) {
			return rc;
		}
		if (rc > 0) {
			return 0;
		}
		session = session_next;
	}
	return 0;
}

/*
 * Limit the number of clients a server will accept.  0 means unlimited.
 */
void amsg_server_set_max_sessions(struct amsg_server *state,
	size_t max_sessions)
{
	ASSERT(state != NULL);

	state->max_sessions = max_sessions;
	if (state->max_sessions && state->num_sessions > state->max_sessions) {
		log_warn("set session limit (%zu) below current session "
		    "count (%zu)", state->max_sessions, state->num_sessions);
	}
}

/*
 * Register a client connection event callback to be called after a client is
 * added or disconnected.
 */
void amsg_server_set_session_event_callback(struct amsg_server *state,
	void (*callback)(struct amsg_endpoint *, enum amsg_endpoint_event))
{
	ASSERT(state != NULL);

	state->session_event_callback = callback;
}

/*
 * Register a handler to support a message interface.  All incoming messages
 * with the specified interface ID will be handled by this function.
 * Interface 0 (AMSG_INTERFACE_INTERNAL) is reserved and cannot be used.
 * Interface handler functions should look up and handle each message type
 * supported by this interface.  If an unexpected message type is received,
 * return AMSG_ERR_TYPE_UNSUPPORTED.  Interface handlers are stored globally,
 * so they only need to be registered once per process.
 */
void amsg_set_interface_handler(uint8_t interface,
	enum amsg_err (*handler)(struct amsg_endpoint *,
	const struct amsg_msg_info *msg, struct amsg_resp_info *))
{
	ASSERT(interface != AMSG_INTERFACE_INTERNAL);

	amsg_interface_handlers[interface] = handler;
}

/*
 * Disconnect a server or client endpoint.
 */
void amsg_disconnect(struct amsg_endpoint *endpoint)
{
	ASSERT(endpoint != NULL);

	if (endpoint->sock <= 0) {
		return;
	}
	if (!endpoint->event_handler) {
		return;
	}
	endpoint->event_handler(endpoint, AMSG_ENDPOINT_DISCONNECT);
}

/*
 * Return true if the endpoint is connected.
 */
bool amsg_connected(const struct amsg_endpoint *endpoint)
{
	return endpoint && endpoint->sock > 0;
}

/*
 * Set user data for an endpoint.  Multiple data slots may be managed by using
 * different IDs.  If free_data is set, it is called when the endpoint is
 * destroyed.
 * Returns data, or NULL if memory allocation failed.
 */
void *amsg_set_user_data(struct amsg_endpoint *endpoint, int id,
	void *data, void (*free_data)(void *))
{
	struct amsg_user_data_entry *entry;

	ASSERT(endpoint != NULL);
	ASSERT(data != NULL);

	entry = amsg_user_data_get_entry(endpoint, id);
	if (entry) {
		/* Update existing user data */
		if (entry->free_data) {
			entry->free_data(entry->data);
		}
	} else {
		/* Add new user data ID */
		entry = (struct amsg_user_data_entry *)malloc(sizeof(*entry));
		if (!entry) {
			return NULL;
		}
		entry->id = id;
		LIST_INSERT_HEAD(&endpoint->user_data, entry, entry);
	}
	entry->data = data;
	entry->free_data = free_data;
	return data;
}

/*
 * Remove an endpoint's user data with the specified ID. The free_data function
 * is NOT called, so any resources in use will need to be freed by the
 * application.
 * Returns the data, or NULL if the ID is invalid.
 */
void *amsg_clear_user_data(struct amsg_endpoint *endpoint, int id)
{
	struct amsg_user_data_entry *entry;
	void *data;

	ASSERT(endpoint != NULL);

	entry = amsg_user_data_get_entry(endpoint, id);
	if (!entry) {
		return NULL;
	}
	data = entry->data;
	LIST_REMOVE(entry, entry);
	free(entry);
	return data;
}

/*
 * Return an endpoint's user data pointer, or NULL, if the ID is invalid.
 */
void *amsg_get_user_data(const struct amsg_endpoint *endpoint, int id)
{
	struct amsg_user_data_entry *entry;

	ASSERT(endpoint != NULL);

	entry = amsg_user_data_get_entry(endpoint, id);
	if (!entry) {
		return NULL;
	}
	return entry->data;
}

/*
 * Send a message.  If a custom resp_handler is specified, the reply
 * handling information is queued so the reply can be handled appropriately
 * when it is received.  resp_arg is passed to the resp_handler.
 * If timeout_ms is 0, timeouts are disabled.
 */
enum amsg_err amsg_send(struct amsg_endpoint *endpoint, uint8_t interface,
	uint8_t type, const void *payload, size_t size,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	enum amsg_err err;
	struct amsg_msg_info info = {
		.flags = resp_handler ? AMSG_FLAGS_RESPONSE_REQUESTED : 0,
		.interface = interface,
		.type = type,
		.sequence_num = 0,
		.payload = (void *)payload, /* Cast but DO NOT modify payload */
		.payload_size = size
	};
	struct amsg_pending_resp *pending_resp;

	ASSERT(endpoint != NULL);
	ASSERT(!resp_handler || endpoint->control->fd_events != NULL);
	ASSERT(!timeout_ms || endpoint->control->timers != NULL);

	/* Increment sequence number */
	info.sequence_num = amsg_endpoint_next_sequence(endpoint);
	/* Send message */
	err = amsg_send_internal(endpoint, &info, -1);
	if (err != AMSG_ERR_NONE) {
		goto error;
	}
	/* No special reply handling necessary */
	if (!resp_handler) {
		return AMSG_ERR_NONE;
	}
	/* Queue async reply handler */
	pending_resp = (struct amsg_pending_resp *)malloc(
	    sizeof(struct amsg_pending_resp));
	if (!pending_resp) {
		err = AMSG_ERR_MEM;
		goto error;
	}
	pending_resp->endpoint = endpoint;
	pending_resp->sequence_num = info.sequence_num;
	pending_resp->handler = resp_handler;
	pending_resp->arg = resp_arg;
	timer_init(&pending_resp->timer, amsg_async_reply_timeout);
	_TAILQ_INSERT_TAIL(&endpoint->pending_resp_queue, pending_resp, entry);
	if (timeout_ms && endpoint->control->timers) {
		timer_set(endpoint->control->timers, &pending_resp->timer,
		    timeout_ms);
	}
	return AMSG_ERR_NONE;
error:
#ifdef AMSG_DEBUG
	log_err("send error: %s", amsg_err_string(err));
#endif
	return err;
}

/*
 * Send a message and block until a response is received.  If a custom
 * resp_handler is specified, it will be invoked before this function
 * returns.  resp_arg is passed to the resp_handler.  A dedicated response
 * socket is created for this call, so other messaging traffic in either
 * direction will not interrupt this message transaction.  If timeout_ms is 0,
 * timeouts are disabled.
 */
enum amsg_err amsg_send_sync(struct amsg_endpoint *endpoint,
	uint8_t interface, uint8_t type, const void *payload, size_t size,
	void (*resp_handler)(struct amsg_endpoint *, enum amsg_err,
	    const struct amsg_msg_info *, void *), void *resp_arg,
	uint32_t timeout_ms)
{
	enum amsg_err err;
	struct amsg_msg_info info = {
		.flags = AMSG_FLAGS_SYNC | AMSG_FLAGS_RESPONSE_REQUESTED,
		.interface = interface,
		.type = type,
		.sequence_num = 0,
		.payload = (void *)payload, /* Cast but DO NOT modify payload */
		.payload_size = size
	};
	struct amsg_msg_info resp_info;
	bool payload_mallocd;
	void *payload_ptr = NULL;
	int reply_socks[2];
	struct pollfd poll_fd;
	int rc;

	ASSERT(endpoint != NULL);

	if (endpoint->sock < 0) {
		log_err("socket not connected");
		return AMSG_ERR_SOCKET;
	}
	/* Create synchronous reply socket */
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, reply_socks) < 0) {
		log_err("failed to create reply socket: %m");
		return AMSG_ERR_SOCKET;
	}
	/* Increment sequence number */
	info.sequence_num = amsg_endpoint_next_sequence(endpoint);
	/* Send message */
	err = amsg_send_internal(endpoint, &info, reply_socks[1]);
	close(reply_socks[1]);
	if (err != AMSG_ERR_NONE) {
		close(reply_socks[0]);
		goto error;
	}
	/* Wait with timeout for data on reply socket */
	if (timeout_ms) {
		poll_fd.fd = reply_socks[0];
		poll_fd.events = POLLIN;
		poll_fd.revents = 0;
		rc = poll(&poll_fd, 1, timeout_ms);
		if (rc < 1) {
			close(reply_socks[0]);
			if (rc < 0) {
#ifdef AMSG_DEBUG
				log_debug("failed waiting for synchronous "
				    "response: %m");
#endif
				err = AMSG_ERR_SOCKET;
			} else {
				err = AMSG_ERR_TIMED_OUT;
			}
			goto error;
		}
	}
	/* Read synchronous response */
	memset(&resp_info, 0, sizeof(resp_info));
	err = amsg_receive_internal(endpoint, &resp_info, &payload_mallocd,
	    &reply_socks[0]);
	close(reply_socks[0]);
	if (err != AMSG_ERR_NONE) {
		log_debug("failed to receive synchronous response");
		goto error;
	}
	if (payload_mallocd) {
		payload_ptr = resp_info.payload;
	}
	/* Sequence number must match for sync responses */
	if (resp_info.sequence_num != info.sequence_num) {
		err = AMSG_ERR_SEQUENCE_BAD;
		goto error;
	}
#ifdef AMSG_DEBUG
	AMSG_DEBUG_PRINT_MSG_INFO("received", resp_info);
#endif
	if (resp_handler) {
		/* Using custom reply handler */
		if (resp_info.interface == AMSG_INTERFACE_INTERNAL &&
		    resp_info.type == AMSG_TYPE_DEFAULT_RESP) {
			/* Extract error in default resp message */
			err = amsg_handler_default_resp(&resp_info,
			    &resp_info.interface, &resp_info.type);
			/* Do not invoke resp_handler on error */
		} else {
			resp_handler(endpoint, AMSG_ERR_NONE,
			    &resp_info, resp_arg);
		}
	} else {
		/* Defaulting to standard message handler */
		err = amsg_handler(endpoint, &resp_info, NULL);
	}
error:
	free(payload_ptr);
#ifdef AMSG_DEBUG
	if (err != AMSG_ERR_NONE) {
		log_err("send error: %s", amsg_err_string(err));
	}
#endif
	return err;
}

/*
 * Returns a pointer to a resp_info structure for use when asynchronously
 * responding to a message (out of the message handler).  This async resp_info
 * is passed to amsg_send_resp() when the application is ready to send the
 * reply. Once amsg_send_resp() has been called, the pointer returned by this
 * function is NO LONGER VALID and should not be used again.  Use
 * amsg_free_async_resp_info() to free the resp_info returned by this function
 * if a response will not be sent.
 */
struct amsg_resp_info *amsg_alloc_async_resp_info(
	struct amsg_resp_info *resp_info)
{
	struct amsg_resp_info *async_resp_info;

	if (!resp_info) {
		log_err("no response info");
		return NULL;
	}
	/* Never allocate more than one reference to the resp_info */
	ASSERT(resp_info->async_resp != true);

	async_resp_info = (struct amsg_resp_info *)malloc(
	    sizeof(struct amsg_resp_info));
	if (!async_resp_info) {
		log_err("malloc failed");
		return NULL;
	}
	resp_info->async_resp = true;
	memcpy(async_resp_info, resp_info, sizeof(*async_resp_info));
	_TAILQ_INSERT_TAIL(&async_resp_info->endpoint->resp_info_queue,
	    async_resp_info, entry);
	return async_resp_info;
}

/*
 * Use this function to free a resp_info structure allocated with
 * amsg_alloc_async_resp_info(), but never passed to amsg_send_resp().  This
 * is useful if an endpoint disconnected or an error occurred before the
 * response was sent.
 */
void amsg_free_async_resp_info(struct amsg_resp_info **resp_info_ptr)
{
	struct amsg_resp_info *resp_info;

	ASSERT(resp_info_ptr != NULL);

	resp_info = *resp_info_ptr;
	*resp_info_ptr = NULL;
	if (!resp_info) {
		return;
	}
	amsg_response_cleanup(resp_info);
	if (resp_info->async_resp) {
		free(resp_info);
	}
}

/*
 * Send a custom message response.  This may be invoked either inside or
 * outside an incoming message handler.  See the comment on the
 * amsg_resp_info structure for details on sending asynchronous responses.
 * resp_info is passed in as a pointer to a pointer, so the function
 * can set the resp_info pointer to NULL once it has been used.
 */
enum amsg_err amsg_send_resp(struct amsg_resp_info **resp_info_ptr,
	uint8_t interface, uint8_t type, const void *payload, size_t size)
{
	struct amsg_resp_info *resp_info;
	struct amsg_msg_info info;
	enum amsg_err err;

	ASSERT(resp_info_ptr != NULL);

	resp_info = *resp_info_ptr;
	if (!resp_info) {
		log_err("no response info");
		return AMSG_ERR_APPLICATION;
	}
	if (!resp_info->endpoint || resp_info->endpoint->sock < 0) {
		log_err("socket not connected");
		err = AMSG_ERR_SOCKET;
		goto cleanup;
	}
	if (!(resp_info->flags & AMSG_FLAGS_RESPONSE_REQUESTED)) {
		log_err("response not requested: type=[%hhu:%hhu] seq#=%hu",
		    resp_info->interface, resp_info->type,
		    resp_info->sequence_num);
		err = AMSG_ERR_APPLICATION;
		goto cleanup;
	}
	info.flags = resp_info->flags;
	info.interface = interface;
	info.type = type;
	info.sequence_num = resp_info->sequence_num;
	info.payload = (void *)payload;	/* Cast but DO NOT modify payload */
	info.payload_size = size;
	/* Set flags for response message */
	info.flags = (resp_info->flags & ~AMSG_FLAGS_RESPONSE_REQUESTED) |
	    AMSG_FLAGS_RESPONSE;
	/* Send response message */
	err = amsg_send_internal(resp_info->endpoint, &info,
	    resp_info->sync_sock);
cleanup:
	*resp_info_ptr = NULL;
	amsg_response_cleanup(resp_info);
	if (resp_info->async_resp) {
		free(resp_info);
	}
	return err;
}

/*
 * Send a default response message.  This should be used to return an amsg_err
 * code when a response is expected, but there is no dedicated response
 * message to send.  A default response message will automatically be returned
 * when the message handler function returns, so this function should only
 * be used when when response timing is critical, and the response is
 * required either before or after the message handler returns.
 */
enum amsg_err amsg_send_default_resp(struct amsg_resp_info **resp_info_ptr,
	enum amsg_err err)
{
	struct amsg_resp_info *resp_info;
	struct amsg_msg_default_resp msg;

	ASSERT(resp_info_ptr != NULL);

	resp_info = *resp_info_ptr;
	if (!resp_info) {
		log_err("no response info");
		return AMSG_ERR_APPLICATION;
	}
	msg.interface = resp_info->interface;
	msg.type = resp_info->type;
	msg.err = err;
	log_debug("msg.interface %d, msg.type %d, msg.err %d",
	    msg.interface, msg.type, msg.err);

	return amsg_send_resp(resp_info_ptr,
	    AMSG_INTERFACE_INTERNAL, AMSG_TYPE_DEFAULT_RESP, &msg, sizeof(msg));
}

/*
 * Ping a messaging endpoint.  If an async_callback is used, the function
 * will return immediately and the callback will be invoked when the ping
 * response arrives.  If async_callback is NULL, the function will block
 * until the response is received.  An optional timeout may be specified.
 */
enum amsg_err amsg_ping(struct amsg_endpoint *endpoint, uint32_t timeout_ms,
	void (*async_callback)(enum amsg_err, uint32_t))
{
	enum amsg_err err;
	struct amsg_ping_async_info *async_info;

	/* Ping with asynchronous callback */
	if (async_callback) {
		async_info = (struct amsg_ping_async_info *)malloc(
		    sizeof(struct amsg_ping_async_info));
		if (!async_info) {
			log_err("malloc failed");
			return AMSG_ERR_MEM;
		}
		async_info->callback = async_callback;
		async_info->ping_time = time_mtime_ms();
		err = amsg_send(endpoint,
		    AMSG_INTERFACE_INTERNAL, AMSG_TYPE_PING, NULL, 0,
		    amsg_resp_handler_ping, async_info, timeout_ms);
		if (err != AMSG_ERR_NONE) {
			free(async_info);
		}
		return err;
	}
	/* Otherwise, ping and block until response is received */
	return amsg_send_sync(endpoint,
	    AMSG_INTERFACE_INTERNAL, AMSG_TYPE_PING, NULL, 0,
	    amsg_resp_handler_ping, NULL, timeout_ms);
}
