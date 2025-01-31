/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <unistd.h>
#include <ayla/utypes.h>
#include <ayla/uri_code.h>

/*
 * Bitmap for unreserved characters in an URI argument (after the ?, / is OK).
 */
const u32 uri_arg_allowed_map[256 / 32] = {
	0,		/* 0 - 0x1f control characters */
	BIT('0' - 0x20) |
	BIT('1' - 0x20) |
	BIT('2' - 0x20) |
	BIT('3' - 0x20) |
	BIT('4' - 0x20) |
	BIT('5' - 0x20) |
	BIT('6' - 0x20) |
	BIT('7' - 0x20) |
	BIT('8' - 0x20) |
	BIT('9' - 0x20) |
	BIT('-' - 0x20) |
	BIT('.' - 0x20) |
	BIT('/' - 0x20) |
	BIT('?' - 0x20), /* 0x20 - 0x3f */
	0x87fffffe,	/* 0x40 - 0x5f:  A-Z and underscore */
	0x47fffffe,	/* 0x60 - 0x7f:  a-z and tilde */
};

/*
 * Bitmap for printable ASCII characters (other than % and +).
 */
const u32 uri_printable_ascii_map[256 / 32] = {
	0,				/* 0x00 - 0x1f control characters */
	0xffffffff &			/* 0x20 - 0x3f printable except: */
	    ~(BIT('%' - 0x20) |		/*        '%' URI escape char and */
	      BIT('+' - 0x20)),		/*        '+' URI encoded space */
	0xffffffff,			/* 0x40 - 0x5f printable */
	0x7fffffff			/* 0x60 - 0x7e printable */
};
