/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_SERVER_H__
#define __AYLA_SERVER_H__

#include <ayla/file_event.h>
#include <ayla/token_table.h>
#include <ayla/http.h>
#include <ayla/buffer.h>

#define SERVER_PORT 80

#define HTTP_CONTENT_TYPE_TEXT_HTML	"text/html"
#define HTTP_CONTENT_TYPE_JSON		"application/json"

#define SERVER_BUF_SIZE			2048

#define SERVER_METHODS(def)		\
	def(GET,	SM_GET)		\
	def(PUT,	SM_PUT)		\
	def(POST,	SM_POST)	\
	def(DELETE,	SM_DELETE)	\
	def(HEAD,	SM_HEAD)

DEF_ENUM(server_method, SERVER_METHODS);

extern const char * const server_method_str[];

struct server_req;

struct server_url_list {
	enum server_method method;		/* Request method */
	const char *url;			/* URI */
	void (*url_op)(struct server_req *);	/* Request handler */
	bool (*filter)(struct server_req *);	/* Access filter (Optional) */
};

struct server_req {
	int sock;			/* Server session socket */
	struct http_state http_state;	/* HTTP header parser state */

	enum server_method method;	/* Request method */
	char *url;			/* URI string (malloc'd) */
	char *args;			/* URL arguments */
	size_t content_len;		/* Content length from header */

	struct queue_buf request;	/* Request data buffer */
	struct queue_buf reply;		/* Server response data buffer */

	bool body_is_json;		/* Set if Content-Type is JSON */
	bool reply_is_json;		/* Set if server_put_json() used */
	json_t *body_json;

	void (*put_body)(struct server_req *, char *, size_t);	/* Reply func */
	void (*put_end)(struct server_req *, int status);	/* Close func */
	void (*complete)(struct server_req *);	/* Request complete callback */

	void *arg; /* Pointer to additional state used by custom handlers */
};


/*
 * Server interface
 */
int server_init(struct file_event_table *, const struct server_url_list *,
		int port);
int server_init_local(struct file_event_table *, const struct server_url_list *,
		const char *socket_path, int mode);

struct server_req *server_req_alloc(void);
void server_req_close(struct server_req *);
void server_handle_req(struct server_req *, const char *method);

void server_set_complete_callback(struct server_req *,
	void (*)(struct server_req *));

void server_put_json(struct server_req *, const json_t *);
void server_put(struct server_req *req, const char *fmt, ...);
		//__attribute__ ((format (printf, 2, 3)));
void server_put_end(struct server_req *req, int status);

/*
 * Server local proxy and client support
 */
void serv_proxy(struct server_req *req, enum server_method method,
    const char *dest);
int serv_local_connect(const char *path);
int serv_local_req(struct server_req *req,
    enum server_method method, const char *url, const char *dest,
    void (*put_body)(struct server_req *req, char *buf, size_t len));
int serv_local_client_req(enum server_method method, const char *url,
    const char *dest, const char *payload, char **reply_buf);

/*
 * Server request helper functions
 */
char *server_get_arg(struct server_req *, char **valp);
char *server_get_arg_len(struct server_req *, char **valp, size_t *lenp);
int server_get_long_arg_by_name(struct server_req *req, const char *name,
		 long *valp);
u8 server_get_bool_arg_by_name(struct server_req *req, const char *name);
ssize_t server_get_arg_by_name(struct server_req *req, const char *name,
				char *buf, size_t len);
/*
 * Given a set of url args, get the value of a name, uri decode it, and store it
 * into buf with max length len. Returns -1 on error or length of value on
 * success.
 */
ssize_t server_get_val_from_args(const char *arg, const char *name, char *buf,
				size_t len);

#endif /* __AYLA_SERVER_H__ */
