/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_SOCKET_H__
#define __AYLA_SOCKET_H__

#define SOCKET_PATH_STR_LEN	70
#define SOCK_DIR_DEFAULT	"/var/run"
#define SOCKET_NAME		"sock"

int socket_bind(const char *path, int type, int dir_mode);
int socket_bind_and_listen(const char *path, int type, int dir_mode);
int socket_connect(const char *path, int type);
int socket_accept(int fd);

#endif /* __AYLA_SOCKET_H__ */
