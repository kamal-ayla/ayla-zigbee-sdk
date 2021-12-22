/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ayla/utypes.h>
#include <ayla/clock.h>
#include <ayla/log.h>

#define	ONE_DAY		(24 * 60 * 60)

static DEF_NAME_TABLE(clock_src_names, CLOCK_SOURCES);

struct timezone_settings timezone_ayla;
struct daylight_settings daylight_ayla;
static enum clock_src clk_src;

/*
 * Set clock
 */
int clock_set_time(time_t new_time, enum clock_src src)
{
	static int eperm;
	time_t t;
	struct timespec ts = { .tv_sec = new_time };
	char buf[24];

	if (eperm || src < clk_src) {
		return 1;
	}

	t = time(NULL);
	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		log_warn("cannot set time: %m");
		if (errno == EPERM) {
			eperm = 1;
			clk_src = CS_SYSTEM_LOCKED;
			clock_fmt(buf, sizeof(buf), (unsigned long)t);
			log_debug("using system time %s", buf);
			return 1;
		}
		return -1;
	}
	clk_src = src;
	clock_fmt(buf, sizeof(buf), (unsigned long)t);
	log_info("clock was %s UTC", buf);
	clock_fmt(buf, sizeof(buf), (unsigned long)new_time);
	log_info("clock now %s UTC, src %s", buf, clock_src_names[src]);
	return 0;
}

/*
 * Get the current clock source
 */
enum clock_src clock_source(void)
{
	return clk_src;
}

/*
 * Set clock source
 */
int clock_set_source(enum clock_src src)
{
	if (src < clk_src) {
		return 1;
	}
	clk_src = src;
	return 0;
}

/*
 * convert time to "MM/DD/YYYY hh:mm:ss" format
 */
void clock_fmt(char *buf, size_t len, u32 time)
{
	struct clock_info clk;

	clock_fill_details(&clk, time);
	snprintf(buf, len, "%2.2u/%2.2u/%4.2u %2.2u:%2.2u:%2.2u",
	    clk.month, clk.days, clk.year, clk.hour, clk.min, clk.sec);
}

int clock_is_leap(u32 year)
{
	/*
	if year is divisible by 400 then
		is_leap_year
	else if year is divisible by 100 then
		not_leap_year
	else if year is divisible by 4 then
		is_leap_year
	else
		not_leap_year
	*/

	return !((year % 4) || ((year % 100) == 0 && (year % 400)));
}

/*
 * Return which occurance for this day is in this month.
 * i.e. 1st Sunday, 3rd Thursday
 */
u8 clock_get_day_occur_in_month(u32 days)
{
	return (days + 6) / 7;
}

/*
 * Fill the clock_info structure for a particular time.
 */
void clock_fill_details(struct clock_info *clk, u32 time)
{
	static u8 mdays[12] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };
	u32 tmp;
	u32 year;
	u8 month;

	clk->time = time;
	clk->sec = time % 60;
	time /= 60;
	clk->min = time % 60;
	time /= 60;
	clk->hour = time % 24;
	time /= 24;

	clk->day_of_week = (time + CLOCK_EPOCH_DAY) % 7;
	if (!clk->day_of_week) {
		clk->day_of_week = 7;
	}
	clk->secs_since_midnight = clk->hour * 60 *  60 +
	    clk->min * 60 + clk->sec;
	clk->day_start = clk->time - clk->secs_since_midnight;
	if (ONE_DAY - 1 > MAX_U32 - clk->day_start) {
		clk->day_end = MAX_U32;
	} else {
		clk->day_end = clk->day_start + ONE_DAY - 1;
	}

	/*
	 * here time is days since epoch
	 * XXX I'm sure this can be more clever and avoid the loops.
	 */
	for (year = CLOCK_EPOCH;; year++) {
		clk->is_leap = clock_is_leap(year);
		tmp = 365 + clk->is_leap;
		if (time < tmp)
			break;
		time -= tmp;
	}
	for (month = 1; ; month++) {
		clk->days = mdays[month - 1] + (month == 2 ? clk->is_leap : 0);
		if (time < clk->days)
			break;
		time -= clk->days;
	}
	clk->month = month;
	clk->year = year;
	clk->days = time + 1;		/* first day of month isn't 0 */
	clk->days_left_in_month = mdays[month - 1] +
	    (month == 2 ? clk->is_leap : 0) - clk->days;
	clk->day_occur_in_month = clock_get_day_occur_in_month(clk->days);
	clk->month_start = clk->day_start - ((clk->days - 1) * ONE_DAY);
	if (clk->days_left_in_month * ONE_DAY > MAX_U32 - clk->day_end) {
		clk->month_end = MAX_U32;
	} else {
		clk->month_end = clk->day_end +
		    clk->days_left_in_month * ONE_DAY;
	}
}

/*
 * Convert time represented by separate integers for year, month, day, etc.,
 * to UTC time in seconds since January 1, 1970.
 * This may have problems after Jan 18, 2038 when bit 31 turns on.
 * Similar to gmtime_r() from libc.
 * Returns non-zero if invalid time given.
 */
int clock_ints_to_time(u32 *timep, u32 year, u32 month, u32 day,
			u32 hour, u32 minute, u32 second)
{
	static u8 mdays[12] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };
	static u16 days_before_month[12] = {
		/* jan */ 0,
		/* feb */ 31,
		/* mar */ 31 + 28,
		/* apr */ 31 + 28 + 31,
		/* may */ 31 + 28 + 31 + 30,
		/* jun */ 31 + 28 + 31 + 30 + 31,
		/* jul */ 31 + 28 + 31 + 30 + 31 + 30,
		/* aug */ 31 + 28 + 31 + 30 + 31 + 30 + 31,
		/* sep */ 31 + 28 + 31 + 30 + 31 + 30 + 31 + 31,
		/* oct */ 31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30,
		/* nov */ 31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31,
		/* dec */ 31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30,
	};
	u32 time;
	u32 yr;
	int leap;

	*timep = 0;
	if (year < CLOCK_EPOCH || month > 12 || month < 1 ||
	   day < 1 || hour > 24 || minute > 59 || second > 59) {
		return -1;
	}
	leap = clock_is_leap(year);
	time = (year - CLOCK_EPOCH) * 365;
	for (yr = CLOCK_EPOCH; yr < year; yr++) {
		time += clock_is_leap(yr);
	}
	time += days_before_month[month - 1];
	time += month > 2 && leap;
	if (day > 28) {
		if (month == 2) {
			if (day > 29 || !leap) {
				return -1;
			}
		} else if (day > mdays[month - 1]) {
			return -1;
		}
	}
	time += day - 1;
	time = time * 24 + hour;
	time = time * 60 + minute;
	time = time * 60 + second;
	*timep = time;
	return 0;
}

/*
 * Increments the day represented by clk and updates the details
 */
void clock_incr_day(struct clock_info *clk)
{
	static u8 mdays[12] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };
	u8 recalc_month_end = 0;

	if (MAX_U32 - clk->time < ONE_DAY) {
		clk->time = MAX_U32;
	} else {
		clk->time += ONE_DAY;
	}
	clk->day_of_week++;
	if (clk->day_of_week == 8) {
		clk->day_of_week = 1;
	}
	clk->day_start = clk->time - clk->secs_since_midnight;
	if (clk->time == MAX_U32 || ONE_DAY - 1 > MAX_U32 - clk->day_start) {
		clk->day_end = MAX_U32;
	} else {
		clk->day_end = clk->day_start + ONE_DAY - 1;
	}
	if (!clk->days_left_in_month) {
		if (clk->month == 12) {
			clk->month = 1;
			clk->year++;
			clk->is_leap = clock_is_leap(clk->year);
		}
		clk->days = 1;
		clk->month_start = clk->day_start;
		recalc_month_end = 1;
	} else {
		clk->days++;
	}
	clk->day_occur_in_month = clock_get_day_occur_in_month(clk->days);
	clk->days_left_in_month = mdays[clk->month - 1] +
	    (clk->month == 2 ? clk->is_leap : 0) - clk->days;
	if (!recalc_month_end) {
		return;
	}
	if (clk->days_left_in_month * ONE_DAY > MAX_U32 - clk->day_end) {
		clk->month_end = MAX_U32;
	} else {
		clk->month_end = clk->day_end +
		    clk->days_left_in_month * ONE_DAY;
	}
}

/*
 * Decrements the day represented by clk and updates the details
 */
void clock_decr_day(struct clock_info *clk)
{
	static u8 mdays[12] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };
	u8 recalc_month_start = 0;

	if (clk->time < ONE_DAY) {
		clk->time = 0;
	} else {
		clk->time -= ONE_DAY;
	}
	clk->day_of_week--;
	if (!clk->day_of_week) {
		clk->day_of_week = 7;
	}
	clk->day_start = clk->time - clk->secs_since_midnight;
	clk->day_end = clk->day_start + ONE_DAY - 1;
	clk->days--;
	if (!clk->days) {
		clk->month--;
		if (!clk->month) {
			clk->month = 12;
			clk->year--;
			clk->is_leap = clock_is_leap(clk->year);
		}
		clk->days = mdays[clk->month - 1] +
		    (clk->month == 2 ? clk->is_leap : 0);
		clk->month_end = clk->day_end;
		recalc_month_start = 1;
	}
	clk->days_left_in_month = mdays[clk->month - 1] +
	    (clk->month == 2 ? clk->is_leap : 0) - clk->days;
	clk->day_occur_in_month = clock_get_day_occur_in_month(clk->days);
	if (!recalc_month_start) {
		return;
	}
	if (!clk->time || clk->days * ONE_DAY > clk->month_end) {
		clk->month_start = 0;
	} else {
		clk->month_start = clk->month_end - clk->days * ONE_DAY;
	}
}

/*
 * Increments the month represented by clk and updates the details.
 * Just sets the time to the start of the following month.
 */
void clock_incr_month(struct clock_info *clk)
{
	static u8 mdays[12] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };

	if (clk->month_end == MAX_U32) {
		return;
	}
	clk->sec = 0;
	clk->min = 0;
	clk->hour = 0;
	clk->secs_since_midnight = 0;
	clk->month_start = clk->month_end + 1;
	clk->day_start = clk->month_start;
	clk->time = clk->month_start;
	clk->day_of_week = (clk->day_of_week + clk->days_left_in_month) % 7;
	if (!clk->day_of_week) {
		clk->day_of_week = 7;
	}
	if (ONE_DAY - 1 > MAX_U32 - clk->day_start) {
		clk->day_end = MAX_U32;
	} else {
		clk->day_end = clk->day_start + ONE_DAY - 1;
	}
	clk->month++;
	if (clk->month == 13) {
		clk->month = 1;
		clk->year++;
		clk->is_leap = clock_is_leap(clk->year);
	}
	clk->days = 1;
	clk->day_occur_in_month = 1;
	clk->days_left_in_month = mdays[clk->month - 1] +
	    (clk->month == 2 ? clk->is_leap : 0) - clk->days;
	if (clk->days_left_in_month * ONE_DAY > MAX_U32 - clk->day_end) {
		clk->month_end = MAX_U32;
	} else {
		clk->month_end = clk->day_end +
		    clk->days_left_in_month * ONE_DAY;
	}
}

/*
 * Decrements the month represented by clk and updates the details.
 * Just sets the time to the last day of the previous month.
 */
void clock_decr_month(struct clock_info *clk)
{
	static u8 mdays[12] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };

	if (!clk->month_start) {
		return;
	}
	clk->month_end = clk->month_start - 1;
	clk->day_end = clk->month_end;
	clk->time = clk->month_end;
	clk->sec = 59;
	clk->min = 59;
	clk->hour = 23;
	clk->secs_since_midnight = ONE_DAY - 1;
	clk->month--;
	if (clk->day_end < ONE_DAY - 1) {
		clk->day_start = 0;
	} else {
		clk->day_start = clk->day_end - (ONE_DAY - 1);
	}
	if (!clk->month) {
		clk->month = 12;
		clk->year--;
		clk->is_leap = clock_is_leap(clk->year);
	}
	clk->days = mdays[clk->month - 1] +
	    (clk->month == 2 ? clk->is_leap : 0);
	clk->days_left_in_month = 0;
	clk->day_occur_in_month = clock_get_day_occur_in_month(clk->days);
	clk->day_of_week = (clk->time / ONE_DAY + CLOCK_EPOCH_DAY) % 7;
	if (clk->day_start < (clk->days - 1) * ONE_DAY) {
		clk->month_start = 0;
	} else {
		clk->month_start = clk->day_start - (clk->days - 1) * ONE_DAY;
	}
}

/*
 * Get local time. Takes into account timezone + DST
 * Optional pointer to current UTC time to calculate
 * local time for.
 */
u32 clock_local(const u32 *utc)
{
#if defined(SCHED_TEST) || defined(DEMO_SCHED_LIB)
	u32 utc_time = *utc;
#else
	u32 utc_time = (utc) ? *utc : time(NULL);
#endif
	u32 local_time;

	if (!timezone_ayla.valid) {
		return utc_time;
	}
	local_time = utc_time - timezone_ayla.mins * 60;
	if (daylight_ayla.valid &&
	    daylight_ayla.active == (utc_time < daylight_ayla.change)) {
		local_time += DAYLIGHT_OFFSET;
	}

	return local_time;
}

/*
 * Convert local time to GMT time. Takes into account timezone + DST.
 * During fallback: Maps the 1 am to 2 am of fallback to the 2nd 1am to 2am's
 * UTC if skip_fb_or_spring_fwd = 1. Otherwise, it uses the 1st 1am to 2am's
 * UTC.
 * During spring forward: Maps the 2 am to 3 am to 1:59:59 if
 * skip_fb_or_spring_fwd = 0. If it equals 1, it maps to the new 3 am.
 * If anything else, it returns the normal utc time conversion of that time.
 */
u32 clock_local_to_utc(u32 local, u8 skip_fb_or_spring_fwd)
{
	u32 utc_time;

	if (!timezone_ayla.valid) {
		return local;
	}
	utc_time = local + timezone_ayla.mins * 60;

	if (local == MAX_U32 ||
	    (timezone_ayla.mins > 0 &&
	    MAX_U32 - local < timezone_ayla.mins * 60)) {
		return MAX_U32;
	}
	if (!daylight_ayla.valid) {
		return utc_time;
	}
	if (daylight_ayla.active) {
		if (utc_time < daylight_ayla.change) {
			utc_time -= DAYLIGHT_OFFSET;
		}
		/* For fallback */
		if (!skip_fb_or_spring_fwd &&
		    (utc_time >= daylight_ayla.change &&
		    utc_time < daylight_ayla.change + DAYLIGHT_OFFSET)) {
			utc_time -= DAYLIGHT_OFFSET;
		}
		return utc_time;
	}
	/* For Spring Forward */
	if (utc_time >= daylight_ayla.change &&
	    utc_time < daylight_ayla.change + DAYLIGHT_OFFSET) {
		if (skip_fb_or_spring_fwd == 2) {
			return utc_time;
		} else if (skip_fb_or_spring_fwd == 1) {
			/* Anytime between 2pm to 3pm local, return 3 pm UTC */
			return daylight_ayla.change;
		} else if (!skip_fb_or_spring_fwd) {
			return daylight_ayla.change - 1;
		}
	}
	if (utc_time >= daylight_ayla.change) {
		utc_time -= DAYLIGHT_OFFSET;
	}

	return utc_time;
}
