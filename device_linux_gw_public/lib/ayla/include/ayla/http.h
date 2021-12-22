/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_HTTP_H__
#define __AYLA_HTTP_H__

#include <ayla/utypes.h>
#include <ayla/token_table.h>

#define HTTP_ARGS	20	/* maximum tokens per line */
#define HTTP_MAX_TEXT	450	/* max text in any line */

struct http_state;

/*
 * Tag description table entry.
 */
struct http_tag  {
	const char *name;
	void (*parse)(int argc, char **argv, void *);
};

enum http_parse_state {
	HS_IDLE = 0,
	HS_INIT,	/* building "HTTP" or other string followed by blank */
	HS_TAG,		/* building tag (looking for :) */
	HS_TEXT_WS,	/* skipping white space before or inside text */
	HS_TEXT,	/* building body of tag */
	HS_CR,		/* saw CR */
	HS_CRLF,	/* saw CRLF */
	HS_CRLFCR,	/* found CRLF + CR */
	HS_DONE,	/* finished with header */
	HS_ERROR,	/* error encountered.  Ignore further input */
};

struct http_state {
	const struct http_tag *list;	/* tag list for handlers */
	u8	depth;		/* current stack depth */
	u8	argc;		/* number of argument pointers filled in */
	u16	bytes;		/* input bytes handled */
	u32	status;		/* HTTP status or Chunk Size */
	char	*textp;		/* pointer to next empty byte in text_buf */
	enum http_parse_state state;
	char	*argv[HTTP_ARGS];	/* argument pointers into text_buf */
	char	text_buf[HTTP_MAX_TEXT];
	u8	chunked;	/* bool for chunked-header parsing */
	u8	chunk_set;	/* bool for determining if chunk-size is set */
	void	*user_arg;	/* user-defined argument for parse funcs */
};

/*
 * Initialize HTTP parser state.
 */
void http_parse_init(struct http_state *, const struct http_tag *, void *arg);
void http_chunk_init(struct http_state *, const struct http_tag *, void *arg);

/*
 * Parse HTTP input string.
 * Match tags against the table provided, and call parse functions with
 * the values obtained.
 *
 * This may be called multiple times as buffers are received, and will
 * continue from where it left off.
 *
 * Returns < 0 on error.
 * Returns length consumed on success.  If less than size_t, the length
 * is the offset of the start of the body of the HTTP message.
 */
int http_parse(struct http_state *, void *buf, size_t);

/*
 * HTTP Protocol status values.
 */
#define HTTP_STATUS_CONTINUE			100
#define HTTP_STATUS_OK				200
#define HTTP_STATUS_CREATED			201
#define HTTP_STATUS_ACCEPTED			202
#define HTTP_STATUS_NO_CONTENT			204
#define HTTP_STATUS_PAR_CONTENT			206
#define HTTP_STATUS_REDIR_MIN			300
#define HTTP_STATUS_REDIR			302
#define HTTP_STATUS_REDIR_MAX			399
#define HTTP_STATUS_BAD_REQUEST			400
#define HTTP_STATUS_UNAUTH			401
#define HTTP_STATUS_FORBIDDEN			403
#define HTTP_STATUS_NOT_FOUND			404
#define HTTP_STATUS_METHOD_NOT_ALLOWED		405
#define HTTP_STATUS_NOT_ACCEPT			406
#define HTTP_STATUS_CONFLICT			409
#define HTTP_STATUS_PRE_FAIL			412
#define HTTP_STATUS_TOO_LARGE			413
#define HTTP_STATUS_UNPROCESSABLE_ENTITY	422
#define HTTP_STATUS_TOO_MANY_REQUESTS		429
#define HTTP_STATUS_INTERNAL_ERR		500
#define HTTP_STATUS_UNAVAIL			503
#define HTTP_STATUS_GATEWAY_TIMEOUT		504

/*
 * struct nameval array initializer for HTTP status messages.
 */
#define HTTP_STATUS_MSGS {				\
	{ "Continue",			100 },		\
	{ "OK",				200 },		\
	{ "Created",			201 },		\
	{ "Accepted",			202 },		\
	{ "No content",			204 },		\
	{ "Partial content",		206 },		\
	{ "Redirect",			302 },		\
	{ "Bad request",		400 },		\
	{ "Unauthorized",		401 },		\
	{ "Forbidden",			403 },		\
	{ "Not found",			404 },		\
	{ "Method not allowed",		405 },		\
	{ "Not acceptable",		406 },		\
	{ "Conflict",			409 },		\
	{ "Precondition failed",	412 },		\
	{ "Request entity too large",	413 },		\
	{ "Unprocessable entity",	422 },		\
	{ "Too many requests",		429 },		\
	{ "Internal server error",	500 },		\
	{ "Service unavailable",	503 },		\
	{ "Gateway timeout",		504 },		\
	{ NULL, -1 }					\
}

/*
 * HTTP status range macros.
 */
#define HTTP_STATUS_IS_INVALID(status)		\
	((status) < 100 || (status) >= 600)
#define HTTP_STATUS_IS_INFO(status)		\
	((status) >= 100 && (status) < 200)
#define HTTP_STATUS_IS_SUCCESS(status)		\
	((status) >= 200 && (status) < 300)
#define HTTP_STATUS_IS_REDIRECT(status)		\
	((status) >= 300 && (status) < 400)
#define HTTP_STATUS_IS_CLIENT_ERR(status)	\
	((status) >= 400 && (status) < 500)
#define HTTP_STATUS_IS_SERVER_ERR(status)	\
	((status) >= 500 && (status) < 600)

/*
 * Definitions of common HTTP methods.
 */
#define HTTP_METHODS(def)			\
	def(GET,	HTTP_GET)		\
	def(PUT,	HTTP_PUT)		\
	def(POST,	HTTP_POST)		\
	def(HEAD,	HTTP_HEAD)		\
	def(DELETE,	HTTP_DELETE)

DEF_ENUM(http_method, HTTP_METHODS);

extern const char * const http_method_names[];

/*
 * Definitions of some common content types.
 */
#define HTTP_CONTENT_TYPES(def)					\
	def(,				HTTP_CONTENT_UNKNOWN)	\
	def(text/html,			HTTP_CONTENT_HTML)	\
	def(application/json,		HTTP_CONTENT_JSON)	\
	def(application/octet-stream,	HTTP_CONTENT_BINARY)	\
	def(application/xml,		HTTP_CONTENT_XML)	\

DEF_ENUM(http_content_type, HTTP_CONTENT_TYPES);

extern const char * const http_content_type_names[];

#endif /* __AYLA_HTTP_H__ */
