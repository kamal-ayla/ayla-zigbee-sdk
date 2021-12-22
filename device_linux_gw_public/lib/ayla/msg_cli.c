/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#define _GNU_SOURCE 1 /* for strndup */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/buffer.h>
#include <ayla/parse.h>
#include <ayla/amsg.h>
#include <ayla/msg_defs.h>
#include <ayla/msg_utils.h>
#include <ayla/msg_cli.h>


/*
 * Remote CLI command table state used by msg_cli_init().
 */
static struct {
	const struct msg_cli_cmd *cmd_table;
	size_t num_cmds;
} msg_cli;


static int msg_cli_exec(const struct msg_cli_cmd *cmd_table, size_t num_cmds,
	const char *cmd, size_t argc, char **argv, struct queue_buf *output)
{
	const struct msg_cli_cmd *c;

	/* Lookup the command handler */
	for (c = cmd_table; c < &cmd_table[num_cmds]; ++c) {
		if (!strcmp(c->name, cmd)) {
			if (!c->handler) {
				return -1;
			}
			/* Execute the command */
			return c->handler(cmd, argc, argv, output);
		}
	}
	return -1;
}

/*
 * Message interface handler for remote CLI messages.
 */
static enum amsg_err msg_cli_handler(
	struct amsg_endpoint *endpoint,
	const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	if (info->type != MSG_CLI_INPUT) {
		return AMSG_ERR_TYPE_UNSUPPORTED;
	}
	return msg_cli_input_handler(msg_cli.cmd_table, msg_cli.num_cmds,
	    info, resp_info);
}

/*
 * Registers the standard remote CLI message handler, and assigns the
 * command table to a global state structure.
 */
void msg_cli_init(const struct msg_cli_cmd *cmd_table, size_t num_cmds)
{
	msg_cli.cmd_table = cmd_table;
	msg_cli.num_cmds = num_cmds;
	amsg_set_interface_handler(MSG_INTERFACE_CLI, msg_cli_handler);
}

/*
 * Message handler for remote CLI input messages.  If a command handler is
 * in the table, it is invoked.  If the command produced any output data,
 * a CLI output message is returned.
 */
enum amsg_err msg_cli_input_handler(const struct msg_cli_cmd *cmd_table,
	size_t num_cmds, const struct amsg_msg_info *info,
	struct amsg_resp_info *resp_info)
{
	char *arg_str;
	size_t argc;
	char *argv[20];
	int rc;
	enum amsg_err err = AMSG_ERR_NONE;
	struct queue_buf out_buf;

	ASSERT(cmd_table != NULL);

	if (!info->payload || !info->payload_size) {
		return AMSG_ERR_DATA_CORRUPT;
	}
	arg_str = strndup(info->payload, info->payload_size);
	if (!arg_str) {
		return AMSG_ERR_MEM;
	}
	rc = parse_argv(argv, ARRAY_LEN(argv), arg_str);
	if (rc <= 0) {
		log_err("failed to parse args: %s", arg_str);
		err = AMSG_ERR_DATA_CORRUPT;
		goto error;
	}
	if (rc >= ARRAY_LEN(argv)) {
		log_err("cannot support more than %zu args",
		    ARRAY_LEN(argv) - 1);
		err = AMSG_ERR_APPLICATION;
		goto error;
	}
	argc = rc;
	queue_buf_init(&out_buf, 0, 1024);
	if (msg_cli_exec_subcmd(cmd_table, num_cmds, argc, argv,
	    &out_buf) < 0) {
		err = AMSG_ERR_APPLICATION;
	} else if (queue_buf_len(&out_buf) && resp_info) {
		/* Send command output as reply */
		queue_buf_put(&out_buf, "", 1);	/* Ensure null termination */
		err = msg_send_qbuf_resp(&resp_info,
		    MSG_INTERFACE_CLI, MSG_CLI_OUTPUT, &out_buf);
	}
	queue_buf_destroy(&out_buf);
error:
	free(arg_str);
	return err;
}

/*
 * Helper function to call inside a command handler to lookup and execute
 * a sub-command from a table of sub-commands.
 */
int msg_cli_exec_subcmd(const struct msg_cli_cmd *subcmd_table,
	size_t num_subcmds, size_t argc, char **argv, struct queue_buf *output)
{
	return msg_cli_exec(subcmd_table, num_subcmds, argv[0],
	    argc - 1, argv + 1, output);
}
