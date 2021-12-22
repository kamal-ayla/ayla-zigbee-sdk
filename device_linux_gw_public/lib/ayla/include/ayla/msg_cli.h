/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef __AYLA_MSG_CLI_H__
#define __AYLA_MSG_CLI_H__

struct queue_buf;

/*
 * Remote CLI command handler entry.
 */
struct msg_cli_cmd {
	const char *name;
	int (*handler)(const char *, size_t, char **,
	    struct queue_buf *output);
};


/*
 * Registers the standard remote CLI message handler (below), and assigns the
 * command table to a global state structure.
 */
void msg_cli_init(const struct msg_cli_cmd *cmd_table, size_t num_cmds);

/*
 * Standard message handler for remote CLI input messages.
 * If a command handler is in the table, it is invoked.  If the
 * command produced any output data, a CLI output message is returned.
 * This CLI input message handler is registered when msg_cli_init() is used to
 * enable remote CLI functionality.  It could also be used on its own,
 * if the standard implementation is not desired.
 */
enum amsg_err msg_cli_input_handler(const struct msg_cli_cmd *cmd_table,
	size_t num_cmds, const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info);

/*
 * Helper function to call inside a command handler to lookup and execute
 * a sub-command from a table of sub-commands.
 */
int msg_cli_exec_subcmd(const struct msg_cli_cmd *subcmd_table,
	size_t num_subcmds, size_t argc, char **argv, struct queue_buf *output);


#endif /* __AYLA_MSG_CLI_H__ */
