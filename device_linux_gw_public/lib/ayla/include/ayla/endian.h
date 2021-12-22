/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_ENDIAN_H__
#define __AYLA_ENDIAN_H__

/*
 * Initializer for byte array in big-endian order from a 32-bit value.
 */
#define U32_BYTES(v) \
		(((v) >> 24) & 0xff), \
		(((v) >> 16) & 0xff), \
		(((v) >> 8) & 0xff), \
		((v) & 0xff)

/*
 * include utypes.h first.
 * Assuming little-endian (or unknown) for now.
 */

static inline void put_ua_be32(void *dest, u32 val)
{
	u8 *byte = dest;

	byte[0] = val >> 24;
	byte[1] = val >> 16;
	byte[2] = val >> 8;
	byte[3] = val;
}

static inline void put_ua_be64(void *dest, u64 val)
{
	u8 *byte = dest;

	byte[0] = val >> 56;
	byte[1] = val >> 48;
	byte[2] = val >> 40;
	byte[3] = val >> 32;
	byte[4] = val >> 24;
	byte[5] = val >> 16;
	byte[6] = val >> 8;
	byte[7] = val;
}

static inline void put_ua_be16(void *dest, u16 val)
{
	u8 *byte = dest;

	byte[0] = val >> 8;
	byte[1] = val;
}

static inline u32 get_ua_be32(const void *src)
{
	const u8 *byte = src;

	return ((u32)byte[0] << 24) | ((u32)byte[1] << 16) |
	    ((u32)byte[2] << 8) | byte[3];
}

static inline u64 get_ua_be64(const void *src)
{
	const u8 *byte = src;

	return ((u64)byte[0] << 56) | ((u64)byte[1] << 48) |
	    ((u64)byte[2] << 40) | ((u64)byte[3] << 32) |
	    ((u64)byte[4] << 24) | ((u64)byte[5] << 16) |
	    ((u64)byte[6] << 8) | byte[7];
}

static inline u16 get_ua_be16(const void *src)
{
	const u8 *byte = src;

	return ((u16)byte[0] << 8) | byte[1];
}

static inline le32 cpu_to_le32(u32 val)
{
#ifdef BIG_ENDIAN
	le32 word;
	u8 *byte = (void *)&word;

	byte[0] = val;
	byte[1] = val >> 8;
	byte[2] = val >> 16;
	byte[3] = val >> 24;
	return val;
#else
	return (le32)val;
#endif
}

/*
 * Convert network to host byte order using a potentially unaligned source
 * buffer, and put the output into a 64-bit signed integer.
 */
int endian_put_ntoh_s64(s64 *out, const void *in, size_t len);

/*
 * Convert network to host byte order using a potentially unaligned source
 * buffer, and put the output into a 64-bit unsigned integer.
 */
int endian_put_ntoh_u64(u64 *out, const void *in, size_t len);

#endif /* __AYLA_ENDIAN_H__ */
