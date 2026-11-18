/*
 * 802.1X authenticator, port based: one authenticated device opens the port.
 *
 * The switch asks a device on an "auto" port for its identity with EAP
 * Request/Identity over EAPOL, and relays the EAP conversation that follows
 * to a RADIUS server in Access-Request / Access-Challenge exchanges (RFC 2865,
 * RFC 3579) until the server accepts or rejects. Until then the port carries
 * only EAPOL (dot1x_plat.c fences it off). A device that never answers with
 * EAPOL can be let in by its MAC address instead (MAB): after DOT1X_MAB_AFTER
 * unanswered requests, the MAC of a frame seen on the port goes to the server
 * as user name and password, as Cisco and others do it.
 *
 * The state machine follows 802.1X-2004's authenticator PAE and backend
 * authentication machines, collapsed into one per port: CONNECTING sends
 * Request/Identity every DOT1X_TX_PERIOD s, AUTHENTICATING relays, and a
 * failure holds the port for DOT1X_QUIET_PERIOD s. Re-authentication keeps the
 * port open until it fails.
 */

#include <string.h>

#include "md5.h"

#include "dot1x.h"

struct dot1x_config dot1x_cfg;

/* EAPOL packet types and EAP codes */
#define EAPOL_EAP	0
#define EAPOL_START	1
#define EAPOL_LOGOFF	2
#define EAP_REQUEST	1
#define EAP_RESPONSE	2
#define EAP_SUCCESS	3
#define EAP_FAILURE	4
#define EAP_IDENTITY	1

/* RADIUS codes and attributes */
#define RAD_ACCESS_REQUEST	1
#define RAD_ACCESS_ACCEPT	2
#define RAD_ACCESS_REJECT	3
#define RAD_ACCESS_CHALLENGE	11
#define A_USER_NAME		1
#define A_USER_PASSWORD		2
#define A_NAS_IP		4
#define A_NAS_PORT		5
#define A_SERVICE_TYPE		6
#define A_FRAMED_MTU		12
#define A_STATE			24
#define A_SESSION_TIMEOUT	27
#define A_TERMINATION_ACTION	29
#define A_CALLED_STATION	30
#define A_CALLING_STATION	31
#define A_NAS_PORT_TYPE		61
#define A_EAP_MESSAGE		79
#define A_MESSAGE_AUTH		80

#define RAD_MAX		1700	/* an Access-Request carrying a 1400-byte EAP fragment */
#define EAP_MAX		1500
#define STATE_MAX	128
#define FRAMED_MTU	1400

struct port {
	struct dot1x_status s;
	uint16_t timer;			/* seconds until the next step */
	uint8_t tx_count;		/* Request/Identity sent without an answer */
	uint8_t eap_id;			/* id of our last Request/Identity */
	uint8_t last_id;		/* id of the last EAP request sent on */
	uint8_t method;			/* of the run in progress */
	bool waiting_server;
	uint8_t state_attr[STATE_MAX];
	uint8_t state_len;
	/* the outstanding RADIUS request, kept for retransmission */
	bool rad_busy;
	uint8_t rad_id;
	uint8_t rad_tries;
	uint8_t req_auth[16];
	uint16_t rad_len;
	uint8_t rad[RAD_MAX];
};

static struct port ports[DOT1X_PORTS];
static uint8_t next_rad_id;
static uint16_t gated;
static bool gate_on, gate_known;
static uint8_t eap_buf[EAP_MAX + 4];

static const char *const state_names[] = {
	"off", "down", "connecting", "authenticating", "authenticated", "held", "blocked",
};

const char *dot1x_state_name(uint8_t state)
{
	return state < sizeof(state_names) / sizeof(state_names[0]) ? state_names[state] : "?";
}


/* ---- MD5 and HMAC-MD5 ---- */

void dot1x_md5(const uint8_t *a, uint16_t na, const uint8_t *b, uint16_t nb, uint8_t out[16])
{
	struct md5 c;

	md5_init(&c);
	md5_update(&c, a, na);
	if (nb)
		md5_update(&c, b, nb);
	md5_final(&c, out);
}


void dot1x_hmac_md5(const uint8_t *key, uint16_t nkey, const uint8_t *m, uint16_t nm, uint8_t out[16])
{
	uint8_t k[64], pad[64], inner[16];
	struct md5 c;

	memset(k, 0, sizeof(k));
	if (nkey > 64)
		dot1x_md5(key, nkey, NULL, 0, k);
	else
		memcpy(k, key, nkey);
	for (int i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	dot1x_md5(pad, 64, m, nm, inner);
	for (int i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	md5_init(&c);
	md5_update(&c, pad, 64);
	md5_update(&c, inner, 16);
	md5_final(&c, out);
}


/* ---- helpers ---- */

static bool port_ok(uint8_t port)
{
	return port < DOT1X_PORTS && ((dot1x_hw_ports() >> port) & 1);
}


static bool controlled(uint8_t port)
{
	return dot1x_cfg.enabled && dot1x_cfg.mode[port] == DOT1X_AUTO;
}


/* Recompute the fenced-off ports and tell the platform when they change */
static void gate(void)
{
	uint16_t b = dot1x_blocked();

	if (gate_known && b == gated && gate_on == dot1x_cfg.enabled)
		return;
	gated = b;
	gate_on = dot1x_cfg.enabled;
	gate_known = true;
	dot1x_hw_gate(b, gate_on);
}


static void set_authorized(uint8_t port, bool on)
{
	ports[port].s.authorized = on;
	gate();
}


static void eapol_send(uint8_t port, uint8_t type, const uint8_t *body, uint16_t len)
{
	uint8_t *f = eap_buf;

	if (len > EAP_MAX)
		return;
	if (body != f + 4)
		memmove(f + 4, body, len);
	f[0] = 2;		/* 802.1X-2004 */
	f[1] = type;
	f[2] = len >> 8;
	f[3] = len;
	dot1x_hw_eapol_tx(port, f, len + 4);
}


static void eap_send_simple(uint8_t port, uint8_t code, uint8_t id)
{
	uint8_t *e = eap_buf + 4;

	e[0] = code;
	e[1] = id;
	e[2] = 0;
	e[3] = 4;
	eapol_send(port, EAPOL_EAP, e, 4);
}


static void request_identity(uint8_t port)
{
	struct port *p = &ports[port];
	uint8_t *e = eap_buf + 4;

	p->eap_id++;
	p->last_id = p->eap_id;
	e[0] = EAP_REQUEST;
	e[1] = p->eap_id;
	e[2] = 0;
	e[3] = 5;
	e[4] = EAP_IDENTITY;
	eapol_send(port, EAPOL_EAP, e, 5);
}


static void cancel_server(struct port *p)
{
	p->rad_busy = false;
	p->waiting_server = false;
}


/* Back to asking for an identity; the port stays open if it was (re-authentication) */
static void connecting(uint8_t port, bool now)
{
	struct port *p = &ports[port];

	cancel_server(p);
	p->s.state = DOT1X_S_CONNECTING;
	p->tx_count = 0;
	p->state_len = 0;
	p->timer = now ? 0 : DOT1X_TX_PERIOD;
	if (now) {
		request_identity(port);
		p->tx_count = 1;
		p->timer = DOT1X_TX_PERIOD;
	}
}


static void fail(uint8_t port, uint8_t id, bool send_failure)
{
	struct port *p = &ports[port];

	cancel_server(p);
	if (send_failure && p->method == DOT1X_M_EAP)
		eap_send_simple(port, EAP_FAILURE, id);
	p->s.failures++;
	p->s.state = DOT1X_S_HELD;
	p->timer = DOT1X_QUIET_PERIOD;
	p->state_len = 0;
	set_authorized(port, false);
	dot1x_hw_log("802.1X: authentication failed on port", port);
}


/* ---- RADIUS ---- */

static uint16_t rad_attr(uint8_t *r, uint16_t n, uint8_t type, const void *v, uint8_t len)
{
	if (n + 2 + len > RAD_MAX)
		return n;
	r[n] = type;
	r[n + 1] = 2 + len;
	memcpy(r + n + 2, v, len);
	return n + 2 + len;
}


static uint16_t rad_attr32(uint8_t *r, uint16_t n, uint8_t type, uint32_t v)
{
	uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };

	return rad_attr(r, n, type, b, 4);
}


static uint16_t rad_mac(uint8_t *r, uint16_t n, uint8_t type, const uint8_t *mac)
{
	static const char hx[] = "0123456789ABCDEF";
	char s[17];

	for (int i = 0; i < 6; i++) {
		s[3 * i] = hx[mac[i] >> 4];
		s[3 * i + 1] = hx[mac[i] & 0xf];
		if (i < 5)
			s[3 * i + 2] = '-';
	}
	return rad_attr(r, n, type, s, 17);
}


/* RFC 2865 5.2: the password, hidden with the secret and the Request Authenticator */
static uint16_t rad_password(uint8_t *r, uint16_t n, const char *pw, const uint8_t *ra)
{
	uint8_t c[64], b[16];
	uint16_t len = strlen(pw), plen = len ? (len + 15) & ~15 : 16;
	const uint8_t *prev = ra;

	if (plen > sizeof(c))
		plen = sizeof(c);
	memset(c, 0, plen);
	memcpy(c, pw, len < plen ? len : plen);
	for (uint16_t i = 0; i < plen; i += 16) {
		dot1x_md5((const uint8_t *)dot1x_cfg.secret, strlen(dot1x_cfg.secret), prev, 16, b);
		for (int k = 0; k < 16; k++)
			c[i + k] ^= b[k];
		prev = c + i;
	}
	return rad_attr(r, n, A_USER_PASSWORD, c, plen);
}


/* Build and send an Access-Request for port: EAP when eap is set, MAB otherwise */
static void access_request(uint8_t port, const uint8_t *eap, uint16_t eap_len)
{
	struct port *p = &ports[port];
	uint8_t *r = p->rad, ip[4], ma[16];
	uint16_t n = 20, ma_at;

	p->rad_id = next_rad_id++;
	p->rad_tries = 1;
	p->rad_busy = true;
	p->waiting_server = true;
	p->timer = DOT1X_SERVER_TIMEOUT;
	dot1x_hw_random(p->req_auth, 16);

	r[0] = RAD_ACCESS_REQUEST;
	r[1] = p->rad_id;
	memcpy(r + 4, p->req_auth, 16);
	if (p->s.identity[0])
		n = rad_attr(r, n, A_USER_NAME, p->s.identity, strlen(p->s.identity));
	else
		n = rad_attr(r, n, A_USER_NAME, "anonymous", 9);
	if (!eap)
		n = rad_password(r, n, p->s.identity, p->req_auth);
	dot1x_hw_nas_ip(ip);
	n = rad_attr(r, n, A_NAS_IP, ip, 4);
	n = rad_attr32(r, n, A_NAS_PORT, dot1x_hw_front_port(port));
	n = rad_attr32(r, n, A_SERVICE_TYPE, eap ? 2 : 10);	/* Framed, or Call-Check for MAB */
	n = rad_attr32(r, n, A_FRAMED_MTU, FRAMED_MTU);
	n = rad_mac(r, n, A_CALLED_STATION, dot1x_hw_mac());
	if (p->s.mac_valid)
		n = rad_mac(r, n, A_CALLING_STATION, p->s.mac);
	n = rad_attr32(r, n, A_NAS_PORT_TYPE, 15);		/* Ethernet */
	if (p->state_len)
		n = rad_attr(r, n, A_STATE, p->state_attr, p->state_len);
	for (uint16_t i = 0; eap && i < eap_len; i += 253) {
		uint16_t k = eap_len - i > 253 ? 253 : eap_len - i;
		n = rad_attr(r, n, A_EAP_MESSAGE, eap + i, k);
	}
	memset(ma, 0, 16);
	ma_at = n + 2;
	n = rad_attr(r, n, A_MESSAGE_AUTH, ma, 16);
	r[2] = n >> 8;
	r[3] = n;
	dot1x_hmac_md5((const uint8_t *)dot1x_cfg.secret, strlen(dot1x_cfg.secret), r, n, ma);
	memcpy(r + ma_at, ma, 16);
	p->rad_len = n;
	dot1x_hw_radius_tx(r, n);
}


/* Response Authenticator and, when present or required, Message-Authenticator */
static bool rad_verify(const struct port *p, uint8_t *r, uint16_t len, bool need_ma)
{
	uint8_t got[16], want[16], ra[16];
	uint16_t slen = strlen(dot1x_cfg.secret);
	struct md5 c;
	int ma_at = -1;

	memcpy(got, r + 4, 16);
	md5_init(&c);
	md5_update(&c, r, 4);
	md5_update(&c, p->req_auth, 16);
	md5_update(&c, r + 20, len - 20);
	md5_update(&c, (const uint8_t *)dot1x_cfg.secret, slen);
	md5_final(&c, want);
	if (memcmp(got, want, 16))
		return false;

	for (uint16_t i = 20; i + 2 <= len && r[i + 1] >= 2; i += r[i + 1])
		if (r[i] == A_MESSAGE_AUTH && r[i + 1] == 18)
			ma_at = i + 2;
	if (ma_at < 0)
		return !need_ma;
	/* RFC 3579 3.2: over the packet with the Request Authenticator and the attribute zeroed */
	memcpy(ra, r + 4, 16);
	memcpy(got, r + ma_at, 16);
	memcpy(r + 4, p->req_auth, 16);
	memset(r + ma_at, 0, 16);
	dot1x_hmac_md5((const uint8_t *)dot1x_cfg.secret, slen, r, len, want);
	memcpy(r + 4, ra, 16);
	memcpy(r + ma_at, got, 16);
	return !memcmp(got, want, 16);
}


static uint32_t get32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | b[2] << 8 | b[3];
}


void dot1x_radius_rx(const uint8_t *in, uint16_t len)
{
	static uint8_t r[RAD_MAX];
	struct port *p = NULL;
	uint8_t port, *eap = eap_buf + 4;
	uint16_t eap_len = 0;
	uint32_t session = 0;
	bool reauth_by_radius = false;
	static uint8_t state[STATE_MAX];
	uint8_t state_len = 0;

	if (len < 20 || len > RAD_MAX || (in[2] << 8 | in[3]) > len)
		return;
	len = in[2] << 8 | in[3];
	memcpy(r, in, len);
	for (port = 0; port < DOT1X_PORTS; port++)
		if (ports[port].rad_busy && ports[port].rad_id == r[1]) {
			p = &ports[port];
			break;
		}
	if (!p)
		return;

	/* EAP-Message and State, before verifying, to know whether Message-Authenticator is required */
	for (uint16_t i = 20; i + 2 <= len; i += r[i + 1]) {
		uint8_t t = r[i], l = r[i + 1];
		if (l < 2 || i + l > len)
			return;
		if (t == A_EAP_MESSAGE && eap_len + l - 2 <= EAP_MAX) {
			memcpy(eap + eap_len, r + i + 2, l - 2);
			eap_len += l - 2;
		} else if (t == A_STATE && l - 2 <= STATE_MAX) {
			memcpy(state, r + i + 2, l - 2);
			state_len = l - 2;
		} else if (t == A_SESSION_TIMEOUT && l == 6) {
			session = get32(r + i + 2);
		} else if (t == A_TERMINATION_ACTION && l == 6) {
			reauth_by_radius = get32(r + i + 2) == 1;
		}
	}
	if (!rad_verify(p, r, len, eap_len > 0 || p->method == DOT1X_M_EAP)) {
		dot1x_hw_log("802.1X: RADIUS reply fails its authenticator (wrong secret?), port", port);
		return;
	}
	cancel_server(p);
	memcpy(p->state_attr, state, state_len);
	p->state_len = state_len;

	switch (r[0]) {
	case RAD_ACCESS_CHALLENGE:
		if (p->method != DOT1X_M_EAP || eap_len < 4) {
			fail(port, p->last_id, true);
			return;
		}
		p->last_id = eap[1];
		eapol_send(port, EAPOL_EAP, eap, eap_len);
		p->timer = DOT1X_SUPP_TIMEOUT;
		return;
	case RAD_ACCESS_ACCEPT:
		if (p->method == DOT1X_M_EAP) {
			if (eap_len >= 4)
				eapol_send(port, EAPOL_EAP, eap, eap_len);
			else
				eap_send_simple(port, EAP_SUCCESS, p->last_id);
		}
		p->s.method = p->method;
		p->s.successes++;
		p->s.state = DOT1X_S_AUTHENTICATED;
		p->state_len = 0;
		p->s.reauth_left = session && (reauth_by_radius || !dot1x_cfg.reauth_s) ? session : dot1x_cfg.reauth_s;
		set_authorized(port, true);
		dot1x_hw_log(p->method == DOT1X_M_MAB ? "802.1X: MAC authenticated on port" : "802.1X: authenticated on port", port);
		return;
	case RAD_ACCESS_REJECT:
		if (p->method == DOT1X_M_EAP && eap_len >= 4) {
			eapol_send(port, EAPOL_EAP, eap, eap_len);
			fail(port, 0, false);
		} else {
			fail(port, p->last_id, true);
		}
		return;
	}
}


/* ---- EAPOL from the supplicant ---- */

static void start_mab(uint8_t port)
{
	struct port *p = &ports[port];
	static const char hx[] = "0123456789abcdef";

	for (int i = 0; i < 6; i++) {
		p->s.identity[2 * i] = hx[p->s.mac[i] >> 4];
		p->s.identity[2 * i + 1] = hx[p->s.mac[i] & 0xf];
	}
	p->s.identity[12] = 0;
	p->method = DOT1X_M_MAB;
	p->s.state = DOT1X_S_AUTHENTICATING;
	p->state_len = 0;
	access_request(port, NULL, 0);
}


void dot1x_eapol_rx(uint8_t port, const uint8_t *src, const uint8_t *pdu, uint16_t len)
{
	struct port *p;
	uint16_t blen;

	if (!port_ok(port) || !controlled(port) || len < 4)
		return;
	p = &ports[port];
	if (p->s.state == DOT1X_S_DOWN || p->s.state == DOT1X_S_HELD)
		return;
	blen = pdu[2] << 8 | pdu[3];
	if (blen > len - 4)
		return;

	switch (pdu[1]) {
	case EAPOL_START:
		memcpy(p->s.mac, src, 6);
		p->s.mac_valid = true;
		connecting(port, true);
		return;
	case EAPOL_LOGOFF:
		set_authorized(port, false);
		connecting(port, false);
		dot1x_hw_log("802.1X: logoff on port", port);
		return;
	case EAPOL_EAP:
		break;
	default:
		return;
	}

	const uint8_t *e = pdu + 4;
	uint16_t elen;
	if (blen < 4 || e[0] != EAP_RESPONSE)
		return;
	elen = e[2] << 8 | e[3];
	if (elen < 5 || elen > blen)
		return;

	if (e[4] == EAP_IDENTITY && e[1] == p->eap_id && p->s.state == DOT1X_S_CONNECTING) {
		uint16_t n = elen - 5 < DOT1X_IDENTITY_MAX - 1 ? elen - 5 : DOT1X_IDENTITY_MAX - 1;
		memcpy(p->s.identity, e + 5, n);
		p->s.identity[n] = 0;
		memcpy(p->s.mac, src, 6);
		p->s.mac_valid = true;
		p->method = DOT1X_M_EAP;
		p->s.state = DOT1X_S_AUTHENTICATING;
		p->state_len = 0;
		access_request(port, e, elen);
		return;
	}
	/* The rest of the conversation, while we wait for the supplicant */
	if (p->s.state == DOT1X_S_AUTHENTICATING && p->method == DOT1X_M_EAP && !p->waiting_server
	    && e[1] == p->last_id)
		access_request(port, e, elen);
}


void dot1x_seen(uint8_t port, const uint8_t *src)
{
	struct port *p;

	if (!port_ok(port) || !controlled(port) || (src[0] & 1))
		return;
	p = &ports[port];
	if (p->s.state == DOT1X_S_CONNECTING && !p->s.mac_valid) {
		memcpy(p->s.mac, src, 6);
		p->s.mac_valid = true;
	}
}


/* ---- time and configuration ---- */

static void reset_port(uint8_t port, uint8_t state)
{
	struct port *p = &ports[port];

	cancel_server(p);
	p->s.state = state;
	p->s.authorized = false;
	p->s.mac_valid = false;
	p->s.identity[0] = 0;
	p->s.reauth_left = 0;
	p->state_len = 0;
	p->tx_count = 0;
	p->timer = 0;
}


void dot1x_second(void)
{
	for (uint8_t port = 0; port < DOT1X_PORTS; port++) {
		struct port *p = &ports[port];

		if (!port_ok(port) || !controlled(port))
			continue;
		if (p->s.state == DOT1X_S_AUTHENTICATED) {
			if (p->s.reauth_left && !--p->s.reauth_left) {
				dot1x_hw_log("802.1X: re-authenticating port", port);
				if (p->s.method == DOT1X_M_MAB) {
					p->s.state = DOT1X_S_AUTHENTICATING;
					p->method = DOT1X_M_MAB;
					access_request(port, NULL, 0);
				} else {
					connecting(port, true);
				}
			}
			continue;
		}
		if (p->timer && --p->timer)
			continue;
		switch (p->s.state) {
		case DOT1X_S_CONNECTING:
			if (p->tx_count >= DOT1X_MAB_AFTER && p->s.authorized) {
				/* re-authentication went unanswered */
				fail(port, p->last_id, false);
			} else if (p->tx_count >= DOT1X_MAB_AFTER && dot1x_cfg.mab[port] && p->s.mac_valid) {
				start_mab(port);
			} else {
				request_identity(port);
				if (p->tx_count < 255)
					p->tx_count++;
				p->timer = DOT1X_TX_PERIOD;
			}
			break;
		case DOT1X_S_AUTHENTICATING:
			if (p->waiting_server && p->rad_tries < DOT1X_SERVER_TRIES) {
				p->rad_tries++;
				p->timer = DOT1X_SERVER_TIMEOUT;
				dot1x_hw_radius_tx(p->rad, p->rad_len);
			} else if (p->waiting_server) {
				dot1x_hw_log("802.1X: no answer from the RADIUS server, port", port);
				fail(port, p->last_id, true);
			} else {
				/* the supplicant went quiet */
				connecting(port, true);
			}
			break;
		case DOT1X_S_HELD:
			connecting(port, true);
			break;
		}
	}
}


void dot1x_link(uint8_t port, bool up)
{
	if (!port_ok(port) || !controlled(port))
		return;
	if (!up) {
		if (ports[port].s.state != DOT1X_S_DOWN) {
			reset_port(port, DOT1X_S_DOWN);
			gate();
		}
		return;
	}
	if (ports[port].s.state == DOT1X_S_DOWN) {
		reset_port(port, DOT1X_S_CONNECTING);
		ports[port].timer = 1;		/* the first request a second after the link */
	}
}


void dot1x_restart(uint8_t port)
{
	if (!port_ok(port) || !controlled(port) || ports[port].s.state == DOT1X_S_DOWN)
		return;
	reset_port(port, DOT1X_S_CONNECTING);
	gate();
	ports[port].timer = 1;
}


void dot1x_apply(void)
{
	for (uint8_t port = 0; port < DOT1X_PORTS; port++) {
		struct port *p = &ports[port];
		uint8_t want;

		if (!port_ok(port))
			continue;
		if (!dot1x_cfg.enabled || dot1x_cfg.mode[port] == DOT1X_FORCE_AUTH)
			want = DOT1X_S_OFF;
		else if (dot1x_cfg.mode[port] == DOT1X_FORCE_UNAUTH)
			want = DOT1X_S_BLOCKED;
		else
			want = DOT1X_S_DOWN;
		if (want == DOT1X_S_DOWN) {
			/* an auto port that was off or blocked waits for its link (dot1x_link) */
			if (p->s.state == DOT1X_S_OFF || p->s.state == DOT1X_S_BLOCKED)
				reset_port(port, DOT1X_S_DOWN);
		} else if (p->s.state != want) {
			reset_port(port, want);
		}
	}
	gate_known = false;
	gate();
}


void dot1x_init(void)
{
	memset(ports, 0, sizeof(ports));
	gate_known = false;
	if (!dot1x_cfg.radius_port)
		dot1x_cfg.radius_port = 1812;
}


uint16_t dot1x_blocked(void)
{
	uint16_t b = 0;

	if (!dot1x_cfg.enabled)
		return 0;
	for (uint8_t port = 0; port < DOT1X_PORTS; port++) {
		if (!port_ok(port))
			continue;
		if (dot1x_cfg.mode[port] == DOT1X_FORCE_UNAUTH
		    || (dot1x_cfg.mode[port] == DOT1X_AUTO && !ports[port].s.authorized))
			b |= 1 << port;
	}
	return b;
}


const struct dot1x_status *dot1x_port_status(uint8_t port)
{
	return port < DOT1X_PORTS ? &ports[port].s : NULL;
}
