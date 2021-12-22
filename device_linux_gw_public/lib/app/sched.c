/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc
 */

#define _GNU_SOURCE 1 /* for strndup */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/queue.h>

#include <poll.h>
#include <unistd.h>
#include <limits.h>

#include <ayla/utypes.h>
#include <ayla/endian.h>
#include <ayla/assert.h>
#include <ayla/json_parser.h>
#include <ayla/clock.h>
#include <ayla/ayla_interface.h>
#include <ayla/tlv.h>
#include <ayla/timer.h>
#include <ayla/log.h>
#include <ayla/base64.h>

#include <app/props.h>

#include "msg_client_internal.h"
#include "sched_internal.h"

struct sched_list {
	int sub_id;			/* subsystem of the schedule */
	struct sched_prop sched_info;
	struct sched_list *next;
};

static bool sched_initialized;
static struct sched_list *sched_head;
static struct timer sched_timer;
static u32 sched_next_event_time; /* time of next scheduled event */
static struct timer_head *appd_timers;	/* pointer to appd's timer_head */

/*
 * Run through all schedules. Fire events as time progresses
 * to current utc time. Determine the next future event and
 * return it. The current time can either be passed in or
 * the function just calls time(NULL).
 */
static u32 sched_run_all(struct sched_list *schedlist, u32 *input_time)
{
	u32 utc_time = input_time ? *input_time : time(NULL);
	u32 next_event;
	u32 earliest_event = MAX_U32;
	struct sched_prop *schedprop;
	struct sched_list *scheds;

	/* Determine if time has been set. If not, then don't run schedules */
	if (clock_source() < CS_SYSTEM) {
		return 0;
	}
	for (scheds = schedlist; scheds; scheds = scheds->next) {
		schedprop = &scheds->sched_info;
		if (!schedprop->len) {
			continue;
		}
		log_debug("looking at sched %s",
		    schedprop->name);
		next_event = sched_evaluate(schedprop, utc_time);
		if (!next_event || next_event == MAX_U32) {
			/* no more events to fire for this schedule */
			continue;
		}
		if (next_event < earliest_event) {
			earliest_event = next_event;
		}
	}

	if (!earliest_event || earliest_event == MAX_U32) {
		/* no events left for any of the schedules */
		return 0;
	}
	log_debug("earliest event %u", earliest_event);

	return earliest_event;
}

static void sched_setup_timer_for_next_event(u32 *time_to_run)
{
	u32 utc_time = time_to_run ? *time_to_run : time(NULL);

	if (!sched_initialized) {
		return;
	}
	timer_cancel(appd_timers, &sched_timer);
	sched_next_event_time = sched_run_all(sched_head, &utc_time);
	if (sched_next_event_time) {
		log_debug("scheduling timeout for %u secs",
		    sched_next_event_time - utc_time);
		timer_set(appd_timers, &sched_timer,
		    (u64)(sched_next_event_time - utc_time) * 1000);
	}
}

/*
 * Callback if time info gets updated
 */
static void sched_time_update_handler(void)
{
	sched_setup_timer_for_next_event(NULL);
}

/*
 * Return a JSON representation of a schedule
 */
static json_t *sched_convert_to_json(struct sched_prop *sched)
{
	json_t *obj;

	obj = json_object();
	REQUIRE(obj, REQUIRE_MSG_ALLOCATION);
	json_object_set_new(obj, "name", json_string(sched->name));
	json_object_set_new(obj, "value", json_string(sched->base64_val));
	if (sched->handler_arg) {
		json_object_set(obj, "arg", sched->handler_arg);
	}
	return obj;
}

/*
 * Return all the schedules for a particular subsystem
 */
json_t *sched_get_json_form_of_scheds(int sub_id)
{
	json_t *arr;
	struct sched_list *scheds;

	arr = json_array();
	REQUIRE(arr, REQUIRE_MSG_ALLOCATION);
	/* Check schedules where the subsystem id exists */
	for (scheds = sched_head; scheds; scheds = scheds->next) {
		if ((scheds->sub_id == sub_id) &&
		    scheds->sched_info.len) {
			json_array_append_new(arr,
			    sched_convert_to_json(&scheds->sched_info));
		}
	}
	return arr;
}

/*
 * Free a sched_prop
 */
static void sched_free_sched_prop(struct sched_prop *sched)
{
	free(sched->name);
	free(sched->base64_val);
	sched->len = 0;
	free(sched->tlvs);
	json_decref(sched->handler_arg);
	memset(sched, 0, sizeof(*sched));
}

/*
 * Return matching sched_list item for a given sub_id and name
 */
static struct sched_list *sched_get_match_list(int sub_id, const char *name)
{
	struct sched_list *scheds;

	if (!name) {
		return NULL;
	}
	for (scheds = sched_head; scheds; scheds = scheds->next) {
		if (scheds->sub_id == sub_id && scheds->sched_info.name &&
		    !strcmp(scheds->sched_info.name, name)) {
			return scheds;
		}
	}
	return NULL;
}
/*
 * Add a new schedule to the list of schedules
 */
int sched_add_new_schedule(int sub_id, const char *name, const char *val,
    json_t *arg, void (*handler)(char *, enum prop_type, void *,
    size_t, json_t *))
{
	struct sched_list *scheds;

	if (!val || !val[0]) {
		log_err("missing schedule value");
		return 0;
	}
	/* Check if a schedule with that name already exists */
	scheds = sched_get_match_list(sub_id, name);
	if (!scheds) {
		scheds = calloc(1, sizeof(struct sched_list));
		scheds->next = sched_head;
		sched_head = scheds;
	} else if (scheds->sched_info.base64_val) {
		if (!strcmp(scheds->sched_info.base64_val, val)) {
			/* do nothing if schedule hasn't changed */
			return -1;
		}
		sched_free_sched_prop(&scheds->sched_info);
	}
	scheds->sched_info.tlvs = (u8 *)base64_decode(val, strlen(val),
	    &scheds->sched_info.len);
	if (!scheds->sched_info.tlvs) {
		log_warn("decode failed for sched %s", name);
		return -1;
	}
	scheds->sched_info.name = strdup(name);
	scheds->sched_info.base64_val = strdup(val);
	scheds->sched_info.handler = handler;
	scheds->sub_id = sub_id;
	if (arg) {
		scheds->sched_info.handler_arg = json_incref(arg);
	}
	sched_setup_timer_for_next_event(NULL);

	return 0;
}

/*
 * Remove a schedule
 */
int sched_remove_schedule(int sub_id, const char *name)
{
	struct sched_list *scheds;

	/* Check if a schedule with that name already exists */
	scheds = sched_get_match_list(sub_id, name);
	if (scheds) {
		sched_free_sched_prop(&scheds->sched_info);
		sched_setup_timer_for_next_event(NULL);
	}

	return 0;
}

/*
 * Sched timer fired. Run sched to see if any events in the schedules need
 * to fire.
 */
void sched_timeout(struct timer *timer)
{
	sched_setup_timer_for_next_event(&sched_next_event_time);
}

/*
 * Reads the schedule action and calls the handler to fire it
 */
void sched_fire_schedule(struct sched_prop *sched_prop, struct ayla_tlv *atlv,
			u8 tot_len)
{
	struct ayla_tlv *tlv;

	s64 swap_val;
	int int_val;
	bool bool_val;
	enum prop_type type;
	size_t name_tlv_len;
	void *sendval;
	size_t sendlen;
	char *name = NULL;

	tlv = (struct ayla_tlv *)(atlv) + 1;	/* point to the NAME of prop */
	if (tlv->type != ATLV_NAME ||
	    tlv->len + sizeof(struct ayla_tlv) > tot_len) {
		log_err("missing or invalid property name");
		goto error;
	}
	name = strndup(TLV_VAL(tlv), tlv->len);
	if (!name) {
		log_err("malloc failed");
		return;
	}
	name_tlv_len = tlv->len;
	tlv = (struct ayla_tlv *)((u8 *)tlv + tlv->len +
	    sizeof(struct ayla_tlv));	/* point to the value */
	type = tlv->type;
	if (tlv->len == 0) {
		log_warn("bad schedule %s with property update length 0",
		    sched_prop->name);
		goto error;
	}
	if (tlv->len + name_tlv_len + 2 * sizeof(struct ayla_tlv) > tot_len) {
		goto error;
	}
	switch (type) {
	case ATLV_INT:
		if (endian_put_ntoh_s64(&swap_val, TLV_VAL(tlv),
		    tlv->len) < 0) {
			goto error;
		}
		int_val = swap_val;
		sendlen = sizeof(int_val);
		sendval = &int_val;
		break;
	case ATLV_BOOL:
		if (tlv->len != sizeof(u8)) {
			goto error;
		}
		bool_val = *(u8 *)TLV_VAL(tlv) != 0;
		sendlen = sizeof(bool_val);
		sendval = &bool_val;
		break;
	case ATLV_UTF8:
		sendval = TLV_VAL(tlv);
		sendlen = tlv->len;
		break;
	default:
		log_warn("unable to handle type %u", type);
		goto error;
	}
	if (sched_prop->handler) {
		sched_prop->handler(name, type, sendval, sendlen,
		    sched_prop->handler_arg);
	}
error:
	free(name);
}

/*
 * Initialize the sched subsystem
 */
int sched_init(struct timer_head *appd_timer_head)
{
	ASSERT(appd_timer_head != NULL);

	appd_timers = appd_timer_head;
	timer_init(&sched_timer, sched_timeout);
	sched_initialized = true;

	return 0;
}

/*
 * Start processing schedules.
 */
int sched_start(void)
{
	msg_client_set_time_change_callback(sched_time_update_handler);
	sched_setup_timer_for_next_event(NULL);
	return 0;
}

/*
 * Stop processing schedules and free any dynamically allocated memory
 * in the sched subsystem.
 */
int sched_destroy(void)
{
	struct sched_list *tmp;

	if (!sched_initialized) {
		return 0;
	}
	msg_client_set_time_change_callback(NULL);
	timer_cancel(appd_timers, &sched_timer);
	while (sched_head) {
		tmp = sched_head->next;
		sched_free_sched_prop(&sched_head->sched_info);
		free(sched_head);
		sched_head = tmp;
	}
	sched_initialized = false;

	return 0;
}
