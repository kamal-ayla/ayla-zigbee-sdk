/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/http.h>
#include <ayla/log.h>

#include <ayla/server.h>

#define CRLF "\r\n"

int serv_local_connect(const char *path)
{
	struct sockaddr_un sa;
	int sock;
	size_t len;

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		log_err("socket failed %m");
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	len = strnlen(path, sizeof(sa.sun_path));
	REQUIRE(len < sizeof(sa.sun_path), REQUIRE_MSG_BUF_SIZE);
	if (len >= sizeof(sa.sun_path)) {
		return -1;
	}
	strncpy(sa.sun_path, path, len);

	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("connect to %s failed %m",
		    path);
		close(sock);
		return -1;
	}
	return sock;
}

static int serv_local_send_sock(const void *data, size_t len, void *arg)
{
	int sock = *((int *)arg);

	if (send(sock, data, len, 0) < 0) {
		return -1;
	}
#ifdef SERV_PROXY_DEBUG
	log_debug("send body: %zu bytes", len);
#endif
	return 0;
}

/*
 * Send an HTTP-like request over a Unix-domain (local) socket.
 * Synchronous.
 * Returns the status.
 */
int serv_local_req(struct server_req *req,
    enum server_method method, const char *url, const char *dest,
    void (*put_body)(struct server_req *req, char *buf, size_t len))
{
	int sock;
	char buf[2048];
	size_t len;
	int rc;
	long status = -1;
	char *body = NULL;
	char *errptr;
	char *cp;

	sock = serv_local_connect(dest);
	if (sock < 0) {
		return -1;
	}

	len = snprintf(buf, sizeof(buf),
	    "%s %s HTTP/1.0" CRLF
	    "Content-Length: %zu" CRLF
	    "Content-Type: %s" CRLF CRLF,
	    server_method_str[method], url, req->content_len,
	    req->body_is_json ?
	    HTTP_CONTENT_TYPE_JSON : HTTP_CONTENT_TYPE_TEXT_HTML);
	rc = send(sock, buf, len, 0);
	if (rc < 0) {
		log_err("send failed: %m");
		close(sock);
		return -1;
	}
#ifdef SERV_PROXY_DEBUG
	log_debug("send header\n%s", buf);
#endif
	queue_buf_walk(&req->request, serv_local_send_sock, &sock);

	len = 0;
	for (;;) {
		if (len >= sizeof(buf)) {
			log_err("head too big");
			status = HTTP_STATUS_INTERNAL_ERR;
		}
		rc = recv(sock, buf + len, sizeof(buf) - 1 - len, 0);
		if (rc == 0) {
#ifdef SERV_PROXY_DEBUG
			log_debug("recv EOF");
#endif
			break;
		}
		if (rc < 0) {
			log_err("recv failed: %m");
			continue;
		}
		len += rc;
		buf[len] = '\0';
#ifdef SERV_PROXY_DEBUG
		log_debug("got %s", buf + len);
#endif
		if (status < 0) {
			cp = strstr(buf, CRLF);
			if (!cp) {
				continue;
			}
			if (strncmp(buf, "HTTP/1", 6)) {
				status = HTTP_STATUS_INTERNAL_ERR;
				break;
			}
			cp = strstr(buf, " ");
			status = strtoul(cp + 1, &errptr, 10);
			if (*errptr != ' ' && *errptr != '\0') {
				log_warn("bad status. buf %s",
				    buf);
				status = HTTP_STATUS_INTERNAL_ERR;
				break;
			}
			log_debug("status %ld", status);
		}
		if (!body) {
			body = strstr(buf, CRLF CRLF);
			if (!body) {
				continue;
			}
			body += 4;
#ifdef SERV_PROXY_DEBUG
			log_debug("start body");
#endif
			len -= body - buf;
		}
		if (put_body) {
			put_body(req, body, len);
		}
		body = buf;
		len = 0;
	}
	close(sock);
	return status;
}

/*
 * Proxy an HTTP request to a local destination path, and return the response.
 */
void serv_proxy(struct server_req *req, enum server_method method,
    const char *dest)
{
	int status;

	status = serv_local_req(req, method, req->url, dest, req->put_body);
	if (status < 0) {
		status = HTTP_STATUS_INTERNAL_ERR;
	}
	server_put_end(req, status);
}

/*
 * Create a new local HTTP request using the local proxy mechanism.
 * To ignore the reply, pass in a NULL reply_buf pointer.
 * If a reply_buf pointer is specified and a response was received,
 * the response data will be malloc'd and reply_buf will be set to point to it.
 * If no response was received, reply_buf is set to NULL.  Be sure to free()
 * the reply_buf when done.
 */
int serv_local_client_req(enum server_method method, const char *url,
    const char *dest, const char *payload, char **reply_buf)
{
	struct server_req *req;
	size_t reply_len;
	int status;

	req = server_req_alloc();
	if (!req) {
		return HTTP_STATUS_INTERNAL_ERR;
	}
	req->url = strdup(url);
	if (payload) {
		req->content_len = strlen(payload);
		queue_buf_put(&req->request, payload, req->content_len);
	} else {
		req->content_len = 0;
	}

	if (reply_buf) {
		status = serv_local_req(req, method, req->url, dest,
		    req->put_body);
		reply_len = queue_buf_len(&req->reply);
		if (!reply_len) {
			*reply_buf = NULL;
			goto done;
		}
		*reply_buf = malloc(reply_len + 1);
		if (!*reply_buf) {
			log_err("malloc failed");
			goto done;
		}
		queue_buf_copyout(&req->reply, *reply_buf, reply_len, 0);
		(*reply_buf)[reply_len] = '\0';
	} else {
		status = serv_local_req(req, method, req->url, dest, NULL);
	}
done:
	server_req_close(req);
	return status;
}
