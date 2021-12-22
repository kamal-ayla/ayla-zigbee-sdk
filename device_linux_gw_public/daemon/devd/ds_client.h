/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <ayla/token_table.h>
#include <ayla/http.h>
#include <ayla/http_client.h>
#include <ayla/buffer.h>

#ifndef _DS_CLIENT_H_
#define _DS_CLIENT_H_

/* Link buffer size */
#define DS_CLIENT_LINK_MAX_LEN		512


/*
 * Definitions of supported protocols.
 */
#define DS_CLIENT_PROTOS(def)			\
	def(https,	DS_PROTO_HTTPS)		\
	def(http,	DS_PROTO_HTTP)

DEF_ENUM(ds_client_proto, DS_CLIENT_PROTOS);

/*
 * Flexible handle to various types of buffered data.  These are used by the
 * client to manage request and response data.  The delete_on_cleanup flag
 * may be set to indicate that data should be automatically freed or cleared
 * at the end of the request.  The content field may be set to indicate what
 * the expected or actual data format is.
 */
struct ds_client_data {
	enum {
		DS_DATA_NONE,
		DS_DATA_BUF,
		DS_DATA_QBUF,
		DS_DATA_FILE
	} type;
	union {
		struct {
			void *ptr;
			size_t len;
			size_t capacity;
			bool mallocd;
		} buf;
		struct queue_buf *qbuf;
		FILE *file;
	};
	bool delete_on_cleanup;
	enum http_content_type content;
};

/*
 * State for an individual HTTP request.  This contains enough information to
 * resend the request, if needed.
 */
struct ds_client_req {
	enum http_method method;
	char link[DS_CLIENT_LINK_MAX_LEN];
	bool ayla_cloud;
	bool ayla_auth;
	struct ds_client_data req_data;
	struct ds_client_data resp_data;
	void (*complete)(enum http_client_err,
	    const struct http_client_req_info *, const struct ds_client_data *,
	    void *);
	void *complete_arg;
	u16 timeout_secs;
};

/*
 * Client state.
 */
struct ds_client {
	struct http_client_context *context;
	struct queue_buf resp_buf;
	struct ds_client_req req;
};

/*
 * Indexes for user-defined URL variables stored in
 * ds_client_req_info.url_vars.  Values may be assigned to these slots and
 * referred to in the URL string using the variable names $1, $2, etc.
 */
enum ds_client_url_var {
	DS_URL_VAR_1	= 0,
	DS_URL_VAR_2,
	DS_URL_VAR_3,
	DS_URL_VAR_4,
	DS_URL_VAR_5,

	DS_URL_VAR_COUNT
};

/*
 * Request information structure.  This structure is is initialized and passed
 * to ds_client_req_init() to initialize the client request.  Note that string
 * data will be copied to the request state, so there is no need to persist
 * it until the request is complete.  raw_url, host, uri, and uri_args are
 * strings that may be used to compose the full request URL.  These strings
 * support variable expansion for some common tokens, including $DSN, $DEV_KEY,
 * $ADS_HOST, and $<n> user-defined variables.
 */
struct ds_client_req_info {
	enum http_method method;	/* HTTP method */
	const char *raw_url;	/* Full URL. Host, uri, & proto are preferred */
	enum ds_client_proto proto;	/* Protocol. Defaults to HTTPS */
	const char *host;		/* Hostname */
	const char *uri;		/* Resource identifier */
	const char *uri_args;		/* URI arguments */
	const char *url_vars[DS_URL_VAR_COUNT];	/* User-defined vars: $1-$5 */
	bool non_ayla;			/* Do not use Ayla cloud auth */
	struct ds_client_data req_data;	/* Handle to request data */
	enum http_content_type resp_content;	/* Response content type */
	const char *resp_file_path;	/* Set to get response data in file */
	u16 timeout_secs;		/* Stall timeout in seconds */
};


/*
 * Return the size in bytes of the client data.
 */
size_t ds_client_data_size(const struct ds_client_data *data);

/*
 * Cleanup the client data.  The client data structure is merely a handle to
 * the actual data buffer, which is stored somewhere else in memory.  If the
 * delete_on_cleanup flag is set, this function will free the data.  Otherwise,
 * it will just clear the handle to the data, and leave the original data
 * intact.
 */
void ds_client_data_cleanup(struct ds_client_data *data);

/*
 * Initialize an empty client data structure.
 */
void ds_client_data_init(struct ds_client_data *data);

/*
 * Setup a client data structure to point to a fixed size buffer.  The
 * buffer will NOT automatically be deleted on cleanup.
 */
void ds_client_data_init_buf(struct ds_client_data *data, void *buf,
	size_t buf_size, size_t len, bool mallocd);

/*
 * Setup a client data structure to point to a queue buffer.  The buffer will
 * NOT automatically be reset on cleanup.
 */
void ds_client_data_init_qbuf(struct ds_client_data *data,
	struct queue_buf *qbuf);

 /*
  * Setup a client data structure to point to a file.  The file will NOT
  * automatically be deleted on cleanup.  Returns the FILE pointer on success,
  * or NULL on failure.
  */
FILE *ds_client_data_init_file(struct ds_client_data *data,
	const char *path, const char *mode);

/*
 * Setup a client data structure to point to a fixed size buffer containing
 * JSON encoded data.  The buffer WILL automatically be freed on cleanup.
 */
char *ds_client_data_init_json(struct ds_client_data *data,
	const json_t *obj);

/*
 * Parse the client data and return a JSON object.  Returns NULL on failure.
 */
json_t *ds_client_data_parse_json(const struct ds_client_data *data);

/*
 * Initialize a client request based on the setup in the request info structure.
 * The request structure should be zeroed out before calling this function.
 */
int ds_client_req_init(struct ds_client_req *req,
	const struct ds_client_req_info *info);

/*
 * Cleanup and reset a client request.  This is called automatically
 * after a request has completed.
 */
void ds_client_req_reset(struct ds_client_req *req);

/*
 * Initialize a client.  This a configures and extends an HTTP client
 * library context.
 */
int ds_client_init(struct http_client *http_client, struct ds_client *client,
	size_t resp_buf_init_size, const char *debug_label);

/*
 * Reset a client and free all resources.
 */
void ds_client_cleanup(struct ds_client *client);

/*
 * Returns true if a request is in progress.
 */
bool ds_client_busy(struct ds_client *client);

/*
 * Send a client request.  Node: currently, the client allocates memory for a
 * single request, so req must point to the client's own request state.
 */
int ds_client_send(struct ds_client *client, struct ds_client_req *req);

/*
 * Cancel a pending request (if there is one), and reset the client's request
 * state.
 */
void ds_client_reset(struct ds_client *client);

#endif	/* _DS_CLIENT_H_ */
