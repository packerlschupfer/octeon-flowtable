// SPDX-License-Identifier: GPL-2.0
/* trafficd — per-client / per-application traffic accounting for a router.
 *
 * Every INTERVAL seconds the conntrack table is dumped over ctnetlink; for
 * each connection the byte delta since the last dump is credited to
 *
 *	client = the LAN-side endpoint (by MAC from the neighbour tables when
 *	         known, else by IP), named from the DHCP server's leases
 *	app    = the nDPI id flowclassd stored in the conntrack mark
 *	         (bit 31 = classified, bits 16-30 = id; 0 = unclassified)
 *
 * split into upload (LAN -> outside) and download. DESTROY events carry the
 * final counters, so short connections ending between dumps are not lost.
 * Offloaded connections are covered too: octeon_flowtable feeds their
 * counters back into conntrack.
 *
 * Storage: (minute, client, app, up, down) records for 24 h and the same per
 * hour for 31 days, in RAM; the hour records + client table persist to
 * STATE_FILE (hourly and on exit). Every minute the views are written as
 * JSON to OUT_DIR for the static web page.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>

#define FC_DONE		0x80000000u
#define MAX_CLIENTS	1024
#define MAX_APPS	32768
#define MIN_CAP		400000		/* minute records (24 h) */
#define HOUR_CAP	300000		/* hour records (31 d) */
#define MIN_KEEP	(24 * 60)
#define HOUR_KEEP	(31 * 24)
#define CT_SLOTS	(1 << 16)
#define MAX_PFX		64
#define TOP_APPS	12		/* per client in the summary */

static const char *out_dir = "/tmp/trafficd";
static const char *state_file = "/etc/trafficd/history.bin";
static const char *protos_file = "/var/run/flowclassd.protos";
static const char *wan_dev = "eth0";
static int interval = 30, verbose;
static volatile sig_atomic_t stop;

/* ---- clients / apps ---- */

struct client {
	char key[24];		/* MAC "aa:bb:..", or "ip:<addr>" */
	char ip[INET6_ADDRSTRLEN];
	char name[64];
	time_t last;
};
static struct client clients[MAX_CLIENTS];
static int nclients;

static char *app_name[MAX_APPS], *app_cat[MAX_APPS];

/* ---- records ---- */

struct rec {
	uint32_t t;		/* minute or hour index since epoch */
	uint16_t client, app;
	uint64_t up, down;
};

struct ring {
	struct rec *r;
	size_t cap, head, n;	/* oldest at head */
};
static struct ring mins, hours;

/* accumulator for the current minute / hour: small open-addressed map */
#define ACC_SLOTS 8192
struct acc {
	uint32_t key;		/* client << 16 | app, +1 so 0 = empty */
	uint64_t up, down;
};
static struct acc cur_min[ACC_SLOTS], cur_hour[ACC_SLOTS];
static uint32_t cur_min_t, cur_hour_t;

/* ---- internet uplink series (bytes to/from outside the LAN + VPNs) ---- */
struct wslot {
	uint32_t t;		/* minute / hour index this slot holds */
	uint64_t up, down;
};
static struct wslot wan_min[MIN_KEEP], wan_hour[HOUR_KEEP];

static void wan_add(struct wslot *w, int n, uint32_t t, uint64_t up, uint64_t down)
{
	struct wslot *x = &w[t % n];

	if (x->t != t) {
		x->t = t;
		x->up = x->down = 0;
	}
	x->up += up;
	x->down += down;
}

/* ---- per-connection state ---- */

struct ctst {
	uint32_t id;		/* 0 = empty */
	uint32_t gen;
	uint64_t ob, rb;	/* last seen orig / reply bytes */
};
static struct ctst cts[CT_SLOTS];
static uint32_t gen;
/* The first dump after start only records each connection's counters as the
 * baseline: their bytes so far were counted before a restart (or predate the
 * collector) and crediting them now would double count / misdate them. */
static int priming = 1;

/* ---- address knowledge ---- */

struct pfx {
	int fam;
	uint8_t a[16];
	int len;
};
static struct pfx lan[MAX_PFX];
static int nlan;
static struct pfx local[MAX_PFX];	/* the router's own addresses (/32, /128) */
#define MAX_EXCLUDE	16
static struct pfx excl[MAX_EXCLUDE];	/* -x: LAN hosts never tracked per client */
static int nexcl;
static int nlocal;
static struct pfx tun[MAX_PFX];		/* VPN (wg*) prefixes: inner traffic */
static int ntun;

struct nb {			/* neighbour: ip -> mac */
	char ip[INET6_ADDRSTRLEN];
	char mac[18];
};
static struct nb *nbs;
static int nnb, nb_cap;

struct hn {			/* mac -> hostname */
	char mac[18];
	char name[64];
};
static struct hn *hns;
static int nhn, hn_cap;

static void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
}

/* ================= rings & accumulators ================= */

static void ring_init(struct ring *g, size_t cap)
{
	g->r = calloc(cap, sizeof(*g->r));
	g->cap = cap;
	if (!g->r) {
		logmsg(LOG_ERR, "out of memory");
		exit(1);
	}
}

static void ring_push(struct ring *g, const struct rec *x)
{
	if (g->n == g->cap) {		/* full: drop oldest */
		g->head = (g->head + 1) % g->cap;
		g->n--;
	}
	g->r[(g->head + g->n) % g->cap] = *x;
	g->n++;
}

static void ring_expire(struct ring *g, uint32_t min_t)
{
	while (g->n && g->r[g->head].t < min_t) {
		g->head = (g->head + 1) % g->cap;
		g->n--;
	}
}

static struct rec *ring_at(struct ring *g, size_t i)
{
	return &g->r[(g->head + i) % g->cap];
}

static struct acc *acc_get(struct acc *a, uint16_t client, uint16_t app)
{
	uint32_t key = ((uint32_t)client << 16 | app) + 1;
	uint32_t h = (key * 2654435761u) & (ACC_SLOTS - 1);

	for (int i = 0; i < ACC_SLOTS; i++, h = (h + 1) & (ACC_SLOTS - 1)) {
		if (a[h].key == key)
			return &a[h];
		if (!a[h].key) {
			a[h].key = key;
			return &a[h];
		}
	}
	return NULL;		/* full: drop (never in practice) */
}

static void acc_flush(struct acc *a, struct ring *g, uint32_t t)
{
	for (int i = 0; i < ACC_SLOTS; i++) {
		if (!a[i].key)
			continue;
		if (a[i].up || a[i].down) {
			struct rec x = {
				.t = t,
				.client = (a[i].key - 1) >> 16,
				.app = (a[i].key - 1) & 0xffff,
				.up = a[i].up, .down = a[i].down,
			};

			ring_push(g, &x);
		}
	}
	memset(a, 0, sizeof(*a) * ACC_SLOTS);
}

/* ================= addresses ================= */

static int pfx_match(const struct pfx *p, int fam, const uint8_t *a)
{
	int bits = p->len, i;

	if (p->fam != fam)
		return 0;
	for (i = 0; bits >= 8; i++, bits -= 8)
		if (p->a[i] != a[i])
			return 0;
	return !bits || ((p->a[i] ^ a[i]) & (0xff << (8 - bits))) == 0;
}

static int in_list(const struct pfx *l, int n, int fam, const uint8_t *a)
{
	for (int i = 0; i < n; i++)
		if (pfx_match(&l[i], fam, a))
			return 1;
	return 0;
}

/* "192.168.24.0/24" or "2001:db8::/64"; no length means a single host */
static int parse_cidr(const char *s, struct pfx *p)
{
	char buf[INET6_ADDRSTRLEN + 4], *slash, *end;
	int max;
	long len;

	if (snprintf(buf, sizeof(buf), "%s", s) >= (int)sizeof(buf))
		return -1;
	slash = strchr(buf, '/');
	if (slash)
		*slash = 0;
	if (inet_pton(AF_INET, buf, p->a) == 1) {
		p->fam = AF_INET;
		max = 32;
	} else if (inet_pton(AF_INET6, buf, p->a) == 1) {
		p->fam = AF_INET6;
		max = 128;
	} else {
		return -1;
	}
	if (!slash) {
		p->len = max;
		return 0;
	}
	len = strtol(slash + 1, &end, 10);
	if (end == slash + 1 || *end || len < 0 || len > max)
		return -1;
	p->len = len;
	return 0;
}

static int masklen(const struct sockaddr *m)
{
	const uint8_t *b;
	int n, len = 0;

	if (m->sa_family == AF_INET) {
		b = (const uint8_t *)&((const struct sockaddr_in *)m)->sin_addr;
		n = 4;
	} else {
		b = ((const struct sockaddr_in6 *)m)->sin6_addr.s6_addr;
		n = 16;
	}
	for (int i = 0; i < n; i++)
		len += __builtin_popcount(b[i]);
	return len;
}

/* LAN prefixes = every address on every interface except the WAN device and
 * loopback; local = all of the router's own addresses. Link-local v6 is ignored. */
static void load_addrs(void)
{
	struct ifaddrs *ifa, *i;

	if (getifaddrs(&ifa))
		return;
	nlan = nlocal = ntun = 0;
	for (i = ifa; i; i = i->ifa_next) {
		struct pfx p = { 0 };
		const uint8_t *a;

		if (!i->ifa_addr || !i->ifa_netmask)
			continue;
		if (i->ifa_addr->sa_family == AF_INET) {
			p.fam = AF_INET;
			a = (const uint8_t *)&((struct sockaddr_in *)i->ifa_addr)->sin_addr;
			memcpy(p.a, a, 4);
		} else if (i->ifa_addr->sa_family == AF_INET6) {
			p.fam = AF_INET6;
			a = ((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr.s6_addr;
			if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)
				continue;
			memcpy(p.a, a, 16);
		} else {
			continue;
		}
		if (nlocal < MAX_PFX) {
			local[nlocal] = p;
			local[nlocal++].len = p.fam == AF_INET ? 32 : 128;
		}
		if (i->ifa_flags & IFF_LOOPBACK)
			continue;
		p.len = masklen(i->ifa_netmask);
		if (!strncmp(i->ifa_name, "wg", 2)) {
			if (ntun < MAX_PFX)
				tun[ntun++] = p;
			continue;
		}
		if (!strcmp(i->ifa_name, wan_dev) || !strncmp(i->ifa_name, "pppoe-", 6))
			continue;
		if (nlan < MAX_PFX)
			lan[nlan++] = p;
	}
	freeifaddrs(ifa);
}

static void nb_add(const char *ip, const char *mac)
{
	if (nnb == nb_cap) {
		nb_cap = nb_cap ? nb_cap * 2 : 256;
		nbs = realloc(nbs, nb_cap * sizeof(*nbs));
	}
	snprintf(nbs[nnb].ip, sizeof(nbs[nnb].ip), "%s", ip);
	snprintf(nbs[nnb].mac, sizeof(nbs[nnb].mac), "%s", mac);
	nnb++;
}

/* neighbour tables: /proc/net/arp (v4) + `ip -6 neigh` (v6) */
static void load_neigh(void)
{
	char l[256], ip[64], mac[32], *p;
	FILE *f;

	nnb = 0;
	if ((f = fopen("/proc/net/arp", "r"))) {
		fgets(l, sizeof(l), f);
		while (fgets(l, sizeof(l), f))
			if (sscanf(l, "%63s %*s %*s %31s", ip, mac) == 2 &&
			    strcmp(mac, "00:00:00:00:00:00"))
				nb_add(ip, mac);
		fclose(f);
	}
	if ((f = popen("ip -6 neigh show 2>/dev/null", "r"))) {
		while (fgets(l, sizeof(l), f))
			if ((p = strstr(l, " lladdr ")) &&
			    sscanf(l, "%63s", ip) == 1 && sscanf(p + 8, "%31s", mac) == 1)
				nb_add(ip, mac);
		pclose(f);
	}
}

static const char *nb_mac(const char *ip)
{
	for (int i = 0; i < nnb; i++)
		if (!strcmp(nbs[i].ip, ip))
			return nbs[i].mac;
	return NULL;
}

static void hn_set(const char *mac, const char *name)
{
	char m[18];
	int i;

	snprintf(m, sizeof(m), "%s", mac);
	for (i = 0; m[i]; i++)
		if (m[i] >= 'A' && m[i] <= 'F')
			m[i] += 32;
	for (i = 0; i < nhn; i++)
		if (!strcmp(hns[i].mac, m))
			break;
	if (i == nhn) {
		if (nhn == hn_cap) {
			hn_cap = hn_cap ? hn_cap * 2 : 128;
			hns = realloc(hns, hn_cap * sizeof(*hns));
		}
		snprintf(hns[nhn].mac, sizeof(hns[nhn].mac), "%s", m);
		nhn++;
	}
	snprintf(hns[i].name, sizeof(hns[i].name), "%s", name);
}

static int duid_mac(const char *duid, char mac[18])
{
	const char *p = NULL;
	unsigned int b[6] = {};
	size_t len = strlen(duid);

	if (len == 28 && !strncmp(duid, "0001", 4))
		p = duid + 16;
	else if (len == 20 && !strncmp(duid, "0003", 4))
		p = duid + 8;
	else
		return 0;

	if (sscanf(p, "%2x%2x%2x%2x%2x%2x",
		&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return 0;
	snprintf(mac, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
		b[0], b[1], b[2], b[3], b[4], b[5]);
	return 1;
}

/* ISC dhcpd: static `host NAME { hardware ethernet MAC; ... }` blocks in the
 * config, then dynamic leases (`client-hostname "x";`), the latest wins. */
static void load_names(void)
{
	char l[512], name[64] = "", mac[32] = "", *p;
	int in_lease = 0;
	FILE *f;

	nhn = 0;
	if ((f = fopen("/etc/dhcpd.conf", "r"))) {
		while (fgets(l, sizeof(l), f)) {
			if (sscanf(l, " host %63[^ {]", name) == 1)
				continue;
			if (*name && (p = strstr(l, "hardware ethernet ")) &&
			    sscanf(p + 18, "%17[0-9a-fA-F:]", mac) == 1) {
				hn_set(mac, name);
				*name = 0;
			}
		}
		fclose(f);
	}
	if ((f = fopen("/tmp/dhcpd.leases", "r"))) {
		*name = *mac = 0;
		while (fgets(l, sizeof(l), f)) {
			if (!strncmp(l, "lease ", 6)) {
				in_lease = 1;
				*name = *mac = 0;
			} else if (in_lease && (p = strstr(l, "hardware ethernet "))) {
				sscanf(p + 18, "%17[0-9a-fA-F:]", mac);
			} else if (in_lease && (p = strstr(l, "client-hostname \""))) {
				sscanf(p + 17, "%63[^\"]", name);
			} else if (in_lease && l[0] == '}') {
				if (*mac && *name)
					hn_set(mac, name);
				in_lease = 0;
			}
		}
		fclose(f);
	}
	/* dnsmasq (the OpenWrt default): "expiry mac ip hostname clientid" */
	if ((f = fopen("/tmp/dhcp.leases", "r"))) {
		while (fgets(l, sizeof(l), f))
			if (sscanf(l, "%*s %31s %*s %63s", mac, name) == 2 && strcmp(name, "*"))
				hn_set(mac, name);
		fclose(f);
	}

	/* odhcpd */
	if ((f = fopen("/tmp/odhcpd.leases", "r"))) {
		time_t t = time(NULL);
		static const char *skip = " \t\r\n";

		while (fgets(l, sizeof(l), f)) {
			char *save = NULL, *id = NULL, *kind = NULL,
				*host = NULL, *expiry = NULL, *end = NULL, *tok = NULL;
			long long valid = -1;
			const char *m = NULL;

			if (strncmp(l, "# ", 2) || !strtok_r(l + 2, skip, &save))
				continue;
			id = strtok_r(NULL, skip, &save);
			kind = strtok_r(NULL, skip, &save);
			host = strtok_r(NULL, skip, &save);
			expiry = strtok_r(NULL, skip, &save);
			if (!expiry || !strcmp(host, "-") || strchr(host, '\\'))
				continue;
			valid = strtoll(expiry, &end, 10);
			if (*end || (valid != -1 && valid <= t))
				continue;

			if (!strcmp(kind, "ipv4")) {
				if (strlen(id) == 17)
					hn_set(id, host);
				continue;
			}

			/* Skip IA_PD, require IA_NA */
			if (!strtok_r(NULL, skip, &save) ||
				!(tok = strtok_r(NULL, skip, &save)) || strcmp(tok, "128"))
				continue;
			/* Prefer the currently active NIC. */
			while ((tok = strtok_r(NULL, skip, &save))) {
				struct in6_addr address6 = {};
				char *slash = strrchr(tok, '/');

				if (!slash || strcmp(slash + 1, "128"))
					continue;
				*slash = 0;
				if (inet_pton(AF_INET6, tok, &address6) == 1 && (m = nb_mac(tok))) {
					hn_set(m, host);
					break;
				}
			}
			if (!m && duid_mac(id, mac))
				hn_set(mac, host);
		}
		fclose(f);
	}
}

static const char *hn_name(const char *mac)
{
	for (int i = 0; i < nhn; i++)
		if (!strcmp(hns[i].mac, mac))
			return hns[i].name;
	return NULL;
}

static void load_protos(void)
{
	char l[256], n[96], c[96];
	unsigned int id;
	FILE *f = fopen(protos_file, "r");

	if (!f)
		return;
	while (fgets(l, sizeof(l), f)) {
		*c = 0;
		if (sscanf(l, "%u %95s %95s", &id, n, c) >= 2 && id < MAX_APPS) {
			free(app_name[id]);
			free(app_cat[id]);
			app_name[id] = strdup(n);
			app_cat[id] = strdup(*c ? c : "");
		}
	}
	fclose(f);
}

/* short display name: first DNS label of a hostname / FQDN */
static void set_name(struct client *c, const char *name)
{
	snprintf(c->name, sizeof(c->name), "%.*s", (int)strcspn(name, "."), name);
}

/* reverse-DNS fallback for clients without a DHCP hostname (bind holds the
 * DDNS PTRs); called from the once-a-minute metadata pass, a few per pass */
static void resolve_names(void)
{
	int budget = 8;

	for (int i = 0; i < nclients && budget; i++) {
		struct client *c = &clients[i];
		struct sockaddr_storage ss = { 0 };
		char host[NI_MAXHOST];
		socklen_t len;

		if (c->name[0] || !c->ip[0])
			continue;
		if (inet_pton(AF_INET, c->ip, &((struct sockaddr_in *)&ss)->sin_addr) == 1) {
			ss.ss_family = AF_INET;
			len = sizeof(struct sockaddr_in);
		} else if (inet_pton(AF_INET6, c->ip, &((struct sockaddr_in6 *)&ss)->sin6_addr) == 1) {
			ss.ss_family = AF_INET6;
			len = sizeof(struct sockaddr_in6);
		} else {
			continue;
		}
		budget--;
		if (getnameinfo((struct sockaddr *)&ss, len, host, sizeof(host), NULL, 0, NI_NAMEREQD))
			continue;
		/* Only trust a PTR whose name resolves back to this address: DDNS
		 * leaves stale PTRs behind (several old IPs -> one hostname). */
		{
			struct addrinfo hints = { .ai_family = ss.ss_family }, *res, *r;
			char back[INET6_ADDRSTRLEN];
			int ok = 0;

			if (getaddrinfo(host, NULL, &hints, &res))
				continue;
			for (r = res; r && !ok; r = r->ai_next) {
				const void *a = r->ai_family == AF_INET ?
					(const void *)&((struct sockaddr_in *)r->ai_addr)->sin_addr :
					(const void *)&((struct sockaddr_in6 *)r->ai_addr)->sin6_addr;

				if (inet_ntop(r->ai_family, a, back, sizeof(back)) &&
				    !strcmp(back, c->ip))
					ok = 1;
			}
			freeaddrinfo(res);
			if (ok)
				set_name(c, host);
		}
	}
}

/* client index for a LAN address (creates it on first sight) */
static int client_for(const char *ip, time_t now)
{
	const char *mac = nb_mac(ip), *name;
	char key[24];
	int i;

	if (mac)
		snprintf(key, sizeof(key), "%s", mac);
	else
		snprintf(key, sizeof(key), "ip:%.20s", ip);
	for (i = 0; i < nclients; i++)
		if (!strcmp(clients[i].key, key))
			break;
	if (i == nclients && mac) {
		/* first seen before its MAC was in the neighbour table: adopt the
		 * IP-keyed entry (keeps its history) instead of adding a twin */
		char ipkey[24];

		snprintf(ipkey, sizeof(ipkey), "ip:%.20s", ip);
		for (i = 0; i < nclients; i++)
			if (!strcmp(clients[i].key, ipkey))
				break;
		if (i < nclients)
			snprintf(clients[i].key, sizeof(clients[i].key), "%s", key);
	}
	if (i == nclients) {
		if (nclients == MAX_CLIENTS)
			return MAX_CLIENTS - 1;	/* overflow bucket */
		nclients++;
		snprintf(clients[i].key, sizeof(clients[i].key), "%s", key);
	}
	snprintf(clients[i].ip, sizeof(clients[i].ip), "%s", ip);
	if (mac && (name = hn_name(mac)))
		set_name(&clients[i], name);
	clients[i].last = now;
	return i;
}

/* the router's own internet traffic (WireGuard tunnels, DNS recursion, NTP, ...) */
static int router_client(time_t now)
{
	int i;

	for (i = 0; i < nclients; i++)
		if (!strcmp(clients[i].key, "router"))
			break;
	if (i == nclients) {
		if (nclients == MAX_CLIENTS)
			return MAX_CLIENTS - 1;
		nclients++;
		snprintf(clients[i].key, sizeof(clients[i].key), "router");
	}
	if (!clients[i].name[0]) {
		char h[48] = "router";

		gethostname(h, sizeof(h) - 1);
		snprintf(clients[i].name, sizeof(clients[i].name), "%s (router)", h);
	}
	clients[i].ip[0] = 0;
	clients[i].last = now;
	return i;
}

/* ================= accounting ================= */

static void credit(time_t now, int client, int app, uint64_t up, uint64_t down, int wan)
{
	uint32_t m = now / 60, h = now / 3600;
	struct acc *a;

	if (!up && !down)
		return;
	if (wan) {
		wan_add(wan_min, MIN_KEEP, m, up, down);
		wan_add(wan_hour, HOUR_KEEP, h, up, down);
	}
	if (client < 0)
		return;		/* excluded host: uplink total only */
	if (m != cur_min_t) {
		acc_flush(cur_min, &mins, cur_min_t);
		cur_min_t = m;
	}
	if (h != cur_hour_t) {
		acc_flush(cur_hour, &hours, cur_hour_t);
		cur_hour_t = h;
	}
	if ((a = acc_get(cur_min, client, app))) {
		a->up += up;
		a->down += down;
	}
	if ((a = acc_get(cur_hour, client, app))) {
		a->up += up;
		a->down += down;
	}
}

static struct ctst *ct_slot(uint32_t id, int create)
{
	uint32_t h = (id * 2654435761u) & (CT_SLOTS - 1);

	for (int i = 0; i < CT_SLOTS; i++, h = (h + 1) & (CT_SLOTS - 1)) {
		if (cts[h].id == id)
			return &cts[h];
		if (!cts[h].id)
			return create ? &cts[h] : NULL;
	}
	return NULL;
}

/* rebuild the table without stale entries (open addressing: no tombstones) */
static void ct_sweep(void)
{
	static struct ctst keep[CT_SLOTS];
	int n = 0;

	for (int i = 0; i < CT_SLOTS; i++)
		if (cts[i].id && cts[i].gen == gen)
			keep[n++] = cts[i];
	memset(cts, 0, sizeof(cts));
	for (int i = 0; i < n; i++) {
		struct ctst *s = ct_slot(keep[i].id, 1);

		if (s)
			*s = keep[i];
	}
}

static void handle_ct(const struct nf_conntrack *ct, int destroyed)
{
	uint8_t fam = nfct_get_attr_u8(ct, ATTR_L3PROTO);
	const uint8_t *osrc, *odst, *rsrc;
	char ip[INET6_ADDRSTRLEN];
	uint64_t ob, rb, dob, drb;
	uint32_t id, mark;
	time_t now = time(NULL);
	int client, app, orig_is_client, wan, router = 0;
	const uint8_t *remote;
	struct ctst *s;

	if (fam == AF_INET) {
		osrc = nfct_get_attr(ct, ATTR_ORIG_IPV4_SRC);
		odst = nfct_get_attr(ct, ATTR_ORIG_IPV4_DST);
		rsrc = nfct_get_attr(ct, ATTR_REPL_IPV4_SRC);
	} else if (fam == AF_INET6) {
		osrc = nfct_get_attr(ct, ATTR_ORIG_IPV6_SRC);
		odst = nfct_get_attr(ct, ATTR_ORIG_IPV6_DST);
		rsrc = nfct_get_attr(ct, ATTR_REPL_IPV6_SRC);
	} else {
		return;
	}
	if (!osrc || !odst || !rsrc)
		return;

	if (in_list(local, nlocal, fam, osrc) || in_list(local, nlocal, fam, odst)) {
		/* the router's own traffic: count it only towards the internet
		 * (LAN <-> router management/DNS is not uplink traffic) */
		orig_is_client = in_list(local, nlocal, fam, osrc);
		remote = orig_is_client ? odst : osrc;
		if (in_list(lan, nlan, fam, remote) || in_list(tun, ntun, fam, remote) ||
		    in_list(local, nlocal, fam, remote))
			return;
		router = 1;
		wan = 1;
	} else if (in_list(lan, nlan, fam, osrc)) {
		orig_is_client = 1;		/* LAN host initiated */
		inet_ntop(fam, osrc, ip, sizeof(ip));
		remote = odst;
	} else if (in_list(lan, nlan, fam, rsrc)) {
		orig_is_client = 0;		/* inbound to a LAN host (DNAT / VPN) */
		inet_ntop(fam, rsrc, ip, sizeof(ip));
		remote = osrc;
	} else {
		return;
	}
	/* uplink = the far end is neither another LAN segment nor a VPN peer
	 * (tunnelled bytes are already counted as the router's tunnel flow) */
	if (!router)
		wan = !in_list(lan, nlan, fam, remote) && !in_list(tun, ntun, fam, remote);

	id = nfct_get_attr_u32(ct, ATTR_ID);
	ob = nfct_get_attr_u64(ct, ATTR_ORIG_COUNTER_BYTES);
	rb = nfct_get_attr_u64(ct, ATTR_REPL_COUNTER_BYTES);
	mark = nfct_get_attr_u32(ct, ATTR_MARK);
	app = mark & FC_DONE ? (mark >> 16) & 0x7fff : 0;

	s = ct_slot(id, !destroyed);
	if (s && s->id == id) {
		dob = ob >= s->ob ? ob - s->ob : ob;	/* counter reset: take all */
		drb = rb >= s->rb ? rb - s->rb : rb;
	} else {
		dob = ob;
		drb = rb;
	}
	if (s) {
		if (destroyed) {
			s->gen = 0;		/* dropped at the next sweep */
		} else {
			s->id = id;
			s->gen = gen;
			s->ob = ob;
			s->rb = rb;
		}
	}
	if (router)
		client = router_client(now);
	else if (in_list(excl, nexcl, fam, orig_is_client ? osrc : rsrc))
		client = -1;	/* no client entry, name lookup or per-app record */
	else
		client = client_for(ip, now);
	if (priming && !destroyed)
		return;
	if (orig_is_client)
		credit(now, client, app, dob, drb, wan);
	else
		credit(now, client, app, drb, dob, wan);
}

static int dump_cb(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data)
{
	handle_ct(ct, 0);
	return NFCT_CB_CONTINUE;
}

static int event_cb(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data)
{
	if (type == NFCT_T_DESTROY)
		handle_ct(ct, 1);
	return NFCT_CB_CONTINUE;
}

/* ================= JSON output ================= */

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++) {
		unsigned char c = *s;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

struct agg {
	uint64_t up, down;
};

/* the summary works on a dense client x app index of the apps actually seen */
static int app_idx[MAX_APPS];
static uint16_t app_of[1024];
static int napps_seen;

static int app_slot(uint16_t app)
{
	if (app_idx[app])
		return app_idx[app] - 1;
	if (napps_seen == 1024)
		return 0;
	app_of[napps_seen] = app;
	app_idx[app] = ++napps_seen;
	return napps_seen - 1;
}

static void add_pair(struct agg *m, uint16_t client, uint16_t app, uint64_t up, uint64_t down)
{
	struct agg *a = &m[(size_t)client * 1024 + app_slot(app)];

	a->up += up;
	a->down += down;
}

static int cmp_desc(const void *a, const void *b)
{
	const uint64_t *x = a, *y = b;	/* [total, index] */

	return x[0] < y[0] ? 1 : x[0] > y[0] ? -1 : 0;
}

static void write_window(FILE *f, const char *name, struct ring *g, struct acc *live,
			 uint32_t from)
{
	struct agg *pm;
	uint64_t (*ord)[2], (*aord)[2];
	int nc = nclients, first = 1;

	pm = calloc((size_t)(nc ? nc : 1) * 1024, sizeof(*pm));
	if (!pm)
		return;
	for (size_t i = 0; i < g->n; i++) {
		struct rec *x = ring_at(g, i);

		if (x->t >= from && x->client < nc)
			add_pair(pm, x->client, x->app, x->up, x->down);
	}
	for (int i = 0; i < ACC_SLOTS; i++)
		if (live[i].key && ((live[i].key - 1) >> 16) < (uint32_t)nc)
			add_pair(pm, (live[i].key - 1) >> 16, (live[i].key - 1) & 0xffff,
				 live[i].up, live[i].down);

	ord = calloc(nc ? nc : 1, sizeof(*ord));
	aord = calloc(napps_seen ? napps_seen : 1, sizeof(*aord));
	if (!ord || !aord) {
		free(pm); free(ord); free(aord);
		return;
	}
	/* app totals */
	for (int a = 0; a < napps_seen; a++) {
		aord[a][1] = a;
		for (int c = 0; c < nc; c++)
			aord[a][0] += pm[(size_t)c * 1024 + a].up + pm[(size_t)c * 1024 + a].down;
	}
	for (int c = 0; c < nc; c++) {
		ord[c][1] = c;
		for (int a = 0; a < napps_seen; a++)
			ord[c][0] += pm[(size_t)c * 1024 + a].up + pm[(size_t)c * 1024 + a].down;
	}
	qsort(ord, nc, sizeof(*ord), cmp_desc);
	qsort(aord, napps_seen, sizeof(*aord), cmp_desc);

	fprintf(f, "\"%s\":{\"clients\":[", name);
	for (int k = 0; k < nc && ord[k][0]; k++) {
		int c = ord[k][1], n = 0;
		uint64_t up = 0, down = 0, (*co)[2] = calloc(napps_seen ? napps_seen : 1, sizeof(*co));

		for (int a = 0; a < napps_seen; a++) {
			struct agg *x = &pm[(size_t)c * 1024 + a];

			up += x->up;
			down += x->down;
			if (co) {
				co[a][0] = x->up + x->down;
				co[a][1] = a;
			}
		}
		fprintf(f, "%s{\"c\":%d,\"up\":%llu,\"down\":%llu,\"apps\":[", first ? "" : ",",
			c, (unsigned long long)up, (unsigned long long)down);
		first = 0;
		if (co) {
			qsort(co, napps_seen, sizeof(*co), cmp_desc);
			for (int j = 0; j < napps_seen && j < TOP_APPS && co[j][0]; j++) {
				struct agg *x = &pm[(size_t)c * 1024 + co[j][1]];

				fprintf(f, "%s[%u,%llu,%llu]", n++ ? "," : "", app_of[co[j][1]],
					(unsigned long long)x->up, (unsigned long long)x->down);
			}
			free(co);
		}
		fputs("]}", f);
	}
	fputs("],\"apps\":[", f);
	first = 1;
	for (int k = 0; k < napps_seen && aord[k][0]; k++) {
		int a = aord[k][1], ncl = 0;
		uint64_t up = 0, down = 0;

		for (int c = 0; c < nc; c++) {
			struct agg *x = &pm[(size_t)c * 1024 + a];

			up += x->up;
			down += x->down;
			ncl += !!(x->up | x->down);
		}
		fprintf(f, "%s{\"a\":%u,\"up\":%llu,\"down\":%llu,\"clients\":%d}", first ? "" : ",",
			app_of[a], (unsigned long long)up, (unsigned long long)down, ncl);
		first = 0;
	}
	fputs("]}", f);
	free(pm);
	free(ord);
	free(aord);
}

/* time series: total + per-client, minute data in BUCKET_MIN buckets (24 h),
 * hour data per hour (31 d). Clients with no traffic are omitted. */
#define BUCKET_MIN 5

static void write_series(FILE *f, time_t now)
{
	uint32_t m_now = now / 60, h_now = now / 3600;
	int nb = MIN_KEEP / BUCKET_MIN, nh = HOUR_KEEP, nc = nclients;
	uint32_t m0 = (m_now / BUCKET_MIN + 1) * BUCKET_MIN - MIN_KEEP;
	struct agg *cm = calloc((size_t)(nc + 1) * nb, sizeof(*cm));
	struct agg *ch = calloc((size_t)(nc + 1) * nh, sizeof(*ch));
	uint32_t h0 = h_now + 1 - nh;

	if (!cm || !ch) {
		free(cm); free(ch);
		return;
	}
	for (size_t i = 0; i < mins.n; i++) {
		struct rec *x = ring_at(&mins, i);

		if (x->t >= m0 && x->client < nc) {
			int b = (x->t - m0) / BUCKET_MIN;

			if (b < nb) {
				cm[(size_t)x->client * nb + b].up += x->up;
				cm[(size_t)x->client * nb + b].down += x->down;
			}
		}
	}
	for (int i = 0; i < ACC_SLOTS; i++) {
		uint32_t c = (cur_min[i].key - 1) >> 16;
		int b = (cur_min_t - m0) / BUCKET_MIN;

		if (cur_min[i].key && c < (uint32_t)nc && cur_min_t >= m0 && b < nb) {
			cm[(size_t)c * nb + b].up += cur_min[i].up;
			cm[(size_t)c * nb + b].down += cur_min[i].down;
		}
	}
	for (size_t i = 0; i < hours.n; i++) {
		struct rec *x = ring_at(&hours, i);

		if (x->t >= h0 && x->client < nc) {
			ch[(size_t)x->client * nh + (x->t - h0)].up += x->up;
			ch[(size_t)x->client * nh + (x->t - h0)].down += x->down;
		}
	}
	for (int i = 0; i < ACC_SLOTS; i++) {
		uint32_t c = (cur_hour[i].key - 1) >> 16;

		if (cur_hour[i].key && c < (uint32_t)nc && cur_hour_t >= h0) {
			ch[(size_t)c * nh + (cur_hour_t - h0)].up += cur_hour[i].up;
			ch[(size_t)c * nh + (cur_hour_t - h0)].down += cur_hour[i].down;
		}
	}
	/* totals go into row nc */
	for (int c = 0; c < nc; c++) {
		for (int b = 0; b < nb; b++) {
			cm[(size_t)nc * nb + b].up += cm[(size_t)c * nb + b].up;
			cm[(size_t)nc * nb + b].down += cm[(size_t)c * nb + b].down;
		}
		for (int b = 0; b < nh; b++) {
			ch[(size_t)nc * nh + b].up += ch[(size_t)c * nh + b].up;
			ch[(size_t)nc * nh + b].down += ch[(size_t)c * nh + b].down;
		}
	}
	fprintf(f, "{\"generated\":%lld,\"min\":{\"start\":%lld,\"step\":%d,\"series\":{",
		(long long)now, (long long)m0 * 60, BUCKET_MIN * 60);
	for (int c = 0, first = 1; c <= nc; c++) {
		uint64_t any = 0;

		for (int b = 0; b < nb; b++)
			any |= cm[(size_t)c * nb + b].up | cm[(size_t)c * nb + b].down;
		if (!any && c < nc)
			continue;
		if (c == nc)
			fprintf(f, "%s\"all\":[", first ? "" : ",");
		else
			fprintf(f, "%s\"%d\":[", first ? "" : ",", c);
		first = 0;
		for (int b = 0; b < nb; b++)
			fprintf(f, "%s%llu,%llu", b ? "," : "",
				(unsigned long long)cm[(size_t)c * nb + b].up,
				(unsigned long long)cm[(size_t)c * nb + b].down);
		fputc(']', f);
	}
	fprintf(f, "}},\"hour\":{\"start\":%lld,\"step\":3600,\"series\":{", (long long)h0 * 3600);
	for (int c = 0, first = 1; c <= nc; c++) {
		uint64_t any = 0;

		for (int b = 0; b < nh; b++)
			any |= ch[(size_t)c * nh + b].up | ch[(size_t)c * nh + b].down;
		if (!any && c < nc)
			continue;
		if (c == nc)
			fprintf(f, "%s\"all\":[", first ? "" : ",");
		else
			fprintf(f, "%s\"%d\":[", first ? "" : ",", c);
		first = 0;
		for (int b = 0; b < nh; b++)
			fprintf(f, "%s%llu,%llu", b ? "," : "",
				(unsigned long long)ch[(size_t)c * nh + b].up,
				(unsigned long long)ch[(size_t)c * nh + b].down);
		fputc(']', f);
	}
	fputs("}}", f);
	/* uplink: 1-minute buckets for 24 h, hourly for 31 d (oldest first) */
	fprintf(f, ",\"wan\":{\"min\":{\"start\":%lld,\"step\":60,\"data\":[",
		(long long)(m_now + 1 - MIN_KEEP) * 60);
	for (uint32_t t = m_now + 1 - MIN_KEEP, k = 0; t <= m_now; t++, k++) {
		struct wslot *x = &wan_min[t % MIN_KEEP];
		int ok = x->t == t;

		fprintf(f, "%s%llu,%llu", k ? "," : "", ok ? (unsigned long long)x->up : 0ULL,
			ok ? (unsigned long long)x->down : 0ULL);
	}
	fprintf(f, "]},\"hour\":{\"start\":%lld,\"step\":3600,\"data\":[",
		(long long)(h_now + 1 - HOUR_KEEP) * 3600);
	for (uint32_t t = h_now + 1 - HOUR_KEEP, k = 0; t <= h_now; t++, k++) {
		struct wslot *x = &wan_hour[t % HOUR_KEEP];
		int ok = x->t == t;

		fprintf(f, "%s%llu,%llu", k ? "," : "", ok ? (unsigned long long)x->up : 0ULL,
			ok ? (unsigned long long)x->down : 0ULL);
	}
	fputs("]}}}\n", f);
	free(cm);
	free(ch);
}

static FILE *open_tmp(const char *name, char *tmp, size_t n)
{
	snprintf(tmp, n, "%s/.%s.tmp", out_dir, name);
	return fopen(tmp, "w");
}

static void commit_tmp(const char *name, const char *tmp)
{
	char dst[256];

	snprintf(dst, sizeof(dst), "%s/%s", out_dir, name);
	rename(tmp, dst);
}

static void write_outputs(time_t now)
{
	char tmp[256];
	uint32_t m_now = now / 60, h_now = now / 3600;
	FILE *f;

	mkdir(out_dir, 0755);
	if ((f = open_tmp("summary.json", tmp, sizeof(tmp)))) {
		int first = 1;

		fprintf(f, "{\"generated\":%lld,\"interval\":%d,\"clients\":[", (long long)now, interval);
		for (int c = 0; c < nclients; c++) {
			fprintf(f, "%s{\"key\":", c ? "," : "");
			json_str(f, clients[c].key);
			fputs(",\"ip\":", f);
			json_str(f, clients[c].ip);
			fputs(",\"name\":", f);
			json_str(f, clients[c].name);
			fprintf(f, ",\"last\":%lld}", (long long)clients[c].last);
		}
		fputs("],\"appnames\":{", f);
		for (int a = 0; a < MAX_APPS; a++) {
			if (!app_name[a])
				continue;
			fprintf(f, "%s\"%d\":[", first ? "" : ",", a);
			json_str(f, app_name[a]);
			fputc(',', f);
			json_str(f, app_cat[a]);
			fputc(']', f);
			first = 0;
		}
		fputs("},\"windows\":{", f);
		write_window(f, "1h", &mins, cur_min, m_now - 59);
		fputc(',', f);
		write_window(f, "24h", &mins, cur_min, m_now - MIN_KEEP + 1);
		fputc(',', f);
		write_window(f, "7d", &hours, cur_hour, h_now - 7 * 24 + 1);
		fputc(',', f);
		write_window(f, "30d", &hours, cur_hour, h_now - 30 * 24 + 1);
		fputs("}}\n", f);
		fclose(f);
		commit_tmp("summary.json", tmp);
	}
	if ((f = open_tmp("series.json", tmp, sizeof(tmp)))) {
		write_series(f, now);
		fclose(f);
		commit_tmp("series.json", tmp);
	}
}

/* ================= persistence ================= */

#define STATE_MAGIC 0x74726664u	/* "trfd" */
#define STATE_VER   2	/* v2: + uplink hour series; v1 still loads */

static void state_save(void)
{
	char tmp[256];
	uint32_t hdr[4] = { STATE_MAGIC, STATE_VER, nclients, 0 };
	FILE *f;

	/* include the live hour so a restart loses at most one interval */
	struct ring snap = hours;

	snprintf(tmp, sizeof(tmp), "%s.tmp", state_file);
	f = fopen(tmp, "wb");
	if (!f)
		return;
	hdr[3] = snap.n;
	for (int i = 0; i < ACC_SLOTS; i++)
		hdr[3] += cur_hour[i].key && (cur_hour[i].up || cur_hour[i].down);
	fwrite(hdr, sizeof(hdr), 1, f);
	fwrite(clients, sizeof(clients[0]), nclients, f);
	for (size_t i = 0; i < snap.n; i++)
		fwrite(ring_at(&snap, i), sizeof(struct rec), 1, f);
	for (int i = 0; i < ACC_SLOTS; i++)
		if (cur_hour[i].key && (cur_hour[i].up || cur_hour[i].down)) {
			struct rec x = { cur_hour_t, (cur_hour[i].key - 1) >> 16,
					 (cur_hour[i].key - 1) & 0xffff,
					 cur_hour[i].up, cur_hour[i].down };

			fwrite(&x, sizeof(x), 1, f);
		}
	fwrite(wan_hour, sizeof(wan_hour), 1, f);
	if (fclose(f) == 0)
		rename(tmp, state_file);
}

static void state_load(void)
{
	uint32_t hdr[4];
	FILE *f = fopen(state_file, "rb");

	if (!f)
		return;
	if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == STATE_MAGIC &&
	    (hdr[1] == 1 || hdr[1] == STATE_VER) && hdr[2] <= MAX_CLIENTS) {
		nclients = fread(clients, sizeof(clients[0]), hdr[2], f);
		/* names are recomputed (DHCP / confirmed PTR) rather than trusted */
		for (int i = 0; i < nclients; i++)
			clients[i].name[0] = 0;
		for (uint32_t i = 0; i < hdr[3]; i++) {
			struct rec x;

			if (fread(&x, sizeof(x), 1, f) != 1)
				break;
			if (x.client < nclients)
				ring_push(&hours, &x);
		}
		if (hdr[1] >= 2 && fread(wan_hour, sizeof(wan_hour), 1, f) != 1)
			memset(wan_hour, 0, sizeof(wan_hour));
		logmsg(LOG_INFO, "restored %d clients, %zu hour records", nclients, hours.n);
	}
	fclose(f);
}

/* ================= main ================= */

static void on_signal(int sig)
{
	stop = 1;
}

int main(int argc, char **argv)
{
	struct nfct_handle *dump_h, *ev_h;
	struct sigaction sa = { .sa_handler = on_signal };
	time_t last_dump = 0, last_write = 0, last_meta = 0, last_save = time(NULL);
	uint32_t family = AF_UNSPEC;
	int opt, evfd;

	while ((opt = getopt(argc, argv, "i:o:s:w:x:v")) != -1) {
		switch (opt) {
		case 'i': interval = atoi(optarg) > 5 ? atoi(optarg) : 5; break;
		case 'o': out_dir = optarg; break;
		case 's': state_file = optarg; break;
		case 'w': wan_dev = optarg; break;
		case 'x':
			if (nexcl == MAX_EXCLUDE || parse_cidr(optarg, &excl[nexcl])) {
				/* refuse to run: silently tracking a host the admin
				 * meant to exclude is the worse failure */
				fprintf(stderr, "trafficd: bad or too many -x: %s\n", optarg);
				openlog("trafficd", LOG_PID, LOG_DAEMON);
				logmsg(LOG_ERR, "bad or too many exclude: %s", optarg);
				return 2;
			}
			nexcl++;
			break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: trafficd [-i secs] [-o outdir] [-s statefile] [-w wandev] [-x cidr]... [-v]\n");
			return 2;
		}
	}
	openlog("trafficd", LOG_PID | (verbose ? LOG_PERROR : 0), LOG_DAEMON);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	ring_init(&mins, MIN_CAP);
	ring_init(&hours, HOUR_CAP);
	state_load();
	cur_min_t = time(NULL) / 60;
	cur_hour_t = time(NULL) / 3600;

	dump_h = nfct_open(CONNTRACK, 0);
	ev_h = nfct_open(CONNTRACK, NF_NETLINK_CONNTRACK_DESTROY);
	if (!dump_h || !ev_h) {
		logmsg(LOG_ERR, "ctnetlink: %s", strerror(errno));
		return 1;
	}
	nfct_callback_register(dump_h, NFCT_T_ALL, dump_cb, NULL);
	nfct_callback_register(ev_h, NFCT_T_DESTROY, event_cb, NULL);
	evfd = nfct_fd(ev_h);
	fcntl(evfd, F_SETFL, fcntl(evfd, F_GETFL) | O_NONBLOCK);
	logmsg(LOG_INFO, "started: interval %ds, wan %s, state %s, %d excluded subnets",
	       interval, wan_dev, state_file, nexcl);

	while (!stop) {
		time_t now = time(NULL);
		struct timeval tv = { 1, 0 };
		fd_set rf;

		if (now - last_meta >= 60 || now < last_meta) {
			load_addrs();
			load_neigh();
			load_names();
			load_protos();
			resolve_names();
			last_meta = now;
		}
		if (now - last_dump >= interval || now < last_dump) {
			gen++;
			nfct_query(dump_h, NFCT_Q_DUMP, &family);
			ct_sweep();
			priming = 0;
			last_dump = now;
		}
		if (now / 60 != (uint32_t)last_write / 60) {
			if (now / 60 != cur_min_t) {
				acc_flush(cur_min, &mins, cur_min_t);
				cur_min_t = now / 60;
			}
			if (now / 3600 != cur_hour_t) {
				acc_flush(cur_hour, &hours, cur_hour_t);
				cur_hour_t = now / 3600;
			}
			ring_expire(&mins, now / 60 - MIN_KEEP);
			ring_expire(&hours, now / 3600 - HOUR_KEEP);
			write_outputs(now);
			last_write = now;
		}
		if (now - last_save >= 3600) {
			state_save();
			last_save = now;
		}
		FD_ZERO(&rf);
		FD_SET(evfd, &rf);
		if (select(evfd + 1, &rf, NULL, NULL, &tv) > 0)
			nfct_catch(ev_h);	/* drains; returns on EAGAIN */
	}
	state_save();
	nfct_close(dump_h);
	nfct_close(ev_h);
	logmsg(LOG_INFO, "stopped");
	return 0;
}
