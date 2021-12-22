/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_PROPS_INTERNAL_H__
#define __LIB_APP_PROPS_INTERNAL_H__

#define PROP_METADATA_MAX_ENTRIES	10	/* max supported key/vals */
#define PROP_METADATA_KEY_MAX_LEN	255	/* max key characters */

/*
 * Property command to be sent to service. i.e. sends, receives
 */
struct prop_cmd {
	struct prop *prop;	/* property this cmd refers to */
	void *val;		/* pointer to the value */
	size_t val_len;		/* length of value to be sent */
	int req_id;		/* if req_id != 0, op uses the req_id */
	enum ayla_data_op op;	/* data operation needed to be done */
	struct op_options opts;	/* opts of the operation */
};

/*
 * Definition of file command states
 */
enum prop_file_state {
	/* file down states */
	PF_DP_RDY_FETCH,		/* ready to fetch data point value */
	PF_DP_FETCHING,		/* fetching data point value */
	PF_DP_FETCHED,		/* data point value has been fetched */
	PF_DP_FETCH_INDICATED,	/* fetch has been indicated */
	/* file up states */
	PF_DP_RDY_CREATE,	/* ready to request DP create */
	PF_DP_CREATING,		/* DP create has been requested */
	PF_DP_RDY_SEND,		/* ready to send datapoint value */
	PF_DP_SENDING,		/* sending entire datapoint value */
	/* backoff delay states */
	PF_DP_TIMER_START,	/* ready to start backoff timer */
	PF_DP_TIMER_WAIT,	/* waiting for backoff timer to fire */
};

/*
 * Definition of a file command
 */
struct prop_file_def {
	STAILQ_ENTRY(prop_file_def) link;
	struct prop *prop;
	char *path;
	char *location;
	int req_id;
	int retries_remaining;
	bool delete_file;
	enum prop_file_state state;
	enum prop_file_state next_state;
	struct op_options opts;	/* opts of the operation */
};

/*
 * Definition of a batch list sent to devd
 */
struct prop_batch_sent_list {
	STAILQ_ENTRY(prop_batch_sent_list) link;
	struct prop_batch_list *batch_list;
	int batch_id;
	int sent_req_id;
	struct op_options opts;
};

/*
 * Definition of a metadata entry.
 */
struct prop_metadata_entry {
	char *key;
	char *value;
};

/*
 * Definition of a metadata structure.
 */
struct prop_metadata {
	size_t num_entries;
	struct prop_metadata_entry entries[PROP_METADATA_MAX_ENTRIES];
};

/*
 * Handle a change in the cloud connectivity state.
 */
void prop_cloud_status_changed(bool up);

/*
 * Process location for file dp
 */
int prop_location_for_file_dp(const char *name, const char *location);

/*
 * Generate responses for prop requests from devd
 */
enum err_t prop_response_by_name(const char *name, int req_id, const void *arg);

/*
 * Process echo failure for a property
 */
int prop_echo_failure_process(const char *echo_name, const json_t *arg);

/*
 * Helper function to setup the confirm dests mask based on the dests given in
 * opts
 */
int prop_set_confirm_dests_mask(u8 opts_dests);

/*
 * Update to a property received from the cloud or a mobile app
 */
int prop_datapoint_set(struct prop *prop, const void *val, size_t len,
	    const struct op_args *args);

/*
 * Allocate a copy of the source prop_metadata structure.
 */
struct prop_metadata *prop_metadata_dup(const struct prop_metadata *source);

/*
 * Create a JSON object with metadata key/value pairs.
 * Format:
 *	{
 *		"key1": "value1",
 *		"key": "value2",
 *		"keyN": "valueN"
 *	}
 * Returns NULL on error.
 */
json_t *prop_metadata_to_json(const struct prop_metadata *metadata);

/*
 * Request data of a message property from the cloud.
 */
enum err_t prop_request_for_message(struct prop *prop, const char *value);

#endif /*  __LIB_APP_PROPS_INTERNAL_H__ */
