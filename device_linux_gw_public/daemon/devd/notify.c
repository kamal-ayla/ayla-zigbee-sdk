/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>

#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/aes.h>

#include <ayla/utypes.h>
#include <ayla/crc.h>
#include <ayla/notify_proto.h>
#include <ayla/assert.h>
#include <ayla/time_utils.h>
#include <ayla/timer.h>
#include <ayla/json_parser.h>
#include <ayla/conf_io.h>
#include <ayla/ayla_interface.h>
#include <ayla/log.h>

#include "notify.h"
#include "dapi.h"
#include "ds.h"
#include "devd_conf.h"

#ifdef NP_DEBUG_LOCAL
#define NP_DEBUG_KEY	"0123456789012345"
#define NP_DEBUG_IP	"172.17.2.100"
#endif /* NP_DEBUG_LOCAL */

#define NP_CLIENT_PORT_N 2	/* # of open ports for notify */
#define REG_LINE 0
#define PROBE_LINE 1

#define NP_NOTIFY_BUFLEN 128	/* max pkt size for sending to ANS */

#define NP_KA_MARGIN	3	/* secs before probed value to do keep-alive */
#define NP_MIN_CHANGE_WAIT 1	/* secs, min wait to re-request ANS server */
#define NP_CHANGE_WAIT_MULT 2	/* exponential backoff multiplier */
#define NP_MAX_CHANGE_WAIT 600	/* secs, max change wait */
#define NP_RETRY_INTERVAL 60

/* default # of secs between ans reachability check */
#define NOTIFY_POLL_DEFAULT	300


enum notify_client_state {
	NCS_DOWN = 0,		/* not started or fatal error */
	NCS_DNS_PASS,		/* not started, DNS lookup succeeded */
	NCS_REGISTER,		/* register sent, waiting for response */
	NCS_UP,			/* up, sending keep-alives */
	NCS_CHANGE_WAIT,	/* wait before asking for a new ANS server */
	NCS_DOWN_RETRY,		/* ANS down, in retry loop (until GET dsns) */
};

enum notify_probe_state {
	NCS_PROBE_DOWN = 0,
	NCS_PROBE_SEND,
	NCS_PROBE_WAIT,
	NCS_PROBE_IDLE,
};

u32 np_poll_default = NOTIFY_POLL_DEFAULT;

struct notify_state {
	int sock[NP_CLIENT_PORT_N];
	enum notify_client_state client_state;
	char	notify_host[40];		/* notify server name */
	u16	sequence;
	u16	keep_alive_delay;		/* period for keep-alives */
	be32	reg_key;

	/*
	 * Probe-related state.
	 */
	enum notify_probe_state probe_state;
	u16	probe_delay;			/* trial period for probes */
	u16	min_probe;
	u16	max_probe;
	u16	change_wait;
	u8	probes_needed;

	u8	probe_send:1;	/* send a probe instead of a keep-alive */
	u8	restart_probing:1;	/* restart probing when conn resumes */
	u64	probe_time;	/* time for the next probe */
	u32	poll_interval;	/* time between probes */

	u16	reg_seq;	/* sequence number for reg and probe requests */
	u16	probe_seq;	/* sequence number for probes and keep-alives */
	u8	reg_attempts;	/* UDP sends left for reg and probe reqs */
	u8	probe_attempts;	/* UDP sends left for probes and keep-alives */

	void	(*notify)(enum notify_event);
	u8	dns_cb_pending:1;
	struct timer callback;
	struct timer gen_timer;
	struct timer probe_timer;

	u8 *aes_key;
	size_t aes_key_len;
	u8	iv[NP_IV_LEN];
	struct sockaddr_in serv_sa;
};

#ifdef NP_DEBUG
static const char *np_op_name(enum np_op op)
{
	static const char * const np_ops[] = NP_OPS;

	if (op < ARRAY_LEN(np_ops) && np_ops[op]) {
		return np_ops[op];
	}

	return "unknown";
}
#endif /* NP_DEBUG */

static struct notify_state notify_state;

static void np_down(struct notify_state *);
static void np_probe_timeout(struct timer *);
static void np_timeout(struct timer *);
static void np_reset_keep_alive(struct notify_state *);
static int np_send(struct notify_state *, int, void *, int);
static int np_sock_open(int *sockfd);
static void np_socket_recv(void *arg, int sock);

/*
 * Allocate and set up header for new packet (not for DSN use).
 * Size includes the encapsulation header.
 */
static void np_init_head(struct np_head *head, enum np_op op, u16 seq)
{
#if NP_VERS != 0
	head->ver = NP_VERS;
#endif
	head->op = op;
	head->seq = htons(seq);
	head->time = htonl((u32)time_mtime_ms());
}

/*
 * FMT_IV_KEY encapsulation.
 */
static int np_new_packet_key(void *pbuf, size_t size)
{
	struct notify_state *state = &notify_state;
	struct np_encaps_key *encaps;
	u8 crc;

	size += sizeof(crc) - sizeof(struct np_encaps_key);
	size += -size & (NP_PAD - 1);
	size += sizeof(struct np_encaps_key);
	if (size > NP_NOTIFY_BUFLEN) {
		log_err("len + padding too large");
	}
	encaps = pbuf;
	encaps->format = NP_FMT_IV_KEY;
	encaps->reg_key = state->reg_key;
	return size;
}

static int np_check_resp(struct notify_state *state, void *buf, int len,
	size_t min_len, enum notify_client_state exp_state, u16 sequence)
{
	struct np_encaps_key *encaps = buf;
	struct np_head *head = (struct np_head *)(encaps + 1);

	if (len < min_len) {
		log_warn("too short %u", len);
		return -1;
	}
	if (ntohs(head->seq) != sequence) {
		log_warn("wrong seq %u exp %u", ntohs(head->seq), sequence);
		return -1;
	}
	if (state->client_state != exp_state) {
		log_warn("wrong state %d", state->client_state);
		return -1;
	}
	if (state->reg_key && state->reg_key != encaps->reg_key) {
		log_warn("wrong key %x exp %x state %d",
		    encaps->reg_key, state->reg_key, state->client_state);
		return -1;
	}
	return 0;
}

static void np_post_event(struct notify_state *state)
{
	timer_set(&device.timers, &state->callback, 0);
}

/*
 * Reset timer for next reach check
 */
static void np_setup_next_reach_check(struct notify_state *state)
{
	timer_set(&device.timers, &state->gen_timer,
	    state->poll_interval * 1000);
	log_info("ANS reachability check in %u seconds", state->poll_interval);
}

static void np_probe_down(struct notify_state *state)
{
	struct device_state *dev = &device;

	state->probe_state = NCS_PROBE_DOWN;
	timer_cancel(&dev->timers, &state->probe_timer);
	file_event_unreg(&dev->file_events, state->sock[PROBE_LINE],
	    np_socket_recv, NULL, NULL);
	if (state->sock[PROBE_LINE] >= 0) {
		/* don't close a socket that hasn't been opened */
		close(state->sock[PROBE_LINE]);
		state->sock[PROBE_LINE] = -1;
	}
}

static void np_down_retry_event(struct notify_state *state)
{
	if (state->client_state == NCS_UP) {
		if (state->probe_state != NCS_PROBE_IDLE) {
			state->restart_probing = 1;
		} else {
			state->restart_probing = 0;
		}
	}
	state->probe_send = 0;
	state->client_state = NCS_DOWN_RETRY;
	state->poll_interval = NP_RETRY_INTERVAL;
	np_probe_down(state);
	np_setup_next_reach_check(state);
	np_post_event(state);
}

static void np_down(struct notify_state *state)
{
	struct device_state *dev = &device;

	if (state->client_state == NCS_UP) {
		log_info("ANS down");
	}
	state->client_state = NCS_DOWN;
	state->probe_send = 0;
	timer_cancel(&dev->timers, &state->gen_timer);
	file_event_unreg(&dev->file_events, state->sock[REG_LINE],
	    np_socket_recv, NULL, NULL);
	if (state->sock[REG_LINE] >= 0) {
		/* don't close a socket that hasn't been opened */
		close(state->sock[REG_LINE]);
		state->sock[REG_LINE] = -1;
	}
	np_probe_down(state);
}

static void np_probe_init(struct notify_state *state)
{
	state->probe_delay = NP_INIT_PROBE;
	state->min_probe = NP_MIN_PROBE;
	state->max_probe = NP_MAX_PROBE;
	state->probes_needed = NP_INIT_PROBES;
}

static void np_req_probe(struct notify_state *state)
{
	unsigned char pbuf[NP_NOTIFY_BUFLEN];
	int sock = state->sock[PROBE_LINE];
	struct np_req_probe *probe;
	int len;
	int rc;

	ASSERT(NP_NOTIFY_BUFLEN >= sizeof(*probe));

	if (!state->reg_attempts) {
		np_probe_down(state);
		return;
	}
	state->reg_attempts--;

	len = np_new_packet_key(pbuf, sizeof(*probe));
	probe = (struct np_req_probe *)pbuf;
	np_init_head(&probe->head, NP_REQ_PROBE, state->reg_seq);
	probe->probe_delay = htons(state->probe_delay);

#ifdef NP_DEBUG
	log_debug("sending #%u probe request, delay %us, min %u max %u",
	    1 + NP_INIT_PROBES - state->probes_needed,
	    state->probe_delay, state->min_probe, state->max_probe);
#endif /* NP_DEBUG */

	rc = np_send(state, sock, pbuf, len);
	if (rc < 0) {
		np_down_retry_event(state);
		return;
	}
	timer_set(&device.timers, &state->probe_timer, NP_RESP_WAIT * 1000);
	state->probe_state = NCS_PROBE_SEND;
}

static void np_req_probe_new(struct notify_state *state)
{
	u16 probe_delay;

	/*
	 * If min_probe + 12.5% is less than max_probe, we're done probing.
	 */
	if (state->min_probe + state->min_probe / 8 >= state->max_probe) {
		state->probe_delay = state->min_probe;
		state->keep_alive_delay = state->min_probe;
		state->probe_state = NCS_PROBE_IDLE;
		log_debug("probe state set to idle, keep-alive %us",
		    state->keep_alive_delay);
		np_reset_keep_alive(state);
		return;
	}

	/*
	 * Either double the probe or take the average between min and max.
	 */
	probe_delay = (state->min_probe + state->max_probe) / 2;
	if (probe_delay > state->probe_delay * 2) {
		probe_delay = state->probe_delay * 2;
	}
	state->probe_delay = probe_delay;

	if (state->keep_alive_delay < state->min_probe) {
		state->keep_alive_delay = state->min_probe;
		np_reset_keep_alive(state);
	}
	state->probes_needed = NP_INIT_PROBES;
	state->reg_attempts = NP_MAX_TRY;
	state->reg_seq = state->sequence++;
	np_req_probe(state);
}

static void np_probe_wait(struct notify_state *state)
{
	timer_set(&device.timers, &state->probe_timer,
	    (NP_PROBE_GRACE + state->probe_delay) * 1000);
	state->probe_state = NCS_PROBE_WAIT;
}

static void np_recv_req_probe_resp(struct notify_state *state,
	void *pbuf, int len)
{
	struct np_resp *resp = pbuf;

	if (np_check_resp(state, pbuf, len, sizeof(*resp), NCS_UP,
	    state->reg_seq)) {
		return;
	}
	if (state->probe_state != NCS_PROBE_SEND) {
		return;
	}
	if (resp->error != NP_ERR_NONE) {
		log_warn("resp error %d", resp->error);
		np_down_retry_event(state);
		return;
	}
	np_probe_wait(state);
}

static void np_setup_probe_send(struct notify_state *state)
{
	u64 curtime = time_mtime_ms();
	u32 next_timeout;

	/* Send probe to check for ANS reachability */
	state->probe_send = 1;
	/* don't retry if notify is not up */
	state->probe_attempts = (state->client_state == NCS_UP) ?
	    NP_MAX_TRY : 1;
	/* fix the probe sequence to use for all the probes */
	state->probe_seq = state->sequence++;
	if (state->probe_time < curtime) {
		next_timeout = 0;
	} else {
		next_timeout = state->probe_time - curtime;
	}
	timer_set(&device.timers, &state->gen_timer, next_timeout);
}

static void np_set_next_np_timeout(struct notify_state *state, int correction)
{
	u32 next_timeout;
	u64 curtime = time_mtime_ms();
	u64 next_keep_alive_time;

	next_keep_alive_time = curtime +
	    (state->keep_alive_delay - NP_KA_MARGIN + correction) * 1000;

	if (state->probe_send) {
		next_timeout = (NP_RESP_WAIT + correction) * 1000;
	} else if (state->probe_time < next_keep_alive_time) {
		np_setup_probe_send(state);
		return;
	} else {
		if (state->keep_alive_delay < NP_KA_MARGIN - correction) {
			next_timeout = 0;
		} else {
			next_timeout = (state->keep_alive_delay - NP_KA_MARGIN +
			    correction) * 1000;
		}
	}
	timer_set(&device.timers, &state->gen_timer, next_timeout);
}

/*
 * Registration success or probe succcess.
 */
static void np_bring_up(struct notify_state *state)
{
	state->change_wait = 0;
	state->client_state = NCS_UP;
	state->keep_alive_delay = state->probe_delay;
	log_info("ANS Up");
	np_reset_keep_alive(state);
	np_post_event(state);
}

static void np_recv_probe_resp(struct notify_state *state,
		void *pbuf, int len)
{
	struct np_resp *resp = pbuf;

	if (len < sizeof(*resp)) {
		log_warn("len %u too short", len);
		return;
	}
	if (!state->probe_send ||
	    state->probe_seq != ntohs(resp->head.seq)) {
		log_warn("response without probe");
		return;
	}
#ifdef NP_DEBUG
	log_debug("state->client_state %d", state->client_state);
#endif /* NP_DEBUG */
	state->probe_send = 0;
	state->probe_time = time_mtime_ms() + state->poll_interval * 1000;
	if (state->client_state == NCS_DOWN_RETRY) {
		state->poll_interval = np_poll_default;
		if (state->restart_probing) {
			if (state->sock[PROBE_LINE] < 0 &&
			    np_sock_open(&state->sock[PROBE_LINE])) {
				np_down_retry_event(state);
				return;
			}
			np_probe_init(state);
			np_probe_wait(state);
		} else {
			state->probe_state = NCS_PROBE_IDLE;
		}
		np_bring_up(state);
		return;
	}

	/* need NP_RESP_WAIT correction to account for probe resp time */
	np_set_next_np_timeout(state, NP_RESP_WAIT * -1);
}

static void np_recv_event(struct notify_state *state, void *pbuf)
{
	unsigned char resp_pbuf[NP_NOTIFY_BUFLEN];
	struct np_event *event = pbuf;
	struct np_resp *resp;
	int len;

	/*
	 * Caller has verified pbuf contains np_head.
	 * Acknowledge event.
	 */
	ASSERT(NP_NOTIFY_BUFLEN >= sizeof(*resp));
	len = np_new_packet_key(resp_pbuf, sizeof(struct np_resp));
	resp = (struct np_resp *)resp_pbuf;
	np_init_head(&resp->head, NP_NOTIFY_RESP, ntohs(event->head.seq));
	np_send(state, state->sock[REG_LINE], resp_pbuf, len);
	np_post_event(state);
}

/*
 * Handle Probe.
 * Caller has verified pbuf contains np_head.
 */
static void np_recv_probe(struct notify_state *state, void *pbuf, int sock)
{
	unsigned char resp_pbuf[NP_NOTIFY_BUFLEN];
	struct np_probe *probe = pbuf;
	struct np_resp *resp;
	int len;

	ASSERT(NP_NOTIFY_BUFLEN >= sizeof(*resp));
#ifdef NP_DEBUG
	log_debug("seq %u", ntohs(probe->head.seq));
#endif /* NP_DEBUG */

	/*
	 * Acknowledge probe.
	 */
	len = np_new_packet_key(resp_pbuf, sizeof(*resp));
	resp = (struct np_resp *)resp_pbuf;
	np_init_head(&resp->head, NP_PROBE_RESP, ntohs(probe->head.seq));
	resp->error = NP_ERR_NONE;
	np_send(state, sock, resp_pbuf, len);

	if (state->probe_state != NCS_PROBE_WAIT ||
	    ntohs(probe->head.seq) != state->reg_seq) {
		return;
	}

	if (state->probes_needed > 1) {
		state->probes_needed--;
		state->reg_attempts = NP_MAX_TRY;
		state->reg_seq = state->sequence++;
		np_req_probe(state);
	} else {
		log_debug("new min probe delay: %us", state->probe_delay);
		state->min_probe = state->probe_delay;
		np_req_probe_new(state);
	}
}

static void np_probe_timeout(struct timer *timer)
{
	struct notify_state *state =
	    CONTAINER_OF(struct notify_state, probe_timer, timer);

	switch (state->probe_state) {
	case NCS_PROBE_SEND:
		np_req_probe(state);
		break;
	case NCS_PROBE_WAIT:
		log_debug("new max probe delay: %us", state->probe_delay);
		state->max_probe = state->probe_delay;
		np_req_probe_new(state);
		break;
	default:
		break;
	}
}

/*
 * Create the SHA-1 signature of the payload and key for the payload.
 */
static void np_sign(struct notify_state *state,
		const void *payload, size_t len, void *sign)
{
	SHA_CTX sha_ctx;

	SHA1_Init(&sha_ctx);
	SHA1_Update(&sha_ctx, (void *) payload, len);
	SHA1_Update(&sha_ctx, (void *) state->aes_key,
	    state->aes_key_len);
	SHA1_Final((unsigned char *) sign, &sha_ctx);
}

/*
 * Registration message doesn't get encrypted because its how we get our
 * key and the notify protocol doesn't know who we are yet.  We do include
 * a signature, however, to verify that we know the key.
 */
static void np_register(struct notify_state *state)
{
	struct device_state *dev = &device;
	unsigned char pbuf[NP_NOTIFY_BUFLEN];
	struct np_encaps_dsn *encaps;
	struct np_register *req;
	size_t dsn_len;
	size_t len;
	size_t pad;
	int rc;

	if (!state->reg_attempts) {
		np_down_retry_event(state);
		return;
	}
	state->reg_attempts--;

	len = strlen(dev->dsn) + 1;
	pad = 0;
	if (len & 3) {
		pad = 4 - (len & 3);
		len += pad;
	}
	if (len == 0 || len > 255) {
		log_err("dev id too long");
		np_down_retry_event(state);
		return;
	}

	state->reg_key = 0;

	dsn_len = len;
	len += sizeof(*encaps) + sizeof(*req) + sizeof(struct np_sig);
	encaps = (struct np_encaps_dsn *)pbuf;
	encaps->error = NP_ERR_NONE;
	encaps->format = NP_FMT_DSN;
	encaps->dsn_len = dsn_len;
	memcpy(encaps + 1, dev->dsn, dsn_len - pad);

	req = (struct np_register *)(pbuf + sizeof(*encaps) + dsn_len);
	np_init_head(&req->head, NP_REG, state->reg_seq);
	req->probe_delay = htons(state->probe_delay);

	len -= sizeof(struct np_sig);
	np_sign(state, pbuf, len, pbuf + len);
	len += sizeof(struct np_sig);

#ifdef NP_DEBUG
	log_debug("register with probe request, delay %us, seq %u",
	    state->probe_delay, state->reg_seq);
#endif

	rc = np_send(state, state->sock[REG_LINE], pbuf, len);
	if (rc < 0) {
		np_down_retry_event(state);
		return;
	}
	state->client_state = NCS_REGISTER;
	timer_set(&device.timers, &state->gen_timer, NP_RESP_WAIT * 1000);
}

/*
 * Re-register to recover from an error indicating ANS has lost the
 * client's registration.  Resets probe state.
 */
static void np_reregister(struct notify_state *state)
{
	struct device_state *dev = &device;

	if (state->client_state == NCS_DOWN ||
	    state->client_state == NCS_DNS_PASS ||
	    state->client_state == NCS_REGISTER) {
		return;
	}

	/* Reset probing if in progress */
	if (state->probe_state != NCS_PROBE_IDLE) {
		state->probe_state = NCS_PROBE_DOWN;
		timer_cancel(&dev->timers, &state->probe_timer);
		np_probe_init(state);
	}
	/* Immediately revert to register state */
	state->probe_send = 0;
	state->reg_attempts = NP_MAX_TRY;
	state->reg_seq = state->sequence++;
	state->client_state = NCS_REGISTER;
	timer_set(&dev->timers, &state->gen_timer, 0);
}

static void np_recv_reg_resp(struct notify_state *state, void *pbuf,
			    int len)
{
	struct np_reg_resp *resp;
	u16 ka_period;

	resp = pbuf;
	if (np_check_resp(state, pbuf, len, sizeof(*resp), NCS_REGISTER,
	    state->reg_seq)) {
		log_warn("resp data error");
		return;
	}
	if (resp->error != NP_ERR_NONE) {
		log_warn("resp error %d", resp->error);
		np_down_retry_event(state);
		return;
	}
#ifdef NP_DEBUG
	log_debug("state->client_state %d", state->client_state);
#endif /* NP_DEBUG */
	ka_period = ntohs(resp->ka_period);
	if (ka_period > NP_KA_MARGIN) {
		state->keep_alive_delay = ka_period;
	}
	state->reg_key = resp->encaps.reg_key;
	np_bring_up(state);
	np_probe_wait(state);
}

/*
 * Notify client to check for ANS server change.
 * Wait a bit first.  Each time we do this, wait longer.
 */
static void np_change(struct notify_state *state)
{
	state->client_state = NCS_CHANGE_WAIT;
	state->probe_send = 0;
	if (state->change_wait == 0) {
		state->change_wait = NP_MIN_CHANGE_WAIT;
	} else {
		state->change_wait *= NP_CHANGE_WAIT_MULT;
		if (state->change_wait > NP_MAX_CHANGE_WAIT) {
			state->change_wait = NP_MAX_CHANGE_WAIT;
		}
	}
	timer_set(&device.timers, &state->gen_timer,
	    state->change_wait * 1000);
}

/*
 * An error response was received for a registration.
 * The response is in format NS_FMT_DSN_ERR.
 * The payload should be the same as our request.
 */
static void np_recv_reg_err(struct notify_state *state, void *pbuf,
			    int inlen)
{
	struct np_encaps_dsn *encaps;
	struct np_register *reg;
	struct np_sig sig;
	size_t len;

	len = sizeof(*encaps) + sizeof(*reg) + sizeof(sig);
	if (inlen < len) {
		log_warn("payload too small: len %u", inlen);
		return;
	}
	encaps = pbuf;
	len += encaps->dsn_len;
	if (inlen < len) {
		log_warn("len %u dsn_len %u", inlen, encaps->dsn_len);
		return;
	}
	if (encaps->dsn[encaps->dsn_len - 1] != '\0') {
		log_warn("dsn not terminated");
		return;
	}
	if (strcmp(encaps->dsn, device.dsn)) {
		log_warn("dsn mismatch %s", encaps->dsn);
		return;
	}
	reg = pbuf + sizeof(*encaps) + encaps->dsn_len;
	if (reg->head.ver != NP_VERS || reg->head.op != NP_REG) {
		log_warn("ver/op mismatch ver %u op %u",
		    reg->head.ver, reg->head.op);
		return;
	}
	if (ntohs(reg->head.seq) != state->reg_seq) {
		log_warn("seq mismatch");
		return;
	}
	/*
	 * Check signature.
	 * A bad signature indicates a replay DOS attack or an old response.
	 */
	encaps->format = NP_FMT_DSN;	/* reset format so signature matches */
	np_sign(state, pbuf, len - sizeof(sig), &sig);
	if (memcmp(reg + 1, &sig, sizeof(sig))) {
		log_warn("bad signature");
		return;
	}
	log_debug("registration error");
	np_change(state);
}

/*
 * AES encryption using openssl lib
 */
void np_aes_encryp(struct notify_state *state, void *start, int elen,
		    void *iv, void *out)
{
	AES_KEY aes_key;
	int diffaddr = 1;
	unsigned char *tmp = out;

	if ((char *)out == (char *)start) {
		tmp = (unsigned char *) calloc(elen, sizeof(char));
		REQUIRE(tmp, REQUIRE_MSG_ALLOCATION);
		diffaddr = 0;
	}

	AES_set_encrypt_key(state->aes_key, state->aes_key_len * 8,
	    &aes_key);

	AES_cbc_encrypt((unsigned char *) start, (unsigned char *) tmp, elen,
	    &aes_key, (unsigned char *) iv, AES_ENCRYPT);

	if (!diffaddr) {
		memcpy((char *) start, tmp, elen);
		free(tmp);
	}
}

/*
 * Send NP packet after encrypting it.
 * This frees the pbuf even if error occurs.
 */
static int np_send(struct notify_state *state, int sock,
		    void *pbuf, int len)
{
	struct sockaddr_in *out = &state->serv_sa;
	struct np_encaps_key *encaps;
	enum np_format format;
	u8 *crc;
	size_t offset;
	int rc;

	encaps = pbuf;
	format = encaps->format;

	if (sock == state->sock[PROBE_LINE]) {
		out->sin_port = htons(NP_UDP_PORT2);
	} else {
		out->sin_port = htons(NP_UDP_PORT);
	}

#ifdef NP_DEBUG
	if (format == NP_FMT_IV_KEY) {
		struct np_head *head;

		head = (struct np_head *)(encaps + 1);
		log_debug("sending op %s, seq %u, %s socket",
		    np_op_name(head->op), ntohs(head->seq),
		    sock == state->sock[PROBE_LINE] ? "probe" : "reg");
	}
#endif /* NP_DEBUG */

	if (format == NP_FMT_IV_KEY && state->aes_key_len) {
		memcpy(encaps->iv, state->iv, sizeof(encaps->iv));

		offset = sizeof(*encaps);

		/*
		 * Add CRC.
		 */
		crc = pbuf + len - sizeof(*crc);
		*crc = crc8(pbuf + offset,
		    len - offset - sizeof(*crc), CRC8_INIT);

		/*
		 * encrypt in place
		 */
		np_aes_encryp(state, pbuf + offset, len - offset, state->iv,
		    pbuf + offset);
	}

	/* send */
	rc = sendto(sock, pbuf, len, 0, (struct sockaddr *) out, sizeof(*out));
	if (rc < 0) {
		rc = -errno;
		log_warn("client %s sendto error - %m", device.dsn);
	}
	return rc;
}

/*
 * Open PCBs
 */
static int np_open_sockets(struct notify_state *state)
{
	if (np_sock_open(&state->sock[REG_LINE])) {
		return -1;
	}

	if (np_sock_open(&state->sock[PROBE_LINE])) {
		return -1;
	}

	return 0;
}

/*
 * ANS DNS resolved callback.
 */
static void np_dns_cb(struct notify_state *state, struct sockaddr_in *newsa)
{
	u8 diffipaddr;

	if (newsa->sin_port) {
		diffipaddr = (state->serv_sa.sin_port != newsa->sin_port) ||
		    (state->serv_sa.sin_addr.s_addr != newsa->sin_addr.s_addr);
		if (!state->serv_sa.sin_port &&
		    state->client_state == NCS_DOWN_RETRY) {
			/* Name wasn't resolved, so ANS wasn't up. */
			state->client_state = NCS_DOWN;
		}
		memcpy(&state->serv_sa, newsa, sizeof(*newsa));
		log_info("ANS server %s at %s", state->notify_host,
		    inet_ntoa(newsa->sin_addr));
		switch (state->client_state) {
		case NCS_DOWN_RETRY:
			if (diffipaddr) {
				np_down(state);
				state->client_state = NCS_DOWN_RETRY;
				state->restart_probing = 1;
				np_open_sockets(state);
			}
			state->probe_time = 0;
			np_setup_probe_send(state);
			break;
		case NCS_DNS_PASS:
		case NCS_UP:
			/* Do nothing */
			break;
		default:
			state->client_state = NCS_DNS_PASS;
			np_post_event(state);
		}
	} else {
		log_warn("ANS server DNS lookup failed");
		memset(&state->serv_sa, 0, sizeof(state->serv_sa));
		if (state->client_state == NCS_DOWN_RETRY) {
			np_setup_next_reach_check(state);
		}
	}
}

/*
 * Set ANS host name. TODO: Spin off a new thread for DNS lookup.
 * Use dns_cb_pending flag to know if a dns thread has already been
 * spun off.
 */
void np_set_server(const char *name)
{
	struct notify_state *state = &notify_state;
	struct sockaddr_in newsa;
	struct addrinfo *res = NULL;
	struct addrinfo hint;
	int err;

	if (state->notify_host != name) {
		strncpy(state->notify_host, name,
		    sizeof(state->notify_host) - 1);
	}
	if (state->dns_cb_pending) {
		return;
	}
	newsa.sin_port = 0;
	if (name[0] == '\0') {
		state->notify_host[0] = '\0';
		memset(&state->serv_sa, 0, sizeof(state->serv_sa));
		np_down_retry_event(state);
	}

	state->dns_cb_pending = 0;
	memset(&hint, 0, sizeof(hint));
	hint.ai_family = AF_INET;

	err = getaddrinfo(name, NULL, &hint, &res);
	if (err) {
		log_warn("gethost %s error %d: %s", name, err,
		    gai_strerror(err));
		if (state->client_state == NCS_DOWN_RETRY) {
			np_setup_next_reach_check(state);
		} else {
			np_down_retry_event(state);
		}
		return;
	}
	if (!res) {
		log_warn("no addrinfo");
		goto callback;
	}

	memcpy(&newsa, res->ai_addr, sizeof(newsa));
	newsa.sin_port = htons(NP_UDP_PORT);
	freeaddrinfo(res);

callback:
	np_dns_cb(state, &newsa);
}

/*
 * Check if the host name of ANS server has been set
 */
int np_server_is_set(void)
{
	struct notify_state *state = &notify_state;

	return state->notify_host[0] != '\0';
}

/*
 * Check if host name has been resolved.
 */
static int np_host_resolved(void)
{
	struct notify_state *state = &notify_state;

	return state->serv_sa.sin_port != 0;
}

/*
 * Clear the DNS entry information about the ANS server
 */
void np_clear_dns_info(void)
{
	struct notify_state *state = &notify_state;

	/* TODO: Clear the DNS cache */
	memset(&state->serv_sa, 0, sizeof(state->serv_sa));
}

/*
 * Sends keep alives OR probes
 */
static void np_send_keep_alive(struct notify_state *state)
{
	unsigned char pbuf[NP_NOTIFY_BUFLEN];
	struct np_keep_alive *ka;
	size_t pbuf_size = (state->probe_send) ?
	    sizeof(struct np_probe) : sizeof(struct np_keep_alive);
	int np_send_err;
	int len;

	len = np_new_packet_key(pbuf, pbuf_size);
	ka = (struct np_keep_alive *)pbuf;
	if (state->probe_send) {
		if (!state->probe_attempts) {
			np_down_retry_event(state);
			return;
		}
		np_init_head(&ka->head, NP_PROBE, state->probe_seq);
		state->probe_attempts--;
	} else {
		state->probe_seq = state->sequence++;
		np_init_head(&ka->head, NP_KEEP_ALIVE, state->probe_seq);
		ka->ka_period = htons(state->keep_alive_delay);
	}

	np_send_err = np_send(state, state->sock[REG_LINE], pbuf, len);
	if (np_send_err < 0) {
		/* non-recoverable error */
		np_down_retry_event(state);
		return;
	}

	np_set_next_np_timeout(state, 0);
}

static void np_recv_keep_alive_resp(struct notify_state *state,
	void *pbuf, int len)
{
	struct np_resp *resp = pbuf;

	if (np_check_resp(state, pbuf, sizeof(*resp), len, NCS_UP,
	    state->probe_seq)) {
		return;
	}
	if (resp->error != NP_ERR_NONE) {
		log_warn("resp error %d", resp->error);
		np_down_retry_event(state);
	}
}

static void np_reset_keep_alive(struct notify_state *state)
{
	struct device_state *dev = &device;

	timer_cancel(&dev->timers, &state->gen_timer);
	state->probe_time = time_mtime_ms() + state->poll_interval * 1000;
	np_send_keep_alive(state);
}

static void np_timeout(struct timer *timer)
{
	struct notify_state *state =
	    CONTAINER_OF(struct notify_state, gen_timer, timer);

	switch (state->client_state) {
	case NCS_REGISTER:
		np_register(state);
		break;
	case NCS_CHANGE_WAIT:
		np_post_event(state);
		break;
	case NCS_UP:
		np_send_keep_alive(state);
		break;
	case NCS_DOWN_RETRY:
		if (np_host_resolved()) {
			if (!state->reg_key) {
				np_reregister(state);
				break;
			}
			if (state->probe_send) {
				np_send_keep_alive(state);
				break;
			}
		}
		if (state->notify_host[0] != '\0') {
			np_set_server(state->notify_host);
		}
		break;
	default:
		break;
	}
}

/*
 * AES decryption using openssl lib
 */
void np_decrypt(struct notify_state *state, void *start, int elen, void *iv,
		void *out)
{
	AES_KEY aes_key;
	unsigned char *tmp = out;
	int diffaddr = 1;

	if ((char *)out == (char *)start) {
		diffaddr = 0;
		tmp = (unsigned char *) calloc(elen, sizeof(char));
		REQUIRE(tmp, REQUIRE_MSG_ALLOCATION);
	}

	AES_set_decrypt_key(state->aes_key, state->aes_key_len * 8,
	    &aes_key);

	/* unsigned char *tmp = (unsigned char*)calloc(elen, sizeof(char)); */
	AES_cbc_encrypt((unsigned char *) start, (unsigned char *) tmp, elen,
	    &aes_key, (unsigned char *) iv, AES_DECRYPT);

	if (!diffaddr) {
		memcpy((char *) start, tmp, elen);
		free(tmp);
	}
}

/*
 * Receive notification messages from server.
 */
static void np_recv(void *pbuf, int len, int sock)
{
	struct notify_state *state = &notify_state;
	struct np_head *head;
	struct np_encaps_key *encaps;
	u8 crc;

	encaps = pbuf;
	head = pbuf + sizeof(*encaps);
	switch (encaps->format) {
	case NP_FMT_IV_KEY:
		if (len < sizeof(*encaps)) {
			log_warn("too short len %u for encaps iv key", len);
			return;
		}
		if (state->client_state == NCS_UP &&
		    encaps->reg_key != state->reg_key) {
			log_warn("reg_key %d expected %d",
			    ntohl(encaps->reg_key), ntohl(state->reg_key));
			return;
		}
		break;
	case NP_FMT_DSN_ERR:
		np_recv_reg_err(state, pbuf, len);
		return;
	case NP_FMT_KEY_ERR:
		log_warn("bad key error - re-registering");
		np_reregister(state);
		return;
	case NP_FMT_DSN:
	case NP_FMT_KEY:
	default:
		log_warn("unexpected format %x", encaps->format);
		state->notify_host[0] = '\0';
		return;
	}
	memcpy(state->iv, encaps->iv, NP_IV_LEN);
	np_decrypt(state, head, len - sizeof(*encaps), state->iv, head);

	if (head->ver != NP_VERS) {
		log_warn("bad version %x", head->ver);
		state->notify_host[0] = '\0';
		return;
	}
	crc = crc8((char *)pbuf + sizeof(*encaps), len - sizeof(*encaps),
	    CRC8_INIT);
	if (crc) {
		log_warn("decrypt has bad CRC %x ", crc);
		return;
	}

#ifdef NP_DEBUG
	log_debug("op %d %s", head->op, np_op_name(head->op));
#endif /* NP_DEBUG */

	if (head->op != NP_REG_RESP && encaps->reg_key != state->reg_key) {
		log_err("msg with wrong reg key - ignoring");
		return;
	}

	switch (head->op) {
	case NP_REG_RESP:
		np_recv_reg_resp(state, pbuf, len);
		break;
	case NP_REQ_PROBE_RESP:
		np_recv_req_probe_resp(state, pbuf, len);
		break;
	case NP_PROBE:
		np_recv_probe(state, pbuf, sock);
		break;
	case NP_KEEP_ALIVE_RESP:
		np_recv_keep_alive_resp(state, pbuf, len);
		break;
	case NP_NOTIFY:
		np_recv_event(state, pbuf);
		break;
	case NP_PROBE_RESP:
		np_recv_probe_resp(state, pbuf, len);
		break;
	case NP_UNREG:		/* TODO */
	case NP_UNREG_RESP:	/* TODO */
		/* fall through */
	default:
		log_warn("unexpected op %x", head->op);
		break;
	}
}

/*
 * Receive notify protocol packet.
 */
static void np_socket_recv(void *arg, int sock)
{
	struct notify_state *state = &notify_state;
	struct sockaddr_in recv_addr;
	socklen_t sa_len;
	int rport;
	char rbuf[NP_NOTIFY_BUFLEN];
	int len;

	sa_len = sizeof(recv_addr);
	len = recvfrom(sock, rbuf, NP_NOTIFY_BUFLEN, 0,
	    (struct sockaddr *)&recv_addr, &sa_len);
	if (len < 0) {
		log_warn("recvfrom err: %m");
		return;
	}
	rport = ntohs(recv_addr.sin_port);
	if ((rport != NP_UDP_PORT && rport != NP_UDP_PORT2) ||
	    recv_addr.sin_addr.s_addr != state->serv_sa.sin_addr.s_addr) {
		log_warn("src addr invalid");
		return;
	}
	np_recv(rbuf, len, sock);
}

static void np_callback(struct timer *timer)
{
	struct notify_state *state =
	    CONTAINER_OF(struct notify_state, callback, timer);
	enum notify_event event;

	switch (state->client_state) {
	case NCS_DOWN:
		event = NS_EV_DOWN;
		break;
	case NCS_DOWN_RETRY:
		event = NS_EV_DOWN_RETRY;
		break;
	case NCS_DNS_PASS:
		event = NS_EV_DNS_PASS;
		break;
	case NCS_CHANGE_WAIT:
		event = NS_EV_CHANGE;
		break;
	default:
		event = NS_EV_CHECK;
		break;
	}
	/* do not hold the lock in case the client wants to restart */
	if (state->notify) {
		state->notify(event);
	}
}

/*
 * Setup notify state from config
 */
static int np_conf_set(json_t *obj)
{
	json_get_uint(obj, "poll_interval", &np_poll_default);
	return 0;
}

void np_init(void (*notify)(enum notify_event))
{
	struct notify_state *state = &notify_state;

	srandom(time(NULL));
	state->sequence = random();
	state->notify = notify;
	state->notify_host[0] = '\0';
	memset(&state->serv_sa, 0, sizeof(state->serv_sa));
	state->sock[PROBE_LINE] = -1;
	state->sock[REG_LINE] = -1;
	state->client_state = NCS_DOWN;
	state->probe_state = NCS_PROBE_DOWN;
	timer_init(&state->callback, np_callback);
	timer_init(&state->probe_timer, np_probe_timeout);
	timer_init(&state->gen_timer, np_timeout);
	conf_register("notify", np_conf_set, NULL);
}

static int np_sock_open(int *sockfd)
{
	struct device_state *dev = &device;
	/* as Server side*/
	int sock;
	int rc;

#ifdef SOCK_NONBLOCK
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
#else
	sock = socket(AF_INET, SOCK_DGRAM, 0);	/* XXX TBD */
	if (sock >= 0) {
		rc = fcntl(sock, F_SETFL, O_NONBLOCK);
		if (rc < 0) {
			log_err("fcntl: %m");
			return rc;
		}
	}
#endif
	if (sock < 0) {
		log_err("socket open failed - %m");
		return sock;
	}
	rc = file_event_reg(&dev->file_events, sock, np_socket_recv, NULL,
	    NULL);
	if (rc) {
		log_err("failed rc %d for sock %d", rc, sock);
		return -1;
	}
	*sockfd = sock;

	return 0;
}

int np_start(u8 *cipher_key, size_t key_len)
{
	struct notify_state *state = &notify_state;

	if (!cipher_key || !key_len) {
		log_err("no ANS key");
		return -1;
	}
	if (state->notify_host[0] == '\0' || !state->serv_sa.sin_addr.s_addr) {
		log_err("ANS host not resolved");
		return -1;
	}
#ifdef NP_DEBUG_KEY
	log_warn("using debug ANS cipher key: %s", NP_DEBUG_KEY);
	cipher_key = (u8 *)NP_DEBUG_KEY;
	key_len = NP_KEY_LEN;
#endif /* DEBUG_KEY */
#ifdef NP_DEBUG_IP
	log_warn("using debug ANS server address: %s", NP_DEBUG_IP);
	state->serv_sa.sin_addr.s_addr = inet_addr(NP_DEBUG_IP);
#endif /* DEBUG_IP */

	np_down(state);
	np_probe_init(state);
	state->poll_interval = np_poll_default;

	/* Setup and test key */
	state->aes_key = cipher_key;
	state->aes_key_len = key_len;
	if (np_open_sockets(state)) {
		return -1;
	}
	state->reg_attempts = NP_MAX_TRY;
	state->reg_seq = state->sequence++;
	np_register(state);
	return 0;
}

void np_stop(void)
{
	struct notify_state *state = &notify_state;

	state->change_wait = 0;
	np_down(state);
}
