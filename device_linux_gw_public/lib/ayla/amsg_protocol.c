/*
 * Copyright 2015-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/amsg_protocol.h>


DEF_NAME_TABLE(amsg_err_names, AMSG_ERR);

const uint8_t amsg_magic[] = AMSG_MAGIC;
const uint8_t amsg_version = AMSG_VERSION;

/*
 * Populate a message header from a message info structure.
 * Payload data pointed to by the info structure is not copied to memory
 * following the header because some messaging implementations may not want
 * to allocate the entire message contiguously.  Payload data should be sent
 * as needed.
 */
void amsg_populate_header(struct amsg_header *header,
	const struct amsg_msg_info *info)
{
	ASSERT(header != NULL);
	ASSERT(info != NULL);

	memset(header, 0, sizeof(*header));
	memcpy(header->magic, amsg_magic, sizeof(amsg_magic));
	header->version = amsg_version;
	header->flags = info->flags;
	header->interface = info->interface;
	header->type = info->type;
	header->sequence_num = htons(info->sequence_num);
	header->data_size = htonl(info->payload_size);
}

/*
 * Validate and parse the supplied buffer pointing to message data.
 * If the message is valid, the message info structure is populated from
 * the header data.  This function assumes the header and payload are in
 * contiguous memory, so if the payload is fragmented, be sure to update the
 * info->payload pointer flag appropriately.
 */
enum amsg_err amsg_parse_header(const void *buf, size_t buf_len,
	struct amsg_msg_info *info)
{
	struct amsg_header *header = (struct amsg_header *)buf;

	ASSERT(buf != NULL);
	ASSERT(info != NULL);

	if (buf_len < sizeof(struct amsg_header)) {
		return AMSG_ERR_MSG_TRUNCATED;
	}
	if (memcmp(header->magic, amsg_magic, sizeof(header->magic))) {
		return AMSG_ERR_MAGIC_BAD;
	}
	if (header->version != amsg_version) {
		return AMSG_ERR_VERSION_UNSUPPORTED;
	}
	info->flags = header->flags;
	info->interface = header->interface;
	info->type = header->type;
	info->sequence_num = ntohs(header->sequence_num);
	info->payload = header->data_size ? header->data : NULL;
	info->payload_size = ntohl(header->data_size);
	return AMSG_ERR_NONE;
}

/*
 * Return a statically allocated string describing the error code.
 */
const char *amsg_err_string(enum amsg_err err)
{
	if (err >= ARRAY_LEN(amsg_err_names)) {
		return "undefined error";
	}
	return amsg_err_names[err];
}
