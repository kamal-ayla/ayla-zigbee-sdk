/*
 * Copyright 2015-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#ifndef AMSG_H_
#define AMSG_H_

#include <sys/queue.h>
#include <stdint.h>

#include <ayla/token_table.h>

#ifndef AMSG_DATA_PACK
#define AMSG_DATA_PACK __attribute__((__packed__))
#endif

/*
 * Arbitrary 2-byte code identifying the beginning of a message header.
 * Magic code NEVER CHANGES.
 */
#define AMSG_MAGIC			{ 0x5f, 0xa0 }
/*
 * Current message protocol version.  Increment version if header format
 * changes.
 */
#define AMSG_VERSION			0x10
/*
 * Message flags indicating how to handle the message.
 * Up to 8 flags are supported in version 0x10 header.
 */
enum amsg_flags {
	AMSG_FLAGS_RESPONSE_REQUESTED	= 0x01,
	AMSG_FLAGS_RESPONSE		= 0x02,
	AMSG_FLAGS_SYNC			= 0x04
};

/*
 * Error codes
 */
#define AMSG_ERR(def)							\
	def(no error,			AMSG_ERR_NONE)			\
	def(bad magic,			AMSG_ERR_MAGIC_BAD)		\
	def(truncated message,		AMSG_ERR_MSG_TRUNCATED)		\
	def(unsupported version,	AMSG_ERR_VERSION_UNSUPPORTED)	\
	def(unsupported interface,	AMSG_ERR_INTERFACE_UNSUPPORTED)	\
	def(unsupported type,		AMSG_ERR_TYPE_UNSUPPORTED)	\
	def(unexpected sequence number,	AMSG_ERR_SEQUENCE_BAD)		\
	def(insufficient privileges,	AMSG_ERR_PRIVS)			\
	def(insufficient memory,	AMSG_ERR_MEM)			\
	def(disconnected,		AMSG_ERR_DISCONNECTED)		\
	def(socket error,		AMSG_ERR_SOCKET)		\
	def(corrupt data,		AMSG_ERR_DATA_CORRUPT)		\
	def(application error,		AMSG_ERR_APPLICATION)		\
	def(timed out,			AMSG_ERR_TIMED_OUT)		\
	def(interrupted,		AMSG_ERR_INTERRUPTED)		\
	def(appd exist,			AMSG_ERR_APPD_EXIST)


DEF_ENUM(amsg_err, AMSG_ERR);

/*
 * Definition of required header at start of message.
 */
struct amsg_header {
	uint8_t magic[2];	/* Message identification code */
	uint8_t version;	/* Header format version */
	uint8_t flags;		/* Message info flag */
	uint8_t interface;	/* Message interface type */
	uint8_t type;		/* Message type (unique within interface) */
	uint16_t sequence_num;	/* Message ID for tracking responses */
	uint32_t data_size;	/* Size of message payload */
	uint8_t data[0];	/* Start of message payload (if contiguous) */
} AMSG_DATA_PACK;

/*
 * Internal message interface type definition.  The internal interface is
 * guaranteed to be supported by all messaging endpoints.
 * Interface 0 is ALWAYS RESERVED for the internal interface.
 */
#define AMSG_INTERFACE_INTERNAL		0x00
/*
 * Message types defined in the internal interface.
 */
#define AMSG_TYPES_INTERNAL(def)				\
	def(default response,		AMSG_TYPE_DEFAULT_RESP)	\
	def(ping,			AMSG_TYPE_PING)		\
	def(ping response,		AMSG_TYPE_PING_RESP)

DEF_ENUM(amsg_type_basic, AMSG_TYPES_INTERNAL);

/*
 * Definition of DEFAULT_RESP message.
 * This message should be returned to the sender if a response was requested,
 * but a custom response message is not needed.
 */
struct amsg_msg_default_resp {
	uint8_t interface;	/* Original message interface */
	uint8_t type;		/* Original message type */
	uint16_t err;		/* Message status */
} AMSG_DATA_PACK;


/*
 * Structure to hold message info and payload.
 * This is used by public interface functions.
 */
struct amsg_msg_info {
	uint8_t flags;
	uint8_t interface;
	uint8_t type;
	uint16_t sequence_num;
	void *payload;
	size_t payload_size;
};

/*
 * Populate a message header from a message info structure.
 * Payload data pointed to by the info structure is not copied to memory
 * following the header because some messaging implementations may not want
 * to allocate the entire message contiguously.  Payload data should be sent
 * as needed.
 */
void amsg_populate_header(struct amsg_header *header,
	const struct amsg_msg_info *info);

/*
 * Validate and parse the supplied buffer pointing to message data.
 * If the message is valid, the message info structure is populated from
 * the header data.  This function assumes the header and payload are in
 * contiguous memory, so if the payload is fragmented, be sure to update the
 * info->payload pointer flag appropriately.
 */
enum amsg_err amsg_parse_header(const void *buf, size_t buf_len,
	struct amsg_msg_info *info);

/*
 * Return a statically allocated string describing the error code.
 */
const char *amsg_err_string(enum amsg_err err);


#endif /* AMSG_H_ */
