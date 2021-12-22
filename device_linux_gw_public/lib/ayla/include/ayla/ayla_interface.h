/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_INTERFACE_H__
#define __AYLA_INTERFACE_H__

#define SOURCE_TO_DEST_MASK(source)	((source) > 0 ? BIT((source) - 1) : 0)
#define LAN_ID_TO_SOURCE(lan_id)	((lan_id) + 1)
#define LAN_ID_TO_DEST_MASK(lan_id)	SOURCE_TO_DEST_MASK( \
					LAN_ID_TO_SOURCE(lan_id))

#define SOURCE_LOCAL	0
#define SOURCE_ADS	1
#define DEST_ADS	SOURCE_TO_DEST_MASK(SOURCE_ADS)
#define DEST_LAN_APPS	0xFE	/* All LAN-mode mobile Apps, not Ayla cloud */

#define MAX_LOC_LEN	256

enum ayla_data_op {

	/*
	 * Ayla Data Service operations
	 */
	AD_PROP_SEND,		/* send property update */
	AD_PROP_BATCH_SEND,	/* send batch property updates */
	AD_PROP_UPDATE,		/* property update */
	AD_PROP_ACK,		/* acknowledge a property update */
	AD_SCHED_UPDATE,	/* schedule update */
	AD_PROP_REQ,		/* request property */
	AD_PROP_REQ_ALL,	/* request all properties */
	AD_PROP_REQ_TO_DEV,	/* request all to-device props */
	AD_PROP_REQ_FROM_DEV,	/* request all from-device */
	AD_PROP_RESP,		/* response to property requests */
	AD_CONFIRM_TRUE,	/* SUCCESS confirmation */
	AD_CONFIRM_FALSE,	/* FAILURE confirmation */
	AD_ACK,			/* ack */
	AD_ERROR,		/* error */
	AD_NAK,			/* nak */
	AD_AUTO_ECHO,		/* auto echo */
	AD_ECHO_FAILURE,	/* echo failure */
	AD_DP_REQ,		/* request data point value */
	AD_DP_RESP,		/* response for data point request */
	AD_DP_CREATE,		/* create a new data point */
	AD_DP_FETCHED,		/* indicate current data point fetched */
	AD_DP_SEND,		/* send data point value */
	AD_DP_STATUS,		/* get status of data point send */
	AD_DP_LOC,		/* location of a newly created datapoint */
	AD_DP_CLOSE,		/* close data point value (internal to devd) */
	AD_DP_REQ_FROM_FILE,	/* fetch dp from  FILE url (internal to devd) */
	AD_MSG_GET,		/* get message property data */
	AD_NOP			/* nop, must be last */
};

#define JINT_DATA_OP_NAMES {			\
	[AD_PROP_SEND] = "prop_send",	\
	[AD_PROP_BATCH_SEND] = "prop_batch_send",	\
	[AD_PROP_UPDATE] = "prop_update",	\
	[AD_PROP_ACK] = "prop_ack",		\
	[AD_SCHED_UPDATE] = "sched_update",	\
	[AD_PROP_REQ] = "prop_req",	\
	[AD_PROP_REQ_ALL] = "prop_req_all",	\
	[AD_PROP_REQ_TO_DEV] = "prop_req_to_dev",	\
	[AD_PROP_REQ_FROM_DEV] = "prop_req_from_dev",	\
	[AD_PROP_RESP] = "prop_resp",	\
	[AD_CONFIRM_TRUE] = "confirm_true",	\
	[AD_CONFIRM_FALSE] = "confirm_false",	\
	[AD_ACK] = "ack",	\
	[AD_ERROR] = "error",	\
	[AD_NAK] = "nak",	\
	[AD_ECHO_FAILURE] = "echo_failure",	\
	[AD_DP_REQ] = "file_dp_req", \
	[AD_DP_RESP] = "file_dp_resp", \
	[AD_DP_CREATE] = "file_dp_create", \
	[AD_DP_FETCHED] = "file_dp_fetched", \
	[AD_DP_SEND] = "file_dp_send", \
	[AD_DP_STATUS] = "file_dp_status", \
	[AD_DP_LOC] = "file_dp_location", \
	[AD_DP_CLOSE] = "file_dp_close", \
	[AD_DP_REQ_FROM_FILE] = "file_dp_req_from_file", \
	[AD_MSG_GET] = "msg_get", \
}

enum ayla_tlv_type {
	ATLV_INVALID = 0x00,	/* Invalid TLV type */
	ATLV_NAME = 0x01,	/* variable name, UTF-8 */
	ATLV_INT = 0x02,	/* integer, with length 1, 2, 4, or 8 */
	ATLV_UINT = 0x03,	/* unsigned integer, 1, 2, 4, or 8 bytes */
	ATLV_BIN = 0x04,	/* unstructured bytes */
	ATLV_UTF8 = 0x05,	/* text */
	ATLV_CONF = 0x06,	/* configuration name indices */
	ATLV_ERR = 0x07,	/* error number */
	ATLV_FORMAT = 0x08,	/* formatting hint */
	ATLV_FRAG = 0x09,	/* fragment descriptor for longer values */
	ATLV_NOP = 0x0a,	/* no-op, ignored TLV inserted for alignment */
	ATLV_FLOAT = 0x0b,	/* IEEE floating point value */
	ATLV_CONF_CD = 0x0c,	/* base path for following config names */
	ATLV_CONF_CD_ABS = 0x0d, /* absolute path for following config names */
	ATLV_CONF_CD_PAR = 0x0e, /* new path in parent directory */
	ATLV_BOOL = 0x0f,	/* boolean value, 1 or 0 */
	ATLV_CONT = 0x10,	/* continuation token for AD_SEND_NEXT_PROP */
	ATLV_OFF = 0x11,	/* offset in data point or other transfer */
	ATLV_LEN = 0x12,	/* length of data point or other transfer */
	ATLV_LOC = 0x13,	/* location of data point or other item */
	ATLV_EOF = 0x14,	/* end of file, e.g., end of data point */
	ATLV_BCD = 0x15,	/* fixed-point decimal number */
	ATLV_CENTS = 0x16,	/* integer value 100 times the actual value */
	ATLV_NODES = 0x17,	/* bitmap of dests or src for prop updates */
	ATLV_ECHO = 0x18,	/* indicates prop update is an echo */
	ATLV_FEATURES = 0x19,	/* bitmap of the supported features in MCU */
	ATLV_CONF_FACTORY = 0x1a,	/* configuration name indices */
	ATLV_DELETE = 0x1b,	/* configuration variable deleted */
				/* reserved gap */
	ATLV_SCHED = 0x20,	/* schedule property */
	ATLV_UTC = 0x21,	/* indicates date/time in schedules are UTC */
	ATLV_AND = 0x22,	/* ANDs the top two conditions in schedule */
	ATLV_DISABLE = 0x23,	/* disables the schedule */
	ATLV_INRANGE = 0x24,	/* stack is true if current time is in range */
	ATLV_ATSTART = 0x25,	/* stack is true if current time is at start */
	ATLV_ATEND = 0x26,	/* stack is true if current time is at end */
	ATLV_STARTDATE = 0x27,	/* date must be after value */
	ATLV_ENDDATE = 0x28,	/* date must be before value */
	ATLV_DAYSOFMON = 0x29,	/* 32-bit mask indicating which day of month */
	ATLV_DAYSOFWK = 0x2a,	/* days of week specified as 7-bit mask */
	ATLV_DAYOCOFMO = 0x2b,	/* day occurence in month */
	ATLV_MOOFYR = 0x2c,	/* months of year */
	ATLV_STTIMEEACHDAY = 0x2d,	/* time of day must be after value */
	ATLV_ENDTIMEEACHDAY = 0x2e,	/* time of day must be before value */
	ATLV_DURATION = 0x2f,	/* must not last more than this (secs) */
	ATLV_TIMEBFEND = 0x30,	/* time must be <value> secs before end */
	ATLV_INTERVAL = 0x31,	/* start every <value> secs since start */
	ATLV_SETPROP = 0x32,	/* value is the property to be toggled */
	ATLV_VERSION = 0x33,	/* version of schedule */
	ATLV_MSG_BIN = 0x37,	/* binary message datapoint */
	ATLV_MSG_UTF8 = 0x38,	/* UTF-8 string message datapoint */
	ATLV_MSG_JSON = 0x39,	/* JSON message datapoint */
				/* reserved gap */
	ATLV_FILE = 0x80,	/* mask for 0x80 thru 0xfe incl. 15-bit len */
	ATLV_RESERVED = 0xff
};

enum app_parse_rc {
	APR_DONE,
	APR_ERR,
	APR_PENDING,
};

#endif /* __AYLA_INTERFACE_H__ */
