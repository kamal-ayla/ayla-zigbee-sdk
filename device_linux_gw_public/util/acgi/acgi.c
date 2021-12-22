/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdio.h>
#include <stdlib.h>
#include "fcgi_stdio.h"
#define _GNU_SOURCE		/* for strcasestr(3) */
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <errno.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/socket.h>
#include <ayla/server.h>
#include <ayla/log.h>
#include <ayla/network_utils.h>


#define CRLF "\r\n"

#define ACGI_SOCK_DEVD_SUBDIR "devd"

static char *cmdname;
static int req_is_local;

char serv_proxy_devd_path[SOCKET_PATH_STR_LEN];
char *strcasestr(const char *, const char *);	/* XXX */

static void acgi_status(int http_status, char *msg)
{
	printf("Status: %d %s" CRLF CRLF "%s\n", http_status, msg, msg);
}

static void acgi_proxy(const char *local_sock_path)
{
	int sock;
	char buf[2048 + 1];	/* buffer for request and response */
	char method[16];
	const char *uri;
	char *str;
	char *end;
	char *errptr;
	char *wbuf;
	unsigned long clen;
	size_t len;
	size_t slen;
	size_t tlen;
	int rc;
	int in_headers;
	int first_line = 1;

	/*
	 * Connect socket to devd or cond or other daemon.
	 */
	sock = serv_local_connect(local_sock_path);
	if (sock < 0) {
		acgi_status(HTTP_STATUS_UNAVAIL, "Unavailable");
		return;
	}
	str = getenv("REQUEST_METHOD");
	if (!str) {
		acgi_status(HTTP_STATUS_BAD_REQUEST, "Bad request");
		log_err("missing HTTP method");
		goto error;
	}
	snprintf(method, sizeof(method), "%s", str);
	clen = 0;
	str = getenv("CONTENT_LENGTH");
	if (str) {
		clen = strtoul(str, &errptr, 10);
		if (*errptr != 0) {
			clen = 0;
		}
	}
	uri = getenv("REQUEST_URI");
	len = snprintf(buf, sizeof(buf),
	    "%s %s HTTP/1.0" CRLF
	    "Content-Length: %lu" CRLF,
	    method, uri, clen);

	str = getenv("CONTENT_TYPE");
	if (str) {
		len += snprintf(buf + len, sizeof(buf) - len,
		    "Content-Type: %s" CRLF, str);
	}
	str = getenv("HTTP_ACCEPT");
	if (str) {
		len += snprintf(buf + len, sizeof(buf) - len,
		    "Accept: %s" CRLF, str);
	}
	len += snprintf(buf + len, sizeof(buf) - len, CRLF);

	/*
	 * Copy headers + stdin to local socket.
	 */
	for (slen = 0; slen < len + clen; slen += tlen, len = 0) {
		tlen = sizeof(buf) - len;
		if (tlen > clen) {
			tlen = clen;
		}
		if (tlen > 0) {
		//	rc = read(0, buf + len, tlen);
                        rc = FCGI_fread(buf + len, tlen, 1, stdin);
			rc = rc * tlen;

			if (rc <= 0) {
				tlen = 0;
			} else {
				tlen = rc;
			}
		}
		tlen += len;
		if (!tlen) {
			break;
		}
		rc = send(sock, buf, tlen, 0);
		if (rc < 0) {
			log_err("send error %m");
			acgi_status(HTTP_STATUS_INTERNAL_ERR,
			    "Internal server error");
			goto error;
		}
	}

	/*
	 * Read reply.
	 */
	in_headers = 1;
	clen = 0;
	slen = 0;		/* amount of header in current buffer */
	do {
		rc = recv(sock, buf, sizeof(buf) - 1, 0);
		if (rc <= 0) {
			log_err("sock read error: %m");
			break;
		}
		tlen = rc;
		buf[tlen] = '\0';
		wbuf = buf;

		/*
		 * Skip first "HTTP/1.0" line.
		 */
		if (first_line) {
			str = strstr(buf, CRLF);
			if (str) {
				first_line = 0;
				wbuf = str + 2;
			}
		}

		if (in_headers) {
			str = strcasestr(wbuf, CRLF "Content-Length:");
			if (str) {
				str = strchr(str, ':');
				if (str) {
					while (*++str == ' ') {
						;
					}
					end = strstr(str, CRLF);
					clen = strtoul(str, &errptr, 10);
					if (*errptr != ' ' && errptr != end) {
						clen = 0;
					}
				}
			}
			str = strstr(wbuf, CRLF CRLF);
			if (str) {
				in_headers = 0;
				slen = str + 4 - buf;
			}
		}
		//rc = write(1, wbuf, tlen - (wbuf - buf));
		rc = FCGI_fwrite(wbuf, tlen - (wbuf - buf), 1, stdout);

		if (rc < 0) {
			log_err("stdout write error: %m");
			break;
		}

		len += tlen - slen;
		slen = 0;

	} while (len < clen);
error:
	close(sock);
}

static int acgi_req(void)
{
	const char *remote_addr;
	struct sockaddr addr;

	if (req_is_local) {
		/* acgi request is originating from the gateway */
		/* no need to check the ip address */
		goto process_request;
	}
	remote_addr = getenv("REMOTE_ADDR");
	if (!remote_addr) {
		log_err("no remote address");
		acgi_status(HTTP_STATUS_INTERNAL_ERR,
		    "Internal server error");
		return 1;
	}

	/* only support IPv4 addresses.  Auto-select family to add IPv6 */
	addr.sa_family = AF_INET;
	if (inet_pton(addr.sa_family,
	    remote_addr,
	    net_get_addr_data(&addr)) != 1) {
		log_err("cannot parse remote address: %s",
		    remote_addr);
		acgi_status(HTTP_STATUS_INTERNAL_ERR,
		    "Internal server error");
			return 1;
	}

	if (!net_is_local_addr(&addr)) {
		log_warn("rejected request from non-local IP: %s",
		    remote_addr);
		acgi_status(HTTP_STATUS_FORBIDDEN, "Forbidden");
		return 2;
	}

process_request:
	acgi_proxy(serv_proxy_devd_path);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
	    "usage: %s [--sockdir <socket_dir>]\n"
	    "       %s [-o <socket_dir>]\n",
	    cmdname, cmdname);
	exit(1);
}

int main(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	char *socket_dir = SOCK_DIR_DEFAULT;
	const struct option options[] = {
		{ .name = "sockdir", .val = 'o', .has_arg = 1},
		{ .name = "local", .val = 'l'},
		{ .name = NULL }
	};
	int ret;


	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	optind = 0;
	while ((opt = getopt_long(argc, argv, "o:l",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'o':
			socket_dir = optarg;
			break;
		case 'l':
			req_is_local = 1;
			break;
		default:
			usage();
			break;
		}
	}

	snprintf(serv_proxy_devd_path, sizeof(serv_proxy_devd_path),
	    "%s/%s/%s", socket_dir, ACGI_SOCK_DEVD_SUBDIR, SOCKET_NAME);
	log_init(cmdname, LOG_OPT_FUNC_NAMES | LOG_OPT_DEBUG);
	log_set_subsystem(LOG_SUB_PROXY);
	while( FCGI_Accept() >= 0 )
	{
        	ret = acgi_req();
	}
	return ret;

}
