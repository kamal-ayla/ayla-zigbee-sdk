/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_TLV_H__
#define __AYLA_TLV_H__

/*
 * Ayla TLV for commands.
 */
struct ayla_tlv {
	u8	type;		/* type code */
	u8	len;		/* length of value */
				/* value follows immediately */
} PACKED;

#define TLV_VAL(tlv)	((void *)(tlv + 1))
#define TLV_NEXT(tlv)	((struct ayla_tlv *)(TLV_VAL(tlv) + (tlv)->len))

/*
 * TLV for a 32-bit integer.
 */
struct ayla_tlv_int {
	struct ayla_tlv head;
	be32	data;
} PACKED;

/*
 * Formatting-hint TLV.
 * This TLV should be between the name and value TLVs, if used.
 * It is only a hint for automatically-generated web pages.
 */
struct ayla_tlv_fmt {
	struct ayla_tlv head;
	u8	fmt_flags;	/* formatting hint flag (see below) */
} PACKED;

/*
 * fmt_flag values.
 */
#define AFMT_READ_ONLY	(1 << 0) /* indicates variable is not settable */
#define AFMT_HEX	(1 << 1) /* value should be formatted in hex */

#endif /* __AYLA_TLV_H__ */
