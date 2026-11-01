// SPDX-License-Identifier: GPL-2.0-or-later
/* flowclassd — classify forwarded flows with nDPI before they are offloaded.
 *
 * The nft ruleset queues the first packets of every forwarded TCP/UDP flow
 * whose conntrack mark lacks FC_DONE (table inet flowclassd, after fw4's
 * forward filtering). This daemon runs each packet through nDPI, keyed by the
 * conntrack id the kernel attaches (NFQA_CFG_F_CONNTRACK), and as soon as the
 * application is known (or after max_pkts, best guess) writes it into the
 * conntrack mark in the verdict:
 *
 *	bit 31      FC_DONE  -> fw4's gated "flow offload" rule fires
 *	bits 16..30 nDPI protocol id (app if known, else master; 0 = unknown)
 *	bits 0..15  untouched (CTA_MARK_MASK)
 *
 * Everything else is accepted with a batch verdict. The queue runs fail-open
 * and the nft rule has the bypass flag, so a dead or overloaded daemon only
 * means unclassified flows; fw4's packet-count rule still offloads them.
 *
 * Protocol id -> name table is written to /var/run/flowclassd.protos for the
 * accounting side.
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_queue.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <ndpi/ndpi_api.h>

#define FC_DONE		0x80000000u
#define FC_MASK		0xffff0000u
#define FC_APP_SHIFT	16
#define FC_APP_MAX	0x7fffu

#define HASH_BITS	14
#define HASH_SIZE	(1u << HASH_BITS)
#define FLOW_IDLE_MS	30000		/* forget unfinished flows idle this long */
#define DONE_KEEP_MS	5000		/* remember finished flows for in-flight pkts */
#define SWEEP_MS	5000
#define BATCH_MAX	64

/* Flows are keyed by the direction-normalised 5-tuple, NOT the conntrack id:
 * a packet that arrives while an earlier one of the same flow is still queued
 * sees an unconfirmed conntrack and gets a second, temporary entry (merged by
 * clash resolution at confirm time). A QUIC ClientHello split over two
 * Initials hits exactly that, so a ct-id key would split it across two nDPI
 * flows. In the forward hook both directions carry the same pre-SNAT /
 * post-DNAT addresses, so the tuple is stable.
 */
struct fkey {
	uint8_t a[16], b[16];		/* a <= b */
	uint16_t pa, pb;
	uint8_t proto;
};

struct flow {
	struct flow *next;
	struct fkey key;
	uint32_t ct_id;			/* first ct seen; logging only */
	uint32_t pkts;
	uint64_t last_ms;
	uint32_t mark;			/* set once done */
	struct ndpi_flow_struct *nf;	/* NULL once done */
};

static struct ndpi_detection_module_struct *ndpi;
static struct flow *htab[HASH_SIZE];
static struct mnl_socket *nl;
static unsigned int portid, queue_num, max_pkts = 16, verbose;
static uint32_t batch_id;
static int batch_pending;
static volatile sig_atomic_t stop, dump;

static struct {
	uint64_t pkts, flows, classified, gaveup, no_ct, evicted, nomem, verdict_err;
} st;

static void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

/* Normalised 5-tuple of an L3 packet; -1 if not IPv4/IPv6 TCP/UDP. */
static int fkey_parse(const uint8_t *l3, unsigned int len, struct fkey *k)
{
	const uint8_t *sa, *da, *l4;
	unsigned int alen, l4off;
	uint16_t sp, dp;

	memset(k, 0, sizeof(*k));
	if (len >= 20 && (l3[0] >> 4) == 4) {
		l4off = (l3[0] & 15) * 4;
		k->proto = l3[9];
		sa = l3 + 12; da = l3 + 16; alen = 4;
	} else if (len >= 40 && (l3[0] >> 4) == 6) {
		l4off = 40;		/* the queue rule only matches tcp/udp */
		k->proto = l3[6];
		sa = l3 + 8; da = l3 + 24; alen = 16;
	} else {
		return -1;
	}
	if ((k->proto != IPPROTO_TCP && k->proto != IPPROTO_UDP) || len < l4off + 4)
		return -1;
	l4 = l3 + l4off;
	sp = (l4[0] << 8) | l4[1];
	dp = (l4[2] << 8) | l4[3];
	if (memcmp(sa, da, alen) < 0 || (!memcmp(sa, da, alen) && sp <= dp)) {
		memcpy(k->a, sa, alen); memcpy(k->b, da, alen);
		k->pa = sp; k->pb = dp;
	} else {
		memcpy(k->a, da, alen); memcpy(k->b, sa, alen);
		k->pa = dp; k->pb = sp;
	}
	return 0;
}

static uint32_t fkey_hash(const struct fkey *k)
{
	const uint8_t *p = (const uint8_t *)k;
	uint32_t h = 2166136261u;

	for (unsigned int i = 0; i < sizeof(*k); i++)
		h = (h ^ p[i]) * 16777619u;
	return h & (HASH_SIZE - 1);
}

static struct flow *flow_get(const struct fkey *key, uint32_t id, uint64_t now)
{
	struct flow **pp = &htab[fkey_hash(key)], *f;

	for (f = *pp; f; f = f->next)
		if (!memcmp(&f->key, key, sizeof(*key)))
			return f;
	f = calloc(1, sizeof(*f));
	if (f)
		f->nf = calloc(1, ndpi_detection_get_sizeof_ndpi_flow_struct());
	if (!f || !f->nf) {
		free(f);
		st.nomem++;
		return NULL;
	}
	f->key = *key;
	f->ct_id = id;
	f->last_ms = now;
	f->next = *pp;
	*pp = f;
	st.flows++;
	return f;
}

static void flow_finish(struct flow *f)
{
	ndpi_free_flow(f->nf);
	f->nf = NULL;
}

static void sweep(uint64_t now)
{
	for (unsigned int i = 0; i < HASH_SIZE; i++) {
		struct flow **pp = &htab[i], *f;

		while ((f = *pp)) {
			uint64_t keep = f->nf ? FLOW_IDLE_MS : DONE_KEEP_MS;

			if (now - f->last_ms > keep) {
				*pp = f->next;
				if (f->nf) {
					ndpi_free_flow(f->nf);
					st.evicted++;
				}
				free(f);
			} else {
				pp = &f->next;
			}
		}
	}
}

/* ---- netlink ---- */

static int send_nlh(struct nlmsghdr *nlh)
{
	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		st.verdict_err++;
		return -1;
	}
	return 0;
}

static void verdict_mark(uint32_t id, uint32_t mark)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = nfq_nlmsg_put(buf, NFQNL_MSG_VERDICT, queue_num);
	struct nlattr *nest;

	nfq_nlmsg_verdict_put(nlh, id, NF_ACCEPT);
	nest = mnl_attr_nest_start(nlh, NFQA_CT);
	mnl_attr_put_u32(nlh, CTA_MARK, htonl(mark));
	mnl_attr_put_u32(nlh, CTA_MARK_MASK, htonl(FC_MASK));
	mnl_attr_nest_end(nlh, nest);
	send_nlh(nlh);
}

static void verdict_batch(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;

	if (!batch_pending)
		return;
	nlh = nfq_nlmsg_put(buf, NFQNL_MSG_VERDICT_BATCH, queue_num);
	nfq_nlmsg_verdict_put(nlh, batch_id, NF_ACCEPT);
	send_nlh(nlh);
	batch_pending = 0;
}

static int ct_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, CTA_MAX) < 0)
		return MNL_CB_OK;
	tb[type] = attr;
	return MNL_CB_OK;
}

static void log_flow(const struct flow *f, const uint8_t *l3, unsigned int len,
		     ndpi_protocol p, const char *how)
{
	char s[INET6_ADDRSTRLEN] = "?", d[INET6_ADDRSTRLEN] = "?";

	if (len >= 20 && (l3[0] >> 4) == 4) {
		inet_ntop(AF_INET, l3 + 12, s, sizeof(s));
		inet_ntop(AF_INET, l3 + 16, d, sizeof(d));
	} else if (len >= 40 && (l3[0] >> 4) == 6) {
		inet_ntop(AF_INET6, l3 + 8, s, sizeof(s));
		inet_ntop(AF_INET6, l3 + 24, d, sizeof(d));
	}
	logmsg(LOG_INFO, "%s ct=%08x %s > %s after %u pkts: %s/%s",
	       how, f->ct_id, s, d, f->pkts,
	       ndpi_get_proto_name(ndpi, p.proto.master_protocol),
	       ndpi_get_proto_name(ndpi, p.proto.app_protocol));
}

static int queue_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nlattr *attr[NFQA_MAX + 1] = { NULL };
	const struct nlattr *ct[CTA_MAX + 1] = { NULL };
	struct nfqnl_msg_packet_hdr *ph;
	struct ndpi_flow_input_info in = { 0 };
	uint64_t now = *(uint64_t *)data;
	ndpi_protocol p;
	const uint8_t *pl;
	unsigned int len, ctinfo = IP_CT_NEW;
	uint32_t id, ct_id;
	struct fkey key;
	struct flow *f;
	uint16_t app;
	int done = 0;

	if (nfq_nlmsg_parse(nlh, attr) < 0 || !attr[NFQA_PACKET_HDR])
		return MNL_CB_OK;
	ph = mnl_attr_get_payload(attr[NFQA_PACKET_HDR]);
	id = ntohl(ph->packet_id);
	st.pkts++;
	batch_id = id;
	batch_pending = 1;

	if (!attr[NFQA_PAYLOAD] || !attr[NFQA_CT] ||
	    mnl_attr_parse_nested(attr[NFQA_CT], ct_attr_cb, ct) < 0 ||
	    !ct[CTA_ID]) {
		st.no_ct++;
		return MNL_CB_OK;
	}
	ct_id = ntohl(mnl_attr_get_u32(ct[CTA_ID]));
	if (attr[NFQA_CT_INFO])
		ctinfo = ntohl(mnl_attr_get_u32(attr[NFQA_CT_INFO]));
	pl = mnl_attr_get_payload(attr[NFQA_PAYLOAD]);
	len = mnl_attr_get_payload_len(attr[NFQA_PAYLOAD]);

	if (fkey_parse(pl, len, &key) < 0)
		return MNL_CB_OK;
	f = flow_get(&key, ct_id, now);
	if (!f)
		return MNL_CB_OK;
	f->last_ms = now;
	if (!f->nf) {
		/* already classified, an in-flight packet: re-assert the mark
		 * in case the first verdict raced a reinjected packet */
		verdict_mark(id, f->mark);
		return MNL_CB_OK;
	}

	in.in_pkt_dir = ctinfo >= IP_CT_IS_REPLY ? NDPI_IN_PKT_DIR_S_TO_C
						 : NDPI_IN_PKT_DIR_C_TO_S;
	/* the queue rule sees every flow from its first packet */
	in.seen_flow_beginning = NDPI_FLOW_BEGINNING_SEEN;
	p = ndpi_detection_process_packet(ndpi, f->nf, pl, len, now, &in);
	f->pkts++;
	if (verbose > 1)
		logmsg(LOG_DEBUG, "pkt ct=%08x #%u dir=%u len=%u ctinfo=%u state=%d %s/%s",
		       ct_id, f->pkts, in.in_pkt_dir, len, ctinfo, p.state,
		       ndpi_get_proto_name(ndpi, p.proto.master_protocol),
		       ndpi_get_proto_name(ndpi, p.proto.app_protocol));

	/* Done when nDPI is final, or when both levels are known (e.g.
	 * QUIC/YouTube). A lone app id is only the transport-level guess nDPI
	 * makes before it has seen the SNI/Host (QUIC, TLS, HTTP): keep going. */
	if (p.state == NDPI_STATE_CLASSIFIED || p.state == NDPI_STATE_MONITORING ||
	    (p.proto.app_protocol != NDPI_PROTOCOL_UNKNOWN &&
	     p.proto.master_protocol != NDPI_PROTOCOL_UNKNOWN)) {
		done = 1;
		st.classified++;
	} else if (f->pkts >= max_pkts) {
		p = ndpi_detection_giveup(ndpi, f->nf);
		done = 2;
		st.gaveup++;
	}
	if (!done)
		return MNL_CB_OK;

	app = p.proto.app_protocol != NDPI_PROTOCOL_UNKNOWN ?
	      p.proto.app_protocol : p.proto.master_protocol;
	if (app > FC_APP_MAX)
		app = 0;
	f->mark = FC_DONE | ((uint32_t)app << FC_APP_SHIFT);
	if (verbose)
		log_flow(f, pl, len, p, done == 1 ? "classified" : "gave-up");
	flow_finish(f);
	/* the batch verdict below only covers ids not yet verdicted, so an
	 * individual verdict for this packet first is ordering-safe */
	verdict_mark(id, f->mark);
	return MNL_CB_OK;
}

static int queue_config(void)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	uint32_t flags = NFQA_CFG_F_FAIL_OPEN | NFQA_CFG_F_CONNTRACK;

	nlh = nfq_nlmsg_put(buf, NFQNL_MSG_CONFIG, queue_num);
	nfq_nlmsg_cfg_put_cmd(nlh, AF_INET, NFQNL_CFG_CMD_BIND);
	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		return -1;

	nlh = nfq_nlmsg_put(buf, NFQNL_MSG_CONFIG, queue_num);
	nfq_nlmsg_cfg_put_params(nlh, NFQNL_COPY_PACKET, 0xffff);
	nfq_nlmsg_cfg_put_qmaxlen(nlh, 4096);
	mnl_attr_put_u32(nlh, NFQA_CFG_FLAGS, htonl(flags));
	mnl_attr_put_u32(nlh, NFQA_CFG_MASK, htonl(flags));
	nlh->nlmsg_flags |= NLM_F_ACK;
	/* the first CONNTRACK request makes the kernel load nf_conntrack_netlink
	 * and answers -EAGAIN; retry */
	for (int tries = 0; tries < 5; tries++) {
		int ret;

		if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
			return -1;
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret < 0)
			return -1;
		ret = mnl_cb_run(buf, ret, 0, portid, NULL, NULL);
		if (ret >= 0)
			return 0;
		if (errno != EAGAIN)
			return -1;
		nlh = nfq_nlmsg_put(buf, NFQNL_MSG_CONFIG, queue_num);
		nfq_nlmsg_cfg_put_params(nlh, NFQNL_COPY_PACKET, 0xffff);
		nfq_nlmsg_cfg_put_qmaxlen(nlh, 4096);
		mnl_attr_put_u32(nlh, NFQA_CFG_FLAGS, htonl(flags));
		mnl_attr_put_u32(nlh, NFQA_CFG_MASK, htonl(flags));
		nlh->nlmsg_flags |= NLM_F_ACK;
	}
	errno = EAGAIN;
	return -1;
}

static void write_protos(const char *path)
{
	char tmp[256];
	FILE *fp;
	u_int16_t n = ndpi_get_num_protocols(ndpi);

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return;
	for (u_int16_t i = 0; i < n && i <= FC_APP_MAX; i++) {
		const char *name = ndpi_get_proto_name(ndpi, i);

		if (name && *name)
			fprintf(fp, "%u %s %s\n", i, name,
				ndpi_category_get_name(ndpi,
					ndpi_get_proto_category(ndpi,
						(ndpi_protocol){ .proto = { 0, i } })));
	}
	fclose(fp);
	rename(tmp, path);
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		dump = 1;
	else
		stop = 1;
}

static void log_stats(void)
{
	logmsg(LOG_INFO, "stats: pkts=%llu flows=%llu classified=%llu gaveup=%llu "
	       "no_ct=%llu evicted=%llu nomem=%llu verdict_err=%llu",
	       (unsigned long long)st.pkts, (unsigned long long)st.flows,
	       (unsigned long long)st.classified, (unsigned long long)st.gaveup,
	       (unsigned long long)st.no_ct, (unsigned long long)st.evicted,
	       (unsigned long long)st.nomem, (unsigned long long)st.verdict_err);
}

int main(int argc, char **argv)
{
	static char buf[0xffff + 4096];
	struct sigaction sa = { .sa_handler = on_signal };
	uint64_t last_sweep;
	int opt, one = 1, rcvbuf = 4 << 20;

	while ((opt = getopt(argc, argv, "q:n:v")) != -1) {
		switch (opt) {
		case 'q': queue_num = atoi(optarg); break;
		case 'n': max_pkts = atoi(optarg); break;
		case 'v': verbose++; break;
		default:
			fprintf(stderr, "usage: flowclassd [-q queue] [-n max_pkts] [-v]\n");
			return 2;
		}
	}
	openlog("flowclassd", LOG_PID | LOG_PERROR, LOG_DAEMON);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	ndpi = ndpi_init_detection_module(NULL);
	if (!ndpi || ndpi_finalize_initialization(ndpi)) {
		logmsg(LOG_ERR, "nDPI init failed");
		return 1;
	}
	if (queue_num == 0)
		write_protos("/var/run/flowclassd.protos");

	nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		logmsg(LOG_ERR, "netlink: %s", strerror(errno));
		return 1;
	}
	portid = mnl_socket_get_portid(nl);
	setsockopt(mnl_socket_get_fd(nl), SOL_NETLINK, NETLINK_NO_ENOBUFS, &one, sizeof(one));
	setsockopt(mnl_socket_get_fd(nl), SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf));
	if (queue_config() < 0) {
		logmsg(LOG_ERR, "queue %u config: %s", queue_num, strerror(errno));
		return 1;
	}
	logmsg(LOG_INFO, "queue %u bound, nDPI %s, max %u pkts/flow",
	       queue_num, ndpi_revision(), max_pkts);

	last_sweep = now_ms();
	while (!stop) {
		uint64_t now;
		int ret, n = 0;

		ret = recv(mnl_socket_get_fd(nl), buf, sizeof(buf), 0);
		while (ret > 0) {
			now = now_ms();
			mnl_cb_run(buf, ret, 0, portid, queue_cb, &now);
			if (++n >= BATCH_MAX)
				break;
			ret = recv(mnl_socket_get_fd(nl), buf, sizeof(buf), MSG_DONTWAIT);
		}
		verdict_batch();
		if (ret < 0 && errno != EAGAIN && errno != EINTR && errno != ENOBUFS) {
			logmsg(LOG_ERR, "recv: %s", strerror(errno));
			break;
		}
		now = now_ms();
		if (now - last_sweep >= SWEEP_MS) {
			sweep(now);
			last_sweep = now;
		}
		if (dump) {
			log_stats();
			dump = 0;
		}
	}
	log_stats();
	mnl_socket_close(nl);
	ndpi_exit_detection_module(ndpi);
	return 0;
}
