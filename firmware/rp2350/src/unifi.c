/*
 * A UniFi controller's view of the switch, see unifi.h. The packets, the JSON
 * and the configuration are unifi_proto.c's; this is the connection, what is
 * reported, and what the controller's replies do. Commands "unifi ..." are in
 * rtl_ext.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

#include "events.h"
#include "lldp_nbr.h"
#include "plat.h"
#include "rng.h"
#include "store.h"
#include "unifi.h"
#include "unifi_proto.h"

#define JSON_MAX	8192		/* with the MAC and LLDP tables */
#define REPLY_MAX	12288		/* what the controller sends, compressed */
#define TEXT_MAX	16384		/* and unpacked */
#define CMDS_MAX	2048
#define REQ_MAX		(200 + UNIFI_SEAL_AT + JSON_MAX + 4 + 16)	/* headers, packet header, zlib, JSON, Adler-32, padding */
#define MACS_MAX	48		/* addresses reported */
#define MACS_EVERY_S	60		/* the table read again, at most */
#define L2_OUT		3072		/* "l2" output read */
#define TIMEOUT_S	10
#define INTERVAL_S	10
#ifndef WATCH_S
#define WATCH_S		120		/* to reach the controller again after its configuration */
#endif
/* Our cfgversion until the controller's configuration is applied: none like it, so it sends that */
#define NONE		"0000000000000000"
#define VERSION		"1.1.0"		/* what we claim to run; the oldest these models take */
/* Where a UniFi device informs when nothing else says: a name the network's DNS may have */
#define DEFAULT_URL	"http://unifi:8080/inform"
#define HOST_MAX	48		/* dns.c's DNS_NAME_LEN */
/* What the controller may set (UNIFI_SWITCH_CAP_*): the keys it sends depend on them */
#ifndef FEATURE_CAPS
#define FEATURE_CAPS	(4 | 8 | 32 | 64)	/* port isolation, storm control, egress rate limit, STP */
#endif
#define STP_CAPS	(1 | 2 | 8)		/* BPDU guard, auto edge, port cost */

#define LED_FAST	1		/* main.c's SYS_LED_* */
#define LED_ON		3

static bool on;
static struct unifi_model model;
static bool model_set;
static uint8_t key[16];

static struct {
	struct tcp_pcb *pcb;
	uint8_t *req, *reply;
	size_t req_len, sent, got;
	uint32_t left;			/* s before giving up */
	bool done, failed;
} x;

static uint32_t next_in = 1, interval = INTERVAL_S;
static char cfgversion[24];
static struct unifi_sw *applied;	/* what we last applied, NULL: nothing yet */
static uint32_t watch;			/* s left to reach the controller after a change, 0: not watching */
static bool locating, upgrade_told, bad_told;
static const char *state = "off";
static uint32_t informs, replies;
static int last_http;
static char dhcp_url[sizeof("http://255.255.255.255:8080/inform")];	/* from DHCP option 43, "": none */
static char resolved[HOST_MAX];		/* the name looked up last, "": none */
static struct {
	uint8_t mac[6];
	uint16_t vlan;
	uint8_t front;			/* front-panel port */
	bool fixed;			/* a static entry */
} l2tab[MACS_MAX];
static uint8_t nmacs;
static uint32_t macs_at;		/* uptime of the last read, 0: never */
static ip4_addr_t resolved_ip;


/* The URL informed: the one set or the controller's, else from DHCP, else the default */
static const char *url_now(const char **from)
{
	if (store.unifi_url[0]) {
		*from = "";
		return store.unifi_url;
	}
	if (dhcp_url[0]) {
		*from = " (from DHCP)";
		return dhcp_url;
	}
	*from = " (the default)";
	return DEFAULT_URL;
}

/* Once adopted, the URL it was adopted through is kept, as UniFi devices do */
static void keep_url(void)
{
	const char *from, *u = url_now(&from);

	if (!store.unifi_url[0])
		snprintf(store.unifi_url, sizeof(store.unifi_url), "%s", u);
}


/* ---- the configuration in flash ---- */

/* The controller's configuration last confirmed, in its own sector */
struct saved {
	char magic[4];
	char cfgversion[24];
	struct unifi_sw sw;
};
_Static_assert(sizeof(struct saved) <= 4096, "one sector");

static void save(void)
{
	struct saved *v = calloc(1, sizeof(*v));

	if (!v)
		return;
	if (applied) {
		memcpy(v->magic, "UNC2", 4);	/* 2: with STP, isolation, storm and egress */
		snprintf(v->cfgversion, sizeof(v->cfgversion), "%s", cfgversion);
		v->sw = *applied;
	}
	plat_sector_write(SECTOR_UNIFI, (const uint8_t *)v, sizeof(*v));
	free(v);
}

static void run(const struct unifi_sw *sw, const struct unifi_sw *before, bool *ok)
{
	char *cmds = malloc(CMDS_MAX), *out = malloc(512);
	long n;

	*ok = false;
	if (!cmds || !out)
		goto done;
	n = unifi_commands(sw, before, &model, cmds, CMDS_MAX);
	if (n < 0) {
		event(EV_CONFIG, 0, 0, "UniFi: configuration too large");
		goto done;
	}
	*ok = true;
	/* one command at a time: rtl_run() takes a line */
	for (char *l = cmds, *e; *l; l = e + 1) {
		bool k;

		e = strchr(l, '\n');
		*e = 0;
		rtl_run(l, out, 512, &k);
		if (!k) {
			*ok = false;
			event(EV_CONFIG, 0, 0, "UniFi: \"%.60s\" failed", l);
		}
	}
done:
	free(cmds);
	free(out);
}

static void use_key(void)
{
	if (!store.unifi_adopted)
		unifi_hex16(UNIFI_DEFAULT_KEY, key);
	else
		memcpy(key, store.unifi_key, 16);
}

static void forget(const char *why)
{
	free(applied);
	applied = NULL;
	save();
	store.unifi_adopted = 0;
	store.unifi_gcm = 0;
	memset(store.unifi_key, 0, sizeof(store.unifi_key));
	store_save();
	use_key();
	cfgversion[0] = 0;
	watch = 0;		/* nothing of the controller's left to check */
	event(EV_CONFIG, 0, 0, "UniFi: %s, back to the default key", why);
}


/* ---- the inform ---- */

/* The switch's MAC table, from the "l2" command: "aa:bb:cc:dd:ee:ff<TAB>vlan<TAB>learned|static<TAB>port" */
static void read_macs(void)
{
	char *out = malloc(L2_OUT), line[8] = "l2";
	const uint8_t *own = rtl_mac();
	bool ok;

	if (!out)
		return;
	rtl_run(line, out, L2_OUT, &ok);
	out[L2_OUT - 1] = 0;
	nmacs = 0;
	for (char *l = out, *e; l && *l && nmacs < MACS_MAX; l = e ? e + 1 : NULL) {
		unsigned m[6], vlan;
		char type[12], port[8];

		e = strchr(l, '\n');
		if (e)
			*e = 0;
		if (sscanf(l, "%2x:%2x:%2x:%2x:%2x:%2x %u %11s %7s", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &vlan, type, port) != 9)
			continue;
		/* print_phys_port() writes port 10 as ':' */
		unsigned front = port[1] ? 0 : port[0] == ':' ? 10 : port[0] >= '1' && port[0] <= '9' ? port[0] - '0' : 0;
		if (!front || vlan > 4094)
			continue;	/* the CPU, a LAG */
		for (int i = 0; i < 6; i++)
			l2tab[nmacs].mac[i] = m[i];
		if (!memcmp(l2tab[nmacs].mac, own, 6))
			continue;
		l2tab[nmacs].vlan = vlan;
		l2tab[nmacs].front = front;
		l2tab[nmacs].fixed = !strcmp(type, "static");
		nmacs++;
	}
	free(out);
}

static void pick_model(void)
{
	static struct rtl_port ports[RTL_MAX_PORTS];
	uint8_t n, copper = 0, sfp = 0;

	if (model_set || !(n = rtl_ports_read(ports, RTL_MAX_PORTS)))
		return;
	for (uint8_t i = 0; i < n; i++)
		ports[i].sfp ? sfp++ : copper++;
	unifi_model_pick(copper, sfp, &model);
	model_set = true;
}

static size_t inform_json(char *b, size_t max)
{
	const uint8_t *mac = rtl_mac();
	static struct rtl_port ports[RTL_MAX_PORTS];
	uint8_t n = rtl_ports_read(ports, RTL_MAX_PORTS);
	const ip4_addr_t *ip = netif_default ? netif_ip4_addr(netif_default) : IP4_ADDR_ANY4;
	char macs[18], serial[13];
	size_t k = 0;

	snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	k = app(b, max, k, "{\"mac\":\"%s\",\"serial\":\"%s\",\"model\":\"%s\",\"model_display\":\"%s\","
		"\"version\":\"" VERSION "\",\"ip\":\"%s\",\"hostname\":", macs, serial, model.code, model.name, ip4addr_ntoa(ip));
	k = app_json_str(b, max, k, hostname);
	k = app(b, max, k, ",\"inform_url\":");
	const char *from;
	k = app_json_str(b, max, k, url_now(&from));
	k = app(b, max, k, ",\"uptime\":%lu,\"cfgversion\":\"%s\",\"default\":%s,\"_default_key\":%s,\"state\":%d,"
		"\"fw_caps\":0,\"isolated\":false,\"locating\":%s,\"selfrun_beacon\":false,"
		"\"switch_caps\":{\"feature_caps\":%lu,\"vlan_caps\":7,\"stp_caps\":%u,\"storm_control_caps\":0}",
		(unsigned long)plat_uptime_s(), cfgversion[0] || !store.unifi_adopted ? cfgversion : NONE, store.unifi_adopted ? "false" : "true",
		store.unifi_adopted ? "false" : "true", store.unifi_adopted ? 4 : 1, locating ? "true" : "false",
		(unsigned long)FEATURE_CAPS, STP_CAPS);
	if (!store.unifi_adopted)
		return app(b, max, k, "}");
	k = app(b, max, k, ",\"sys_stats\":{\"cpu\":\"0\",\"mem_total\":%u,\"mem_used\":%u,\"mem_buffer\":0}",
		520 * 1024, (unsigned)(520 * 1024 - plat_heap_free()));
	k = app(b, max, k, ",\"ethernet_table\":[{\"mac\":\"%s\",\"name\":\"eth0\",\"num_port\":%u}],\"port_table\":[",
		macs, model.ports);
	for (uint8_t p = 1; p <= model.ports; p++) {
		static const uint16_t mbps[] = { 10, 100, 1000, 500, 10000, 2500, 5000 };
		const struct rtl_port *r = model.front[p] && model.front[p] <= n ? &ports[model.front[p] - 1] : NULL;
		bool up = r && r->link && r->link <= 7;

		k = app(b, max, k, "%s{\"port_idx\":%u,\"ifname\":\"eth%u\",\"media\":\"%s\",\"poe_caps\":0,\"is_uplink\":false,"
			"\"enable\":%s,\"up\":%s,\"speed\":%u,\"full_duplex\":%s,\"rx_bytes\":%llu,\"tx_bytes\":%llu,"
			"\"rx_packets\":%llu,\"tx_packets\":%llu,\"rx_errors\":%lu,\"tx_errors\":%lu,\"rx_dropped\":%lu,"
			"\"tx_dropped\":%lu}",
			p > 1 ? "," : "", p, p - 1, model.sfp[p] ? "SFP+" : "GE", r && r->enabled ? "true" : "false",
			up ? "true" : "false", up ? mbps[r->link - 1] : 0, up ? "true" : "false",
			r ? (unsigned long long)r->in_octets : 0ULL, r ? (unsigned long long)r->out_octets : 0ULL,
			r ? (unsigned long long)(r->in_ucast + r->in_mcast + r->in_bcast) : 0ULL,
			r ? (unsigned long long)(r->out_ucast + r->out_mcast + r->out_bcast) : 0ULL,
			r ? (unsigned long)r->in_errors : 0UL, r ? (unsigned long)r->out_errors : 0UL,
			r ? (unsigned long)r->in_discards : 0UL, r ? (unsigned long)r->out_discards : 0UL);
		/* the addresses learned on it, as room allows: the rest of the inform has its 1.5 KB */
		if (k && b[k - 1] == '}')
			k--;	/* back over the port's "}" */
		k = app(b, max, k, ",\"mac_table\":[");
		bool first = true;
		for (uint8_t i = 0; i < nmacs && k + 1600 < max; i++) {
			if (!model.front[p] || l2tab[i].front != model.front[p])
				continue;
			const uint8_t *m = l2tab[i].mac;
			k = app(b, max, k, "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"vlan\":%u,\"age\":0,\"static\":%s}",
				first ? "" : ",", m[0], m[1], m[2], m[3], m[4], m[5], l2tab[i].vlan, l2tab[i].fixed ? "true" : "false");
			first = false;
		}
		k = app(b, max, k, "]}");
	}
	k = app(b, max, k, "],\"lldp_table\":[");
	bool first = true;
	for (uint8_t p = 1; p <= model.ports; p++) {
		const struct lldp_nbr *nb = model.front[p] ? lldp_nbr_get(model.front[p], plat_uptime_s()) : NULL;

		if (!nb || k + 800 >= max)
			continue;
		k = app(b, max, k, "%s{\"local_port_idx\":%u,\"local_port_name\":\"Port %u\",\"is_wired\":true,\"chassis_id\":",
			first ? "" : ",", p, p);
		k = app_json_str(b, max, k, nb->chassis);
		k = app(b, max, k, ",\"port_id\":");
		k = app_json_str(b, max, k, nb->port);
		if (nb->name[0]) {
			k = app(b, max, k, ",\"system_name\":");
			k = app_json_str(b, max, k, nb->name);
		}
		if (nb->port_desc[0]) {
			k = app(b, max, k, ",\"port_descr\":");
			k = app_json_str(b, max, k, nb->port_desc);
		}
		k = app(b, max, k, "}");
		first = false;
	}
	return app(b, max, k, "]}");
}


/* ---- what the controller says ---- */

static void apply(const char *system_cfg, const char *version)
{
	struct unifi_sw *sw = malloc(sizeof(*sw));
	bool ok;

	if (!sw)
		return;
	if (!version[0]) {
		/* nothing to report back as applied, nor to refuse if it cuts the switch off */
		event(EV_CONFIG, 0, 0, "UniFi: configuration without a version not applied");
		free(sw);
		return;
	}
	if (!strcmp(version, store.unifi_bad)) {
		if (!bad_told)
			event(EV_CONFIG, 0, 0, "UniFi: configuration %s not applied: it cut the switch off before", version);
		bad_told = true;
		free(sw);
		return;
	}
#ifdef UNIFI_TRACE
	for (const char *l = system_cfg, *e; *l; l = e ? e + 1 : l + strlen(l)) {
		e = strchr(l, '\n');
		printf("CFG %.*s\n", e ? (int)(e - l) : (int)strlen(l), l);
	}
#endif
	if (!unifi_parse(system_cfg, sw)) {
		event(EV_CONFIG, 0, 0, "UniFi: configuration not understood");
		free(sw);
		return;
	}
	pick_model();
	run(sw, applied, &ok);
	free(applied);
	applied = sw;
	snprintf(cfgversion, sizeof(cfgversion), "%s", version);
	watch = WATCH_S;	/* kept once the controller is reached again */
	bad_told = false;
	event(EV_CONFIG, 0, 0, "UniFi: configuration %s applied%s", version, ok ? "" : " with errors");
}

static void handle(char *j)
{
	char type[24], s[96];
	long v;

	replies++;
	if (!unifi_json_str(j, "_type", type, sizeof(type)))
		return;
	if (unifi_json_int(j, "interval", &v) && v >= 2 && v <= 600)
		interval = v;
	if (!strcmp(type, "setparam")) {
		char *mgmt = malloc(1024), *sys;
		char version[24] = "";

		if (mgmt && unifi_json_str(j, "mgmt_cfg", mgmt, 1024)) {
			uint8_t k[16];

			if (unifi_kv(mgmt, "authkey", s, sizeof(s)) && unifi_hex16(s, k)
			    && (!store.unifi_adopted || memcmp(store.unifi_key, k, 16))) {
				bool was = store.unifi_adopted;

				memcpy(store.unifi_key, k, 16);
				store.unifi_adopted = 1;
				keep_url();
				store_save();
				event(EV_CONFIG, 0, 0, was ? "UniFi: a new key from the controller" : "UniFi: adopted");
			}
			if (unifi_kv(mgmt, "use_aes_gcm", s, sizeof(s)) && !strcmp(s, "true") && !store.unifi_gcm) {
				store.unifi_gcm = 1;
				store_save();
			}
			unifi_kv(mgmt, "cfgversion", version, sizeof(version));
		}
		if (!unifi_json_str(j, "cfgversion", s, sizeof(s)) || strlen(s) >= sizeof(version))
			s[0] = 0;
		if (s[0])
			strcpy(version, s);
		free(mgmt);
		/* last: unescaped where it is, with no copy, so j is no longer whole */
		if ((sys = unifi_json_str_here(j, "system_cfg")))
			apply(sys, version);
		use_key();
	} else if (!strcmp(type, "cmd")) {
		char cmd[24];

		if (!unifi_json_str(j, "cmd", cmd, sizeof(cmd)))
			return;
		if (!strcmp(cmd, "set-adopt")) {
			uint8_t k[16];

			if (unifi_json_str(j, "key", s, sizeof(s)) && unifi_hex16(s, k)) {
				memcpy(store.unifi_key, k, 16);
				store.unifi_adopted = 1;
			}
			if (unifi_json_str(j, "uri", s, sizeof(s)) && strcmp(s, store.unifi_url)) {
				if (unifi_url_ok(s))
					snprintf(store.unifi_url, sizeof(store.unifi_url), "%s", s);
				else
					event(EV_CONFIG, 0, 0, "UniFi: inform URL %.80s not usable, kept the one before", s);
			}
			keep_url();
			store_save();
			use_key();
			event(EV_CONFIG, 0, 0, "UniFi: adopted by %s", store.unifi_url);
		} else if (!strcmp(cmd, "reboot")) {
			event(EV_SYSTEM, 0, 0, "UniFi: restart");
			plat_reboot();
		} else if (!strcmp(cmd, "setdefault")) {
			forget("reset by the controller");
		} else if (!strcmp(cmd, "set-locate") || !strcmp(cmd, "unset-locate")) {
			locating = cmd[0] == 's';
			plat_sys_led(locating ? LED_FAST : LED_ON);
		}
	} else if (!strcmp(type, "upgrade")) {
		if (!upgrade_told)
			event(EV_SYSTEM, 0, 0, "UniFi: firmware upgrade refused: this is no UniFi switch");
		upgrade_told = true;
	} else if (!strcmp(type, "reboot")) {
		event(EV_SYSTEM, 0, 0, "UniFi: restart");
		plat_reboot();
	} else if (!strcmp(type, "setdefault")) {
		forget("reset by the controller");
	}
}


/* ---- one exchange over HTTP ---- */

/* true: the pcb was aborted, which a callback must return ERR_ABRT for */
static bool finish(bool failed)
{
	bool aborted = false;

	if (x.pcb) {
		tcp_arg(x.pcb, NULL);
		tcp_recv(x.pcb, NULL);
		tcp_sent(x.pcb, NULL);
		tcp_err(x.pcb, NULL);
		if (tcp_close(x.pcb) != ERR_OK) {
			tcp_abort(x.pcb);
			aborted = true;
		}
		x.pcb = NULL;
	}
	x.done = true;
	x.failed = failed;
	return aborted;
}

static void pump(void)
{
	while (x.sent < x.req_len && x.pcb) {
		u16_t n = tcp_sndbuf(x.pcb);

		if (!n)
			break;
		if (n > x.req_len - x.sent)
			n = x.req_len - x.sent;
		/* copied: x.req is freed when the exchange ends, maybe before lwIP is done with it */
		if (tcp_write(x.pcb, x.req + x.sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
			break;
		x.sent += n;
	}
	if (x.pcb)
		tcp_output(x.pcb);
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	pump();
	return ERR_OK;
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	if (!p)
		return finish(false) ? ERR_ABRT : ERR_OK;
	for (struct pbuf *q = p; q; q = q->next) {
		size_t n = q->len;

		if (n > REPLY_MAX - x.got)
			n = REPLY_MAX - x.got;
		memcpy(x.reply + x.got, q->payload, n);
		x.got += n;
	}
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
	x.pcb = NULL;		/* freed by lwIP */
	finish(true);
}

static err_t connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
	pump();
	return ERR_OK;
}

/* "http://<a.b.c.d or a name>[:port]/path" */
static bool parse_url(const char *url, char host[HOST_MAX], uint16_t *port, const char **path)
{
	const char *h = url + 7, *e;
	size_t n;

	if (strncmp(url, "http://", 7))
		return false;
	e = h + strcspn(h, ":/");
	n = e - h;
	if (!n || n >= HOST_MAX)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!(h[i] >= 'a' && h[i] <= 'z') && !(h[i] >= 'A' && h[i] <= 'Z') && !(h[i] >= '0' && h[i] <= '9')
		    && h[i] != '-' && h[i] != '.')
			return false;
	memcpy(host, h, n);
	host[n] = 0;
	*port = 80;
	if (*e == ':') {
		char *end;
		long v = strtol(e + 1, &end, 10);

		if (v < 1 || v > 65535)
			return false;
		*port = v;
		e = end;
	}
	*path = *e == '/' ? e : "/inform";
	return *e == '/' || !*e;
}

bool unifi_url_ok(const char *url)
{
	char host[HOST_MAX];
	uint16_t port;
	const char *path;

	return strlen(url) < sizeof(store.unifi_url) && parse_url(url, host, &port, &path);
}

/* The controller's address, NULL: being looked up, or not found (state says) */
static const ip4_addr_t *address(const char *host)
{
	static ip4_addr_t ip;
	uint8_t a[4];

	if (ip4addr_aton(host, &ip))
		return &ip;
	if (resolved[0] && !strcmp(resolved, host))
		return &resolved_ip;
	switch (rtl_resolve(host, a)) {
	case 0:
		state = "looking up the controller";
		next_in = 1;	/* again in a second */
		return NULL;
	case 1:
		IP4_ADDR(&resolved_ip, a[0], a[1], a[2], a[3]);
		snprintf(resolved, sizeof(resolved), "%s", host);
		return &resolved_ip;
	default:
		state = "the controller's name not found";
		return NULL;
	}
}

void unifi_dhcp_option43(const uint8_t *d, uint8_t len)
{
	/* sub-options: Ubiquiti's 1 is the controller's IPv4 address */
	for (uint8_t i = 0; i + 2 <= len && i + 2 + d[i + 1] <= len; i += 2 + d[i + 1])
		if (d[i] == 1 && d[i + 1] == 4) {
			snprintf(dhcp_url, sizeof(dhcp_url), "http://%u.%u.%u.%u:8080/inform", d[i + 2], d[i + 3], d[i + 4], d[i + 5]);
			return;
		}
}

bool unifi_enabled(void)
{
	return on;
}

static void start(void)
{
	const ip4_addr_t *to4;
	char host[HOST_MAX];
	uint16_t port;
	const char *path, *from;
	char *json = NULL;
	uint8_t iv[16];
	long n;

	memset(&x, 0, sizeof(x));
	if (!parse_url(url_now(&from), host, &port, &path)) {
		state = "no inform URL";
		return;
	}
	pick_model();
	if (!model_set) {	/* the bridge isn't up yet */
		state = "waiting for the switch's ports";
		return;
	}
	if (!(to4 = address(host)))
		return;
	if (store.unifi_adopted && (!macs_at || plat_uptime_s() - macs_at >= MACS_EVERY_S)) {
		read_macs();
		macs_at = plat_uptime_s();
		if (!macs_at)
			macs_at = 1;
	}
	/* the request: room for the request line and headers, then the packet, the JSON
	   written where the packet has it, so that it is sealed in place */
	x.req = malloc(REQ_MAX);
	x.reply = malloc(REPLY_MAX);
	if (!x.req || !x.reply)
		goto fail;
	json = (char *)x.req + 200 + UNIFI_SEAL_AT;
	size_t jl = inform_json(json, JSON_MAX);
	if (jl >= JSON_MAX - 1) {
		free(x.req);
		free(x.reply);
		memset(&x, 0, sizeof(x));
		state = "inform too large";
		return;
	}
	rng_get(NULL, iv, sizeof(iv));
	n = unifi_seal(x.req + 200, REQ_MAX - 200, rtl_mac(), key, store.unifi_gcm, iv, json, jl);
	if (n < 0)
		goto fail;
	char head[200];
	int hl = snprintf(head, sizeof(head), "POST %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: AirControl Agent v1.0\r\n"
			  "Content-Type: application/x-binary\r\nContent-Length: %ld\r\n\r\n", path, host, port, n);
	if (hl < 0 || hl >= (int)sizeof(head))
		goto fail;
	memmove(x.req + hl, x.req + 200, n);
	memcpy(x.req, head, hl);
	x.req_len = hl + n;
	x.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
	if (!x.pcb)
		goto fail;
	tcp_err(x.pcb, err_cb);
	tcp_recv(x.pcb, recv_cb);
	tcp_sent(x.pcb, sent_cb);
	x.left = TIMEOUT_S;
	informs++;
	ip_addr_t to;
	ip_addr_copy_from_ip4(to, *to4);
	if (tcp_connect(x.pcb, &to, port, connected_cb) != ERR_OK) {
		tcp_abort(x.pcb);
		x.pcb = NULL;
		goto fail;
	}
	return;
fail:
	free(x.req);
	free(x.reply);
	memset(&x, 0, sizeof(x));
	state = "out of memory";
}

/* The HTTP reply in x.reply: its status, the body */
static void reply_done(void)
{
	int status = 0;
	uint8_t *body = NULL;
	size_t i, len = 0;

	if (x.got > 12 && !memcmp(x.reply, "HTTP/1.", 7))
		status = atoi((const char *)x.reply + 9);
	for (i = 0; i + 4 <= x.got; i++)
		if (!memcmp(x.reply + i, "\r\n\r\n", 4)) {
			body = x.reply + i + 4;
			len = x.got - i - 4;
			break;
		}
	last_http = status;
	if (status && watch) {	/* still in reach after a change: the change is good */
		watch = 0;
		save();
		if (store.unifi_bad[0]) {
			store.unifi_bad[0] = 0;
			store_save();
		}
	}
	if (status == 404 || status == 400) {
		/* not adopted, or no longer known */
		state = store.unifi_adopted ? "unknown to the controller" : "waiting for adoption";
		return;
	}
	if (status != 200 || !body) {
		state = "controller error";
		return;
	}
	if (!len) {
		state = store.unifi_adopted ? "connected" : "waiting for adoption";
		return;
	}
	char *text = malloc(TEXT_MAX);
	if (!text) {
		state = "out of memory";
		return;
	}
	if (unifi_open(body, len, key, text, TEXT_MAX) < 0) {
		state = "reply not readable (wrong key?)";
	} else {
		state = store.unifi_adopted ? "connected" : "waiting for adoption";
		free(x.reply);		/* room for what handle() needs */
		x.reply = NULL;
		handle(text);
		if (store.unifi_adopted && !strcmp(state, "waiting for adoption"))
			state = "connected";
	}
	free(text);
}

void unifi_second(void)
{
	if (x.pcb || x.done) {
		if (!x.done && x.left && --x.left)
			return;
		if (!x.done) {
			finish(true);
			state = "controller not answering";
			resolved[0] = 0;	/* it may have moved */
		} else if (x.failed) {
			state = "controller not reachable";
			resolved[0] = 0;
		} else {
			free(x.req);	/* sent: room for opening the reply */
			x.req = NULL;
			reply_done();
		}
		free(x.req);
		free(x.reply);
		memset(&x, 0, sizeof(x));
		next_in = interval;
	}
	if (watch && !--watch) {
		snprintf(store.unifi_bad, sizeof(store.unifi_bad), "%s", cfgversion);
		store_save();
		event(EV_SYSTEM, 0, 0, "UniFi: controller lost after configuration %s; restarting with the saved one", cfgversion);
		plat_reboot();
		return;
	}
	if (!on || !netif_default || !netif_is_up(netif_default) || ip4_addr_isany(netif_ip4_addr(netif_default)))
		return;
	if (next_in && --next_in)
		return;
	next_in = interval;
	start();
}


/* ---- the switch's own events ---- */

/* A saved configuration as unifi_parse() makes them */
static bool saved_ok(const struct saved *v)
{
	const struct unifi_sw *sw = &v->sw;

	if (!memchr(v->cfgversion, 0, sizeof(v->cfgversion)) || !v->cfgversion[0] || sw->nvlans > UNIFI_VLANS)
		return false;
	if (sw->stp.on < -1 || sw->stp.on > 1 || sw->stp.rstp < -1 || sw->stp.rstp > 1 || sw->stp.prio < -1 || sw->stp.prio > 61440)
		return false;
	for (int i = 0; i < sw->nvlans; i++) {
		if (sw->vlan[i].mode > UNIFI_EXCLUDE)
			return false;
		for (int p = 0; p <= UNIFI_PORTS; p++)
			if (sw->vlan[i].port[p] > UNIFI_EXCLUDE)
				return false;
	}
	for (int p = 0; p <= UNIFI_PORTS; p++)
		if (!memchr(sw->port[p].name, 0, sizeof(sw->port[p].name)) || sw->port[p].pvid > 4094
		    || sw->port[p].stp < -1 || sw->port[p].stp > 1 || sw->port[p].edge < -1 || sw->port[p].edge > 2
		    || sw->port[p].guard < -1 || sw->port[p].guard > 1)
			return false;
	return true;
}

void unifi_boot(void)
{
	struct saved *v = malloc(sizeof(*v));

	if (!v)
		return;
	plat_sector_read(SECTOR_UNIFI, (uint8_t *)v, sizeof(*v));
	if (store.unifi_adopted && !memcmp(v->magic, "UNC2", 4)) {
		if (!saved_ok(v))
			event(EV_CONFIG, 0, 0, "UniFi: the saved configuration is damaged, not used");
		else if ((applied = malloc(sizeof(*applied)))) {
			*applied = v->sw;
			snprintf(cfgversion, sizeof(cfgversion), "%s", v->cfgversion);
		}
	}
	free(v);
}

void unifi_bridge_up(void)
{
	bool ok;

	/* the switch's own configuration has just been replayed: ours again on top */
	if (!on || !applied)
		return;
	pick_model();
	run(applied, NULL, &ok);
	event(EV_CONFIG, 0, 0, "UniFi: configuration %s restored%s", cfgversion, ok ? "" : " with errors");
}


/* ---- settings ---- */

void unifi_enable(bool enable)
{
	if (enable && !on) {
		use_key();
		next_in = 1;
	}
	on = enable;
	if (!on) {
		state = "off";
		watch = 0;
		if (locating) {
			locating = false;
			plat_sys_led(LED_ON);
		}
	}
}

bool unifi_inform(const char *url)
{
	if (!strcmp(url, "auto"))
		url = "";	/* from DHCP, or the default */
	else if (!unifi_url_ok(url))
		return false;
	if (strcmp(store.unifi_url, url)) {
		snprintf(store.unifi_url, sizeof(store.unifi_url), "%s", url);
		store_save();
	}
	next_in = 1;
	return true;
}

void unifi_forget(void)
{
	forget("forgotten");
}

size_t unifi_status(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "UniFi %s: %s\n", on ? "on" : "off", on ? state : "not informing");

	const char *from, *u = url_now(&from);

	n = app(buf, max, n, "  inform URL %s%s\n", u, from);
	if (resolved[0])
		n = app(buf, max, n, "  %s is %s\n", resolved, ip4addr_ntoa(&resolved_ip));
	if (model_set)
		n = app(buf, max, n, "  claims to be a %s (%s), %u ports\n", model.name, model.code, model.ports);
	n = app(buf, max, n, "  %s, %s, every %lu s; %lu informs, %lu replies, last HTTP %d\n",
		store.unifi_adopted ? "adopted" : "not adopted", store.unifi_gcm ? "AES-GCM" : "AES-CBC",
		(unsigned long)interval, (unsigned long)informs, (unsigned long)replies, last_http);
	if (cfgversion[0])
		n = app(buf, max, n, "  configuration %s applied%s\n", cfgversion, watch ? ", checking the controller can still be reached" : "");
	if (store.unifi_bad[0])
		n = app(buf, max, n, "  refusing configuration %s: it cut the switch off\n", store.unifi_bad);
	if (locating)
		n = app(buf, max, n, "  locating: the system LED flashes\n");
	return n;
}
