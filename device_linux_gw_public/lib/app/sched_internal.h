/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __LIB_APP_SCHED_INTERNAL_H__
#define __LIB_APP_SCHED_INTERNAL_H__

struct ayla_tlv;

struct sched_prop {
	char *name;			/* name of schedule */
	char *base64_val;		/* base64 encoded value */
	size_t len;			/* len of schedule after base64 dec */
	u8 *tlvs;			/* base64-decoded tlvs of schedule */
	json_t *handler_arg;		/* additional arg if needed */
	void (*handler)(char *name, enum prop_type type, void *val,
	    size_t val_len, json_t *handler_arg);
};

/*
 * Takes a schedtlv structure and fills a schedule structure with
 * the information.
 * Uses the current UTC time to calculate when the next event will
 * occur in UTC time. Returns this value.
 * If it returns 0 or MAX_U32, no more events are set to occur for
 * this schedule.
 */
u32 sched_evaluate(struct sched_prop *schedtlv, u32 time);

/*
 * Add a new schedule to the list of schedules
 */
int sched_add_new_schedule(int sub_id, const char *name, const char *val,
    json_t *arg, void (*handler)(char *, enum prop_type, void *,
    size_t, json_t *));

/*
 * Remove a schedule
 */
int sched_remove_schedule(int sub_id, const char *name);

/*
 * Reads the schedule action and calls the handler to fire it
 */
void sched_fire_schedule(struct sched_prop *sched_prop, struct ayla_tlv *atlv,
			u8 tot_len);
/*
 * Return all the schedules for a particular subsystem
 */
json_t *sched_get_json_form_of_scheds(int sub_id);

#endif /* __LIB_APP_SCHED_INTERNAL_H__ */
