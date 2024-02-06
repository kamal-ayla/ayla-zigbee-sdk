/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 *
 * This code is offered as an example without any guarantee or warranty.
 */
#define _GNU_SOURCE	/* for asprintf() */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <sys/queue.h>

#include <arpa/inet.h>
#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/json_parser.h>
#include <ayla/hashmap.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/log.h>
#include <ayla/conf_io.h>
#include <ayla/file_io.h>
#include <ayla/timer.h>

#include <app/app.h>
#include <app/ops.h>
#include <app/props.h>
#include <app/data.h>
#include <app/conf_access.h>

#include "props_internal.h"
#include "ops_internal.h"
#include "data_internal.h"
#include "sched_internal.h"

/*
 * Define PROP_CLONE_SEND_FILES to have files cloned prior to sending
 * so the source file can be modified or deleted immediately after the
 * send call. This is useful if the application desires to send snapshots
 * of a file it modifies while prior sends are in process
 */
#ifndef PROP_CLONE_SEND_FILES
#define PROP_CLONE_SEND_FILES	false
#endif

/*
 * Define PROP_FILEQ_PURGE_ON_CONN_ERR to have all queued files operations
 * purged on any connectivity failure. This is useful if the files
 * are time sensitive and of no value if they are significantly
 * delayed
 */
#ifndef PROP_FILEQ_PURGE_ON_CONN_ERR
#define PROP_FILEQ_PURGE_ON_CONN_ERR false
#endif

/*
 * PROP_MAX_FILE_RETRIES controls how many times a file
 * transfer may be retried in the presence of connection failures.
 * The define PROP_FILEQ_PURGE_ON_CONN_ERR takes precedence over
 * this setting
 */
#ifndef PROP_MAX_FILE_RETRIES
#define PROP_MAX_FILE_RETRIES	3
#endif

/*
 * PROP_MAX_OUTSTANDING_FILES defines the maximum number of file transactions
 * that may be queued at any point in time
 */
#ifndef PROP_MAX_OUTSTANDING_FILES
#define PROP_MAX_OUTSTANDING_FILES 5
#endif

/*
 * PROP_MAX_FILE_SIZE specifies the largest support file size
 */
#ifndef PROP_MAX_FILE_SIZE
#define PROP_MAX_FILE_SIZE 2000000000
#endif

/*
 * PROP_FILE_START_BACKOFF specifies the starting backoff
 * delay time in milliseconds when retrying a file operation.
 * The delay is doubled on each retry until it reaches
 * the maximum setting
 */
#ifndef PROP_FILE_START_BACKOFF
#define PROP_FILE_START_BACKOFF	15000
#endif

/*
 * PROP_FILE_MAX_BACKOFF specifies the maximum backoff
 * delay time in milliseconds when retrying a file operation.
 * This is the cap on the longest time a retry will be
 * delayed
 */
#ifndef PROP_FILE_MAX_BACKOFF
#define PROP_FILE_MAX_BACKOFF	300000
#endif

#define PROP_SUBSYSTEM_ID	   1
#define PROP_SCHEDULES "prop_schedules"

/* Declare prop structure specific hashmap functions */
HASHMAP_FUNCS_CREATE(prop, const char, struct prop);

/*
 * Property map is a hashmap of pointers to added prop structures.
 * Use prop_add() to put more properties in the map.
 */
struct prop_state {
	bool initialized;
	bool clone_send_files;		/* clone files prior to sending */
	bool purge_files_on_err;
	int prop_fileq_len;
	int max_queued_files;		/* limit on outstanding file ops */
	int max_file_retries;
	int prop_batch_sent_counter;
	u32 start_backoff;
	u32 current_backoff;
	u32 max_backoff;
	struct timer timer;		/* backoff timer */
	struct hashmap map;
	STAILQ_HEAD(, prop_file_def) prop_fileq;
};

/* Temp directory used for in-progress file properties */
const char *prop_temp_dir = "/tmp";

static struct prop_state prop_state = {
	.max_queued_files = PROP_MAX_OUTSTANDING_FILES,
	.max_file_retries = PROP_MAX_FILE_RETRIES,
	.clone_send_files = PROP_CLONE_SEND_FILES,
	.purge_files_on_err = PROP_FILEQ_PURGE_ON_CONN_ERR,
	.start_backoff = PROP_FILE_START_BACKOFF,
	.current_backoff = PROP_FILE_START_BACKOFF,
	.max_backoff = PROP_FILE_MAX_BACKOFF,
};

static int prop_cmd_handler(void *arg, int *req_id, int confirm_needed);
static int prop_batch_cmd_handler(void *arg, int *req_id, int confirm_needed);
static int prop_batch_nak_process(void *arg, int req_id, enum confirm_err err,
				json_t *obj_j);
static int prop_process_batch_confirmation(void *arg,
	enum confirm_status status, enum confirm_err err, int dests);
static int prop_process_confirmation(void *arg, enum confirm_status status,
	enum confirm_err err, int dests);
static void prop_cmd_free(void *arg);
static int (*prop_batch_confirm_handler)(int, const struct op_options *,
	const struct confirm_info *);

/*
 * Free a prop cmd
 */
static void prop_cmd_free(void *arg)
{
	struct prop_cmd *pcmd = arg;

	if (!pcmd) {
		return;
	}
	free(pcmd->val);
	prop_metadata_free(pcmd->opts.metadata);
	free(pcmd);
}

/*
 * Helper function to free the prop_batch_list
 */
static void prop_batch_list_free_helper(struct prop_batch_list *list)
{
	struct prop_batch_entry *entry;

	if (!list) {
		return;
	}
	while ((entry = STAILQ_FIRST(&list->batchq)) != NULL) {
		STAILQ_REMOVE_HEAD(&list->batchq, link);
		prop_cmd_free(entry->pcmd);
		free(entry);
	}
	free(list);
}

/*
 * Free the prop_batch_list. Can be used to abort/free a batch before being
 * sent. This function must not be called after the batch has already been sent.
 * It should also not be called twice. To be safe, the lib takes away the
 * application's pointer to the batch by setting *list_ptr to NULL.
 */
void prop_batch_list_free(struct prop_batch_list **list_ptr)
{
	struct prop_batch_list *list;

	if (!list_ptr) {
		return;
	}
	list = *list_ptr;
	if (!list) {
		return;
	}
	if (list->sent) {
		log_warn("app should not free a sent list");
		return;
	}
	prop_batch_list_free_helper(list);
	*list_ptr = NULL;
}

/*
 * Free the prop_batch_sent_list
 */
static void prop_batch_sent_list_free(void *arg)
{
	struct prop_batch_sent_list *batch_sent_list = arg;

	prop_batch_list_free_helper(batch_sent_list->batch_list);
	free(batch_sent_list);
}

/*
 * Perform necessary ADS recovery actions for a property that failed to send
 * to ADS.
 */
static void prop_ads_recovery_process(struct prop *prop)
{
	if (!prop->ads_failure) {
		return;
	}
	prop->ads_failure = 0;
	if (prop->ads_recovery_cb) {
		prop->ads_recovery_cb(prop);
	} else if (prop->send) {
		log_debug("resending prop: %s", prop->name);
		prop->send(prop, 0, NULL);
	}
}

/*
 * Mark a property as ADS failure when the value is known
 */
static void prop_with_val_ads_failure_process(struct prop *prop,
	const void *val, size_t len, struct op_options *opts)
{
	struct op_options opts_bkup;

	if (!prop || !prop->ads_failure_cb) {
		return;
	}
	if (!opts) {
		memset(&opts_bkup, 0, sizeof(opts_bkup));
		opts_bkup.dev_time_ms = ops_get_system_time_ms();
		opts = &opts_bkup;
	}
	if (prop->type == PROP_MESSAGE) {
		log_debug("prop %s val %s",
		    prop->name, val ? (char *)val:"");
		if (val) {
			if (unlink((char *)val) < 0) {
				log_warn("can't remove %s: %m", (char *)val);
			}
		}
		prop->ads_failure_cb(prop, NULL, 0, opts);
	} else if (prop->type == PROP_FILE) {
		prop->ads_failure_cb(prop, NULL, 0, opts);
	} else {
		prop->ads_failure_cb(prop, val, len, opts);
	}
}

/*
 * Mark a property as ADS failure
 */
static int prop_ads_failure_process(struct prop *prop, struct prop_cmd *pcmd)
{
	if (!prop) {
		return 0;
	}
	prop->ads_failure = 1;
	if (pcmd) {
		prop_with_val_ads_failure_process(prop, pcmd->val,
		    pcmd->val_len, &pcmd->opts);
	} else {
		prop_with_val_ads_failure_process(prop, NULL, 0, NULL);
	}
	/* Immediately process ADS recovery if ADS is back up */
	if (ops_cloud_up()) {
		prop_ads_recovery_process(prop);
	}

	return 0;
}

/*
 * Process NAK for a property
 */
static int prop_nak_process(void *arg, int req_id, enum confirm_err err,
			    json_t *obj_j)
{
	struct prop_cmd *pcmd = arg;

	if (!pcmd) {
		return 0;
	}

	/*
	 * Message prop failure will be handled in confirm cb.
	*/
	if (pcmd->prop->type == PROP_MESSAGE) {
		return 0;
	}

	/*
	 * Only treat as an ADS failure if it was a connection
	 * error and not during a file service operation
	 */
	if (err != CONF_ERR_CONN ||
	    pcmd->op == AD_DP_SEND || pcmd->op == AD_DP_REQ) {
		return 0;
	}
	prop_ads_failure_process(pcmd->prop, pcmd);

	return 0;
}

/*
 * Process echo failure for a property
 */
int prop_echo_failure_process(const char *echo_name, const json_t *arg)
{
	struct prop *prop;

	prop = prop_lookup(echo_name);
	prop_ads_failure_process(prop, NULL);

	return 0;
}

/*
 * Send a batch list. The application SHOULD NOT modify the batch list
 * after its sent (i.e. calling append on the list again). To be safe, the lib
 * takes away the application's pointer to the list by setting *list_ptr to
 * NULL. The application has op_options available to set *dests* for the batch,
 * etc. Note that if the application wants explicit confirmation for the op, it
 * must first use *prop_batch_confirm_handler_set*. The *batch_id* arg can be
 * optionally given to store the batch # assigned for this batch. This can be
 * useful if using *prop_batch_confirm_handler_set*. On success, 0 is returned.
 */
int prop_batch_send(struct prop_batch_list **list_ptr,
		const struct op_options *opts, int *batch_id)
{
	struct prop_batch_sent_list *batch_sent_list;
	struct prop_batch_list *list;
	struct prop_state *state = &prop_state;

	if (!list_ptr) {
		return -1;
	}
	list = *list_ptr;
	if (!list) {
		return -1;
	}
	if (STAILQ_EMPTY(&list->batchq)) {
		free(list);
		*list_ptr = NULL;
		return -1;
	}
	log_debug("sending batch");
	list->sent = 1;
	batch_sent_list = calloc(1, sizeof(*batch_sent_list));
	batch_sent_list->batch_list = list;
	batch_sent_list->batch_id = ++state->prop_batch_sent_counter;
	if (batch_id) {
		*batch_id = batch_sent_list->batch_id;
	}
	if (opts) {
		memcpy(&batch_sent_list->opts, opts,
		    sizeof(batch_sent_list->opts));
	}
	if (!batch_sent_list->opts.dev_time_ms) {
		batch_sent_list->opts.dev_time_ms = ops_get_system_time_ms();
	}
	ops_add(prop_batch_cmd_handler, batch_sent_list,
	    prop_batch_nak_process, prop_process_batch_confirmation,
	    prop_batch_sent_list_free);
	*list_ptr = NULL;

	return 0;
}

/*
 * (Optional) Set the confirmation handler for prop batch sends. This handler
 * will be called when a batch is sent with the confirm option set to 1. The
 * *batch_id* will be the same one given in *prop_batch_send*.
 */
void prop_batch_confirm_handler_set(int (*handler)(int batch_id,
	const struct op_options *opts, const struct confirm_info *confirm_info))
{
	prop_batch_confirm_handler = handler;
}

/*
 * Allocate an empty prop_metadata structure with a capacity of
 * PROP_METADATA_MAX_ENTRIES.
 */
struct prop_metadata *prop_metadata_alloc(void)
{
	struct prop_metadata *metadata;

	metadata = (struct prop_metadata *)malloc(sizeof(*metadata));
	if (!metadata) {
		log_err("malloc failed");
		return NULL;
	}
	metadata->num_entries = 0;
	return metadata;
}

/*
 * Free a prop_metadata_list structure.
 */
void prop_metadata_free(struct prop_metadata *metadata)
{
	if (!metadata) {
		return;
	}
	prop_metadata_clear(metadata);	/* Free all key/value pairs */
	free(metadata);
}

/*
 * Allocate a copy of the source prop_metadata structure.
 */
struct prop_metadata *prop_metadata_dup(const struct prop_metadata *source)
{
	unsigned i;
	struct prop_metadata *metadata;

	ASSERT(source != NULL);

	metadata = prop_metadata_alloc();
	if (!metadata) {
		return NULL;
	}
	for (i = 0; i < source->num_entries; ++i) {
		metadata->entries[i].key = strdup(source->entries[i].key);
		metadata->entries[i].value = strdup(source->entries[i].value);
	}
	metadata->num_entries = source->num_entries;
	return metadata;
}

/*
 * Create a JSON object with metadata key/value pairs.
 * Format:
 *	{
 *		"key1": "value1",
 *		"key2": "value2",
 *		"keyN": "valueN"
 *	}
 * Returns NULL on error.
 */
json_t *prop_metadata_to_json(const struct prop_metadata *metadata)
{
	const struct prop_metadata_entry *entry;
	json_t *obj;

	ASSERT(metadata != NULL);

	obj = json_object();
	if (!obj) {
		return NULL;
	}
	for (entry = metadata->entries;
	    entry < metadata->entries + metadata->num_entries;
	    ++entry) {
		json_object_set_new(obj, entry->key, json_string(entry->value));
	}
	return obj;
}

/*
 * Add a new key/value pair to a prop_metadata_list structure.
 * Use prop_metadata_free() to free this structure once it has been passed
 * to a send function or is no longer needed.
 */
enum err_t prop_metadata_add(struct prop_metadata *metadata,
	const char *key, const char *val)
{
	return prop_metadata_addf(metadata, key, "%s", val);
}

/*
 * Add a new key/value pair to a prop_metadata_list structure using a printf-
 * style formatted value.
 * Use prop_metadata_free() to free this structure once it has been passed
 * to a send function or is no longer needed.
 */
enum err_t prop_metadata_addf(struct prop_metadata *metadata,
	const char *key, const char *fmt, ...)
{
	struct prop_metadata_entry *entry;
	const char *cp;
	va_list args;

	ASSERT(metadata != NULL);
	ASSERT(key != NULL);
	ASSERT(fmt != NULL);

	/* Do runtime check here in case metadata alloc failed */
	if (!metadata) {
		log_err("null metadata structure");
		return ERR_ARG;
	}

	if (metadata->num_entries >= PROP_METADATA_MAX_ENTRIES) {
		log_err("cannot exceed %u metadata entries",
		    PROP_METADATA_MAX_ENTRIES);
		return ERR_VAL;
	}
	/* Validate key */
	cp = key;
	while (*cp) {
		if (!isalnum(*cp)) {
			log_err("key must be alphanumeric");
			return ERR_ARG;
		}
		++cp;
	}
	if (cp - key > PROP_METADATA_KEY_MAX_LEN) {
		log_err("key exceeds %u byte limit", PROP_METADATA_KEY_MAX_LEN);
		return ERR_MEM;
	}

	entry = metadata->entries + metadata->num_entries;
	/* Allocate copies of key and formatted value strings */
	entry->key = strdup(key);
	va_start(args, fmt);
	if (vasprintf(&entry->value, fmt, args) < 0) {
		entry->value = NULL;
	}
	va_end(args);
	if (!entry->key || !entry->value) {
		free(entry->key);
		free(entry->value);
		return ERR_MEM;
	}
	++metadata->num_entries;
	return ERR_OK;
}

/*
 * Clear all key/value pairs added to a prop_metadata structure.  This allows
 * it to be reused, as an alternative to allocating a new one.
 */
void prop_metadata_clear(struct prop_metadata *metadata)
{
	unsigned i;

	if (!metadata) {
		log_err("null metadata structure");
		return;
	}
	for (i = 0; i < metadata->num_entries; ++i) {
		free(metadata->entries[i].key);
		free(metadata->entries[i].value);
	}
	metadata->num_entries = 0;
}

/*
 * Create a new prop_cmd. cmd is set to the created prop_cmd if the function
 * returns ERR_OK. Otherwise its NULL.
 */
static enum err_t prop_new_cmd_helper(struct prop *prop, const void *val,
	size_t val_len, enum ayla_data_op op, int req_id, struct prop_cmd **cmd,
	const struct op_options *opts)
{
	struct prop_cmd *pcmd;

	*cmd = NULL;
	pcmd = calloc(1, sizeof(*pcmd));
	if (!pcmd) {
		return ERR_MEM;
	}
	pcmd->op = op;
	pcmd->req_id = req_id;
	pcmd->val_len = val_len;
	pcmd->prop = prop;
	switch (op) {
	case AD_PROP_REQ:
		if (val) {
			pcmd->val = strdup(val);
			pcmd->opts.confirm = 1;
		}
		break;
	case AD_PROP_SEND:
		if (opts) {
			pcmd->opts = *opts;
			if (opts->metadata) {
				pcmd->opts.metadata =
				    prop_metadata_dup(opts->metadata);
			}
		}
		/* prop send always requires a confirmation */
		pcmd->opts.confirm = 1;
		/* no break */
	case AD_PROP_RESP:
		if ((prop->type == PROP_STRING)
		    || (prop->type == PROP_MESSAGE)) {
			/*
			 * add 1 for strings to make sure it'll be null
			 * terminated
			 */
			pcmd->val = strndup(val, val_len);
		} else {
			pcmd->val = malloc(val_len);
			if (pcmd->val) {
				memcpy(pcmd->val, val, val_len);
			}
		}
		if (!pcmd->val) {
			free(pcmd);
			return ERR_MEM;
		}
		break;
	case AD_DP_CREATE:
		/* Add datapoint metadata to file property DP create ops */
		if (opts) {
			if (opts->metadata) {
				pcmd->opts.metadata =
				    prop_metadata_dup(opts->metadata);
			}
			pcmd->opts.dev_time_ms = opts->dev_time_ms;
		}
		/* no break */
	case AD_DP_SEND:
	case AD_DP_REQ:
	case AD_DP_FETCHED:
		pcmd->val = (void *)val; /* val is pointer to prop_file_def */
		/* certain file ops require confirmations */
		pcmd->opts.confirm = 1;
		break;
	default:
		break;
	}
	if (!pcmd->opts.dev_time_ms) {
		pcmd->opts.dev_time_ms = ops_get_system_time_ms();
	}
	*cmd = pcmd;
	return ERR_OK;
}

/*
 * Create a new prop_cmd and send it over to ops
 */
static enum err_t prop_new_cmd(struct prop *prop, const void *val,
	size_t val_len, enum ayla_data_op op, int req_id,
	const struct op_options *opts)
{
	enum err_t err;
	struct prop_cmd *pcmd;

	err = prop_new_cmd_helper(prop, val, val_len, op, req_id, &pcmd, opts);
	if (err != ERR_OK) {
		return err;
	}
	if (pcmd->opts.confirm) {
		ops_add(prop_cmd_handler, pcmd, prop_nak_process,
		    prop_process_confirmation, prop_cmd_free);
	} else {
		ops_add(prop_cmd_handler, pcmd, NULL, NULL,
		    prop_cmd_free);
	}
	return ERR_OK;
}

/*
 * Create a new prop_cmd and prop_batch_entry and store it for batching
 */
static enum err_t prop_new_batch_entry(struct prop_batch_list *list,
	struct prop *prop, const void *val, size_t val_len,
	enum ayla_data_op op, const struct op_options *opts)
{
	enum err_t err;
	struct prop_cmd *pcmd;
	struct prop_batch_entry *new_entry;
	struct prop_batch_entry *entry;
	struct prop_batch_entry *entry_before = NULL;

	err = prop_new_cmd_helper(prop, val, val_len, op, 0, &pcmd, opts);
	if (err != ERR_OK) {
		return err;
	}
	/*
	 * Insert the new update in timestamp order
	 */
	STAILQ_FOREACH(entry, &list->batchq, link) {
		if (entry->pcmd->opts.dev_time_ms >
		    pcmd->opts.dev_time_ms) {
			break;
		}
		entry_before = entry;
	}
	new_entry = calloc(1, sizeof(*new_entry));
	new_entry->entry_id = ++list->batchq_len;
	new_entry->pcmd = pcmd;
	if (entry_before) {
		STAILQ_INSERT_AFTER(&list->batchq, entry_before, new_entry,
		   link);
	} else {
		STAILQ_INSERT_HEAD(&list->batchq, new_entry, link);
	}
	return err;
}

/*
 * Process the head operation of the prop_fileq
 */
static int prop_process_head_file(void)
{
	struct prop_file_def *prop_file;
	enum ayla_data_op op;
	struct prop_state *state = &prop_state;

	/* Don't start new file ops if cloud is down */
	if (!ops_cloud_up()) {
		return 0;
	}

	if (STAILQ_EMPTY(&state->prop_fileq)) {
		return 0;
	}
	prop_file = STAILQ_FIRST(&state->prop_fileq);

	switch (prop_file->state) {
	case PF_DP_RDY_FETCH:
		/* Request file fetch */
		op = AD_DP_REQ;
		prop_file->state = PF_DP_FETCHING;
		break;
	case PF_DP_FETCHED:
		/* Indicate file has been fetched */
		op = AD_DP_FETCHED;
		prop_file->state = PF_DP_FETCH_INDICATED;
		break;
	case PF_DP_RDY_CREATE:
		/* Request data point creation */
		op = AD_DP_CREATE;
		prop_file->state = PF_DP_CREATING;
		break;
	case PF_DP_RDY_SEND:
		/* Send file */
		op = AD_DP_SEND;
		prop_file->state = PF_DP_SENDING;
		break;
	case PF_DP_TIMER_START:
		/* start backoff timer */
		log_debug("starting backoff timer: %u ms",
		    state->current_backoff);
		timer_set(app_get_timers(), &state->timer,
		    state->current_backoff);
		/* exponentially increase backoff for next time */
		state->current_backoff *= 2;
		if (state->current_backoff > state->max_backoff) {
			state->current_backoff = state->max_backoff;
		}
		prop_file->state = PF_DP_TIMER_WAIT;
		return 0;
	default:
		/* Waiting for pending operation, nothing to do */
		return 0;
	}

	return prop_new_cmd(prop_file->prop, prop_file, sizeof(prop_file),
	    op, prop_file->req_id, &prop_file->opts);
}

/*
 * Handler for file op backoff timer
 */
static void prop_file_backoff_timeout(struct timer *timer)
{
	struct prop_file_def *prop_file;
	struct prop_state *state = &prop_state;

	log_debug("backoff timer fired");

	if (STAILQ_EMPTY(&state->prop_fileq)) {
		return;
	}
	prop_file = STAILQ_FIRST(&state->prop_fileq);

	if (prop_file->state == PF_DP_TIMER_WAIT) {
		prop_file->state = prop_file->next_state;
	}

	prop_process_head_file();
}

/*
 * Free the prop file structure
 */
static void prop_file_free(struct prop_file_def *prop_file)
{
	struct prop_state *state = &prop_state;

	if (prop_file->location) {
		free(prop_file->location);
	}
	if (prop_file->path) {
		if (prop_file->delete_file) {
			/* delete the temporary file */
			if (unlink(prop_file->path) < 0) {
				log_warn("can't remove %s: %m",
				    prop_file->path);
			}
		}
		free(prop_file->path);
	}
	prop_metadata_free(prop_file->opts.metadata);
	free(prop_file);
	if (state->prop_fileq_len) {
		state->prop_fileq_len--;
	}
}

/*
 * Helper function for adding a prop_file_def to the prop_file queue
 * and creating a pcmd for it if needed.
 */
static int prop_add_to_prop_file_queue(struct prop_file_def *prop_file)
{
	struct prop_state *state = &prop_state;

	state->prop_fileq_len++;
	STAILQ_INSERT_TAIL(&state->prop_fileq, prop_file, link);
	if (state->prop_fileq_len == 1) {
		return prop_process_head_file();
	}

	return 0;
}

/*
 * Process location for file dp
 */
int prop_location_for_file_dp(const char *name, const char *location)
{
	struct prop_file_def *prop_file;
	struct prop_state *state = &prop_state;

	prop_file = STAILQ_FIRST(&state->prop_fileq);
	if (!prop_file || strcmp(prop_file->prop->name, name)) {
		log_debug("recvd location for unknown property %s", name);
		return -1;
	}
	prop_file->location = strdup(location);
	prop_file->state = PF_DP_RDY_SEND;
	return 0;
}

/*
 * Add a list of properties to the library's property lookup table. This table
 * should match the property list in the device's cloud template.
 */
int prop_add(struct prop *props, unsigned int count)
{
	struct prop_state *state = &prop_state;
	struct prop *p;
	struct prop *prop;
	int rc = 0;

	ASSERT(props != NULL);

	for (p = props; p < &props[count]; ++p) {
		prop = prop_hashmap_put(&state->map, p->name, p);
		if (prop != p) {
			rc = -1;
			if (!prop) {
				log_err("failed to add property: %s", p->name);
			} else {
				log_warn("ignoring duplicate property: %s",
				    p->name);
			}
		}
	}
	return rc;
}

/*
 * Lookup a property table entry by name.
 */
struct prop *prop_lookup(const char *name)
{
	struct prop_state *props = &prop_state;

	ASSERT(name != NULL);

	return prop_hashmap_get(&props->map, name);
}

/*
 * Basic function for sending a file property datapoint from a file at the
 * specified path.
 */
int prop_file_send(struct prop *prop, int req_id, const char *path,
		const struct op_options *opts)
{
	int err;
	char temp_filename[PATH_MAX];
	int tmp_fd;
	struct stat stat_buf;
	struct prop_file_def *prop_file;
	int input_fd;
	bool delete_file = false;
	struct prop_state *state = &prop_state;

	ASSERT(prop != NULL);
	ASSERT(prop->type == PROP_FILE);

	if (state->prop_fileq_len >= state->max_queued_files) {
		log_err("%s: too many outstanding files", prop->name);
		return ERR_MEM;
	}
	input_fd = open(path, O_RDONLY);
	if (input_fd == -1) {
		log_err("file open err %s: %m", path);
		return ERR_VAL;
	}
	fstat(input_fd, &stat_buf);
	if (stat_buf.st_size > PROP_MAX_FILE_SIZE) {
		log_err("file exceeds max size: %zu bytes",
		    (size_t)stat_buf.st_size);
		close(input_fd);
		return ERR_VAL;
	}
	if (state->clone_send_files) {
		/* clone the file so the app can freely modify the original */
		snprintf(temp_filename, sizeof(temp_filename),
		    "%s/appd_temp_file.XXXXXX", prop_temp_dir);
		path = temp_filename;
		tmp_fd = mkstemp(temp_filename);
		err = sendfile(tmp_fd, input_fd, NULL, stat_buf.st_size);
		if (err == -1) {
			log_err("sendfile err %m");
			close(input_fd);
			close(tmp_fd);
			return ERR_VAL;
		}
		/* delete after send */
		delete_file = true;
		close(tmp_fd);
	}
	close(input_fd);
	prop_file = calloc(1, sizeof(*prop_file));
	prop_file->prop = prop;
	prop_file->path = strdup(path);
	prop_file->state = PF_DP_RDY_CREATE;
	prop_file->req_id = req_id;
	prop_file->retries_remaining = state->max_file_retries;
	prop_file->delete_file = delete_file;
	if (opts) {
		prop_file->opts = *opts;
		if (opts->metadata) {
			prop_file->opts.metadata =
			    prop_metadata_dup(opts->metadata);
		}
	}
	if (!prop_file->opts.dev_time_ms) {
		prop_file->opts.dev_time_ms = ops_get_system_time_ms();
	}

	log_debug("%s sending file: %s", prop->name, path);

	return prop_add_to_prop_file_queue(prop_file);
}

/*
 * Basic function for sending a message property datapoint.
 */
int prop_message_send(struct prop *prop,
		const void *val, size_t val_len,
		enum ayla_data_op op, int req_id,
		const struct op_options *opts)
{
	char tmp_filename[PATH_MAX];
	int tmp_fd;
	ssize_t ret;

	ASSERT(prop != NULL);
	ASSERT(prop->type == PROP_MESSAGE);

	/*
	 * Save the val to a tmp file,
	 * and set pcmd->val to the tmp file path
	 */
	snprintf(tmp_filename, sizeof(tmp_filename),
	    "%s/appd_up_%s.XXXXXX", prop_temp_dir, prop->name);
	tmp_fd = mkstemp(tmp_filename);
	if (tmp_fd == -1) {
		log_err("mkstemp %s err %m", tmp_filename);
		return ERR_VAL;
	}
	ret = write(tmp_fd, val, val_len);
	if (ret != val_len) {
		log_err("write ret %d != val_len %u"
		    " to %s err %m", ret, val_len, tmp_filename);
		close(tmp_fd);
		return ERR_VAL;
	}
	close(tmp_fd);

	return prop_new_cmd(prop, tmp_filename, strlen(tmp_filename),
	    op, req_id, opts);
}

/*
 * Helper function to send a value for a property.
 */
static enum err_t prop_val_send_helper(struct prop *prop, int req_id,
	const void *val, size_t val_len, const struct op_options *opts,
	struct prop_batch_list *batch_list)
{
	enum ayla_data_op op;
	enum err_t err;
	size_t str_len;

	log_debug("prop_val_send_helper start");

	ASSERT(prop !=  NULL);
	if (!val) {
		return ERR_VAL;
	}
	if (prop->type == PROP_STRING) {
		str_len = strlen((const char *)val);
		if (!val_len || val_len > str_len) {
			val_len = str_len;
		}
	} else if (!val_len) {
		return ERR_VAL;
	}
	op = req_id ? AD_PROP_RESP : AD_PROP_SEND;
	switch (prop->type) {
	case PROP_INTEGER:
	case PROP_BOOLEAN:
	case PROP_DECIMAL:
	case PROP_STRING:
	case PROP_BLOB:
		if (batch_list) {
			err = prop_new_batch_entry(batch_list, prop, val,
			    val_len, op, opts);
		} else {
			err = prop_new_cmd(prop, val, val_len, op, req_id,
			    opts);
		}
		break;
	case PROP_FILE:
		if (batch_list) {
			/* batching is not allowed for FILE properties */
			return ERR_TYPE;
		}
		err = prop_file_send(prop, req_id, (const char *)val, opts);
		break;
	case PROP_MESSAGE:
		if (batch_list) {
			return ERR_TYPE;
		}
		err = prop_message_send(prop, val, val_len,
		    op, req_id, opts);
		break;
	default:
		log_err("unsupported type");
		err = ERR_TYPE;
	}
	log_debug("prop_val_send_helper end");
	return err;
}

/*
 * This function can be used as the "send" function in the 'struct prop'
 * defined above. This function sends prop->arg to cloud and mobile apps (LAN).
 * Applications should set req_id to 0 UNLESS they are responding to property
 * requests in which case *req_id* MUST be set to the value passed to the
 * application by the library. Please see *get* in prop struct for details.
 * The function can return the following values:
 *    ERR_OK: Success
 *    ERR_MEM: Memory Allocation failure
 *    ERR_VAL: prop->val is NULL or val_len is 0 (except for PROP_STRING)
 *    ERR_ARG: Incorrect parameter
 *    ERR_TYPE: Invalid property type
 *
 * The op_options provide further features on send operations. Most applications
 * don't need to use anything other than the defaults in which case a NULL can
 * be passed in. Here are the features that op_options give:
 * 1) The application can timestamp the update by providing *dev_time_ms*. If no
 * timestamp is given, the current system time will be used.
 * 2) The *dests* flag can be used to specify where the property will be sent.
 * The application can use this to send to cloud only or mobile apps only. By
 * default, all available dests will be sent to.
 * 3) The *confirm* flag can be used to ask the library to give explicity
 * confirmation that the property update has been delivered. See "confirm_cb"
 * function handler for more details above.
 */
inline enum err_t prop_arg_send(struct prop *prop, int req_id,
			const struct op_options *opts)
{
	return prop_val_send(prop, req_id, prop->arg, prop->len, opts);
}

/*
 * Send a value for a property. This is similar to *prop_arg_send* except it
 * allows you to give a value instead of just using what's pointed to by
 * prop->arg. Recommended option is to use "prop_arg_send".
 */
inline enum err_t prop_val_send(struct prop *prop, int req_id, const void *val,
			size_t val_len, const struct op_options *opts)
{
	log_debug("%s", prop->name);
	return prop_val_send_helper(prop, req_id, val, val_len, opts, NULL);
}

/*
 * Same as *prop_val_send* except in "batch mode". See descriptions of
 * *prop_arg_batch_append* and *prop_val_send*.
 */
struct prop_batch_list *prop_val_batch_append(struct prop_batch_list *list,
			struct prop *prop, void *val, size_t val_len,
			const struct op_options *opts)
{
	struct prop_batch_list *new_list = NULL;
	enum err_t err;

	if ((prop->type == PROP_MESSAGE) || (prop->type == PROP_FILE)) {
		/* batching is not allowed for FILE properties */
		log_err("cannot batch send Message/FILE properties");
		return NULL;
	}
	if (!list) {
		/* create a new queue */
		new_list = calloc(1, sizeof(*new_list));
		STAILQ_INIT(&new_list->batchq);
		list = new_list;
	} else if (list->sent) {
		log_warn("cannot modify list after it's sent");
		return NULL;
	}
	err =  prop_val_send_helper(prop, 0, val, val_len, opts, list);
	if (err != ERR_OK) {
		prop_batch_list_free_helper(new_list);
		return NULL;
	}
	log_debug("%s appended to batch", prop->name);

	return list;
}

/*
 * Same as prop_arg_send but the property is put in a queue to be sent later.
 * If the given list is set to NULL, a new batch list is created and can be
 * used in subsequent calls. Otherwise, the new update is appended to the given
 * list. Properties of type PROP_FILE cannot be batched. A *NULL* will
 * be returned if there was a failure appending to the batch. The batch can be
 * sent by calling *prop_batch_send*. The only opts allowed in this call are
 * *dev_time_ms* and *confirm*. See *prop_arg_send* for details on what the
 * options mean. Note that this function allows the application to have the
 * flexibility of having multiple ongoing batches if it desires. It may be
 * desirable to send the property updates to LAN clients right away and only
 * batch updates for the cloud. In this case, you can call prop_arg_send for LAN
 * clients and prop_batch_send for just the cloud (by setting op_options).
 */
inline struct prop_batch_list *prop_arg_batch_append(
	struct prop_batch_list *list, struct prop *prop,
	const struct op_options *opts)
{
	return prop_val_batch_append(list, prop, prop->arg, prop->len, opts);
}

/*
 * Send property by prop pointer. Default op_options will be used.
 */
enum err_t prop_send(struct prop *prop)
{
	ASSERT(prop != NULL);
	ASSERT(prop->send != NULL);

	return prop->send(prop, 0, NULL);
}

/*
 * Send property by looking it up by name. Default op_options will be used.
 */
enum err_t prop_send_by_name(const char *name)
{
	struct prop *prop;

	prop = prop_lookup(name);
	if (!prop) {
		return ERR_ARG;
	}

	return prop_send(prop);
}

/*
 * Request value of a property from the cloud using a pointer to the struct.
 */
enum err_t prop_request(struct prop *prop)
{
	ASSERT(prop != NULL);
	ASSERT(prop->set != NULL);

	return prop_new_cmd(prop, NULL, 0, AD_PROP_REQ, 0, NULL);
}

/*
 * Request value of a property from the cloud using property's name.
 */
enum err_t prop_request_by_name(const char *name)
{
	struct prop *prop;

	prop = prop_lookup(name);
	if (!prop) {
		return ERR_ARG;
	}

	return prop_request(prop);
}

/*
 * Request values of all properties from the service. This function could be
 * useful in making sure it's in sync with the cloud at bootup.
 */
enum err_t prop_request_all(void)
{
	return prop_new_cmd(NULL, NULL, 0, AD_PROP_REQ_ALL, 0, NULL);
}

/*
 * Request values of all to-device properties from the service. This function
 * could be useful in making sure it's in sync with the cloud at bootup.
 */
enum err_t prop_request_to_dev(void)
{
	return prop_new_cmd(NULL, NULL, 0, AD_PROP_REQ_TO_DEV, 0, NULL);
}

/*
 * Request data of a message property from the cloud.
 */
enum err_t prop_request_for_message(struct prop *prop, const char *value)
{
	ASSERT(prop != NULL);
	ASSERT(prop->type == PROP_MESSAGE);

	return prop_new_cmd(prop, value, strlen(value),
	    AD_PROP_REQ, 0, NULL);
}

/*
 * Send all from-device properties.  Property structs currently do not indicate
 * direction, so this function assumes that from-device properties do not have
 * a set handler defined.  If batch argument is true, all non-file props
 * will be sent in a single batch.
 */
enum err_t prop_send_from_dev(bool batch)
{
	struct prop_state *props = &prop_state;
	struct prop_batch_list *batch_list = NULL;
	struct prop_batch_list *result;
	struct prop *prop;
	struct prop *template_version;
	struct hashmap_iter *iter;

	log_debug("sending all from-device properties");

	/* Send special template version property first */
	template_version = prop_hashmap_get(&props->map, "oem_host_version");
	if (template_version && template_version->send) {
		template_version->send(template_version, 0, NULL);
	}
	for (iter = hashmap_iter(&props->map); iter;
	    iter = hashmap_iter_next(&props->map, iter)) {
		prop = prop_hashmap_iter_get_data(iter);
		if (prop->set || !prop->send
		    || prop->type == PROP_MESSAGE || prop->type == PROP_FILE
		    || prop == template_version) {
			continue;
		}
		/*
		 * Props without a value prop->arg cannot be batched.
		 */
		if (batch && prop->arg) {
			result = prop_arg_batch_append(batch_list, prop, NULL);
			if (result) {
				batch_list = result;
			}
		} else {
			prop->send(prop, 0, NULL);
		}
	}
	if (batch && batch_list) {
		prop_batch_send(&batch_list, NULL, NULL);
	}
	return ERR_OK;
}

/*
 * Ask a struct prop to response to a property request. If the response handler
 * for it doesn't exist, call the send function directly.
 */
static enum err_t prop_response_by_prop(struct prop *prop, int req_id,
					const void *arg)
{
	u8 val;
	log_debug("req_id %d requested prop %s, prop->get %p, prop->send %p",
	    req_id, prop->name, prop->get, prop->send);
	if (prop->get) {
		return prop->get(prop, req_id, arg);
	} else if (prop->send) {
		return prop->send(prop, req_id, NULL);
	} else {
		if (prop->type == PROP_STRING) {
			return prop_val_send(prop, req_id, "", 0, NULL);
		} else {
			val = 0;
			return prop_val_send(prop, req_id, &val, 1, NULL);
		}
	}
}

/*
 * Generate responses for prop requests from devd
 */
enum err_t prop_response_by_name(const char *name, int req_id, const void *arg)
{
	struct prop *prop;
	log_debug("name %s, req_id %d", name, req_id);
	prop = prop_lookup(name);
	if (!prop) {
		return ERR_ARG;
	}
	log_debug("req_id %d requested %s with data %s",
	    req_id, prop->name, (arg) ? (char *)arg:"none");
	return prop_response_by_prop(prop, req_id, arg);
}

/*
 * Basic function for retrieving file properties from the cloud.
 *
 * XXX Assumes that the app wants the file stored in the path pointed to by
 * prop->arg.
 */
int prop_file_set(struct prop *prop, const void *val, size_t len,
		const struct op_args *args)
{
	struct prop_file_def *prop_file;
	const char *location;
	const char *destination;
	struct prop_state *state = &prop_state;

	ASSERT(prop != NULL);
	ASSERT(prop->type == PROP_FILE);

	if (state->prop_fileq_len >= state->max_queued_files) {
		log_err("%s: too many outstanding files", prop->name);
		return ERR_MEM;
	}
	location = (const char *)val;
	destination = (const char *)prop->arg;
	if (!location || location[0] == '\0') {
		return ERR_VAL;
	}
	if (!destination || destination[0] == '\0') {
		log_err("property arg must point to destination file path");
		return ERR_ARG;
	}
	if (file_touch(destination) < 0) {
		return ERR_VAL;
	}
	prop_file = calloc(1, sizeof(*prop_file));
	prop_file->prop = prop;
	prop_file->path = realpath(destination, NULL);
	prop_file->location = strdup(location);
	prop_file->state = PF_DP_RDY_FETCH;
	prop_file->req_id = 0;
	prop_file->retries_remaining = state->max_file_retries;
	prop_file->delete_file = true; /* Save only if xfer succeeds */
	prop_file->opts.confirm = 1; /* Automatically confirm when fetched */
	log_debug("%s fetching file: %s", prop->name, destination);

	return prop_add_to_prop_file_queue(prop_file);
}

/*
 * This function can be used as the "set" function in the 'struct prop'
 * defined above. Basic function for handling incoming property updates. This
 * function sets the object pointed to prop->arg with *val*.
 * *val* is a pointer to the data and len is the size of data.
 */
int prop_arg_set(struct prop *prop, const void *val, size_t len,
		const struct op_args *args)
{
	int err = ERR_OK;

	ASSERT(prop != NULL);
	if (!val) {
		/* default set handler does not support NULL values */
		return ERR_OK;
	}
	/* default set handler does not support JSON values */
	ASSERT(!prop->pass_jsonobj);

	switch (prop->type) {
	case PROP_INTEGER:
		ASSERT(len == prop->len);
		*((int32_t *)prop->arg) = *((int32_t *)val);
		log_debug("%s = %d", prop->name, *((int32_t *)prop->arg));
		break;
	case PROP_BOOLEAN:
		ASSERT(len == prop->len);
		*((uint8_t *)prop->arg) = *((uint8_t *)val) ? 1 : 0;
		log_debug("%s = %hhu", prop->name, *((uint8_t *)prop->arg));
		break;
	case PROP_DECIMAL:
		ASSERT(len == prop->len);
		*((double *)prop->arg) = *((double *)val);
		log_debug("%s = %.03f", prop->name, *((double *)prop->arg));
		break;
	case PROP_BLOB:
		ASSERT(len == prop->len);
		memcpy(prop->arg, val, prop->len);
		log_debug("%s set %zu bytes", prop->name, len);
		break;
	case PROP_STRING:
		if (prop->len <= len) {
			log_warn("string too large for buffer");
			err = ERR_VAL;
			break;
		}
		memcpy(prop->arg, val, len);
		((char *)prop->arg)[len] = '\0';
		log_debug("%s = %s", prop->name, (char *)prop->arg);
		break;
	case PROP_MESSAGE:
		if (prop->buflen < len) {
			log_warn("message data too large for buffer");
			err = ERR_VAL;
			break;
		}
		memcpy(prop->arg, val, len);
		prop->len = len;
		break;
	case PROP_FILE:
		err = prop_file_set(prop, val, len, args);
		break;
	default:
		log_err("%s: unsupported type %d", prop->name, prop->type);
		err = ERR_TYPE;
	}
	return err;
}

/*
 * Execute a prop_cmd.
 */
static int prop_cmd_handler(void *arg, int *req_id, int confirm_needed)
{
	struct prop_file_def *prop_file;
	struct prop_cmd *pcmd = arg;
	struct prop_state *state = &prop_state;
	int request_id;
	u8 confirm_opt_bkup;
	int rc;

	if (!ops_cloud_up() &&
	    (pcmd->op == AD_DP_CREATE || pcmd->op == AD_DP_SEND)) {
		/*
		 * since we don't have cloud connectivity, don't even try a file
		 * op.
		 */
		prop_ads_failure_process(pcmd->prop, pcmd);
		prop_process_confirmation(pcmd, CONF_STAT_FAIL, CONF_ERR_CONN,
		    DEST_ADS);
		return -1;
	}
	/*
	 * If confirm_needed is set, that overrides opts.confirm
	 */
	confirm_opt_bkup = pcmd->opts.confirm;
	pcmd->opts.confirm = confirm_needed;
	rc = data_execute_prop_cmd(pcmd, &request_id);
	pcmd->opts.confirm = confirm_opt_bkup;
	if (req_id) {
		*req_id = request_id;
	}
	if (pcmd->op == AD_DP_CREATE || pcmd->op == AD_DP_SEND ||
	    pcmd->op == AD_DP_REQ || pcmd->op == AD_DP_FETCHED) {
		/* store the req id so we can process NAKs */
		prop_file = STAILQ_FIRST(&state->prop_fileq);
		if (prop_file) {
			prop_file->req_id = request_id;
		}
	}
	if (rc) {
		log_warn("handler failed");
		return -1;
	}
	return 0;
}

/*
 * Execute a prop_batch_cmd.
 */
static int prop_batch_cmd_handler(void *arg, int *req_id, int confirm_needed)
{
	struct prop_batch_sent_list *batch_sent_list = arg;
	int request_id;
	u8 confirm_opt_bkup;
	int rc;

	/*
	 * If confirm_needed is set, that overrides opts.confirm
	 */
	confirm_opt_bkup = batch_sent_list->opts.confirm;
	batch_sent_list->opts.confirm = confirm_needed;
	rc = data_execute_batch_cmd(batch_sent_list, &request_id);
	batch_sent_list->opts.confirm = confirm_opt_bkup;
	if (req_id) {
		*req_id = request_id;
	}
	batch_sent_list->sent_req_id = request_id;
	if (rc) {
		log_warn("batch_send handler failed");
		return -1;
	}
	return 0;
}

/*
 * Handle a change in the cloud connectivity state.
 */
void prop_cloud_status_changed(bool up)
{
	struct prop_state *props = &prop_state;
	struct prop *prop, *template_version;
	struct hashmap_iter *iter;
	struct prop_batch_list *batch_list = NULL, *result;

	if (!up) {
		return;
	}

	/* Send special template version property first */
	template_version = prop_hashmap_get(&props->map, "oem_host_version");
	if (template_version && template_version->ads_failure) {
		template_version->ads_failure = 0;
		if (template_version->send) {
			log_debug("resending prop: %s",
			    template_version->name);
			template_version->send(template_version, 0, NULL);
		}
	}

	/* Cloud up: process cloud recovery action on all properties */
	for (iter = hashmap_iter(&props->map); iter;
	    iter = hashmap_iter_next(&props->map, iter)) {
		prop = prop_hashmap_iter_get_data(iter);
		if ((!prop->ads_failure) || (prop == template_version)) {
			continue;
		}
		prop->ads_failure = 0;

		log_debug("resending prop: %s, ads_recovery_cb %p,"
		    " send %p, type 0x%x, arg %p",
		    prop->name, prop->ads_recovery_cb,
		    prop->send, prop->type, prop->arg);

		if (prop->ads_recovery_cb) {
			prop->ads_recovery_cb(prop);
		} else if (prop->send) {
			if ((prop->type == PROP_MESSAGE)
			    || (prop->type == PROP_FILE)) {
				prop->send(prop, 0, NULL);
			} else if (prop->arg) {
				result = prop_arg_batch_append(
				    batch_list, prop, NULL);
				if (result) {
					batch_list = result;
				} else {
					log_err("batch_append failed for %s",
					    prop->name);
				}
			}
		}
	}

	if (batch_list) {
		prop_batch_send(&batch_list, NULL, NULL);
	}

	/* Continue file operations */
	prop_process_head_file();
}

/*
 * Helper function to setup the confirm dests mask based on the dests given in
 * opts
 */
int prop_set_confirm_dests_mask(u8 opts_dests)
{
	/*
	 * a successful confirmation from devd if opts_dests isn't given means
	 * that the confirm true is just for DEST_ADS. Unless of course DEST_ADS
	 * isn't available in which case confirm true is for ops_dests_avail.
	 */
	if (opts_dests) {
		return opts_dests;
	}
	if (ops_cloud_up()) {
		return DEST_ADS;
	}
	if (ops_lan_up()) {
		/* Processing of specific LAN client dests is deprecated, so
		 * just return LAN ID 1 for backward compatibility.
		 */
		return LAN_ID_TO_DEST_MASK(1);
	}
	return 0;
}

static void prop_confirmation_cb_helper(struct prop_cmd *pcmd,
	struct confirm_info *confirm_info)
{
	struct prop *prop = pcmd->prop;
	struct prop_file_def *prop_file;

	if (!prop->confirm_cb) {
		return;
	}
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		confirm_info->dests =
		    prop_set_confirm_dests_mask(pcmd->opts.dests);
	}
	if (prop->type == PROP_MESSAGE) {
		log_debug("prop %s confirm status %d, val %s",
		    prop->name, confirm_info->status,
		    pcmd->val ? (char *)pcmd->val : "");
		if (pcmd->val) {
			if (unlink((char *)pcmd->val) < 0) {
				log_warn("can't remove %s: %m",
				    (char *)pcmd->val);
			}
		}
		if (pcmd->op == AD_PROP_SEND) {
			prop->confirm_cb(prop, NULL, 0,
			    &pcmd->opts, confirm_info);
		}
	} else if (prop->type == PROP_FILE) {
		prop_file = (struct prop_file_def *)pcmd->val;
		if (!prop_file) {
			log_err("invalid file command");
			return;
		}
		if (prop_file->opts.confirm) {
			prop->confirm_cb(prop, NULL, 0, &prop_file->opts,
			    confirm_info);
		}
	} else if (pcmd->opts.confirm) {
		/* non-file property callback */
		prop->confirm_cb(prop, pcmd->val, pcmd->val_len, &pcmd->opts,
		    confirm_info);
	}
}

/*
 * Process confirmation for a queue
 */
static int prop_process_batch_confirmation(void *arg,
	enum confirm_status status, enum confirm_err err, int dests)
{
	struct confirm_info confirm_info = {.status = status, .err = err,
	    .dests = dests};
	struct prop_batch_sent_list *batch_sent_list = arg;
	struct prop_batch_entry *entry;
	struct prop_cmd *pcmd;
	u8 has_naks = 0; /* 1 if any naks were recvd for the batch send */

	if (!batch_sent_list) {
		return 0;
	}
	/*
	 * Go through every datapoint to see which need confirmation
	 */
	STAILQ_FOREACH(entry, &batch_sent_list->batch_list->batchq, link) {
		pcmd = entry->pcmd;
		if (status == CONF_STAT_SUCCESS) {
			if (entry->recvd_nak) {
				/* this entry failed, send confirmation fail */
				has_naks = 1;
				confirm_info.status = CONF_STAT_FAIL;
				confirm_info.err = CONF_ERR_CONN;
			}
			prop_confirmation_cb_helper(pcmd, &confirm_info);
			confirm_info.status = status;
			confirm_info.err = err;
		} else {
			prop_confirmation_cb_helper(pcmd, &confirm_info);
		}
	}
	if (batch_sent_list->opts.confirm && prop_batch_confirm_handler) {
		if (status == CONF_STAT_SUCCESS && has_naks) {
			/* if one of the entries failed, mark it partial */
			confirm_info.status = CONF_STAT_PARTIAL_SUCCESS;
		} else {
			confirm_info.status = status;
		}
		if (confirm_info.status == CONF_STAT_SUCCESS) {
			confirm_info.dests =
			    prop_set_confirm_dests_mask(batch_sent_list->
			    opts.dests);
		}
		prop_batch_confirm_handler(batch_sent_list->batch_id,
		    &batch_sent_list->opts, &confirm_info);
	}
	return 0;
}

/*
 * Process NAK for a batch entry
 */
static int prop_batch_nak_process(void *arg, int req_id, enum confirm_err err,
				json_t *obj_j)
{
	struct prop_batch_sent_list *batch_sent_list = arg;
	struct prop_batch_entry *entry;
	json_t *op_args;
	int batch_id;

	if (!batch_sent_list) {
		return 0;
	}
	op_args = json_object_get(obj_j, "op_args");
	if (!op_args || json_get_int(op_args, "batch_id", &batch_id) < 0) {
		/*
		 * Individual batch_id is not included in the nak if the
		 * entire batch failed.
		 */
		batch_id = -1;
	}
	/* Process failure for all failed entries */
	STAILQ_FOREACH(entry, &batch_sent_list->batch_list->batchq, link) {
		if (batch_id == -1 || entry->entry_id == batch_id) {
			entry->recvd_nak = 1;
			/* ADS failure only if this is a conn err */
			if (err == CONF_ERR_CONN) {
				prop_ads_failure_process(entry->pcmd->prop,
					entry->pcmd);
			}
			if (entry->entry_id == batch_id) {
				/*
				 * In the case of a partial batch failure,
				 * an individual nak will be sent for each
				 * failed property.
				 */
				break;
			}
		}
	}
	if (batch_id != -1 && !entry) {
		log_debug("received nak for unknown batch_id: %d", batch_id);
		return -1;
	}

	return 0;
}

/*
 * Process confirmation for a file property
 */
static int prop_process_confirmation_file_helper(struct prop_cmd *pcmd,
	struct confirm_info *confirm_info)
{
	struct prop *prop;
	struct prop_state *state = &prop_state;
	struct prop_file_def *prop_file;
	bool file_ops_complete = false;

	prop_file = STAILQ_FIRST(&state->prop_fileq);
	/* received confirmation prop that is no longer being tracked */
	if (!prop_file || prop_file != pcmd->val) {
		log_debug("file prop %s is not in queue", pcmd->prop->name);
		pcmd->val = NULL;
		return -1;
	}
	prop = prop_file->prop;
	if (confirm_info->status == CONF_STAT_SUCCESS) {
		/* reset retry counter on any success */
		prop_file->retries_remaining = state->max_file_retries;
		state->current_backoff = state->start_backoff;
		/* success actions */
		switch (pcmd->op) {
		case AD_DP_REQ:
			/* File fetch completed */
			prop_file->state = PF_DP_FETCHED;
			break;
		case AD_DP_FETCHED:
			/* Fetch indication completed */
			file_ops_complete = true;
			prop_file->delete_file = false;
			break;
		case AD_DP_CREATE:
			/* Datapoint create completed */
			prop_file->state = PF_DP_RDY_SEND;
			break;
		case AD_DP_SEND:
			/* File prop send complete */
			file_ops_complete = true;
			break;
		default:
			log_debug("unsupported file op (%d) for prop %s",
			    pcmd->op, pcmd->prop->name);
			file_ops_complete = true;
			break;
		}
	} else if (prop_file->retries_remaining-- > 0) {
		switch (prop_file->state) {
		case PF_DP_FETCHING:
			log_debug("retrying file fetch for prop %s",
			    pcmd->prop->name);
			prop_file->state = PF_DP_TIMER_START;
			prop_file->next_state = PF_DP_RDY_FETCH;
			break;
		case PF_DP_FETCH_INDICATED:
			log_debug("retrying fetched indication for prop %s",
			    pcmd->prop->name);
			prop_file->state = PF_DP_TIMER_START;
			prop_file->next_state = PF_DP_FETCHED;
			break;
		case PF_DP_CREATING:
			log_debug("retrying dp creation for prop %s",
			    pcmd->prop->name);
			prop_file->state = PF_DP_TIMER_START;
			prop_file->next_state = PF_DP_RDY_CREATE;
			break;
		case PF_DP_SENDING:
			log_debug("retrying file send for prop %s",
			    pcmd->prop->name);
			prop_file->state = PF_DP_TIMER_START;
			prop_file->next_state = PF_DP_RDY_SEND;
			break;
		default:
			log_debug("file op %d failed for prop %s, aborting",
			    pcmd->op, pcmd->prop->name);
			file_ops_complete = true;
			break;
		}
	} else {
		/* operation failed, so stop tracking it */
		log_debug("file op %d failed for prop %s, aborting",
		    pcmd->op, pcmd->prop->name);
		file_ops_complete = true;
	}
	if (file_ops_complete) {
		/* done with file queue entry */
		STAILQ_REMOVE_HEAD(&state->prop_fileq, link);
		prop_file_free(prop_file);
		/* invoke callback if needed */
		prop_confirmation_cb_helper(pcmd, confirm_info);
	}
	/* set val to NULL so we don't try to free it */
	pcmd->val = NULL;
	if (confirm_info->err == CONF_ERR_CONN && state->purge_files_on_err) {
		/* purge queued file properties */
		log_debug("clearing pending file ops: connectivity failure");
		for (prop_file = STAILQ_FIRST(&state->prop_fileq); prop_file;
		    prop_file = STAILQ_FIRST(&state->prop_fileq)) {
			STAILQ_REMOVE_HEAD(&state->prop_fileq, link);
			if (prop_file->opts.confirm && prop->confirm_cb) {
				prop->confirm_cb(prop, NULL, 0,
				    &prop_file->opts, confirm_info);
			}
			prop_file_free(prop_file);
		}
		return 0;
	}
	/* process the next file property operation */
	prop_process_head_file();
	return 0;
}

/*
 * Process confirmation for a property
 */
static int prop_process_confirmation(void *arg, enum confirm_status status,
	enum confirm_err err, int dests)
{
	struct confirm_info confirm_info = {.status = status, .err = err,
	    .dests = dests};
	struct prop_cmd *pcmd = arg;
	struct prop *prop = pcmd->prop;

	/* file props require additional management */
	if (prop->type == PROP_FILE) {
		return prop_process_confirmation_file_helper(pcmd,
		    &confirm_info);
	}
	prop_confirmation_cb_helper(pcmd, &confirm_info);
	return 0;
}

/*
 * Update to a property received from the cloud or a mobile app
 */
int prop_datapoint_set(struct prop *prop, const void *val, size_t len,
	    const struct op_args *args)
{
	int rc = -1;

	if (prop && prop->set) {
		rc = prop->set(prop, val, len, args);
	}
	if (!args) {
		return rc;
	}
	if (args->source != SOURCE_ADS && !ops_cloud_up()) {
		/* property update came from a LAN client and ADS is not up */
		prop_with_val_ads_failure_process(prop, val, len, NULL);
	}

	return rc;
}

/*
 * Fire a scheduled event for a property
 */
static void prop_fire_schedule(char *name, enum prop_type type, void *val,
				size_t val_len, json_t *arg)
{
	struct prop *prop;
	json_t *root = NULL;

	prop = prop_lookup(name);
	if (!prop || !prop->set || type != prop->type) {
		log_warn("unable to fire scheduled event for %s", name);
		return;
	}
	if (prop->pass_jsonobj) {
		if (type == PROP_INTEGER || type == PROP_BOOLEAN) {
			root = json_integer(*((int *)val));
		} else if (type == PROP_STRING) {
			root = json_stringn((char *)val, val_len);
		}
		prop->set(prop, root, sizeof(root), NULL);
		json_decref(root);
	} else {
		prop->set(prop, val, val_len, NULL);
	}

	/* Send the property update to cloud */
	prop_response_by_prop(prop, 0, NULL);
}

/*
 * Load property schedules
 */
static int prop_schedules_set(json_t *scheds)
{
	const char *name;
	const char *value;
	json_t *arg;
	json_t *sched;
	int i;

	if (!json_is_array(scheds)) {
		return -1;
	}
	for (i = 0; i < json_array_size(scheds); i++) {
		sched = json_array_get(scheds, i);
		name = json_get_string(sched, "name");
		value = json_get_string(sched, "value");
		arg = json_object_get(sched, "arg");
		if (!name || !value) {
			log_warn("bad prop_schedule in config");
			continue;
		}
		sched_add_new_schedule(PROP_SUBSYSTEM_ID, name, value, arg,
		    prop_fire_schedule);
	}
	return 0;
}

/*
 * Handle incoming property schedules from devd
 */
static void prop_schedule_handler(const char *name, const void *val, size_t len,
				json_t *metadata)
{
	json_t *sched_arr;
	const char *node_id;
	char *zigbee_sched_name = NULL;
	int name_len;

	/*
	 * In the Zigbee solution, the schedules come down looking like this:
	 * "schedule": {
	 *      "base_type": "schedule"
	 *         "metadata": {
	 *            "node_id": "0x00158D0000626E6D"
	 *          },
	 *          "name": "1_in_0x0006_sched2",
	 *          "value": "MwEBIQAtBAABKsEiACcEVd0BgCgE=="
	 *       }
	 *   }
	 * To make sure that the schedues have unique names for the sched
	 * subsystem, we have to prepeand the schedule name with the node id
	 */
	if (metadata) {
		node_id = json_get_string(metadata, "node_id");
		if (node_id) {
			name_len = asprintf(&zigbee_sched_name, "%s_%s",
			    node_id, name);
			if (name_len == -1) {
				log_warn("allocation failure");
				return;
			}
			name = zigbee_sched_name;
		}
	}
	if (!sched_add_new_schedule(PROP_SUBSYSTEM_ID, name, val, metadata,
	    prop_fire_schedule)) {
		sched_arr = sched_get_json_form_of_scheds(PROP_SUBSYSTEM_ID);
		if (!conf_set_new(PROP_SCHEDULES, sched_arr)) {
			conf_save();
		}
	}
	free(zigbee_sched_name);
}

/*
 * Delete a property schedule. Should not be used except for the Zigbee
 * solution.
 */
void prop_schedule_delete(const char *prefix)
{
	size_t len;
	json_t *scheds;
	json_t *sched;
	const char *sched_name;
	int deleted = 0;
	int i;
	int rc;

	if (!prefix) {
		return;
	}
	len = strlen(prefix);
	if (!len) {
		return;
	}
	scheds = sched_get_json_form_of_scheds(PROP_SUBSYSTEM_ID);
	for (i = 0; i < json_array_size(scheds); i++) {
		sched = json_array_get(scheds, i);
		sched_name = json_get_string(sched, "name");
		if (!strncmp(sched_name, prefix, len)) {
			sched_remove_schedule(PROP_SUBSYSTEM_ID, sched_name);
			json_array_remove(scheds, i);
			i--;
			deleted = 1;
		}
	}
	if (deleted) {
		rc = conf_set(PROP_SCHEDULES, scheds);
		if (rc < 0) {
			log_err("failed to set %s", PROP_SCHEDULES);
		} else if (!rc) {
			conf_save();
		}
	}
	json_decref(scheds);
}

/*
 * Helper function for converting value to string format for printing. Given
 * a value of a certain type, this function will use snprintf to create a
 * string representation of the value unless type == PROP_STRING. The returned
 * pointer will either point to *str (if type != PROP_STRING) or
 * val (if type == PROP_STRING).
 */
const char *prop_val_to_str(const void *val, enum prop_type type)
{
	static char buf[64];

	if (!val) {
		return "(null)";
	}
	switch (type) {
	case PROP_STRING:
		return (const char *)val;
	case PROP_BOOLEAN:
		return *((u8 *)val) ? "true" : "false";
	case PROP_INTEGER:
		snprintf(buf, sizeof(buf), "%d", *(int *)val);
		return buf;
	case PROP_DECIMAL:
		snprintf(buf, sizeof(buf), "%.03f", *(double *)val);
		return buf;
	case PROP_FILE:
		snprintf(buf, sizeof(buf), "file: %s",
		    (const char *)val);
		return buf;
	default:
		return "";
	}
}

/*
 * Initialize the prop block.
 */
void prop_initialize(void)
{
	struct prop_state *state = &prop_state;

	if (state->initialized) {
		return;
	}
	hashmap_init(&state->map, hashmap_hash_string,
	    hashmap_compare_string, 96);
	STAILQ_INIT(&state->prop_fileq);
	conf_register(PROP_SCHEDULES, prop_schedules_set, NULL);
	data_set_schedule_handler(prop_schedule_handler);
	timer_init(&state->timer, prop_file_backoff_timeout);
	state->initialized = true;
}
