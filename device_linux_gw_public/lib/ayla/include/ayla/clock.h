/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_CLOCK_H__
#define __AYLA_CLOCK_H__

#include <sys/time.h>
#include <ayla/token_table.h>

#define TSTAMP_CMP(cmp, a, b)   ((s32)((a) - (b)) cmp 0)
#define TSTAMP_LT(a, b)		TSTAMP_CMP(<, a, b)
#define TSTAMP_GT(a, b)		TSTAMP_CMP(>, a, b)
#define TSTAMP_LEQ(a, b)	TSTAMP_CMP(<=, a, b)
#define TSTAMP_GEQ(a, b)	TSTAMP_CMP(>=, a, b)
#define	TSTAMP_ABS(a)		(((s32)(a) < 0) ? -(a) : (a))

/*
 * Time source codes.
 * Larger numbers indicate more reliable clock sources.
 * Do not change the existing numbers, for upgrade compatibility.
 */
#define CLOCK_SOURCES(def)						\
	def(none,	CS_NONE)	/* never been set */		\
	def(default,	CS_DEFAULT)	/* set to CLOCK_START */	\
	def(system,	CS_SYSTEM)	/* use the system clock */	\
	def(local,	CS_LOCAL)	/* set by internal web server */\
	def(server,	CS_SERVER)	/* set using server time */	\
	def(NTP,	CS_NTP)		/* set using NTP */		\
	def(system locked, CS_SYSTEM_LOCKED)	/* cannot set; must be last */

DEF_ENUM(clock_src, CLOCK_SOURCES);


/*
 * Timezone settings
 */
struct timezone_settings {
	u8 valid:1;		/* 1 if settings should be followed */
	s16 mins;		/* mins west of UTC */
};

/*
 * Daylight settings
 */
struct daylight_settings {
	u8 valid:1;		/* 1 if settings should be followed */
	u8 active:1;		/* 1 if DST is active before change */
	u32 change;		/* when DST flips inactive/active */
};

/*
 * Calendar information about a particular time
 */
struct clock_info {
	u32 time;		/* time that this struct represents */
	u32 month_start;	/* start time of the month */
	u32 month_end;		/* end time of the month */
	u32 day_start;		/* start time of the day */
	u32 day_end;		/* end time of the day */
	u32 secs_since_midnight;/* secs since midnight */
	u32 year;		/* current year */
	u8 month;		/* current month starting from 1 */
	u8 days;		/* current day of month */
	u8 hour;		/* current hour */
	u8 min;			/* current min */
	u8 sec;			/* current seconds */
	u8 days_left_in_month;	/* # days left in current month */
	u8 day_of_week;		/* day of the week. Mon = 1, Sun = 7 */
	u8 day_occur_in_month;	/* occurence of day in month. i.e. 2nd sun */
	u8 is_leap:1;		/* flag to signify if year is leap year */
};

/*
 * convert time to "MM/DD/YYYY hh:mm:ss" format
 */
void clock_fmt(char *buf, size_t len, u32 time);
u32 clock_local(const u32 *utc);
int clock_set_time(time_t, enum clock_src);
int clock_set_source(enum clock_src src);
int clock_is_leap(u32 year);
void clock_fill_details(struct clock_info *clk, u32 time);
int clock_ints_to_time(u32 *timep, u32 year, u32 month, u32 day,
			u32 hour, u32 minute, u32 second);
void clock_incr_day(struct clock_info *clk);
void clock_decr_day(struct clock_info *clk);
void clock_incr_month(struct clock_info *clk);
void clock_decr_month(struct clock_info *clk);
u8 clock_get_day_occur_in_month(u32 days);
u32 clock_local_to_utc(u32 local, u8 skip_fb);

enum clock_src clock_source(void);

extern struct timezone_settings timezone_ayla;	/* timezone settings */
extern struct daylight_settings daylight_ayla;	/* daylight settings */

/*
 * default value to be set in clock after power loss.
 * This is for testing algorithms and certificates.
 */
#define CLOCK_EPOCH	1970U		/* Unix Epoch */
#define	CLOCK_EPOCH_DAY	4	/* day of the week for Jan 1, CLOCK_EPOCH */
#define CLOCK_START	1483228800	/* Jan 1, 2017 00:00:00 UTC */
#define DAYLIGHT_OFFSET	3600	/* daylight offset (secs) */

#define CLOCK_FMT_LEN	20	/* max len of clock_fmt() */


#endif /* __AYLA_CLOCK_H__ */
