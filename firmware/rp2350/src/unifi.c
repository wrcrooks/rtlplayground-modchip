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
#include "plat.h"
#include "rng.h"
#include "store.h"
#include "unifi.h"
#include "unifi_proto.h"

#define JSON_MAX	6144
#define REPLY_MAX	12288		/* what the controller sends, compressed */
#define TEXT_MAX	16384		/* and unpacked */
#define CMDS_MAX	2048
#define TIMEOUT_S	10
#define INTERVAL_S	10
#ifndef WATCH_S
#define WATCH_S		120		/* to reach the controller again after its configuration */
#endif
/* Our cfgversion until the controller's configuration is applied: none like it, so it sends that */
#define NONE		"0000000000000000"
#define VERSION		"1.1.0"		/* what we claim to run; the oldest these models take */

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
		memcpy(v->magic, "UNC1", 4);
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
	event(EV_CONFIG, 0, 0, "UniFi: %s, back to the default key", why);
}


/* ---- the inform ---- */

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

	if (!model_set)
		pick_model();
	snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	k = app(b, max, k, "{\"mac\":\"%s\",\"serial\":\"%s\",\"model\":\"%s\",\"model_display\":\"%s\","
		"\"version\":\"" VERSION "\",\"ip\":\"%s\",\"hostname\":", macs, serial, model.code, model.name, ip4addr_ntoa(ip));
	k = app_json_str(b, max, k, hostname);
	k = app(b, max, k, ",\"inform_url\":");
	k = app_json_str(b, max, k, store.unifi_url);
	k = app(b, max, k, ",\"uptime\":%lu,\"cfgversion\":\"%s\",\"default\":%s,\"_default_key\":%s,\"state\":%d,"
		"\"fw_caps\":0,\"isolated\":false,\"locating\":%s,\"selfrun_beacon\":false",
		(unsigned long)plat_uptime_s(), cfgversion[0] || !store.unifi_adopted ? cfgversion : NONE, store.unifi_adopted ? "false" : "true",
		store.unifi_adopted ? "false" : "true", store.unifi_adopted ? 4 : 1, locating ? "true" : "false");
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
	if (version[0] && !strcmp(version, store.unifi_bad)) {
		if (!bad_told)
			event(EV_CONFIG, 0, 0, "UniFi: configuration %s not applied: it cut the switch off before", version);
		bad_told = true;
		free(sw);
		return;
	}
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

static void handle(const char *j)
{
	char type[24], s[96];
	long v;

	replies++;
	if (!unifi_json_str(j, "_type", type, sizeof(type)))
		return;
	if (unifi_json_int(j, "interval", &v) && v >= 2 && v <= 600)
		interval = v;
	if (!strcmp(type, "setparam")) {
		char *mgmt = malloc(1024), *sys = malloc(TEXT_MAX);
		char version[24] = "";

		if (mgmt && unifi_json_str(j, "mgmt_cfg", mgmt, 1024)) {
			uint8_t k[16];

			if (unifi_kv(mgmt, "authkey", s, sizeof(s)) && unifi_hex16(s, k) && !store.unifi_adopted) {
				memcpy(store.unifi_key, k, 16);
				store.unifi_adopted = 1;
				store_save();
				event(EV_CONFIG, 0, 0, "UniFi: adopted");
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
		if (sys && unifi_json_str(j, "system_cfg", sys, TEXT_MAX))
			apply(sys, version);
		free(mgmt);
		free(sys);
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
			if (unifi_json_str(j, "uri", s, sizeof(s)) && !strncmp(s, "http://", 7))
				snprintf(store.unifi_url, sizeof(store.unifi_url), "%s", s);
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

static void finish(bool failed)
{
	if (x.pcb) {
		tcp_arg(x.pcb, NULL);
		tcp_recv(x.pcb, NULL);
		tcp_sent(x.pcb, NULL);
		tcp_err(x.pcb, NULL);
		if (tcp_close(x.pcb) != ERR_OK)
			tcp_abort(x.pcb);
		x.pcb = NULL;
	}
	x.done = true;
	x.failed = failed;
}

static void pump(void)
{
	while (x.sent < x.req_len && x.pcb) {
		u16_t n = tcp_sndbuf(x.pcb);

		if (!n)
			break;
		if (n > x.req_len - x.sent)
			n = x.req_len - x.sent;
		if (tcp_write(x.pcb, x.req + x.sent, n, 0) != ERR_OK)
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
	if (!p) {
		finish(false);
		return ERR_OK;
	}
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

/* "http://a.b.c.d[:port]/path" */
static bool parse_url(const char *url, ip4_addr_t *ip, uint16_t *port, const char **path)
{
	char host[16];
	const char *h = url + 7, *e;
	size_t n;

	if (strncmp(url, "http://", 7))
		return false;
	e = h + strcspn(h, ":/");
	n = e - h;
	if (!n || n >= sizeof(host))
		return false;
	memcpy(host, h, n);
	host[n] = 0;
	if (!ip4addr_aton(host, ip))
		return false;
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
	ip4_addr_t ip;
	uint16_t port;
	const char *path;

	return strlen(url) < sizeof(store.unifi_url) && parse_url(url, &ip, &port, &path);
}

static void start(void)
{
	ip4_addr_t ip;
	uint16_t port;
	const char *path;
	char *json = NULL;
	uint8_t iv[16];
	long n;

	memset(&x, 0, sizeof(x));
	if (!parse_url(store.unifi_url, &ip, &port, &path)) {
		state = "no inform URL";
		return;
	}
	json = malloc(JSON_MAX);
	x.req = malloc(JSON_MAX + 256);
	x.reply = malloc(REPLY_MAX);
	if (!json || !x.req || !x.reply)
		goto fail;
	size_t jl = inform_json(json, JSON_MAX);
	if (jl >= JSON_MAX - 1)
		goto fail;
	rng_get(NULL, iv, sizeof(iv));
	/* the packet after room for the request line and headers */
	n = unifi_seal(x.req + 200, JSON_MAX + 56, rtl_mac(), key, store.unifi_gcm, iv, json, jl);
	if (n < 0)
		goto fail;
	char head[200];
	int hl = snprintf(head, sizeof(head), "POST %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: AirControl Agent v1.0\r\n"
			  "Content-Type: application/x-binary\r\nContent-Length: %ld\r\n\r\n", path, ip4addr_ntoa(&ip), port, n);
	if (hl < 0 || hl >= (int)sizeof(head))
		goto fail;
	memmove(x.req + hl, x.req + 200, n);
	memcpy(x.req, head, hl);
	x.req_len = hl + n;
	free(json);
	json = NULL;
	x.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
	if (!x.pcb)
		goto fail;
	tcp_err(x.pcb, err_cb);
	tcp_recv(x.pcb, recv_cb);
	tcp_sent(x.pcb, sent_cb);
	x.left = TIMEOUT_S;
	informs++;
	ip_addr_t to;
	ip_addr_copy_from_ip4(to, ip);
	if (tcp_connect(x.pcb, &to, port, connected_cb) != ERR_OK) {
		tcp_abort(x.pcb);
		x.pcb = NULL;
		goto fail;
	}
	return;
fail:
	free(json);
	free(x.req);
	free(x.reply);
	memset(&x, 0, sizeof(x));
	state = "out of memory";
}

/* The HTTP reply in x.reply: its status, the body */
static void reply_done(void)
{
	int status = 0;
	const uint8_t *body = NULL;
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
		} else if (x.failed) {
			state = "controller not reachable";
		} else {
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

void unifi_boot(void)
{
	struct saved *v = malloc(sizeof(*v));

	if (!v)
		return;
	plat_sector_read(SECTOR_UNIFI, (uint8_t *)v, sizeof(*v));
	if (store.unifi_adopted && !memcmp(v->magic, "UNC1", 4) && (applied = malloc(sizeof(*applied)))) {
		v->cfgversion[sizeof(v->cfgversion) - 1] = 0;
		*applied = v->sw;
		snprintf(cfgversion, sizeof(cfgversion), "%s", v->cfgversion);
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
	if (!unifi_url_ok(url))
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

	n = app(buf, max, n, "  inform URL %s\n", store.unifi_url[0] ? store.unifi_url : "(none)");
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
