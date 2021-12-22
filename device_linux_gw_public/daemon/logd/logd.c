/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
 #define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#include <jansson.h>
#include <curl/curl.h>

#include <syslog.h>

#include <ayla/log.h>
#include <ayla/utypes.h>
#include <ayla/http.h>
#include <ayla/filesystem_monitor.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/network_utils.h>

#include "logd.h"

char *strptime(const char *buf, const char *format, struct tm *tm);

/*
 * Log masks contain one bit for each defined log level.
 * Masks are 32 bits wide.
 */
#define LOG_AYLA_MASK(level)	BIT(level)

/*
 * Always log these levels, regardless of configuration.
 */
#define LOG_MASK_DEFAULT	(LOG_AYLA_MASK(LOG_AYLA_WARN) | \
				LOG_AYLA_MASK(LOG_AYLA_ERR))

#define LOG_DSN_SIZE_MAX	32	/* max DSN string buffer size */
#define LOG_URL_SIZE_MAX	200	/* arbitrary max URL size */
#define LOG_BUFFER_MSG_MAX	500	/* max number of messages to buffer */

/*
 * To address different methods of config and log file rotation,
 * monitor file change, delete, and move/rename operations.
 */
#define INOTIFY_ALL_CHANGES_MASK (IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF)

struct syslog_entry {
	time_t time;			/* message time */
	enum log_level level;		/* Ayla log level */
	enum log_subsystem subsystem;	/* subsystem */
	const char *tag;		/* process or system name */
	const char *message;		/* message text */
};

struct file_state {
	FILE *file;		/* log file */
	struct stat st;		/* file info */
};

struct log_client_state {
	uint8_t initialized;	/* flag indicating the client is initialized */
	CURL *curl;		/* handle to curl interface */
	CURLcode curl_err;	/* result of last curl operation */
	json_t *logs_json;	/* JSON array containing outgoing messages */
	struct file_state log_file; /* log file info as of latest read */
};

struct log_client_config {
	const char *factory_file;		/* devd factory config */
	const char *startup_dir;		/* devd startup config dir */
	const char *log_file;			/* syslog output */
	int log_enabled;			/* enable logging client */
	uint32_t log_all;			/* master log mask */
	uint32_t log_subsystem[LOG_SUB_NUM];	/* subsystem log masks */
	char log_url[LOG_URL_SIZE_MAX];		/* log server URL */
	char id_dsn[LOG_DSN_SIZE_MAX];		/* DSN */
};

static struct log_client_state state;
static struct log_client_config config;

const char *cmd_name;
int debug;
int foreground;


/*
 * Return a 32-bit mask for use in filtering log messages.
 */
static uint32_t logd_calc_log_mask(const struct syslog_entry *entry)
{
	if (entry->level != -1) {
		return LOG_AYLA_MASK(entry->level);
	}
	return 0;
}

/*
 * Return a statically allocated string indicating the log level.
 */
static const char *logd_log_level_name(const struct syslog_entry *entry)
{
	const char *name;

	name = log_get_level_name(entry->level);
	if (name) {
		return name;
	}
	return "undefined";
}

/*
 * Return a string indicating the log system or subsystem.
 */
static const char *logd_log_module_name(const struct syslog_entry *entry)
{
	const char *name;

	name = log_get_subsystem_name(entry->subsystem);
	if (name) {
		return name;
	}
	return entry->tag;
}

/*
 * Dump the content of a syslog message for debugging purposes.
 */
static void logd_print_entry(struct syslog_entry *entry)
{
	printf("syslog:\n\t"
	    "time: %s\t"
	    "level: %s\n\t"
	    "module: %s\n\t"
	    "tag: \"%s\"\n\t"
	    "msg: \"%s\"\n",
	    asctime(gmtime(&entry->time)),
	    logd_log_level_name(entry),
	    logd_log_module_name(entry),
	    entry->tag,
	    entry->message);
}

/*
 * Dump Curl state for debugging purposes.
 */
static void logd_print_curl_info()
{
	printf("\ncurl:\n\t");
	if (!state.curl) {
		printf("uninitialized\n");
	} else {
		const char *url = "";
		long resp = -1;
		double time = 0;
		double size = 0;
		double speed = 0;

		curl_easy_getinfo(state.curl, CURLINFO_EFFECTIVE_URL, &url);
		curl_easy_getinfo(state.curl, CURLINFO_RESPONSE_CODE, &resp);
		curl_easy_getinfo(state.curl, CURLINFO_TOTAL_TIME, &time);
		curl_easy_getinfo(state.curl, CURLINFO_SIZE_UPLOAD, &size);
		curl_easy_getinfo(state.curl, CURLINFO_SPEED_UPLOAD, &speed);

		printf("last status: %s\n\t"
		    "url: %s\n\t"
		    "response: %ld\n\t"
		    "total time: %.3f s\n\t"
		    "upload size: %.3f B\n\t"
		    "upload speed: %.3f Kbps\n",
		    curl_easy_strerror(state.curl_err),
		    url,
		    resp,
		    time,
		    size,
		    speed / 125);
	}
}

/*
 * Return the number of log messages buffered to send.
 */
static size_t logd_num_queued_msgs()
{
	return state.logs_json ? json_array_size(state.logs_json) : 0;
}

/*
 * Initialize log daemon message generation infrastructure.
 */
static int logd_msg_client_init(void)
{
	if (state.initialized) {
		return 0;
	}
	state.logs_json = json_array();
	state.curl_err = curl_global_init(CURL_GLOBAL_ALL);
	if (state.curl_err != CURLE_OK) {
		log_err("curl_global_init() failed: %s",
		    curl_easy_strerror(state.curl_err));
		/* global_init() will be invoked again by easy_init() */
	}
	state.initialized = 1;
	return 0;
}

/*
 * Cleanup all resources initialized by msg_client_init()
 */
static void logd_msg_client_cleanup(void)
{
	if (!state.initialized) {
		return;
	}
	if (state.curl) {
		curl_easy_cleanup(state.curl);
	}
	curl_global_cleanup();
	if (state.logs_json) {
		json_decref(state.logs_json);
	}
	if (state.log_file.file) {
		fclose(state.log_file.file);
	}
	memset(&state, 0, sizeof(state));
}

/*
 * Convert buffered log message data to an allocated string to prepare
 * to POST to the log server.
 */
static char *logd_generate_post_data(json_t *logs_json, size_t *len)
{
	char *post_data;

	json_t *msgs = json_object();
	json_object_set_new(msgs, "dsn", json_string(config.id_dsn));
	json_object_set(msgs, "logs", logs_json);

	if (debug) {
		json_dump_file(msgs, "/tmp/logd.dump", JSON_INDENT(3));
	}

	post_data = json_dumps(msgs, JSON_COMPACT);
	json_decref(msgs);
	if (!post_data) {
		log_err("failed to create JSON string");
		*len = 0;
		return NULL;
	}
	*len = strlen(post_data); /* omit null terminator */
	return post_data;
}

/*
 * Send buffered messages
 */
static int logd_curl_post_msgs(void)
{
	int rc = 0;
	char *post_data = NULL;
	size_t post_size;
	struct curl_slist *header = NULL;
	long http_response = -1;

	state.curl_err = CURLE_OK;

	/* don't send empty message */
	if (!logd_num_queued_msgs()) {
		goto done;
	}

	/* leave curl initialized after first POST */
	if (!state.curl) {
		state.curl = curl_easy_init();
		if (!state.curl) {
			log_err("failed to init curl");
			state.curl_err = CURLE_FAILED_INIT;
			rc = -1;
			goto done;
		}

		/* setup static options */
		curl_easy_setopt(state.curl, CURLOPT_LOW_SPEED_LIMIT, 100);
		curl_easy_setopt(state.curl, CURLOPT_LOW_SPEED_TIME, 30);
	}

	/* create JSON string for POST body */
	post_data = logd_generate_post_data(state.logs_json, &post_size);
	if (!post_data) {
		rc = -1;
		goto done;
	}

	/* configure fixed size POST */
	curl_easy_setopt(state.curl, CURLOPT_URL, config.log_url);
	curl_easy_setopt(state.curl, CURLOPT_POSTFIELDSIZE, post_size);
	curl_easy_setopt(state.curl, CURLOPT_POSTFIELDS, post_data);

	/* setup log header */
	header = curl_slist_append(header, "Content-Type: application/json");
	curl_easy_setopt(state.curl, CURLOPT_HTTPHEADER, header);

	if (debug) {
		curl_easy_setopt(state.curl, CURLOPT_VERBOSE, 1);
	}

	log_debug("Posting %zu messages to %s", logd_num_queued_msgs(),
	    config.log_url);

	/* send */
	state.curl_err = curl_easy_perform(state.curl);
	if (state.curl_err != CURLE_OK) {
		log_err("curl_easy_perform() failed: %s",
		    curl_easy_strerror(state.curl_err));
		rc = -1;
	} else {
		state.curl_err = curl_easy_getinfo(state.curl,
		    CURLINFO_RESPONSE_CODE, &http_response);
		if (state.curl_err != CURLE_OK) {
			log_err("curl_easy_getinfo() failed: %s",
			    curl_easy_strerror(state.curl_err));
			rc = -1;
		} else if (!HTTP_STATUS_IS_SUCCESS(http_response)) {
			log_err("Received %ld response from server",
			    http_response);
			rc = -1;
		} else {
			/* send successful: clear buffered message data */
			json_array_clear(state.logs_json);
		}
	}

	if (debug) {
		logd_print_curl_info();
	}

done:
	free(post_data);
	curl_slist_free_all(header);
	return rc;
}

/*
 * Determine if syslog message should be sent or ignored based on
 * configured log masks.  A default log mask is applied to guarantee
 * logging of WARN and ERR level messages regardless of configured levels.
 * Returns 1 to send, or 0 to ignore.
 */
static int logd_msg_filter(const struct syslog_entry *entry)
{
	uint32_t mask;
	int subsystem;

	mask = config.log_all | LOG_MASK_DEFAULT;

	/*
	 * If this was an Ayla log call, but a subsystem was not indicated,
	 * attempt to match the syslog tag to a known subsystem.
	 */
	if (entry->subsystem == -1) {
		subsystem = log_get_subsystem_val(entry->tag);
	} else {
		subsystem = entry->subsystem;
	}
	if (subsystem != -1) {
		mask |= config.log_subsystem[subsystem];
	}
	if (mask & logd_calc_log_mask(entry)) {
		return 1;
	}
	return 0;
}

/*
 * Push a syslog message into the send buffer.  May automatically
 * post the messages if the buffer's maximum capacity is reached.
 */
static int logd_msg_push(const struct syslog_entry *entry)
{
	json_t *msg;

	/* create log entry */
	msg = json_object();
	json_object_set_new(msg, "time", json_integer(entry->time));
	json_object_set_new(msg, "mod", json_string(
	    logd_log_module_name(entry)));
	json_object_set_new(msg, "level",
	    json_string(logd_log_level_name(entry)));
	json_object_set_new(msg, "text", json_string(entry->message));

	/* append to message array */
	json_array_append_new(state.logs_json, msg);

	/* immediately post if message capacity reached */
	if (logd_num_queued_msgs() > LOG_BUFFER_MSG_MAX) {
		if (logd_curl_post_msgs() < 0) {
			/* post failed, so make space for this entry */
			json_array_remove(state.logs_json, 0);
			log_warn("log buffer full, removing oldest entry");
		}
	}

	return 0;
}

/*
 * Parse log level from the middle of a string.
 */
static int logd_parse_level(const char *str, size_t len,
	enum log_level *level)
{
	int rc;
	char buf[len + 1];

	memcpy(buf, str, len);
	buf[len] = '\0';
	rc = log_get_level_val(buf);
	if (rc < 0) {
		return -1;
	}
	*level = rc;
	return 0;
}

/*
 * Parse log subsystem from the middle of a string.
 */
static int logd_parse_subsystem(const char *str, size_t len,
	enum log_subsystem *subsystem)
{
	int rc;
	char buf[len + 1];

	memcpy(buf, str, len);
	buf[len] = '\0';
	rc = log_get_subsystem_val(buf);
	if (rc < 0) {
		return -1;
	}
	*subsystem = rc;
	return 0;
}

/*
 * Fill out a syslog struct from the contents of a line of syslog data.
 * Format:
 *   month day hh:mm:ss hostname tag: [level-subsystem] message
 * NOTE1: Strings in syslog struct point to original string memory.
 * NOTE2: Original string will be modified by this function.
 */
static int logd_parse_line(char *line, struct syslog_entry *entry)
{
	const char *tag = NULL;
	const char *message = NULL;
	char *end;
	char *tok;
	char *cp;
	time_t rawtime;
	struct tm timeinfo;
	enum log_level level = -1;
	enum log_subsystem subsystem = -1;

	/* init time with current time to guestimate fields missing from log */
	time(&rawtime);
	timeinfo = *gmtime(&rawtime);

	/*
	 * Parse time
	 */
	end = strptime(line, "%b %d %T", &timeinfo);
	if (!end) {
		log_err("failed to parse time: \"%s\"", line);
		return -1;
	}
	tok = strchr(end, ':');
	if (!tok) {
		log_err("syslog tag missing");
		return -1;
	}
	tag = tok - 1;
	if (*tag == ' ') {
		log_err("syslog tag empty");
		return -1;
	}
	while (*(tag - 1) != ' ') {
		--tag;
		if (tag <= end) {
			log_err("failed to parse tag");
			return -1;
		}
	}
	*tok = '\0';	/* Insert null terminator for tag */
	tok += 2;	/* advance past the ": " */

	/*
	 * Parse custom message header:
	 * Format: "[L] " or "[L-S] "
	 * Where L is level, and S is an optional subsystem name.
	 */
	if (tok[0] != '[') {
		goto invalid_header;	/* no Ayla header found */
	}
	end = strchr(tok + 1, ']');
	if (!end || end[1] != ' ') {	/* end of header not found; ignore */
		goto invalid_header;
	}
	cp = strchr(tok + 1, '-');
	if (cp && cp < end) {
		if (logd_parse_subsystem(cp + 1, end - cp - 1,
		    &subsystem) < 0) {
			goto invalid_header;
		}
	} else {
		cp = end;
	}
	if (logd_parse_level(tok + 1, cp - tok - 1, &level) < 0) {
		goto invalid_header;
	}
	tok = end + 2;
	goto parse_message;
invalid_header:
	level = LOG_AYLA_UNKNOWN;	/* unable to determine syslog level */
	subsystem = LOG_SUB_SYSLOG;
parse_message:
	if (!tok || *tok == '\0') {
		log_err("failed parsing message");
		return -1;
	}
	end = strrchr(tok, '\n');
	if (end) {
		*end = '\0';	/* remove trailing newline, if found */
	}
	message = tok;

	entry->time = mktime(&timeinfo);
	entry->level = level;
	entry->subsystem = subsystem;
	entry->tag = tag;
	entry->message = message;

	return 0;
}

/*
 * Handle a log file event
 */
static void logd_log_event(const char *path, uint32_t flags, const char *name)
{
	struct file_state new_file;
	char *lineptr = NULL;
	size_t len = 0;
	ssize_t rc;
	struct syslog_entry log_entry;

	log_debug("path=%s event_flags=%08x", path, flags);

	new_file.file = fopen(path, "r");
	if (!new_file.file) {
		log_err("fopen %s failed %m", path);
		return;
	}

	if (fstat(fileno(new_file.file), &new_file.st) < 0) {
		log_err("stat %s failed %m", path);
		return;
	}

	/* read old/current log file */
	if (state.log_file.file) {
		/* need to clear EOF set at end of previous read */
		clearerr(state.log_file.file);

		while ((rc = getline(&lineptr, &len,
		    state.log_file.file)) != -1) {
			if (logd_parse_line(lineptr, &log_entry) != -1) {
				if (debug) {
					logd_print_entry(&log_entry);
				}
				if (logd_msg_filter(&log_entry)) {
					logd_msg_push(&log_entry);
				}
			}
		}

		/*
		 * Rotate log file if:
		 * 1. the log file has been renamed (new inode)
		 * 2. the log is the same file, but it has shrunk
		 *    (content erased)
		 */
		if (state.log_file.st.st_ino != new_file.st.st_ino ||
		    state.log_file.st.st_size > new_file.st.st_size) {
			log_debug("log file rotated: "
			    "fd=%d->%d inode=%u->%u "
			    "size=%u->%u offset=%u->%u",
			    fileno(state.log_file.file),
			    fileno(new_file.file),
			    (unsigned)state.log_file.st.st_ino,
			    (unsigned)new_file.st.st_ino,
			    (unsigned)state.log_file.st.st_size,
			    (unsigned)new_file.st.st_size,
			    (unsigned)lseek(fileno(state.log_file.file),
			    0, SEEK_CUR),
			    (unsigned)lseek(fileno(new_file.file),
			    0, SEEK_CUR));

			/* log file was rotated: close it */
			fclose(state.log_file.file);
		} else {
			log_debug("same log file: "
			    "fd=%d inode=%u old_size=%u bytes "
			    "offset=%u",
			    fileno(state.log_file.file),
			    (unsigned)state.log_file.st.st_ino,
			    (unsigned)state.log_file.st.st_size,
			    (unsigned)lseek(fileno(state.log_file.file),
			    0, SEEK_CUR));

			/* preserve existing file descriptor and offset */
			fclose(new_file.file);
			new_file.file = state.log_file.file;
			goto advance_file;
		}
	} else {
		/* start first read at end of current log file */
		fseek(new_file.file, 0, SEEK_END);
	}

	log_debug("reading new log file: fd=%d inode=%u size=%u offset=%u",
	    fileno(new_file.file),
	    (unsigned)new_file.st.st_ino,
	    (unsigned)new_file.st.st_size,
	    (unsigned)lseek(fileno(new_file.file),
	    0, SEEK_CUR));

	/* if first read or log was rotated, read the new file too */
	while ((rc = getline(&lineptr, &len, new_file.file)) != -1) {
		if (logd_parse_line(lineptr, &log_entry) != -1) {
			if (debug) {
				logd_print_entry(&log_entry);
			}
			if (logd_msg_filter(&log_entry)) {
				logd_msg_push(&log_entry);
			}
		}
	}

advance_file:
	/* cache file state */
	memcpy(&state.log_file, &new_file, sizeof(state.log_file));
	free(lineptr);

	/* send queued log messages */
	logd_curl_post_msgs();
}

/*
 * Handle log file event
 */
static void logd_config_event(const char *path,
	uint32_t flags,
	const char *name)
{
	log_debug("path=%s event_flags=%08x", path, flags);

	conf_load();
}

/*
 * Config "id" group load callback
 */
static int logd_conf_id_set(json_t *obj)
{
	int rc = 0;

	if (json_get_string_copy(obj, "dsn", config.id_dsn,
	    sizeof(config.id_dsn)) < 0) {
		log_err("missing config: dsn");
		rc = -1;
	}

	if (!rc) {
		log_debug("dsn=%s", config.id_dsn);
	}

	return rc;
}

/*
 * Config "log" group load callback
 */
static int logd_conf_log_set(json_t *obj)
{
	int rc = 0;
	bool enabled = false;
	size_t i;
	size_t j;
	json_t *mods;
	json_t *sub;
	json_t *elem;
	json_t *level;
	const char *protocol;
	const char *host;
	const char *uri;
	const char *str;
	int val;
	uint32_t *mask;
	uint32_t new_mask;

	if (json_get_bool(obj, "enabled", &enabled) < 0) {
		log_err("missing config: enabled");
		return -1;
	}
	/* no need to parse the rest of config if disabled */
	if (!enabled) {
		goto apply_config;
	}
	host = json_get_string(obj, "host");
	if (!host) {
		log_err("missing config: host");
		return -1;
	}
	uri = json_get_string(obj, "uri");
	if (!uri) {
		log_err("missing config: uri");
		return -1;
	}
	protocol = json_get_string(obj, "protocol");
	if (!protocol) {
		log_err("missing config: protocol");
		return -1;
	}
	snprintf(config.log_url, sizeof(config.log_url), "%s://%s/%s.json",
	    protocol, host, uri);
	log_debug("log_url=%s", config.log_url);

	mods = json_object_get(obj, "mods");
	if (!mods) {
		log_debug("no mods configured");
		config.log_all = 0;
		memset(config.log_subsystem, 0, sizeof(config.log_subsystem));
		goto apply_config;
	} else if (!json_is_array(mods)) {
		log_err("invalid array: mods");
		rc = -1;
		goto apply_config;
	}
	/* iterate through all subsystem entries */
	json_array_foreach(mods, i, sub) {
		elem = json_object_get(sub, "name");
		if (!elem || !json_is_string(elem)) {
			log_err("no mod name");
			continue;
		}
		str = json_string_value(elem);
		/* "all" has its own mask */
		if (!strcmp("all", str)) {
			mask = &config.log_all;
		} else {
			val = log_get_subsystem_val(str);
			mask = (val != -1) ? &config.log_subsystem[val] : NULL;
		}
		if (!mask) {
			log_err("unknown subsystem: %s", str);
			continue;
		}

		elem = json_object_get(sub, "levels");
		if (!elem || !json_is_array(elem)) {
			log_err("invalid array: levels");
			continue;
		}

		log_debug("updating subsystem: %s", str);

		new_mask = 0;
		/* iterate through all enabled levels */
		json_array_foreach(elem, j, level) {
			str = json_string_value(level);
			if (!str) {
				log_err("invalid level string");
				continue;
			}
			val = log_get_level_val(str);
			if (val == -1) {
				log_err("invalid log level: %s", str);
				continue;
			}
			/* supported Ayla level found */
			new_mask |= LOG_AYLA_MASK(val);
		}

		log_debug("mask: %08x -> %08x", *mask, new_mask);
		*mask = new_mask;
	}

apply_config:
	/* apply config change */
	if (!rc && enabled != config.log_enabled) {
		if (enabled) {
			log_info("logging enabled");

			/* setup daemon state and HTTP client */
			rc |= logd_msg_client_init();

			/* start monitoring the log file */
			rc |= fs_monitor_add_watcher(config.log_file,
			    logd_log_event, INOTIFY_ALL_CHANGES_MASK);

			/* start reading log file from current offset */
			logd_log_event(config.log_file, 0, NULL);
		} else {
			log_info("logging disabled");

			/* stop monitoring the log file */
			fs_monitor_del_watcher(config.log_file);

			/* cleanup data and HTTP client state */
			logd_msg_client_cleanup();
		}

		if (rc) {
			log_err("failed to enable logging");
		} else {
			config.log_enabled = enabled;
		}
	}
	return rc;
}

/*
 * Initialize config subsystem
 */
static int logd_conf_init(void)
{
	if (conf_init(config.factory_file, config.startup_dir) < 0) {
		return -1;
	}
	if (conf_register("log", logd_conf_log_set, NULL) < 0) {
		return -1;
	}
	if (conf_register("id", logd_conf_id_set, NULL) < 0) {
		return -1;
	}
	return 0;
}

/*
 * Cleanup everything
 */
static void logd_cleanup(void)
{
	/* stop watching files */
	fs_monitor_cleanup();

	/* cleanup config subsystem */
	conf_cleanup();

	/* send any queued messages */
	logd_curl_post_msgs();

	/* lights out */
	logd_msg_client_cleanup();
}

/*
 * Initialize everything
 */
static int logd_init(void)
{
	/* initialize file system monitoring framework */
	if (fs_monitor_init(0) < 0) {
		goto error;
	}

	/* register config handlers */
	if (logd_conf_init() < 0) {
		goto error;
	}

	/* read initial config state */
	if (conf_load() < 0) {
		goto error;
	}

	/* monitor for config file changes */
	if (fs_monitor_add_watcher(conf_startup_file_path(),
	    logd_config_event, INOTIFY_ALL_CHANGES_MASK) < 0) {
		goto error;
	}
	return 0;
error:
	logd_cleanup();
	return -1;
}

static void logd_usage(void)
{
	fprintf(stderr,
	    "Usage: %s [OPTIONS]\n"
	    "  OPTIONS:\n"
	    "    -c, --config <config file> (default is %s)\n"
	    "    -l, --log-file <log file> (default is %s)\n"
	    "    -d, --debug\n"
	    "    -f  --foreground"
	    "    -h, --help print this message\n",
	    cmd_name,
	    DEFAULT_CONFIG_FILE,
	    DEFAULT_LOG_FILE);
}

int main(int argc, char **argv)
{
	int rc;
	int long_index = 0;
	int opt;

	static struct option options[] = {
		{"factory_config",	required_argument,	NULL,	'c'},
		{"startup_dir",		required_argument,	NULL,	's'},
		{"log-file",		required_argument,	NULL,	'l'},
		{"debug",		no_argument,		NULL,	'd'},
		{"foreground",		no_argument,		NULL,	'f'},
		{"help",		no_argument,		NULL,	'h'},
		{NULL}
	};

	cmd_name = strrchr(argv[0], '/');
	if (cmd_name) {
		++cmd_name;
	} else {
		cmd_name = argv[0];
	}

	/* set defaults */
	config.factory_file = DEFAULT_CONFIG_FILE;
	config.log_file = DEFAULT_LOG_FILE;

	while ((opt = getopt_long(argc, argv, "c:s:l:df",
	    options, &long_index)) != -1) {
		switch (opt) {
		case 'c':
			config.factory_file = optarg;
			break;
		case 's':
			config.startup_dir = optarg;
			break;
		case 'l':
			config.log_file = optarg;
			break;
		case 'd':
			debug = 1;
			break;
		case 'f':
			foreground = 1;
			break;
		case ':':
			fprintf(stderr, "Missing arg for %s option\n",
			    options[long_index].name);
			logd_usage();
			exit(1);
			break;
		case '?':
			fprintf(stderr, "Unsupported option: %s\n",
			    argv[optind]);
			logd_usage();
			break;
		case 'h':
		default:
			logd_usage();
			exit(1);
			break;
		}
	}

	log_init(cmd_name, LOG_OPT_FUNC_NAMES);
	if (foreground) {
		log_set_options(LOG_OPT_CONSOLE_OUT);
	}
	if (debug) {
		log_set_options(LOG_OPT_DEBUG | LOG_OPT_TIMESTAMPS);
	}
	log_set_subsystem(LOG_SUB_LOGGER);

	if (!foreground) {
		log_info("daemonizing");
		if (daemon(0, 0) < 0) {
			log_err("daemon failed: %m");
			return 1;
		}
	}

	while (logd_init() < 0) {
		/* Retry initialization periodically in case of a failure */
		sleep(30);
	}

	/* Disable syslog output once initialization is complete */
	log_set_options(LOG_OPT_NO_SYSLOG);

	/* Run main loop (blocks) */
	rc = fs_monitor_task();

	log_clear_options(LOG_OPT_NO_SYSLOG);
	log_info("exiting");
	logd_cleanup();

	return rc ? 1 : 0;
}
