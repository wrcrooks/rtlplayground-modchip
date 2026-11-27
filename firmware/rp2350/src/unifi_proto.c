/* The UniFi inform protocol without the network, see unifi_proto.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"

#include "inflate.h"
#include "unifi_proto.h"

#define F_ENC		0x01
#define F_ZLIB		0x02
#define F_SNAPPY	0x04
#define F_GCM		0x08

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void header(uint8_t *h, const uint8_t mac[6], uint16_t flags, const uint8_t iv[16], uint32_t len)
{
	memcpy(h, "TNBU", 4);
	put32(h + 4, 1);
	memcpy(h + 8, mac, 6);
	h[14] = flags >> 8;
	h[15] = flags;
	memcpy(h + 16, iv, 16);
	put32(h + 32, 1);
	put32(h + 36, len);
}

long unifi_seal(uint8_t *out, size_t max, const uint8_t mac[6], const uint8_t key[16], bool gcm,
		const uint8_t iv[16], const char *json, size_t len)
{
	uint8_t *body = out + UNIFI_HDR;
	long n;
	int r;

	if (max < UNIFI_HDR + 32)
		return -1;
	/* compressed in place after room for the header, padding and the tag */
	n = zlib_store((const uint8_t *)json, len, body, max - UNIFI_HDR - 16);
	if (n < 0)
		return -1;
	if (gcm) {
		mbedtls_gcm_context c;

		header(out, mac, F_ENC | F_ZLIB | F_GCM, iv, n + 16);
		mbedtls_gcm_init(&c);
		r = mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, key, 128) ||
			mbedtls_gcm_crypt_and_tag(&c, MBEDTLS_GCM_ENCRYPT, n, iv, 16, out, UNIFI_HDR, body, body, 16, body + n);
		mbedtls_gcm_free(&c);
		n += 16;
	} else {
		mbedtls_aes_context c;
		uint8_t v[16];
		uint8_t pad = 16 - n % 16;

		memset(body + n, pad, pad);
		n += pad;
		header(out, mac, F_ENC | F_ZLIB, iv, n);
		memcpy(v, iv, 16);
		mbedtls_aes_init(&c);
		r = mbedtls_aes_setkey_enc(&c, key, 128) ||
			mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, n, v, body, body);
		mbedtls_aes_free(&c);
	}
	return r ? -1 : UNIFI_HDR + n;
}

long unifi_open(const uint8_t *pkt, size_t len, const uint8_t key[16], char *out, size_t max)
{
	uint16_t flags;
	uint32_t n;
	uint8_t *body;
	long r = -1;

	if (len < UNIFI_HDR || memcmp(pkt, "TNBU", 4) || get32(pkt + 32) != 1 || !max)
		return -1;
	flags = pkt[14] << 8 | pkt[15];
	n = get32(pkt + 36);
	if (n > len - UNIFI_HDR || (flags & F_SNAPPY) || !(flags & F_ENC))
		return -1;
	body = malloc(n ? n : 1);
	if (!body)
		return -1;
	memcpy(body, pkt + UNIFI_HDR, n);
	if (flags & F_GCM) {
		mbedtls_gcm_context c;

		if (n < 16)
			goto out;
		n -= 16;
		mbedtls_gcm_init(&c);
		int e = mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, key, 128) ||
			mbedtls_gcm_auth_decrypt(&c, n, pkt + 16, 16, pkt, UNIFI_HDR, pkt + UNIFI_HDR + n, 16, body, body);
		mbedtls_gcm_free(&c);
		if (e)
			goto out;
	} else {
		mbedtls_aes_context c;
		uint8_t v[16];

		if (!n || n % 16)
			goto out;
		memcpy(v, pkt + 16, 16);
		mbedtls_aes_init(&c);
		int e = mbedtls_aes_setkey_dec(&c, key, 128) ||
			mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_DECRYPT, n, v, body, body);
		mbedtls_aes_free(&c);
		uint8_t pad = body[n - 1];
		if (e || !pad || pad > 16)
			goto out;
		for (unsigned i = 1; i <= pad; i++)
			if (body[n - i] != pad)
				goto out;
		n -= pad;
	}
	if (flags & F_ZLIB) {
		r = zlib_inflate(body, n, (uint8_t *)out, max - 1);
	} else if (n < max) {
		memcpy(out, body, n);
		r = n;
	}
	if (r >= 0)
		out[r] = 0;
out:
	free(body);
	return r;
}


/* ---- JSON: only what the controller's replies need ---- */

static const char *ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* Past a string at p (on its quote); copies it unescaped when out; NULL: bad */
static const char *string(const char *p, char *out, size_t max, bool *fits)
{
	size_t n = 0;

	*fits = true;
	if (*p++ != '"')
		return NULL;
	for (;;) {
		char c = *p++;
		unsigned u;

		if (!c)
			return NULL;
		if (c == '"')
			break;
		if (c == '\\') {
			c = *p++;
			switch (c) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u':
				/* four hex digits: sscanf's %4x takes fewer, and signs and spaces */
				u = 0;
				for (int i = 0; i < 4; i++, p++) {
					if (*p >= '0' && *p <= '9')
						u = u << 4 | (*p - '0');
					else if ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'f')
						u = u << 4 | ((*p | 0x20) - 'a' + 10);
					else
						return NULL;
				}
				c = u < 0x80 ? u : '?';
				break;
			case '"': case '\\': case '/': break;
			default: return NULL;
			}
		}
		if (out) {
			if (n + 1 < max)
				out[n++] = c;
			else
				*fits = false;
		}
	}
	if (out && max)
		out[n] = 0;
	return p;
}

static const char *skip(const char *p, int depth)
{
	bool fits;

	p = ws(p);
	if (*p == '"')
		return string(p, NULL, 0, &fits);
	if (*p == '{' || *p == '[') {
		char close = *p == '{' ? '}' : ']';

		if (depth > 16)
			return NULL;
		p = ws(p + 1);
		if (*p == close)
			return p + 1;
		for (;;) {
			if (close == '}') {
				p = string(ws(p), NULL, 0, &fits);
				if (!p || *(p = ws(p)) != ':')
					return NULL;
				p++;
			}
			p = skip(p, depth + 1);
			if (!p)
				return NULL;
			p = ws(p);
			if (*p == close)
				return p + 1;
			if (*p++ != ',')
				return NULL;
		}
	}
	if (!*p || strchr(",}]:", *p))
		return NULL;
	while (*p && !strchr(",}] \t\r\n", *p))
		p++;
	return p;
}

/* The value of a top-level member, or NULL */
static const char *member(const char *json, const char *name)
{
	const char *p = ws(json);
	char key[32];
	bool fits;

	if (*p++ != '{')
		return NULL;
	for (;;) {
		p = ws(p);
		if (*p == '}')
			return NULL;
		p = string(p, key, sizeof(key), &fits);
		if (!p || *(p = ws(p)) != ':')
			return NULL;
		p = ws(p + 1);
		if (fits && !strcmp(key, name))
			return p;
		p = skip(p, 0);
		if (!p)
			return NULL;
		p = ws(p);
		if (*p++ != ',')
			return NULL;
	}
}

bool unifi_json_str(const char *json, const char *name, char *out, size_t max)
{
	const char *p = member(json, name);
	bool fits;

	return p && *p == '"' && string(p, out, max, &fits) && fits;
}

bool unifi_json_int(const char *json, const char *name, long *v)
{
	const char *p = member(json, name);
	char *end;

	if (!p)
		return false;
	if (*p == '"')		/* some come as strings */
		p++;
	*v = strtol(p, &end, 10);
	return end != p;
}

bool unifi_kv(const char *text, const char *key, char *out, size_t max)
{
	size_t k = strlen(key);

	for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
		if (strncmp(l, key, k) || l[k] != '=')
			continue;
		const char *v = l + k + 1, *e = strchr(v, '\n');
		size_t n = e ? (size_t)(e - v) : strlen(v);
		if (n >= max)
			return false;
		memcpy(out, v, n);
		out[n] = 0;
		return true;
	}
	return false;
}

bool unifi_hex16(const char *hex, uint8_t key[16])
{
	if (strlen(hex) != 32)
		return false;
	for (int i = 0; i < 16; i++) {
		unsigned b;

		if (!strchr("0123456789abcdefABCDEF", hex[2 * i]) || !strchr("0123456789abcdefABCDEF", hex[2 * i + 1])
		    || sscanf(hex + 2 * i, "%2x", &b) != 1)
			return false;
		key[i] = b;
	}
	return true;
}


/* ---- the model and its configuration ---- */

void unifi_model_pick(uint8_t copper, uint8_t sfp, struct unifi_model *m)
{
	uint8_t p = 1;

	memset(m, 0, sizeof(*m));
	if (copper + sfp <= 5) {
		/* USW Flex 2.5G 5: five ports, like the 4 + 1 boards */
		m->code = "USWED35";
		m->name = "USW Flex 2.5G 5";
		m->ports = 5;
		for (uint8_t i = 0; i < copper + sfp; i++, p++) {
			m->front[p] = i + 1;
			m->sfp[p] = i >= copper;
		}
		return;
	}
	/* USW Flex 2.5G 8: eight copper ports and two 10G ones (9 and 10) */
	m->code = "USWED36";
	m->name = "USW Flex 2.5G 8";
	m->ports = 10;
	for (uint8_t i = 0; i < copper && i < 8; i++)
		m->front[1 + i] = i + 1;
	for (uint8_t i = 0; i < sfp && i < 2; i++) {
		m->front[9 + i] = copper + i + 1;
		m->sfp[9 + i] = true;
	}
}

bool unifi_parse(const char *cfg, struct unifi_sw *sw)
{
	int map[UNIFI_VLANS * 4];		/* system_cfg's VLAN slot -> sw->vlan[] */
	bool any = false;

	memset(sw, 0, sizeof(*sw));
	sw->stp.on = sw->stp.rstp = -1;
	sw->stp.prio = -1;
	for (int b = 0; b <= UNIFI_PORTS; b++)
		sw->port[b].stp = sw->port[b].edge = sw->port[b].guard = -1;
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		map[i] = -1;
	/* two passes: VLAN slots may be named before their id */
	for (int pass = 0; pass < 2; pass++)
		for (const char *l = cfg; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
			char key[48], val[64];
			int a, b, n = 0;
			const char *e = strchr(l, '='), *end = strchr(l, '\n');

			if (!end)
				end = l + strlen(l);
			if (!e || e > end || e - l >= (long)sizeof(key) || end - e - 1 >= (long)sizeof(val))
				continue;
			memcpy(key, l, e - l);
			key[e - l] = 0;
			memcpy(val, e + 1, end - e - 1);
			val[end - e - 1] = 0;
			if (pass == 0) {
				if (sscanf(key, "switch.vlan.%d.id%n", &a, &n) == 1 && !key[n]) {
					if (a < 0 || a >= (int)(sizeof(map) / sizeof(map[0])) || map[a] >= 0)
						continue;
					if (sw->nvlans == UNIFI_VLANS)
						return false;
					map[a] = sw->nvlans;
					sw->vlan[sw->nvlans++].id = atoi(val);
					any = true;
				}
				continue;
			}
			if (!strcmp(key, "switch.stp.status")) {
				sw->stp.on = !strcmp(val, "enabled");
				any = true;
			} else if (!strcmp(key, "switch.stp.version")) {
				sw->stp.rstp = !strcmp(val, "stp") ? 0 : 1;
			} else if (!strcmp(key, "switch.stp.priority")) {
				long v = strtol(val, NULL, 10);
				if (v >= 0 && v <= 61440)
					sw->stp.prio = v;
			} else if (sscanf(key, "switch.vlan.%d.mode%n", &a, &n) == 1 && !key[n]) {
				if (a >= 0 && a < (int)(sizeof(map) / sizeof(map[0])) && map[a] >= 0)
					sw->vlan[map[a]].mode = !strcmp(val, "untagged") ? UNIFI_UNTAGGED : UNIFI_TAGGED;
			} else if (sscanf(key, "switch.vlan.%d.port.%d.mode%n", &a, &b, &n) == 2 && !key[n]) {
				if (a >= 0 && a < (int)(sizeof(map) / sizeof(map[0])) && map[a] >= 0 && b >= 1 && b <= UNIFI_PORTS)
					sw->vlan[map[a]].port[b] = !strcmp(val, "untagged") ? UNIFI_UNTAGGED
						: !strcmp(val, "tagged") ? UNIFI_TAGGED : UNIFI_EXCLUDE;
			} else if (sscanf(key, "switch.port.%d.%n", &b, &n) == 1 && b >= 1 && b <= UNIFI_PORTS) {
				const char *f = key + n;

				any = true;
				if (!strcmp(f, "pvid"))
					sw->port[b].pvid = atoi(val);
				else if (!strcmp(f, "speed"))
					sw->port[b].speed = atoi(val);
				else if (!strcmp(f, "duplex"))
					sw->port[b].full = !strcmp(val, "enabled");
				else if (!strcmp(f, "name"))
					snprintf(sw->port[b].name, sizeof(sw->port[b].name), "%.23s", val);
				else if (!strcmp(f, "stp.port_mode"))
					sw->port[b].stp = !strcmp(val, "enabled");
				else if (!strcmp(f, "stp.edge"))
					sw->port[b].edge = !strcmp(val, "auto") ? 2 : !strcmp(val, "enabled");
				else if (!strcmp(f, "stp.bpdu_guard"))
					sw->port[b].guard = !strcmp(val, "enabled");
				else if (!strcmp(f, "isolation"))
					sw->port[b].isolated = !strcmp(val, "enabled");
				else if (!strcmp(f, "stormctrl.bcast"))
					sw->port[b].storm[UNIFI_STORM_B] = strtoul(val, NULL, 10);
				else if (!strcmp(f, "stormctrl.mcast"))
					sw->port[b].storm[UNIFI_STORM_M] = strtoul(val, NULL, 10);
				else if (!strcmp(f, "stormctrl.ucast"))
					sw->port[b].storm[UNIFI_STORM_U] = strtoul(val, NULL, 10);
				else if (!strcmp(f, "egress_rate_limit_kbps"))
					sw->port[b].egress_kbps = strtoul(val, NULL, 10);
			}
		}
	/* a speed counts only with autoneg off; storm limits only with storm control on, as rates */
	for (int b = 1; b <= UNIFI_PORTS; b++) {
		char k[48], v[16];

		snprintf(k, sizeof(k), "switch.port.%d.autoneg", b);
		if (!unifi_kv(cfg, k, v, sizeof(v)) || strcmp(v, "disabled"))
			sw->port[b].speed = 0;
		snprintf(k, sizeof(k), "switch.port.%d.stormctrl.status", b);
		bool storm = unifi_kv(cfg, k, v, sizeof(v)) && !strcmp(v, "enabled");
		snprintf(k, sizeof(k), "switch.port.%d.stormctrl.type", b);
		if (!storm || (unifi_kv(cfg, k, v, sizeof(v)) && strcmp(v, "rate")))
			memset(sw->port[b].storm, 0, sizeof(sw->port[b].storm));
	}
	return any;
}

static uint8_t mode_of(const struct unifi_sw *sw, int v, int p)
{
	return sw->vlan[v].port[p] ? sw->vlan[v].port[p] : sw->vlan[v].mode ? sw->vlan[v].mode : UNIFI_TAGGED;
}

static bool has_vlan(const struct unifi_sw *sw, uint16_t id)
{
	for (int v = 0; v < sw->nvlans; v++)
		if (sw->vlan[v].id == id)
			return true;
	return false;
}

/* The port's name for the command line: words in it would be words of the command */
static void port_name(const char *in, int idx, char *out, size_t max)
{
	char def[16];
	size_t n = 0;

	snprintf(def, sizeof(def), "Port %d", idx);
	if (!strcmp(in, def))
		in = "";
	for (; *in && n + 1 < max; in++)
		if ((*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') || (*in >= '0' && *in <= '9') || strchr("-_.", *in))
			out[n++] = *in;
		else if (*in == ' ')
			out[n++] = '_';
	out[n] = 0;
}

static const char *speed_word(uint16_t mbps)
{
	switch (mbps) {
	case 10: return "10m";
	case 100: return "100m";
	case 1000: return "1g";
	case 2500: return "2g5";
	case 5000: return "5g";
	case 10000: return "10g";
	}
	return NULL;
}

#define APP(...) do { \
		int k_ = snprintf(out + n, max - n, __VA_ARGS__); \
		if (k_ < 0 || (size_t)k_ >= max - n) \
			return -1; \
		n += k_; \
	} while (0)

long unifi_commands(const struct unifi_sw *sw, const struct unifi_sw *before, const struct unifi_model *m,
		    char *out, size_t max)
{
	size_t n = 0;

	if (!max)
		return -1;
	out[0] = 0;
	if (before)
		for (int v = 0; v < before->nvlans; v++)
			if (!has_vlan(sw, before->vlan[v].id) && before->vlan[v].id != 1)
				APP("vlan %u d\n", before->vlan[v].id);
	for (int v = 0; v < sw->nvlans; v++) {
		if (!sw->vlan[v].id || sw->vlan[v].id > 4094)
			continue;
		APP("vlan %u", sw->vlan[v].id);
		for (int p = 1; p <= m->ports; p++) {
			uint8_t mode = mode_of(sw, v, p);

			if (m->front[p] && mode != UNIFI_EXCLUDE)
				APP(" %u%s", m->front[p], mode == UNIFI_TAGGED ? "t" : "");
		}
		APP("\n");
	}
	for (int p = 1; p <= m->ports; p++) {
		uint16_t pvid = sw->port[p].pvid;
		char name[24], was[24];

		if (!m->front[p])
			continue;
		if (!pvid)	/* the VLAN the port is untagged in, else 1 */
			for (int v = 0; v < sw->nvlans && !pvid; v++)
				if (mode_of(sw, v, p) == UNIFI_UNTAGGED)
					pvid = sw->vlan[v].id;
		APP("pvid %u %u\n", m->front[p], pvid ? pvid : 1);
		port_name(sw->port[p].name, p, name, sizeof(name));
		port_name(before ? before->port[p].name : "", p, was, sizeof(was));
		if (strcmp(name, was) || (!before && name[0]))
			APP("port %u name%s%s\n", m->front[p], name[0] ? " " : "", name);
		if (m->sfp[p])
			continue;
		if (sw->port[p].speed && speed_word(sw->port[p].speed)) {
			if (!before || before->port[p].speed != sw->port[p].speed || before->port[p].full != sw->port[p].full)
				APP("port %u %s%s\n", m->front[p], speed_word(sw->port[p].speed),
				    sw->port[p].speed > 100 ? "" : sw->port[p].full ? " full" : " half");
		} else if (before && before->port[p].speed) {
			APP("port %u auto\n", m->front[p]);
		}
	}

	/* STP: what's given, when it changed */
#define CHANGED(f) (sw->f >= 0 && (!before || before->f != sw->f))
	if (CHANGED(stp.rstp))
		APP("stp version %s\n", sw->stp.rstp ? "rstp" : "stp");
	if (CHANGED(stp.prio))
		APP("stp prio %ld\n", (long)(sw->stp.prio / 4096));
	if (CHANGED(stp.on))
		APP("stp %s\n", sw->stp.on ? "on" : "off");
	for (int p = 1; p <= m->ports; p++) {
		if (!m->front[p])
			continue;
		if (CHANGED(port[p].stp))
			APP("stp port %u %s\n", m->front[p], sw->port[p].stp ? "on" : "off");
		if (CHANGED(port[p].edge))
			APP("stp port %u edge %s\n", m->front[p], sw->port[p].edge == 2 ? "auto" : sw->port[p].edge ? "on" : "off");
		if (CHANGED(port[p].guard))
			APP("stp port %u guard %s\n", m->front[p], sw->port[p].guard ? "bpdu" : "none");
	}
#undef CHANGED

	/* Isolation: an isolated port forwards to the ports that aren't, and the CPU (0) */
	bool iso_changed = false, any_iso = false;
	for (int p = 1; p <= m->ports; p++) {
		any_iso |= m->front[p] && sw->port[p].isolated;
		iso_changed |= m->front[p] && (before ? before->port[p].isolated != sw->port[p].isolated : sw->port[p].isolated);
	}
	if (iso_changed)
		for (int p = 1; p <= m->ports; p++) {
			if (!m->front[p])
				continue;
			if (!sw->port[p].isolated) {
				if (any_iso || before)
					APP("isolate %u off\n", m->front[p]);
				continue;
			}
			APP("isolate %u", m->front[p]);
			for (int q = 1; q <= m->ports; q++)
				if (m->front[q] && q != p && !sw->port[q].isolated)
					APP(" %u", m->front[q]);
			APP(" 0\n");
		}

	/* Storm control and egress limits: set, changed, or no longer there */
	static const char *const storm[3] = { "bcast", "mcast", "ucast" };
	for (int p = 1; p <= m->ports; p++) {
		if (!m->front[p])
			continue;
		for (int t = 0; t < 3; t++) {
			uint32_t now = sw->port[p].storm[t], was = before ? before->port[p].storm[t] : 0;

			if (now && now != was)
				APP("storm %u %s %lu pps\n", m->front[p], storm[t], (unsigned long)now);
			else if (!now && was)
				APP("storm %u %s off\n", m->front[p], storm[t]);
		}
		uint32_t now = sw->port[p].egress_kbps, was = before ? before->port[p].egress_kbps : 0;
		if (now && now != was)
			APP("bw out %u %08lx\n", m->front[p], (unsigned long)now);
		else if (!now && was)
			APP("bw out %u off\n", m->front[p]);
	}
	return n;
}
