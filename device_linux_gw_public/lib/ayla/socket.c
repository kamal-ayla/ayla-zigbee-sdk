/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/file_io.h>


/*
 * Make directory to hold the path.
 * Note that the last component of the path is intended to be a normal file,
 * so make the directory only for the "dirname" of the path, if not empty.
 * The directory may already exist.  Rely on umask.
 */
static int socket_mkdir(const char *path, int mode)
{
	size_t len = strlen(path);
	char dir[len + 1];

	file_get_dir(path, dir, sizeof(dir));
	if (file_create_dir(dir, mode) < 0) {
		log_err("failed to create socket directory %s: %m", dir);
		return -1;
	}
	return 0;
}

static void socket_fill_sa(const char *path, struct sockaddr_un *sa)
{
	size_t len;

	len = strnlen(path, sizeof(sa->sun_path));
	REQUIRE(len < sizeof(sa->sun_path), REQUIRE_MSG_BUF_SIZE);
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	strncpy(sa->sun_path, path, len);
}

int socket_bind(const char *path, int type, int dir_mode)
{
	struct sockaddr_un sa;
	int sock;
	int yes = 1;

	sock = socket(PF_UNIX, type, 0);
	if (sock < 0) {
		log_err("socket failed %m");
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes,
	    sizeof(yes)) == -1) {
		log_err("setsockopt on %s failed: %m", path);
		return -1;
	}
	unlink(path);
	socket_mkdir(path, dir_mode);
	socket_fill_sa(path, &sa);
	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("bind to %s failed: %m", path);
		close(sock);
		return -1;
	}
	return sock;
}

int socket_bind_and_listen(const char *path, int type, int dir_mode)
{
	int sock;

	sock = socket_bind(path, type, dir_mode);
	if (sock < 0) {
		return -1;
	}
	if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
		log_err("fcntl on %s failed: %m", path);
		close(sock);
		return -1;
	}
	if (listen(sock, 1) < 0) {
		log_err("listen on %s failed: %m", path);
		close(sock);
		return -1;
	}
	return sock;
}

/*
 * Connect to socket
 */
int socket_connect(const char *path, int type)
{
	struct sockaddr_un sa;
	int sock;

	sock = socket(PF_UNIX, type, 0);
	if (sock < 0) {
		log_err("socket failed %m");
		return -1;
	}
	socket_fill_sa(path, &sa);
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("connect to %s failed: %m", path);
		close(sock);
		return -1;
	}
	return sock;
}

/*
 * Accept function for sockets
 */
int socket_accept(int fd)
{
	int sock;

	sock = accept(fd, NULL, NULL);
	if (sock < 0) {
		log_err("accept failed: %m");
		return -1;
	}

	if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
		log_err("fcntl failed: %m");
		close(sock);
		return -1;
	}
	return sock;
}
