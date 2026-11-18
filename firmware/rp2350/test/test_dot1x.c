/*
 * dot1x.c against a simulated supplicant (EAP-MD5) and a simulated RADIUS
 * server that checks every Request's Message-Authenticator and signs every
 * reply as RFC 2865/3579 say. run_dot1x_radius.sh runs the same core against a
 * real FreeRADIUS.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dot1x.h"

static int failures, checks;
#define check(c, ...) do { checks++; if (c) { printf("PASS  "); } else { printf("FAIL  "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- the platform ---- */
static const uint8_t sw_mac[6] = { 0x06, 0x0f, 0x00, 0xdd, 0x3b, 0x3e };
static uint16_t gate_mask = 0xffff;
static bool gate_on;
static int gate_calls;
static unsigned rnd = 1;

uint16_t dot1x_hw_ports(void) { return 0x1f8; }		/* logical 3-8, as an RTL8372 */
uint8_t dot1x_hw_front_port(uint8_t port) { return port - 2; }
void dot1x_hw_random(uint8_t *p, uint8_t n) { while (n--) *p++ = (rnd = rnd * 1103515245 + 12345) >> 16; }
void dot1x_hw_nas_ip(uint8_t ip[4]) { ip[0] = 192; ip[1] = 168; ip[2] = 10; ip[3] = 247; }
const uint8_t *dot1x_hw_mac(void) { return sw_mac; }
void dot1x_hw_log(const char *msg, uint8_t port) { printf("      log: %s %u\n", msg, port); }
void dot1x_hw_gate(uint16_t blocked, bool on) { gate_mask = blocked; gate_on = on; gate_calls++; }

/* Messages in flight: to the supplicant on a port, and to the server */
#define QN 64
struct msg { int port; uint16_t len; uint8_t d[1800]; };
static struct msg q_supp[QN], q_srv[QN];
static int n_supp, n_srv;
static int radius_sent;
static bool server_down;

void dot1x_hw_eapol_tx(uint8_t port, const uint8_t *pdu, uint16_t len)
{
	if (n_supp < QN) {
		q_supp[n_supp].port = port;
		q_supp[n_supp].len = len;
		memcpy(q_supp[n_supp++].d, pdu, len);
	}
}

void dot1x_hw_radius_tx(const uint8_t *p, uint16_t len)
{
	radius_sent++;
	if (server_down || n_srv >= QN)
		return;
	q_srv[n_srv].len = len;
	memcpy(q_srv[n_srv++].d, p, len);
}

/* ---- the supplicant ---- */
struct supp {
	bool present;			/* answers EAPOL at all */
	const char *user, *password;
	uint8_t mac[6];
	int successes, failures, id_requests, big_seen;
};
static struct supp supp[10];

static void supp_send_eap(int port, const uint8_t *eap, uint16_t len)
{
	uint8_t f[1800];
	f[0] = 2; f[1] = 0; f[2] = len >> 8; f[3] = len;
	memcpy(f + 4, eap, len);
	dot1x_eapol_rx(port, supp[port].mac, f, len + 4);
}

static void supp_rx(int port, const uint8_t *pdu, uint16_t len)
{
	struct supp *s = &supp[port];
	const uint8_t *e = pdu + 4;
	uint8_t r[1600];

	if (!s->present || pdu[1] != 0 || len < 8)
		return;
	if (e[0] == 3) { s->successes++; return; }
	if (e[0] == 4) { s->failures++; return; }
	if (e[0] != 1)
		return;
	r[0] = 2; r[1] = e[1];
	if (e[4] == 1) {			/* Identity */
		s->id_requests++;
		uint16_t n = strlen(s->user);
		r[4] = 1; memcpy(r + 5, s->user, n);
		n += 5; r[2] = n >> 8; r[3] = n;
		supp_send_eap(port, r, n);
	} else if (e[4] == 4) {			/* MD5-Challenge: MD5(id || password || challenge) */
		uint8_t buf[200], h[16];
		uint16_t vs = e[5];
		buf[0] = e[1];
		memcpy(buf + 1, s->password, strlen(s->password));
		memcpy(buf + 1 + strlen(s->password), e + 6, vs);
		dot1x_md5(buf, 1 + strlen(s->password) + vs, NULL, 0, h);
		r[4] = 4; r[5] = 16; memcpy(r + 6, h, 16);
		r[2] = 0; r[3] = 22;
		supp_send_eap(port, r, 22);
	} else if (e[4] == 13) {		/* a big request: answer with a big response */
		uint16_t elen = e[2] << 8 | e[3];
		s->big_seen = elen;
		uint16_t n = 700;
		memset(r, 0x5a, n);
		r[0] = 2; r[1] = e[1]; r[2] = n >> 8; r[3] = n; r[4] = 13;
		supp_send_eap(port, r, n);
	}
}

/* ---- the RADIUS server ---- */
static const char *srv_secret = "testing123";
static const char *nas_secret = "testing123";
static int srv_requests, srv_bad_ma, srv_mab_ok, srv_big;
static uint8_t challenge[16];
static uint32_t session_timeout;
static bool srv_big_mode;
static char last_calling[32];
static uint32_t last_nas_port, last_service;

static int attr_find(const uint8_t *r, int len, int type, int from)
{
	for (int i = from; i + 2 <= len; i += r[i + 1]) {
		if (r[i + 1] < 2)
			return -1;
		if (r[i] == type)
			return i;
	}
	return -1;
}

static void srv_reply(const uint8_t *req, uint8_t code, const uint8_t *eap, uint16_t elen, bool with_state)
{
	uint8_t r[1800], h[16];
	uint16_t n = 20;
	int ma_at;

	r[0] = code; r[1] = req[1];
	memcpy(r + 4, req + 4, 16);	/* the Request Authenticator, for MA and the hash */
	for (uint16_t i = 0; eap && i < elen; i += 253) {
		uint16_t k = elen - i > 253 ? 253 : elen - i;
		r[n] = 79; r[n + 1] = k + 2; memcpy(r + n + 2, eap + i, k); n += k + 2;
	}
	if (with_state) { r[n] = 24; r[n + 1] = 6; memcpy(r + n + 2, "st-1", 4); n += 6; }
	if (session_timeout && code == 2) {
		r[n] = 27; r[n + 1] = 6;
		r[n + 2] = session_timeout >> 24; r[n + 3] = session_timeout >> 16; r[n + 4] = session_timeout >> 8; r[n + 5] = session_timeout;
		n += 6;
		r[n] = 29; r[n + 1] = 6; r[n + 2] = 0; r[n + 3] = 0; r[n + 4] = 0; r[n + 5] = 1; n += 6;
	}
	ma_at = n + 2;
	r[n] = 80; r[n + 1] = 18; memset(r + n + 2, 0, 16); n += 18;
	r[2] = n >> 8; r[3] = n;
	dot1x_hmac_md5((const uint8_t *)srv_secret, strlen(srv_secret), r, n, h);
	memcpy(r + ma_at, h, 16);
	/* Response Authenticator: MD5(Code+ID+Length+RequestAuth+Attributes+Secret) */
	uint8_t tmp[1900];
	memcpy(tmp, r, n);
	memcpy(tmp + n, srv_secret, strlen(srv_secret));
	dot1x_md5(tmp, n + strlen(srv_secret), NULL, 0, h);
	memcpy(r + 4, h, 16);
	dot1x_radius_rx(r, n);
}

static void srv_rx(uint8_t *req, uint16_t len)
{
	uint8_t eap[1600], h[16], ma[16];
	uint16_t elen = 0;
	int i;

	srv_requests++;
	/* The Request's Message-Authenticator, with the attribute zeroed */
	i = attr_find(req, len, 80, 20);
	if (i < 0) { srv_bad_ma++; return; }
	memcpy(ma, req + i + 2, 16);
	memset(req + i + 2, 0, 16);
	dot1x_hmac_md5((const uint8_t *)srv_secret, strlen(srv_secret), req, len, h);
	memcpy(req + i + 2, ma, 16);
	if (memcmp(h, ma, 16)) { srv_bad_ma++; return; }

	i = attr_find(req, len, 31, 20);
	last_calling[0] = 0;
	if (i >= 0) { memcpy(last_calling, req + i + 2, req[i + 1] - 2); last_calling[req[i + 1] - 2] = 0; }
	i = attr_find(req, len, 5, 20);
	last_nas_port = i >= 0 ? (uint32_t)req[i + 5] : 0;
	i = attr_find(req, len, 6, 20);
	last_service = i >= 0 ? (uint32_t)req[i + 5] : 0;

	for (i = 20; (i = attr_find(req, len, 79, i)) >= 0; i += req[i + 1]) {
		memcpy(eap + elen, req + i + 2, req[i + 1] - 2);
		elen += req[i + 1] - 2;
	}
	if (!elen) {
		/* MAB: User-Password = User-Name, hidden with the secret */
		int u = attr_find(req, len, 1, 20), p = attr_find(req, len, 2, 20);
		char user[64], pw[64];
		memcpy(user, req + u + 2, req[u + 1] - 2); user[req[u + 1] - 2] = 0;
		uint8_t b[16], tmp[100];
		memcpy(tmp, srv_secret, strlen(srv_secret));
		memcpy(tmp + strlen(srv_secret), req + 4, 16);
		dot1x_md5(tmp, strlen(srv_secret) + 16, NULL, 0, b);
		for (int k = 0; k < 16; k++) pw[k] = req[p + 2 + k] ^ b[k];
		pw[16] = 0;
		bool ok = !strcmp(user, pw) && !strcmp(user, "020000000003");
		srv_mab_ok += ok;
		srv_reply(req, ok ? 2 : 3, NULL, 0, false);
		return;
	}
	if ((eap[2] << 8 | eap[3]) != elen) { srv_reply(req, 3, NULL, 0, false); return; }
	if (eap[4] == 1) {			/* Response/Identity: challenge it */
		uint8_t c[64];
		if (srv_big_mode) {
			uint16_t n = 900;
			memset(c, 0, sizeof(c));
			static uint8_t big[900];
			memset(big, 0xa5, n);
			big[0] = 1; big[1] = eap[1] + 1; big[2] = n >> 8; big[3] = n; big[4] = 13;
			srv_reply(req, 11, big, n, true);
			return;
		}
		for (int k = 0; k < 16; k++) challenge[k] = rand();
		c[0] = 1; c[1] = eap[1] + 1; c[2] = 0; c[3] = 22; c[4] = 4; c[5] = 16;
		memcpy(c + 6, challenge, 16);
		srv_reply(req, 11, c, 22, true);
		return;
	}
	if (attr_find(req, len, 24, 20) < 0) { srv_reply(req, 3, NULL, 0, false); return; }
	uint8_t res[4] = { 3, eap[1], 0, 4 }, rej[4] = { 4, eap[1], 0, 4 };
	if (eap[4] == 13) {
		srv_big = elen;
		srv_reply(req, 2, res, 4, false);
		return;
	}
	if (eap[4] == 4) {
		uint8_t buf[200], want[16];
		const char *pw = "secret";
		buf[0] = eap[1];
		memcpy(buf + 1, pw, strlen(pw));
		memcpy(buf + 1 + strlen(pw), challenge, 16);
		dot1x_md5(buf, 1 + strlen(pw) + 16, NULL, 0, want);
		bool ok = !memcmp(want, eap + 6, 16);
		srv_reply(req, ok ? 2 : 3, ok ? res : rej, 4, false);
		return;
	}
	srv_reply(req, 3, rej, 4, false);
}

/* Deliver everything in flight until nothing moves */
static void pump(void)
{
	for (int round = 0; round < 100 && (n_supp || n_srv); round++) {
		static struct msg cur[QN];
		int n = n_srv;
		memcpy(cur, q_srv, sizeof(cur[0]) * n);
		n_srv = 0;
		for (int k = 0; k < n; k++)
			srv_rx(cur[k].d, cur[k].len);
		n = n_supp;
		memcpy(cur, q_supp, sizeof(cur[0]) * n);
		n_supp = 0;
		for (int k = 0; k < n; k++)
			supp_rx(cur[k].port, cur[k].d, cur[k].len);
	}
}

static void seconds(int n)
{
	while (n--) {
		dot1x_second();
		pump();
	}
}

static uint8_t st(int port) { return dot1x_port_status(port)->state; }
static bool open_(int port) { return !(dot1x_blocked() >> port & 1); }

static void hex16(const uint8_t *h, char *out) { for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", h[i]); }

static void t_md5(void)
{
	static const struct { const char *in, *out; } v[] = {	/* RFC 1321 A.5 */
		{ "", "d41d8cd98f00b204e9800998ecf8427e" },
		{ "a", "0cc175b9c0f1b6a831c399e269772661" },
		{ "abc", "900150983cd24fb0d6963f7d28e17f72" },
		{ "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
		{ "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
		{ "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
		  "57edf4a22be3c955ac49da2e2107b67a" },
	};
	uint8_t h[16];
	char x[33];
	int ok = 1;
	for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		dot1x_md5((const uint8_t *)v[i].in, strlen(v[i].in), NULL, 0, h);
		hex16(h, x);
		ok &= !strcmp(x, v[i].out);
	}
	check(ok, "MD5: the RFC 1321 test suite");
	/* RFC 2202 test cases 1 and 2 */
	uint8_t key[16];
	memset(key, 0x0b, 16);
	dot1x_hmac_md5(key, 16, (const uint8_t *)"Hi There", 8, h);
	hex16(h, x);
	ok = !strcmp(x, "9294727a3638bb1c13f48ef8158bfc9d");
	dot1x_hmac_md5((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, h);
	hex16(h, x);
	ok &= !strcmp(x, "750c783e6ab0b503eaa86e310a5db738");
	check(ok, "HMAC-MD5: RFC 2202 cases 1 and 2");
}

int main(void)
{
	t_md5();
	dot1x_cfg.enabled = true;
	memcpy(dot1x_cfg.radius_ip, "\xc0\xa8\x0a\x01", 4);
	strcpy(dot1x_cfg.secret, nas_secret);
	dot1x_cfg.mode[4] = DOT1X_AUTO;
	dot1x_cfg.mode[5] = DOT1X_AUTO;
	dot1x_cfg.mode[6] = DOT1X_AUTO;
	dot1x_cfg.mab[6] = true;
	dot1x_cfg.mode[8] = DOT1X_FORCE_UNAUTH;
	dot1x_init();
	dot1x_apply();
	check(gate_on && gate_mask == (1 << 4 | 1 << 5 | 1 << 6 | 1 << 8), "auto ports wait, force-unauth is blocked, the rest open: %03x", gate_mask);

	/* EAP-MD5 on port 4 (front port 2) */
	supp[4] = (struct supp){ true, "alice", "secret", { 0x02, 0, 0, 0, 0, 0x04 }, 0, 0, 0, 0 };
	dot1x_link(4, true);
	seconds(2);
	check(supp[4].id_requests == 1 && supp[4].successes == 1, "port 2: Request/Identity, then EAP Success");
	check(st(4) == DOT1X_S_AUTHENTICATED && open_(4) && !(gate_mask >> 4 & 1), "port 2 authorized and open");
	check(!strcmp(dot1x_port_status(4)->identity, "alice") && dot1x_port_status(4)->method == DOT1X_M_EAP, "identity alice, by EAP");
	check(!strcmp(last_calling, "02-00-00-00-00-04") && last_nas_port == 2 && last_service == 2,
	      "Calling-Station-Id %s, NAS-Port %u, Service-Type Framed", last_calling, last_nas_port);
	check(srv_bad_ma == 0, "every Request's Message-Authenticator checks out");

	/* A wrong password: rejected, held, then asked again */
	supp[5] = (struct supp){ true, "bob", "wrong", { 0x02, 0, 0, 0, 0, 0x05 }, 0, 0, 0, 0 };
	dot1x_link(5, true);
	seconds(2);
	check(supp[5].failures == 1 && st(5) == DOT1X_S_HELD && !open_(5), "port 3: wrong password, EAP Failure, held and closed");
	supp[5].password = "secret";
	seconds(DOT1X_QUIET_PERIOD - 2);	/* the failure came in the first of the two seconds above */
	check(supp[5].id_requests == 1 && !open_(5), "nothing during the quiet period (%d requests)", supp[5].id_requests);
	seconds(1);
	check(supp[5].id_requests == 2 && st(5) == DOT1X_S_AUTHENTICATED && open_(5),
	      "then it asks again and lets the right password in");

	/* Logoff */
	uint8_t logoff[4] = { 2, 2, 0, 0 };
	dot1x_eapol_rx(5, supp[5].mac, logoff, 4);
	check(!open_(5) && st(5) == DOT1X_S_CONNECTING, "EAPOL-Logoff closes the port");

	/* MAB on port 6: a device without a supplicant */
	dot1x_link(6, true);
	uint8_t dev[6] = { 0x02, 0, 0, 0, 0, 0x03 };
	dot1x_seen(6, dev);
	seconds(DOT1X_TX_PERIOD * DOT1X_MAB_AFTER);
	check(srv_mab_ok == 0 && st(6) == DOT1X_S_CONNECTING, "no MAB before %d unanswered requests", DOT1X_MAB_AFTER);
	seconds(2);
	check(srv_mab_ok == 1 && st(6) == DOT1X_S_AUTHENTICATED && open_(6) && dot1x_port_status(6)->method == DOT1X_M_MAB
	      && last_service == 10, "MAB: the MAC as user and hidden password, Call-Check, accepted");

	/* An EAP-TLS-sized exchange: EAP-Message split and joined at 253 bytes */
	srv_big_mode = true;
	dot1x_restart(4);
	seconds(2);
	check(supp[4].big_seen == 900 && srv_big == 700, "a 900-byte request and a 700-byte response cross whole (%d, %d)",
	      supp[4].big_seen, srv_big);
	check(open_(4), "and the port opens");
	srv_big_mode = false;

	/* Re-authentication: the port stays open while it runs */
	dot1x_cfg.reauth_s = 100;
	dot1x_restart(4);
	seconds(2);
	int ids = supp[4].id_requests;
	seconds(99);
	check(supp[4].id_requests == ids + 1 && open_(4), "re-authentication after 100 s, port open throughout");
	supp[4].password = "wrong";
	seconds(100);
	check(!open_(4) && st(4) == DOT1X_S_HELD, "a failed re-authentication closes it");
	supp[4].password = "secret";
	dot1x_cfg.reauth_s = 0;

	/* Session-Timeout from the server sets the re-authentication */
	session_timeout = 600;
	seconds(DOT1X_QUIET_PERIOD + 1);
	check(open_(4) && dot1x_port_status(4)->reauth_left > 590, "Session-Timeout 600: re-authentication due in %u s",
	      dot1x_port_status(4)->reauth_left);
	session_timeout = 0;

	/* The server down: three tries, then held */
	server_down = true;
	dot1x_restart(4);
	int s0 = radius_sent;
	seconds(1 + DOT1X_SERVER_TIMEOUT * DOT1X_SERVER_TRIES);
	check(radius_sent - s0 == DOT1X_SERVER_TRIES && st(4) == DOT1X_S_HELD && !open_(4),
	      "no server: %d tries, then held", radius_sent - s0);
	server_down = false;

	/* A server with another secret: its replies don't verify, so nothing opens */
	srv_secret = "other";
	dot1x_restart(5);
	seconds(1 + DOT1X_SERVER_TIMEOUT * DOT1X_SERVER_TRIES);
	check(!open_(5) && st(5) == DOT1X_S_HELD, "replies signed with another secret are ignored");
	srv_secret = "testing123";

	/* Link down */
	dot1x_link(6, false);
	check(!open_(6) && st(6) == DOT1X_S_DOWN, "link down: closed, waiting for the link");

	/* Off: everything open, EAPOL no longer trapped */
	dot1x_cfg.enabled = false;
	dot1x_apply();
	check(!gate_on && gate_mask == 0 && st(4) == DOT1X_S_OFF, "802.1X off: no port gated");

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
