/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>

#include <ayla/assert.h>
#include <ayla/utypes.h>
#include <ayla/endian.h>
#include <ayla/file_event.h>
#include <ayla/dns.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>
#include <ayla/log.h>

#include "dnss.h"
#include "dapi.h"
#include "notify.h"
#include "ds.h"

#undef DNSS_DEBUG

#define DNSS_BACKLOG	2
#define DNSS_MAX_DEPTH	2	/* max recursion depth for msg compression */
#define DNSS_TTL	10	/* seconds time-to-live for responses */
#define DNSS_QNAME_LEN  128	/* query name buf len */

#define IPADDR_MULTICAST    ((224U << 24) | 251)  /* 224.0.0.251 */

/* set DNSS subsystem for all log calls in this file */
#undef log_base
#define log_base(func, level, ...)	\
	log_base_subsystem(func, level, LOG_SUB_DNSS, __VA_ARGS__)

struct dnss_state {
	int	mdns_sock;
};
static struct dnss_state dnss;

struct dnss_req_state {
	void *req;
	size_t req_len;
	void *resp;
	size_t resp_len;
	size_t off;		/* offset in request */
	size_t roff;		/* offset in response */
	struct in_addr local_ip;
};

/*
 * Add a reply to the response for the current request.
 */
static enum dns_rcode dnss_reply(struct dnss_req_state *rstate,
	u8 *query, size_t query_len, int is_mdns)
{
	u8 *bp;
	struct dns_head *head;
	struct dns_rr *rr;
	size_t roff;
	size_t next_roff;

	head = rstate->resp;
	roff = rstate->roff;
	next_roff = roff + query_len + sizeof(*rr);
	if (next_roff > rstate->resp_len) {
		log_err("resp too short");
		return DNSR_ERR_FMT;	/* we didn't allocate enough */
	}
	bp = (u8 *)head;
	memcpy(bp + roff, query, query_len);

	/*
	 * Note that resource record may be unaligned.
	 */
	rr = (struct dns_rr *)(bp + roff + query_len);
	put_ua_be16(&rr->type, ns_t_a);
#if DNSS_TTL
	put_ua_be32(&rr->ttl, DNSS_TTL);
#endif
	put_ua_be16(&rr->rdlength, sizeof(be32));
	put_ua_be16(&rr->class, ns_c_in);

#ifdef DNSS_DEBUG
	log_debug("response IP %s", inet_ntoa(rstate->local_ip));
#endif /* DNSS_DEBUG */

	memcpy(rr->rdata, &rstate->local_ip, sizeof(rstate->local_ip));
	rstate->roff = next_roff;
	head->ancount = htons(ntohs(head->ancount) + 1);
	return DNSR_OK;
}

/*
 * Handle DNS query item inside request.
 * bp is pointer to header at start of request.
 * len is size of overall request.
 * offp is pointer to offset of the next query item.  It us updated on return.
 * resp is pointer to buf for response.
 * roffp is pointer to offset of next reply in response.
 */
static enum dns_rcode dnss_query(struct dnss_req_state *rstate, int is_mdns)
{
	struct device_state *dev = &device;
	enum dns_rcode rcode;
	u8 *bp = rstate->req;
	size_t len = rstate->req_len;
	size_t off;
	size_t tlen;
	u16 qtype;
	u16 qclass;
	char name[DNSS_QNAME_LEN] = "\0";
	char hostname[25];
	size_t nlen = 0;

	for (off = rstate->off; off < len; off += tlen) {
		tlen = bp[off++];
		if (tlen == 0) {
			goto tail;
		}

		/*
		 * TBD: Handle pointer.
		 */
		if (tlen > DNSL_MASK) {
			if (off >= len) {
				return DNSR_ERR_FMT;
			}
			tlen = (tlen & DNSL_MASK << 8) | bp[off++];
			log_debug("ptr detected off %zx len %zx",
			    off - 2, tlen);
			return DNSR_ERR_FMT;
		}
		if (off + tlen >= len) {
			log_debug("query: bad len %zu", tlen);
			return DNSR_ERR_FMT;
		}
		if (nlen + tlen < sizeof(name) - 2) {
			memcpy(name + nlen, bp + off, tlen);
			nlen += tlen;
			name[nlen++] = '.';
			name[nlen] = '\0';
		}
	}
	return DNSR_ERR_FMT;		/* no null termination */

	/*
	 * Look for qtype and qclass.
	 */
tail:
	if (off + 2 * sizeof(be16) > len) {
		return DNSR_ERR_FMT;
	}
	qtype = get_ua_be16(bp + off);
	qclass = get_ua_be16(bp + off + sizeof(be16));
#ifdef DNSS_DEBUG
	log_debug("query type 0x%x class 0x%x \"%s\"", qtype, qclass, name);
#endif /* DNSS_DEBUG */
	/* TBD: handle qtypes PTR and text */
	if ((qtype != ns_t_a && qtype != ns_t_any) || qclass != ns_c_in) {
		log_debug("err qtype %x qclass %x", qtype, qclass);
		return DNSR_ERR_UNIMP;
	}
	snprintf(hostname, sizeof(hostname), "%s.local.", dev->dsn);
	if (is_mdns) {
		if (strcasecmp(name, hostname)) {
			return DNSR_ERR_NAME;
		}
		log_debug("host mdns query");
	}
	rcode = dnss_reply(rstate, bp + rstate->off,
	    off - rstate->off, is_mdns);
	rstate->off = off + 2 * sizeof(be16);

	return rcode;
}

/*
 * Handles dnss messages.
 */
static void dnss_recv(void *arg, int sock)
{
	struct device_state *dev = &device;
	long is_mdns = (long)arg;
	struct dnss_req_state rstate;
	struct dns_head *head;
	struct dns_head *rhead;
	u8 *resp = NULL;
	enum dns_rcode rcode;
	u16 qdcount;
	u16 flags;
	struct sockaddr_in from;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct cmsghdr *pkt_cmsg = NULL;
	struct in_pktinfo *pktinfo = NULL;
	struct iovec iov[1];
	size_t len;
	size_t resp_len = 0;
	u8 buf[256];
	u8 cbuf[64];
	int rc;

	memset(&rstate, 0, sizeof(rstate));
	memset(&msg, 0, sizeof(msg));
	memset(&from, 0, sizeof(from));
	memset(&iov, 0, sizeof(iov));

	iov[0].iov_base = buf;
	iov[0].iov_len = sizeof(buf);

	msg.msg_name = &from;
	msg.msg_namelen = sizeof(from);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	rc = recvmsg(sock, &msg, 0);
	if (rc <= 0) {
		log_warn("recvmsg failed: %m");
		return;
	}
	len = rc;

	/*
	 * Look for an IP_PKTINFO control message indicating which local IP
	 * belongs to the interface on which this message was received.
	 */
	pktinfo = NULL;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_PKTINFO &&
		    cmsg->cmsg_len >= sizeof(struct in_pktinfo)) {
			pkt_cmsg = cmsg;
			pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
			rstate.local_ip = pktinfo->ipi_spec_dst;
			continue;
		}
	}

	if (!pktinfo) {
		log_warn("PKTINFO missing");
		rstate.local_ip = dev->lan_ip;	/* default local address */
	}

	if (len < sizeof(*head)) {
		log_warn("len %zd too short", len);
		return;
	}
	head = (struct dns_head *)buf;
	flags = ntohs(head->flags);
	if (flags & DNSF_QR) {
#ifdef DNSS_DEBUG
		log_debug("non-request: QR off");
#endif /* DNSS_DEBUG */
		rcode = DNSR_ERR_FMT;
		goto error;
	}
	qdcount = ntohs(head->qdcount);
	if (!qdcount || head->ancount || head->nscount || head->arcount) {
#ifdef DNSS_DEBUG
		log_debug("non-request: no questions");
#endif /* DNSS_DEBUG */
		rcode = DNSR_ERR_SERVER;
		goto error;
	}

	if (is_mdns && qdcount > 1) {
		/* Restrict the type of MDNS messages to 1 question only.
		 * We can relax this req if needed in the future.
		*/
#ifdef DNSS_DEBUG
		log_warn("> 1 question not supported");
#endif /* DNSS_DEBUG */
		return;
	}

	/*
	 * Allocate response and clear its payload.
	 */
	resp_len = len + sizeof(struct dns_rr) * qdcount;
	resp = calloc(1, resp_len);
	if (!resp) {
		rcode = DNSR_ERR_SERVER;
		goto error;
	}

	/*
	 * Parse questions, form response.
	 */
	rstate.req = buf;
	rstate.req_len = len;
	rstate.resp = resp;
	rstate.resp_len = resp_len;
	rstate.off = sizeof(*head);
	rstate.roff = sizeof(*head);
	while (qdcount-- > 0) {
		rcode = dnss_query(&rstate, is_mdns);
		if (rcode != DNSR_OK) {
			if (is_mdns) {
				goto drop_resp;
			} else {
				goto error;
			}
		}
	}

	/*
	 * Set up response header.
	 */
	rhead = (struct dns_head *)resp;
	if (is_mdns) {
		rhead->flags =
		    htons(DNSF_QR | (DNSQ_IQUERY << (DNSF_OP_BIT - 1)));
	} else {
		rhead->flags = htons(DNSF_QR | (DNSQ_QUERY << DNSF_OP_BIT));
	}

	/*
	 * Send response.
	 */
send:
	iov[0].iov_base = resp;
	iov[0].iov_len = resp_len;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &from;
	msg.msg_namelen = sizeof(from);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	if (pkt_cmsg) {
		pktinfo->ipi_addr.s_addr = 0;
		msg.msg_control = (char *)pkt_cmsg;
		msg.msg_controllen = pkt_cmsg->cmsg_len;
	}

	rhead->id = head->id;
	rc = sendmsg(sock, &msg, 0);
	if (rc < 0) {
		log_err("sendmsg err: %m");
	}
drop_resp:
	free(resp);
	return;

error:
	log_debug("replying rcode %x", rcode);
	if (!resp) {
		return;
	}
	rhead = (struct dns_head *)resp;
	rhead->flags = htons(DNSF_QR | (DNSQ_QUERY << DNSF_OP_BIT) | rcode);
	rhead->ancount = 0;
	resp_len = sizeof(*rhead);
	goto send;
}

/*
 * dnss_up - start DNS service.
 */
static int dnss_sock_open(u32 bind_addr, u16 bind_port)
{
	struct sockaddr_in sa;
	int sock;
	int rc;
	int opt;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		log_err("socket failed: %m");
		return sock;
	}

	sa.sin_family = AF_INET;
	sa.sin_port = htons(bind_port);
	sa.sin_addr.s_addr = htonl(bind_addr);

	rc = bind(sock, (struct sockaddr *)&sa, sizeof(sa));
	if (rc < 0) {
		log_err("bind failed: %m");
		close(sock);
		return rc;
	}

	opt = 1;
	rc = setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt));
	if (rc < 0) {
		log_err("setsockopt failed: %m");
		close(sock);
		return rc;
	}
	return sock;
}

/*
 * dnss_mdns_up - start MDNS service.
 */
void dnss_mdns_up(u32 ifaddr)
{
	struct device_state *dev = &device;
	struct dnss_state *state = &dnss;
	struct ip_mreqn mreq;
	int sock;
	int rc;

	mreq.imr_multiaddr.s_addr = htonl(IPADDR_MULTICAST);
	mreq.imr_address.s_addr = ifaddr;
	mreq.imr_ifindex = 0;

	sock = dnss_sock_open(INADDR_ANY, AYLA_MDNS_PORT);
	if (sock < 0) {
		return;
	}

	rc = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
	    &mreq, sizeof(mreq));
	if (rc < 0) {
		log_warn("failed join group: %m");
		close(sock);
		return;
	}
	state->mdns_sock = sock;
	file_event_reg(&dev->file_events, sock, dnss_recv, NULL, (void *)1);
}

void dnss_mdns_down(void)
{
	struct device_state *dev = &device;
	struct dnss_state *state = &dnss;
	int sock;

	sock = state->mdns_sock;
	if (sock >= 0) {
		state->mdns_sock = -1;
		file_event_unreg(&dev->file_events, sock,
		    dnss_recv, NULL, (void *)1);
		close(sock);
	}
}
