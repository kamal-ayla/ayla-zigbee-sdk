/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <time.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/cmd.h>
#include <ayla/conf_io.h>
#include <ayla/log.h>
#include <ayla/build.h>
#include <ayla/conf_rom.h>
#include <ayla/base64.h>
#include <ayla/crypto.h>
#include <platform/conf.h>

#include "gsa_conf.h"

#define PROGRAM "gw_setup_agent"

static const char version[] = PROGRAM " " BUILD_VERSION_LABEL;
static const char prompt[] = PROGRAM "-> ";
static const char *conf_file = CONF_FILE;

int debug;
static char *cmdname;

static json_t *rom_conf;

static const struct option options[] = {
	{ .val = 'c', .name = "config"},
	{ .val = 'd', .name = "debug"},
	{ .name = NULL }
};

static int cmd_help(int argc, char **argv);

static void usage(void)
{
	fprintf(stderr, "%s\n\n", version);
	fprintf(stderr,
	    "usage: %s [-d | --debug] [-c | --config <conf_file>]\n",
	    cmdname);
	exit(1);
}

/*
 * Override the standard console log function to simplify output formatting
 * */
static void gsa_log_console(const char *func,
	enum log_level level,
	enum log_subsystem subsystem,
	const char *fmt, va_list args)
{
	char fmt_buf[LOG_MAX_FMT_STR_SIZE];

	snprintf(fmt_buf, sizeof(fmt_buf), PROGRAM ": %s\n", fmt);
	vprintf(fmt_buf, args);
}

/*
 * Set the value of an existing JSON node, converting the string
 * valuestr into the data type of the node.
 * Returns 0 for success and -1 for failure.
 */
int gsa_json_set_valuestr(json_t *obj, const char *valuestr)
{
	int int_val;
	double dbl_val;
	char *endptr;
	switch (json_typeof(obj)) {
	case JSON_STRING:
		return json_string_set(obj, valuestr);
	case JSON_INTEGER:
		errno = 0;
		int_val = strtol(valuestr, &endptr, 10);
		if (endptr == valuestr || *endptr != '\0' ||
		    ((int_val == LONG_MAX || int_val == LONG_MIN) &&
		    errno == ERANGE)) {
			return -1;
		}
		return json_integer_set(obj, int_val);
	case JSON_REAL:
		errno = 0;
		dbl_val = strtod(valuestr, &endptr);
		if (endptr == valuestr || *endptr != '\0' ||
		    ((dbl_val == HUGE_VAL || dbl_val == -HUGE_VAL) &&
		    errno == ERANGE)) {
			return -1;
		}
		return json_real_set(obj, dbl_val);
	case JSON_TRUE:
	case JSON_FALSE:
		/* XXX: No json_boolean_set() provided.
		 * Do not implement boolean set since not needed. */
		return -1;
	break;
	default:
		return -1;
	}
}

/*
 * Convert the value of a JSON node to a string.
 * Returns 0 for success and -1 for failure.
 */
int gsa_json_get_valuestr(json_t *obj, char *valuebuf, size_t size)
{
	int rc;
	int int_val;
	double dbl_val;
	const char *val;
	switch (json_typeof(obj)) {
	case JSON_STRING:
		val = json_string_value(obj);
		if (!val) {
			return -1;
		}
		rc = snprintf(valuebuf, size, "%s", val);
		if (rc >= size) {
			return -1;
		}
		break;
	case JSON_INTEGER:
		int_val = json_integer_value(obj);
		rc = snprintf(valuebuf, size, "%d", int_val);
		if (rc >= size) {
			return -1;
		}
		break;
	case JSON_REAL:
		dbl_val = json_real_value(obj);
		rc = snprintf(valuebuf, size, "%lf", dbl_val);
		if (rc >= size) {
			return -1;
		}
		break;
	case JSON_TRUE:
	case JSON_FALSE:
		val = (obj == json_true()) ? "true" : "false";
		rc = snprintf(valuebuf, size, "%s", val);
		if (rc >= size) {
			return -1;
		}
		break;
	default:
		return -1;
	}
	return 0;
}

char *gsa_oem_key_generate(const char *secret,
    const char *oem,
    const char *oem_model,
    const char *pub_key)
{
	struct crypto_state rsa = { 0 };
	time_t t;
	char input_buf[1024];
	char rsa_buf[1024];
	size_t len;
	ssize_t rsa_len;
	char *output;

	time(&t);

	/* compose the raw data string */
	len = snprintf(input_buf, sizeof(input_buf), "%u %s %s %s\n",
	    (unsigned)t, secret, oem, oem_model);
	if (len >= sizeof(input_buf)) {
		log_err("%zu byte input buffer is too small",
		    sizeof(input_buf));
		return NULL;
	}

	/* encrypt the data using RSA */
	if (crypto_init_rsa(&rsa, RSA_KEY_PUBLIC, pub_key) < 0) {
		return NULL;
	}
	rsa_len = crypto_encrypt(&rsa, input_buf, strlen(input_buf),
	    rsa_buf, sizeof(rsa_buf));
	crypto_cleanup(&rsa);
	if (rsa_len <= 0) {
		log_err("oem key encryption failed");
		return NULL;
	}

	/* encode the OEM key in base64 */
	output = base64_encode(rsa_buf, rsa_len, NULL);
	if (!output) {
		log_err("base64 encoding failed");
		return NULL;
	}
	return output;
}

/*
 * Encrypt the OEM key string using the OEM code, model,
 * and the device's public key
 */
static char *gsa_oem_key_encrypt(const char *secret)
{
	json_t *obj;
	const char *oem;
	const char *oem_model;
	const char *pub_key;

	obj = conf_get("oem");
	if (!obj) {
		log_err("missing \"oem\" config");
		return NULL;
	}
	if (json_unpack(obj, "{s:s,s:s}", "oem", &oem, "model", &oem_model)) {
		log_err("invalid oem config");
		return NULL;
	}

	obj = conf_get("id");
	if (!obj) {
		log_err("missing \"id\" config");
		return NULL;
	}
	if (json_unpack(obj, "{s:s}", "rsa_pub_key", &pub_key)) {
		log_err("invalid id config");
		return NULL;
	}

	if (!oem || !oem_model || !pub_key) {
		log_err("missing required config");
		return NULL;
	}

	return gsa_oem_key_generate(secret, oem, oem_model, pub_key);
}

static void gsa_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;

	cmdname = strrchr(argv[0], '/');
	if (cmdname) {
		cmdname++;
	} else {
		cmdname = argv[0];
	}

	optind = 0;
	while ((opt = getopt_long(argc, argv, "c:d?",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'c':
			conf_file = optarg;
			break;
		case 'd':
			debug = 1;
			break;
		case '?':
		default:
			usage();
			break;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unused arguments\n", cmdname);
		usage();
	}
}

int gsa_load_rom()
{
	/* If DSN and key are stored in ROM, load them */
	return conf_rom_load_id();
}

/*
 * Similar to getline(3), but supports appending lines
 * to an existing allocated buffer.  The next line is inserted at
 * offset nchars from the beginning of *linep.  Returns the total
 * number of characters.
 */
static ssize_t gsa_readline(char **linep, size_t *lenp, size_t nchars)
{
	ssize_t rlen = 0;
	size_t len;
	char *line;

	/*
	 * if no offset, use existing line pointer, otherwise
	 * allocate a new temp buffer for the new line segment
	 */
	if (!nchars) {
		len = *lenp;
		line = *linep;
	} else {
		len = 0;
		line = NULL;
	}

	rlen = getline(&line, &len, stdin);
	if (rlen == EOF) {
		free(*linep);
		return rlen;
	}

	if (!nchars) {
		*lenp = len;
		*linep = line;
	} else {
		/* if existing buffer is too small, expand it */
		if (*lenp <= nchars + len) {
			*lenp = nchars + len + 1;
			*linep = realloc(*linep, *lenp);
		}
		strcpy((char *)*linep + nchars, line);
		free(line);
		rlen += nchars;
	}
	return rlen;
}

/*
 * Read lines using malloced buffer.
 * Caller may free or reuse buffer.
 * See manpage for getline(3).
 * This variant ignores newline characters when
 * there are open single or double quotes in the line.
 */
static ssize_t gsa_readlines(char **linep, size_t *lenp)
{
	ssize_t rlen = 0;
	size_t line_len = 0;
	char *cp;
	char delim = 0;

	/* get lines until any open delimiters are closed */
	do {
		rlen = gsa_readline(linep, lenp, line_len);
		if (rlen == EOF) {
			return rlen;
		}

		cp = (char *)*linep + line_len;
		while (*cp) {
			if (!delim) {
				if (*cp == '\'' || *cp == '"') {
					delim = *cp;
				}
			} else {
				if (*cp == delim) {
					delim = 0;
				}
			}
			++cp;
		}
		line_len = rlen;
	} while (delim);

	/* Remove trailing newline left by getline(). */
	cp = strrchr(*linep, '\n');
	if (cp) {
		*cp = '\0';
		rlen = cp - *linep;
	}
	return rlen;
}

static int cmd_set(int argc, char **argv)
{
	const char *path;
	const char *val;
	char *key = NULL;
	json_t *obj;

	if (argc != 3) {
		log_err("usage: set <path> <val>");
		return 1;
	}
	path = argv[1];
	val = argv[2];

	obj = conf_get(path);
	if (!obj) {
		log_err("%s does not exist", path);
		return 1;
	}

	/* special case to encrypt the key value */
	if (!strcmp(path, "oem/key")) {
		key = gsa_oem_key_encrypt(val);
		if (!key) {
			return 1;
		}
		val = key;
	}

	if (gsa_json_set_valuestr(obj, val)) {
		log_err("cannot set value: %s=%s", path, val);
		free(key);
		return 1;
	}
	free(key);
	return 0;
}

static int cmd_get(int argc, char **argv)
{
	char val_buf[1024];
	const char *path;
	json_t *obj;

	if (argc != 2) {
		log_err("usage: get <path>");
		return 1;
	}
	path = argv[1];

	obj = conf_get(path);
	if (!obj) {
		return 1;
	}

	if (gsa_json_get_valuestr(obj, val_buf, sizeof(val_buf))) {
		log_err("cannot get value: %s", path);
			return 1;
	}

	printf("%s\n", val_buf);
	return 0;
}

static int cmd_set_rom(int argc, char **argv)
{
	const char *path;
	const char *val;

	if (argc != 3) {
		log_err("usage: set_rom <path> <val>");
		return 1;
	}

	path = argv[1];
	val = argv[2];

	if (!rom_conf) {
		rom_conf = json_object();
	}
	/* cache changes in uboot_conf JSON object prior to 'save' call */
	return json_object_set_new(rom_conf, path, json_string(val));
}

static int cmd_get_rom(int argc, char **argv)
{
	int rc;
	const char *name;
	char val_buf[1024];

	if (argc != 2) {
		log_err("usage: get_rom <name>");
		return 1;
	}

	name = argv[1];

	/* using uboot environment variables as ROM */
	rc = platform_conf_read(name, val_buf, sizeof(val_buf));
	if (rc) {
		log_err("cannot get value: %s", name);
		return 1;
	}

	printf("%s\n", val_buf);
	return 0;
}

static int cmd_setup_mode(int argc, char **argv)
{
	char *val;
	int enable;
	int rc;

	if (argc != 2) {
		log_err("usage: setup_mode <enable|disable>");
		return 1;
	}
	val = argv[1];

	if (!strcmp(val, "enable")) {
		enable = 1;
	} else if (!strcmp(val, "disable")) {
		enable = 0;
	} else {
		log_err("invalid arg: \"%s\"", val);
		return 1;
	}
	rc = conf_set_new("sys/setup_mode", json_integer(enable));
	if (rc) {
		log_err("json_object_set failed");
		return 1;
	}
	return 0;
}

static int cmd_save(int argc, char **argv)
{
	int rc = 0;
	const char *path;
	json_t *obj;

	/* save ROM config to uboot environment variables */
	json_object_foreach(rom_conf, path, obj) {
		rc |= platform_conf_write(path, json_string_value(obj));
	}
	if (rc) {
		log_err("save to ROM failed");
	}

	/* update config cached in JSON with latest ROM values */
	gsa_load_rom();

	/* save config file */
	rc = conf_save();
	if (rc) {
		log_err("save failed");
	}

	return rc ? 1 : 0;
}

static int cmd_unknown(int argc, char **argv)
{
	log_err("Unknown command.");
	return 1;
}

static int cmd_quit(int argc, char **argv)
{
	exit(0);
}

static const struct cmd_info cmds[] = {
	{ "help", cmd_help, "show command list"},
	{ "save", cmd_save, "save configuration"},
	{ "set", cmd_set, "set config item"},
	{ "get", cmd_get, "display item from config" },
	{ "set_rom", cmd_set_rom, "set variable in ROM" },
	{ "get_rom", cmd_get_rom, "display variable from ROM" },
	{ "setup_mode", cmd_setup_mode, "configure setup mode"},
	{ "quit", cmd_quit, "quit"},
	{ NULL, cmd_unknown, NULL}
};

static int cmd_help(int argc, char **argv)
{
	const struct cmd_info *cmd;

	printf("\n");
	for (cmd = cmds; cmd->name; cmd++) {
		if (cmd->help) {
			printf("%-10s %s\n", cmd->name, cmd->help);
		}
	}
	printf("\n");
	return 0;
}

int main(int argc, char **argv)
{
	char *line = NULL;
	size_t len = 0;
	ssize_t rlen;
	int rc;
	char *cp;
	char c;

	printf("%s\n", version);

	gsa_opts(argc, argv);

	log_init(cmdname,
	    LOG_OPT_DEBUG |
	    LOG_OPT_CONSOLE_OUT |
	    LOG_OPT_NO_SYSLOG);
	log_set_console_func(gsa_log_console);

	rc = conf_init(conf_file, NULL);
	if (rc) {
		log_err("config init failed");
		return 1;
	}
	/* Enable direct access to factory config */
	conf_factory_edit_mode_enable();
	rc = conf_load();
	if (rc) {
		log_err("config load failed");
		return 1;
	}
	/* Load device ID from ROM, if supported by platform */
	gsa_load_rom();
	/* Ensure factory flag is set */
	conf_set_new("sys/factory", json_integer(1));

	for (;;) {
		printf("%s", prompt);
		rlen = gsa_readlines(&line, &len);
		if (rlen == EOF) {
			rc = 0;
			break;
		}

		/*
		 * Trim spaces from beginning of line and ignore empty lines.
		 */
		for (cp = line; (c = *cp) != '\0'; cp++) {
			if (!isblank(c)) {
				break;
			}
		}
		if (c == '\0') {
			continue;
		}

		/*
		 * lookup command and call handler.
		 */
		rc = cmd_handle(cmds, cp);
		if (rc) {
			log_err("error: command err %d", rc);
		}
	}
	free(line);
	conf_cleanup();
	printf("\n");
	return rc;
}
