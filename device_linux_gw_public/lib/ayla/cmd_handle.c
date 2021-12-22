/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <string.h>
#include <stdio.h>

#include <ayla/utypes.h>
#include <ayla/parse.h>
#include <ayla/cmd.h>

int cmd_handle(const struct cmd_info *cmds, char *buf)
{
	char *argv[CMD_ARGV_LIMIT];
	int argc;

	argc = parse_argv(argv, CMD_ARGV_LIMIT, buf);
	if (argc <= 0 || argc >= CMD_ARGV_LIMIT - 1) {
		return -1;
	}
	return cmd_handle_argv(cmds, argv[0], argc, argv);
}

int cmd_handle_argv(const struct cmd_info *cmds, const char *cmdname,
			int argc, char **argv)
{
	const struct cmd_info *cmd;

	for (cmd = cmds; cmd->name; cmd++) {
		if (!strcmp(cmd->name, cmdname)) {
			break;
		}
	}
	return cmd->handler(argc, argv);
}
