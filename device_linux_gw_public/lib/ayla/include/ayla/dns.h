/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_DNS_H__
#define __AYLA_DNS_H__

/*
 * DNS protocol info from RFC1035.
 */
#define DNSS_PORT	53
#define MDNS_PORT	5353

struct dns_head {
	be16	id;		/* request identifier */
	be16	flags;		/* flags and opcode (see below) */
	be16	qdcount;	/* questions count */
	be16	ancount;	/* answers */
	be16	nscount;	/* name server sections */
	be16	arcount;	/* number of resource records */
} PACKED;

/*
 * DNS Header Flags.
 */
#define DNSF_QR		0x8000		/* query (0) or response (1) */
#define DNSF_OP_BIT	11		/* shift for opcode */
#define DNSF_OP_MASK	0xf		/* mask for opcode */
#define DNSF_AA		(1 << 10)	/* authoritative answer */
#define DNSF_TC		(1 << 9)	/* truncation */
#define DNSF_RD		(1 << 8)	/* recursion desired */
#define DNSF_RA		(1 << 7)	/* recursion available */
#define	DNSF_RCODE_MASK	0xf		/* mask for response code */

/*
 * Query code.
 */
enum dns_query {
	DNSQ_QUERY = 0,
	DNSQ_IQUERY = 1,
	DNSQ_STATUS = 2,
};

/*
 * Response code.
 */
enum dns_rcode {
	DNSR_OK = 0,
	DNSR_ERR_FMT = 1,	/* server unable to interpret query */
	DNSR_ERR_SERVER = 2,	/* server unable to process query */
	DNSR_ERR_NAME = 3,	/* name in query does not exist */
	DNSR_ERR_UNIMP = 4,	/* unimplimented query */
	DNSR_ERR_REFUSED = 5,	/* server refuses to answer */
};

#define DNSL_MASK	0x3f	/* mask for length in sections */
#define DNSL_PTR	0xc0	/* flag bits in count */

/*
 * Resource record (after variable-length name).
 */
struct dns_rr {
	be16 type;		/* RR type code */
	be16 class;		/* RR class */
	be32 ttl;		/* time to live, seconds */
	be16 rdlength;		/* resource data length */
	be16 rdata[2];		/* resource data (variable, 2 for IPv4 addr) */
} PACKED;

#endif /* __AYLA_DNS_H__ */
