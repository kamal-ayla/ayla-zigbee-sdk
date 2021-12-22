/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <string.h>
#include <endian.h>

#include <ayla/utypes.h>
#include <ayla/endian.h>

/*
 * Convert network to host byte order using a potentially unaligned source
 * buffer, and put the output into a 64-bit signed integer.
 */
int endian_put_ntoh_s64(s64 *out, const void *in, size_t len)
{
	u64 temp;
	u8 *buf = (u8 *)&temp;

	switch (len) {
	case 1:
		*out = *((s8 *)in);
		break;
	case 2:
		memcpy(buf, in, len);
		*out = be16toh(*((u16 *)buf));
		break;
	case 4:
		memcpy(buf, in, len);
		*out = be32toh(*((u32 *)buf));
		break;
	case 8:
		memcpy(buf, in, len);
		*out = be64toh(*((u64 *)buf));
		break;
	default:
		return -1;
	}
	return 0;
}

/*
 * Convert network to host byte order using a potentially unaligned source
 * buffer, and put the output into a 64-bit unsigned integer.
 */
int endian_put_ntoh_u64(u64 *out, const void *in, size_t len)
{
	u64 temp;
	u8 *buf = (u8 *)&temp;

	switch (len) {
	case 1:
		*out = *((u8 *)in);
		break;
	case 2:
		memcpy(buf, in, len);
		*out = be16toh(*((u16 *)buf));
		break;
	case 4:
		memcpy(buf, in, len);
		*out = be32toh(*((u32 *)buf));
		break;
	case 8:
		memcpy(buf, in, len);
		*out = be64toh(*((u64 *)buf));
		break;
	default:
		return -1;
	}
	return 0;
}
