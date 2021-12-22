/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <unistd.h>
#include <string.h>
#include <ayla/utypes.h>
#include <ayla/hex.h>
#include <ayla/uri_code.h>

/*
 * URI-decode (i.e. percent-decode) a string into a buffer.
 * Return the length of the decoded string.
 * If the buffer overflows, the return value will be -1.
 * Any invalid hex digits following a percent sign will also
 * cause an error return of -1.
 *
 * The resulting string will be NUL-terminated if there is room.
 * This is for the benefit of 32-byte SSIDs.
 */
ssize_t uri_decode_n(char *dest, size_t dest_len,
	const char *src, size_t src_len)
{
	ssize_t len = 0;
	u8 c;

	while ((c = *src++) != '\0' && src_len-- > 0) {
		if (c == '%') {
			src = hex_parse_byte(src, &c);
			if (!src) {
				return -1;
			}
		} else if (c == '+') {
			c = ' ';
		}
		if (dest_len-- == 0) {
			return -1;
		}
		*dest++ = c;
		len++;
	}
	if (dest_len) {
		*dest++ = '\0';
	}
	return len;
}

ssize_t uri_decode(char *dest, size_t dest_len, const char *src)
{
	return uri_decode_n(dest, dest_len, src, MAX_U32);
}
