/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#define _GNU_SOURCE 1 /* for strndup */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/nameval.h>
#include <ayla/http.h>
#include <ayla/json_parser.h>
#include <ayla/uri_code.h>
#include <ayla/log.h>

#include <ayla/server.h>
#include <ayla/socket.h>

#define CRLF "\r\n"

#define HTTP_HEAD_MAX_LEN		1024

/* set SERVER subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_SERVER, __VA_ARGS__)

struct server_state {
	int sock_listen;
	bool debug;
	struct file_event_table *file_events;
	const struct server_url_list *url_table;
};
static struct server_state server_state;

DEF_NAME_TABLE(server_method_str, SERVER_METHODS);

static const struct name_val server_status_msgs[] = HTTP_STATUS_MSGS;

static void server_recv(void *arg, int sock);

void server_req_close(struct server_req *req)
{
	if (!req) {
		return;
	}
	if (req->complete) {
		req->complete(req);
	}
	queue_buf_destroy(&req->request);
	queue_buf_destroy(&req->reply);
	json_decref(req->body_json);
	free(req->url);
	free(req);
}

static void server_put_body_default(struct server_req *req, char *buf,
	size_t len)
{
	if (req->method != SM_HEAD) {
		queue_buf_put(&req->reply, buf, len);
	}
}

static int server_send_sock(const void *data, size_t len, void *arg)
{
	struct server_req *req = (struct server_req *)arg;

	if (send(req->sock, data, len, 0) < 0) {
		log_err("send failed: %m");
		return -1;
	}
	if (server_state.debug) {
		log_debug("reply body: %zu bytes", len);
	}
	return 0;
}

static void server_put_end_default(struct server_req *req, int status)
{
	char header[HTTP_HEAD_MAX_LEN];
	const char *status_msg;
	const char *sep;
	const char *content_type;
	int len;
	int rc;

	status_msg = lookup_by_val(server_status_msgs, status);
	sep = " ";
	if (!status_msg) {
		status_msg = "";
		sep = "";
	}

	if (server_state.debug) {
		log_debug("reply status: %d%s%s", status, sep, status_msg);
	}

	if (req->reply_is_json) {
		content_type = HTTP_CONTENT_TYPE_JSON;
	} else {
		content_type = HTTP_CONTENT_TYPE_TEXT_HTML;
	}

	len = snprintf(header, sizeof(header),
	    "HTTP/1.0 %d%s%s" CRLF
	    "Status: %d%s%s" CRLF
	    "Content-Type: %s; charset=UTF-8" CRLF
	    "Content-Length: %zu" CRLF CRLF,
	    status, sep, status_msg,
	    status, sep, status_msg,
	    content_type,
	    queue_buf_len(&req->reply));

	rc = send(req->sock, header, len, 0);
	if (rc < 0) {
		log_err("send header failed %m");
		goto error;
	}
	queue_buf_walk(&req->reply, server_send_sock, req);
error:
	close(req->sock);
	server_req_close(req);
}

void server_put(struct server_req *req, const char *fmt, ...)
{
	va_list args;
	char buf[2048];
	size_t len;

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (len >= sizeof(buf)) {
		log_warn("put too large: truncating %zu bytes",
		    len - sizeof(buf) + 1);
		len = sizeof(buf) - 1;
	}
	req->put_body(req, buf, len);
}

struct server_req *server_req_alloc(void)
{
	struct server_req *req;

	req = calloc(1, sizeof(*req));
	if (!req) {
		log_warn("malloc failed");
		return NULL;
	}
	queue_buf_init(&req->request, 0, SERVER_BUF_SIZE);
	queue_buf_init(&req->reply, 0, SERVER_BUF_SIZE);
	req->put_body = server_put_body_default;
	req->put_end = server_put_end_default;
	return req;
}

void server_put_end(struct server_req *req, int status)
{
	req->put_end(req, status);
}

void server_put_json(struct server_req *req, const json_t *root)
{
	struct server_state *server = &server_state;

	if (server->debug) {
		fprintf(stderr, "server_put_json:\n");
		json_dumpf(root, stderr, JSON_INDENT(4) | JSON_SORT_KEYS);
		fprintf(stderr, "\n");
	}
	/* Only one JSON object can be in buffer */
	if (queue_buf_len(&req->reply)) {
		log_warn("overwriting previous response");
		queue_buf_reset(&req->reply);
	}
	if (queue_buf_put_json(&req->reply, root) < 0) {
		log_err("buffer put failed");
		return;
	}
	req->reply_is_json = true;
}

static void server_json_not_found(struct server_req *req)
{
	server_put_end(req, HTTP_STATUS_NOT_FOUND);
}

void server_handle_req(struct server_req *req, const char *method)
{
	struct server_state *server = &server_state;
	const struct server_url_list *tp;
	void (*handler)(struct server_req *);
	char *cp;

	if (server->debug) {
		log_debug("%s %s", method, req->url);
	}
	cp = strchr(req->url, '?');
	if (cp) {
		*cp++ = '\0';
		req->args = cp;
	} else {
		req->args = NULL;
	}
	if (!method || !strcmp(method, "GET")) {
		req->method = SM_GET;
	} else if (!strcmp(method, "DELETE")) {
		req->method = SM_DELETE;
	} else if (!strcmp(method, "HEAD")) {
		req->method = SM_HEAD;
	} else if (!strcmp(method, "POST")) {
		req->method = SM_POST;
	} else if (!strcmp(method, "PUT")) {
		req->method = SM_PUT;
	} else {
		log_warn("unsupported method %s", method);
		server_put_end(req, HTTP_STATUS_METHOD_NOT_ALLOWED);
		return;
	}

	handler = server_json_not_found;
	for (tp = server->url_table; tp->url != NULL; tp++) {
		if (req->method == tp->method && !strcmp(req->url, tp->url)) {
			if (tp->filter && !tp->filter(req)) {
				log_warn("request denied: %s", tp->url);
				break;
			}
			handler = tp->url_op;
			break;
		}
	}
	handler(req);
}

void server_set_complete_callback(struct server_req *req,
	void (*callback)(struct server_req *))
{
	req->complete = callback;
}

static void server_recv(void *arg, int sock)
{
	struct server_state *server = &server_state;
	struct server_req *req = arg;
	char buf[1024];
	char method[16];
	size_t off;
	ssize_t len;
	char *cp, *cp2;
	int err_status = HTTP_STATUS_BAD_REQUEST;

	while ((len = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
		if (len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* All data received on non-blocking socket */
				return;
			}
			err_status = HTTP_STATUS_INTERNAL_ERR;
			log_err("recv failed %m");
			goto cancel_recv;
		}
		if (len == 0) {
			log_err("end of file");
			goto cancel_recv;
		}
		if (server->debug) {
			buf[len] = '\0';
			log_debug("%s", buf);
		}
		off = queue_buf_len(&req->request);
		queue_buf_put(&req->request, buf, len);
		if (req->http_state.state != HS_DONE) {
			len = http_parse(&req->http_state, buf, len);
			if (len < 0) {
				log_err("failed to parse HTTP header");
				goto cancel_recv;
			}
			/* Process header when fully received */
			if (req->http_state.state == HS_DONE) {
				cp = (char *)queue_buf_coalesce(&req->request);
				/* Find method and URI */
				cp2 = strchr(cp, ' ');
				if (!cp2) {
					log_err("cannot parse method");
					err_status =
					    HTTP_STATUS_METHOD_NOT_ALLOWED;
					goto cancel_recv;
				}
				if (cp2 - cp > sizeof(method) - 1) {
					log_err("method too long");
					err_status =
					    HTTP_STATUS_METHOD_NOT_ALLOWED;
					goto cancel_recv;
				}
				memcpy(method, cp, cp2 - cp);
				method[cp2 - cp] = '\0';
				cp = cp2 + 1;
				if (*cp == '/') {
					cp++;
				}
				cp2 = strchr(cp, ' ');
				if (!cp2) {
					log_err("cannot parse URI");
					goto cancel_recv;
				}
				req->url = strndup(cp, cp2 - cp);
				/* Trim header data from request buffer */
				queue_buf_trim_head(&req->request,
				    queue_buf_len(&req->request) - off - len);
			}
		}
	}
	/* Return if body has not been completely received */
	if (req->http_state.state != HS_DONE ||
	    queue_buf_len(&req->request) < req->content_len) {
		return;
	}
	if (queue_buf_len(&req->request) != req->content_len) {
		log_warn("content length is %zu, received %zu bytes",
		    req->content_len, queue_buf_len(&req->request));
	}
	/* Parse body, if JSON */
	if (req->body_is_json && queue_buf_len(&req->request)) {
		req->body_json = queue_buf_parse_json(&req->request, 0);
	}
	/* Done receiving */
	file_event_unreg(server->file_events, sock, server_recv, NULL, req);
	server_handle_req(req, method);
	return;

cancel_recv:
	file_event_unreg(server->file_events, sock, server_recv, NULL, req);
	server_put_end(req, err_status);
}

static void server_parse_len(int argc, char **argv, void *arg)
{
	struct server_req *req = (struct server_req *)arg;
	char *errptr;
	unsigned long len;

	if (argc > 0) {
		len = strtoul(argv[0], &errptr, 10);
		if (*errptr == '\0') {
			req->content_len = len;
		}
	}
}

static void server_parse_content_type(int argc, char **argv, void *arg)
{
	struct server_req *req = (struct server_req *)arg;

	if (argc > 0) {
		if (!strncmp(argv[0], HTTP_CONTENT_TYPE_JSON,
		    strlen(HTTP_CONTENT_TYPE_JSON))) {
			req->body_is_json = true;
		}
	}
}

static struct http_tag server_http_tags[] = {
	{ "Content-Length", server_parse_len },
	{ "Content-Type", server_parse_content_type },
	{ NULL }
};

static void server_accept(void *arg, int listen_sock)
{
	struct server_state *server = &server_state;
	struct server_req *req;
	int sock;

	sock = accept(listen_sock, NULL, NULL);
	if (sock < 0) {
		log_err("accept failed %m");
		return;
	}
	if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
		log_err("fcntl failed %m");
		close(sock);
		return;
	}
	req = server_req_alloc();
	if (!req) {
		close(sock);
		return;
	}

	req->sock = sock;
	http_parse_init(&req->http_state, server_http_tags, req);

	if (file_event_reg(server->file_events, sock, server_recv, NULL,
	    req) < 0) {
		log_err("file_event_reg failed");
		close(sock);
		server_req_close(req);
	}
}

static int server_listen(int sock)
{
	struct server_state *server = &server_state;
	int rc;

	rc = fcntl(sock, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		log_err("fcntl failed %m");
		close(sock);
		return -1;
	}
	if (listen(sock, 3) < 0) {
		log_err("listen failed %m");
		close(sock);
		return -1;
	}

	server->sock_listen = sock;
	rc = file_event_reg(server->file_events, sock,
	    server_accept, NULL, server);
	return rc;
}

static int server_open_port(int port)
{
	struct sockaddr_in sa;
	int sock;
	int opt;

	sock = socket(AF_INET, SOCK_STREAM, 0);

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("port %d bind failed %m", port);
		return -1;
	}
	return server_listen(sock);
}

int server_init(struct file_event_table *file_events,
			const struct server_url_list *table, int port)
{
	struct server_state *server = &server_state;

	server->debug = log_debug_enabled();
	server->url_table = table;
	server->file_events = file_events;

	return server_open_port(port);
}

int server_init_local(struct file_event_table *file_events,
			const struct server_url_list *table, const char *path,
			int mode)
{
	struct server_state *server = &server_state;
	struct sigaction act;
	int sock;
	int rc;

	/*
	 * Ignore SIGPIPE, so that client close doesn't kill us.
	 */
	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_IGN;
	rc = sigaction(SIGPIPE, &act, NULL);
	if (rc < 0) {
		log_err("sigaction failed: %m");
		return -1;
	}

	server->debug = log_debug_enabled();
	server->url_table = table;
	server->file_events = file_events;

	sock = socket_bind(path, SOCK_STREAM, mode);
	if (sock < 0) {
		return sock;
	}
	return server_listen(sock);
}

/*
 * Get next argument in request.
 * Caller must not use *valp or return string after
 * calling server_put(), server_banner(), or anything else
 * that uses req->buf.
 */
char *server_get_arg(struct server_req *req, char **valp)
{
	return server_get_arg_len(req, valp, NULL);
}

char *server_get_arg_len(struct server_req *req, char **valp, size_t *lenp)
{
	char *arg;
	char *cp;
	ssize_t len;

	arg = req->args;
	if (arg) {
		cp = strchr(arg, '&');
		if (cp) {
			*cp++ = '\0';
		}
		req->args = cp;
		cp = strchr(arg, '=');
		len = 0;
		if (cp) {
			*cp++ = '\0';
			len = strlen(cp);

			/*
			 * Do URI-decode in place.
			 */
			len = uri_decode(cp, len, cp);
			if (len < 0) {
				len = 0;
				arg = NULL;
				cp = NULL;
			}
		}
		*valp = cp;
		if (lenp) {
			*lenp = len;
		}
	}
	return arg;
}

/*
 * Given a set of url args, get the value of a name, uri decode it, and store it
 * into buf with max length len. Returns -1 on error or length of value on
 * success.
 */
ssize_t server_get_val_from_args(const char *arg, const char *name, char *buf,
				size_t len)
{
	const char *val;
	const char *next;
	const char *endp;
	size_t name_len = strlen(name);
	size_t vlen;
	ssize_t rc;

	if (!arg || !buf || !len) {
		return -1;
	}
	for (endp = arg + strlen(arg); arg < endp; arg = next) {
		val = strchr(arg, '=');
		if (!val) {
			break;
		}
		val++;
		next = strchr(arg, '&');
		if (next) {
			vlen = next - val;
			next++;
		} else {
			vlen = endp - val;
			next = endp;
		}
		if (val >= next || arg + name_len + 1 != val) {
			continue;
		}
		if (strncmp(arg, name, name_len)) {
			continue;
		}
		rc = uri_decode_n(buf, len, val, vlen);
		if (rc < 0 || rc >= len) {
			break;
		}
		return rc;
	}
	return -1;
}

/*
 * Get value of named arg from server request to the supplied buffer.
 * Return the length of the arg (not including null termination) or -1 if not
 * found.
 */
ssize_t server_get_arg_by_name(struct server_req *req, const char *name,
				char *buf, size_t len)
{
	return server_get_val_from_args(req->args, name, buf, len);
}

/*
 * Get long argument with default of 0.
 * Returns non-zero on error.  Fills in *valp with value, or 0 on error.
 */
int server_get_long_arg_by_name(struct server_req *req, const char *name,
		 long *valp)
{
	char buf[20];
	char *errptr;
	long val;

	*valp = 0;
	if (server_get_arg_by_name(req, name, buf, sizeof(buf)) < 0) {
		return -1;
	}
	val = strtoul(buf, &errptr, 0);
	if (*errptr != '\0') {
		return -1;
	}
	*valp = val;
	return 0;
}

/*
 * Get boolean argument with default of 0.
 * Ignores errors.  Missing or improper URL query strings are ignored.
 */
u8 server_get_bool_arg_by_name(struct server_req *req, const char *name)
{
	long val;

	return !server_get_long_arg_by_name(req, name, &val) && val;
}

