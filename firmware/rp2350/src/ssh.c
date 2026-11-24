/* The SSH server's protocol, see ssh.h */

#define MBEDTLS_ALLOW_PRIVATE_ACCESS	/* the host key's numbers */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#include "rng.h"
#include "ssh.h"

#define VERSION		"SSH-2.0-modchip_1"
#define IN_MAX		4096		/* a whole packet on the wire */
#define PAYLOAD_MAX	(IN_MAX - 64)
#define OUT_CHUNK	1024		/* channel data per packet */
#define WINDOW		2048
#define OUTPUT_MAX	24576		/* a command's output */
#define RESTORE_MAX	6144
#define LINE_MAX_	126
#define HISTORY		8
#define TRIES		3
#define LOGIN_S		60
#define IDLE_S		900
#define LOCK_AFTER	6
#define LOCK_S		30

enum {
	M_DISCONNECT = 1, M_IGNORE, M_UNIMPLEMENTED, M_DEBUG, M_SERVICE_REQUEST, M_SERVICE_ACCEPT,
	M_KEXINIT = 20, M_NEWKEYS, M_KEX_INIT = 30, M_KEX_REPLY,
	M_AUTH_REQUEST = 50, M_AUTH_FAILURE, M_AUTH_SUCCESS,
	M_GLOBAL_REQUEST = 80, M_REQUEST_SUCCESS, M_REQUEST_FAILURE,
	M_CHANNEL_OPEN = 90, M_OPEN_CONFIRM, M_OPEN_FAILURE, M_WINDOW_ADJUST, M_DATA, M_EXT_DATA, M_EOF, M_CLOSE,
	M_CHANNEL_REQUEST, M_CHANNEL_SUCCESS, M_CHANNEL_FAILURE
};

enum { C_AES128_CTR, C_AES256_CTR, C_AES128_GCM, C_AES256_GCM, CIPHERS };
enum { H_SHA256, H_SHA256_ETM, MACS };
enum { K_X25519, K_X25519_LIBSSH, K_P256, KEXES };

static const char *const cipher_names[CIPHERS] = { "aes128-ctr", "aes256-ctr", "aes128-gcm@openssh.com", "aes256-gcm@openssh.com" };
static const char *const mac_names[MACS] = { "hmac-sha2-256", "hmac-sha2-256-etm@openssh.com" };
static const char *const kex_names[KEXES] = { "curve25519-sha256", "curve25519-sha256@libssh.org", "ecdh-sha2-nistp256" };
static const char hostkey_name[] = "ecdsa-sha2-nistp256";

/* As offered, by preference */
static const char kex_list[] = "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,kex-strict-s-v00@openssh.com";
static const char cipher_list[] = "aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes256-ctr";
static const char mac_list[] = "hmac-sha2-256-etm@openssh.com,hmac-sha2-256";

/* One direction */
struct keys {
	bool on;
	uint8_t cipher, mac;
	uint32_t seq;
	uint8_t iv[16], mac_key[32];
	mbedtls_aes_context aes;
	mbedtls_gcm_context gcm;
	size_t nc_off;
	uint8_t stream[16];
};

struct ssh {
	void *io;
	char peer[48];
	bool over;			/* close once everything is out */

	/* the version exchange */
	char v_c[256];
	size_t v_len;
	bool v_done;

	/* packets in */
	uint8_t in[IN_MAX];
	size_t in_len;
	uint32_t pkt_len;		/* of the one being received, 0: not known yet */
	struct keys rx, tx;

	/* key exchange */
	bool kex_sent, kex_got, first_kex_done, strict, skip_guess, wait_newkeys;
	uint8_t kex, next_rx_c, next_rx_m, next_tx_c, next_tx_m;
	uint8_t *i_c, *i_s;		/* both KEXINIT payloads */
	size_t i_c_len, i_s_len;
	uint8_t session_id[32];
	uint8_t k_mpint[38], h[32];
	size_t k_len;

	/* authentication */
	bool service, authed;
	uint8_t tries;
	uint16_t delay;			/* s until a failure is answered */
	uint16_t login_left, idle;

	/* the channel */
	bool open, pty, shell, exec, restoring, eof_in, close_sent, close_got;
	uint32_t peer_ch, peer_window, peer_max;
	uint32_t window;		/* what the peer may still send */
	char line[LINE_MAX_ + 2];
	size_t line_len;
	uint8_t esc;
	bool last_cr;
	char (*hist)[LINE_MAX_ + 2];
	uint8_t hist_n, hist_at;

	/* output waiting for the window and the connection */
	char *pend;
	size_t pend_len, pend_off, pend_max;
	bool finishing;			/* then exit status and close */
	uint8_t status;

	char *file;			/* restore */
	size_t file_len;
	unsigned minutes;
};

static mbedtls_pk_context host_key;
static bool have_key;
static uint8_t key_blob[128];
static size_t key_blob_len;
static uint8_t failures_, lock_left;

/* ---- buffers ---- */

struct buf { uint8_t *p; size_t n, max; bool bad; };

static void w_raw(struct buf *b, const void *d, size_t n)
{
	if (b->bad || b->n + n > b->max) {
		b->bad = true;
		return;
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void w8(struct buf *b, uint8_t v) { w_raw(b, &v, 1); }

static void w32(struct buf *b, uint32_t v)
{
	uint8_t d[4] = { v >> 24, v >> 16, v >> 8, v };

	w_raw(b, d, 4);
}

static void w_str(struct buf *b, const void *d, size_t n)
{
	w32(b, n);
	w_raw(b, d, n);
}

static void w_cstr(struct buf *b, const char *s) { w_str(b, s, strlen(s)); }

/* An unsigned big-endian number as an mpint */
static void w_mpint(struct buf *b, const uint8_t *d, size_t n)
{
	while (n && !*d) {
		d++;
		n--;
	}
	w32(b, n + (n && (*d & 0x80)));
	if (n && (*d & 0x80))
		w8(b, 0);
	w_raw(b, d, n);
}

struct rd { const uint8_t *p, *end; bool bad; };

static uint8_t r8(struct rd *r)
{
	if (r->bad || r->p == r->end) {
		r->bad = true;
		return 0;
	}
	return *r->p++;
}

static uint32_t r32(struct rd *r)
{
	uint32_t v = 0;

	for (int i = 0; i < 4; i++)
		v = v << 8 | r8(r);
	return v;
}

static struct rd r_str(struct rd *r)
{
	uint32_t n = r32(r);
	struct rd s = { r->p, r->p, true };

	if (r->bad || n > (size_t)(r->end - r->p)) {
		r->bad = true;
		return s;
	}
	s.end = r->p += n;
	s.bad = false;
	return s;
}

static bool is(const struct rd *s, const char *t)
{
	return !s->bad && (size_t)(s->end - s->p) == strlen(t) && !memcmp(s->p, t, s->end - s->p);
}

/* Is name one of the comma separated list? */
static bool in_list(const struct rd *l, const char *name)
{
	size_t n = strlen(name);
	const uint8_t *p = l->p;

	while (p < l->end) {
		const uint8_t *c = memchr(p, ',', l->end - p), *e = c ? c : l->end;
		if ((size_t)(e - p) == n && !memcmp(p, name, n))
			return true;
		p = c ? c + 1 : l->end;
	}
	return false;
}

/* The client's first that we have, -1: none */
static int choose(const struct rd *l, const char *const *names, int count)
{
	const uint8_t *p = l->p;

	while (p < l->end) {
		const uint8_t *c = memchr(p, ',', l->end - p), *e = c ? c : l->end;
		for (int i = 0; i < count; i++)
			if ((size_t)(e - p) == strlen(names[i]) && !memcmp(p, names[i], e - p))
				return i;
		p = c ? c + 1 : l->end;
	}
	return -1;
}


/* ---- packets ---- */

static bool is_gcm(const struct keys *k) { return k->cipher >= C_AES128_GCM; }
static uint8_t key_bits_8(uint8_t cipher) { return cipher == C_AES128_CTR || cipher == C_AES128_GCM ? 16 : 32; }

static void gcm_iv_next(struct keys *k)
{
	for (int i = 11; i >= 4 && !++k->iv[i]; i--)
		;
}

static int mac_of(const struct keys *k, const uint8_t *a, size_t an, const uint8_t *d, size_t n, uint8_t out[32])
{
	uint8_t seq[4] = { k->seq >> 24, k->seq >> 16, k->seq >> 8, k->seq };
	mbedtls_md_context_t c;
	int r;

	mbedtls_md_init(&c);
	r = mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
	if (!r)
		r = mbedtls_md_hmac_starts(&c, k->mac_key, 32);
	if (!r)
		r = mbedtls_md_hmac_update(&c, seq, 4);
	if (!r && an)
		r = mbedtls_md_hmac_update(&c, a, an);
	if (!r)
		r = mbedtls_md_hmac_update(&c, d, n);
	if (!r)
		r = mbedtls_md_hmac_finish(&c, out);
	mbedtls_md_free(&c);
	return r;
}


/* What a packet of n payload bytes takes at most */
#define WIRE(n)		((n) + 4 + 1 + 32 + 32)

/* payload into a packet; the caller has seen to the room */
static void send_packet(struct ssh *s, const uint8_t *payload, size_t n)
{
	static uint8_t p[WIRE(PAYLOAD_MAX) + 32];
	struct keys *k = &s->tx;
	size_t block = k->on ? 16 : 8, aad = k->on && (is_gcm(k) || k->mac == H_SHA256_ETM) ? 4 : 0, pad, len, total;

	if (n > PAYLOAD_MAX) {
		s->over = true;
		return;
	}
	pad = block - (4 - aad + 1 + n) % block;
	if (pad < 4)
		pad += block;
	len = 1 + n + pad;
	p[0] = len >> 24; p[1] = len >> 16; p[2] = len >> 8; p[3] = len;
	p[4] = pad;
	memcpy(p + 5, payload, n);
	if (rng_get(NULL, p + 5 + n, pad))
		s->over = true;
	total = 4 + len;
	if (k->on && is_gcm(k)) {
		if (mbedtls_gcm_crypt_and_tag(&k->gcm, MBEDTLS_GCM_ENCRYPT, len, k->iv, 12, p, 4, p + 4, p + 4, 16, p + total))
			s->over = true;
		gcm_iv_next(k);
		total += 16;
	} else if (k->on) {
		uint8_t m[32];

		if (!aad && mac_of(k, NULL, 0, p, total, m))
			s->over = true;
		if (mbedtls_aes_crypt_ctr(&k->aes, total - aad, &k->nc_off, k->iv, k->stream, p + aad, p + aad))
			s->over = true;
		if (aad && mac_of(k, NULL, 0, p, total, m))
			s->over = true;
		memcpy(p + total, m, 32);
		total += 32;
	}
	k->seq++;
	if (ssh_io_room(s->io) < total) {
		s->over = true;		/* a peer that doesn't read what it asked for */
		return;
	}
	ssh_io_write(s->io, p, total);
}


static void send_buf(struct ssh *s, struct buf *b)
{
	if (b->bad)
		s->over = true;
	else
		send_packet(s, b->p, b->n);
}

#define BUF(name, size)	uint8_t name##_[size]; struct buf name = { name##_, 0, sizeof(name##_), false }

static void disconnect(struct ssh *s, uint32_t why, const char *text)
{
	BUF(b, 120);

	if (s->over)
		return;
	w8(&b, M_DISCONNECT);
	w32(&b, why);
	w_cstr(&b, text);
	w_cstr(&b, "");
	send_buf(s, &b);
	s->over = true;
}


/* ---- key exchange ---- */

static void send_kexinit(struct ssh *s)
{
	BUF(b, 600);
	uint8_t cookie[16];

	if (rng_get(NULL, cookie, 16)) {
		s->over = true;
		return;
	}
	w8(&b, M_KEXINIT);
	w_raw(&b, cookie, 16);
	w_cstr(&b, kex_list);
	w_cstr(&b, hostkey_name);
	w_cstr(&b, cipher_list);
	w_cstr(&b, cipher_list);
	w_cstr(&b, mac_list);
	w_cstr(&b, mac_list);
	w_cstr(&b, "none");
	w_cstr(&b, "none");
	w_cstr(&b, "");
	w_cstr(&b, "");
	w8(&b, 0);
	w32(&b, 0);
	free(s->i_s);
	s->i_s = malloc(b.n);
	if (!s->i_s || b.bad) {
		s->over = true;
		return;
	}
	memcpy(s->i_s, b.p, b.n);
	s->i_s_len = b.n;
	send_buf(s, &b);
	s->kex_sent = true;
}


static void got_kexinit(struct ssh *s, const uint8_t *d, size_t n)
{
	struct rd r = { d + 17, d + n, n < 17 }, kex, hk, c1, c2, m1, m2, z1, z2;
	int k, e1, e2, x1, x2;
	bool follows;

	if (s->kex_got) {
		disconnect(s, 2, "KEXINIT twice");
		return;
	}
	kex = r_str(&r); hk = r_str(&r);
	c1 = r_str(&r); c2 = r_str(&r);
	m1 = r_str(&r); m2 = r_str(&r);
	z1 = r_str(&r); z2 = r_str(&r);
	r_str(&r); r_str(&r);
	follows = r8(&r);
	if (r.bad) {
		disconnect(s, 2, "malformed KEXINIT");
		return;
	}
	k = choose(&kex, kex_names, KEXES);
	e1 = choose(&c1, cipher_names, CIPHERS);
	e2 = choose(&c2, cipher_names, CIPHERS);
	x1 = choose(&m1, mac_names, MACS);
	x2 = choose(&m2, mac_names, MACS);
	if (k < 0 || !in_list(&hk, hostkey_name) || e1 < 0 || e2 < 0 || (x1 < 0 && e1 < C_AES128_GCM)
	    || (x2 < 0 && e2 < C_AES128_GCM) || !in_list(&z1, "none") || !in_list(&z2, "none")) {
		disconnect(s, 3, "no algorithms in common");
		return;
	}
	if (!s->first_kex_done)
		s->strict = in_list(&kex, "kex-strict-c-v00@openssh.com");
	if (!s->first_kex_done && s->strict && s->rx.seq != 1) {
		disconnect(s, 2, "strict KEX: something came before KEXINIT");
		return;
	}
	/* A guess is right only if both sides like both methods most */
	s->skip_guess = follows && !(kex.end - kex.p >= 17 && !memcmp(kex.p, kex_list, 17)
				     && (kex.end - kex.p == 17 || kex.p[17] == ',')
				     && hk.end - hk.p >= 19 && !memcmp(hk.p, hostkey_name, 19)
				     && (hk.end - hk.p == 19 || hk.p[19] == ','));
	free(s->i_c);
	s->i_c = malloc(n);
	if (!s->i_c) {
		s->over = true;
		return;
	}
	memcpy(s->i_c, d, n);
	s->i_c_len = n;
	s->kex = k;
	s->next_rx_c = e1;
	s->next_tx_c = e2;
	s->next_rx_m = x1 < 0 ? 0 : x1;
	s->next_tx_m = x2 < 0 ? 0 : x2;
	s->kex_got = true;
	if (!s->kex_sent)
		send_kexinit(s);
}


static int hash_str(mbedtls_sha256_context *c, const void *d, size_t n)
{
	uint8_t l[4] = { n >> 24, n >> 16, n >> 8, n };

	return mbedtls_sha256_update(c, l, 4) | mbedtls_sha256_update(c, d, n);
}


/* Our ephemeral key, the shared secret into s->k_mpint; 0: fine */
static int agree(struct ssh *s, const struct rd *q_c, uint8_t *q_s, size_t *q_s_len)
{
	mbedtls_ecp_group g;
	mbedtls_ecp_point q, peer;
	mbedtls_mpi d, z;
	uint8_t k[32], nz = 0;
	struct buf kb = { s->k_mpint, 0, sizeof(s->k_mpint), false };
	bool mont = s->kex != K_P256;
	int r;

	mbedtls_ecp_group_init(&g);
	mbedtls_ecp_point_init(&q);
	mbedtls_ecp_point_init(&peer);
	mbedtls_mpi_init(&d);
	mbedtls_mpi_init(&z);
	r = mbedtls_ecp_group_load(&g, mont ? MBEDTLS_ECP_DP_CURVE25519 : MBEDTLS_ECP_DP_SECP256R1);
	if (!r && (size_t)(q_c->end - q_c->p) != (mont ? 32u : 65u))
		r = -1;
	if (!r)
		r = mbedtls_ecp_point_read_binary(&g, &peer, q_c->p, q_c->end - q_c->p);
	if (!r)
		r = mbedtls_ecp_check_pubkey(&g, &peer);
	if (!r)
		r = mbedtls_ecdh_gen_public(&g, &d, &q, rng_get, NULL);
	if (!r)
		r = mbedtls_ecp_point_write_binary(&g, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, q_s_len, q_s, 65);
	if (!r)
		r = mbedtls_ecdh_compute_shared(&g, &z, &peer, &d, rng_get, NULL);
	if (!r)
		r = mont ? mbedtls_mpi_write_binary_le(&z, k, 32) : mbedtls_mpi_write_binary(&z, k, 32);
	for (int i = 0; i < 32 && !r; i++)
		nz |= k[i];
	if (!r && !nz)
		r = -1;
	if (!r)
		w_mpint(&kb, k, 32);
	s->k_len = kb.n;
	mbedtls_platform_zeroize(k, sizeof(k));
	mbedtls_mpi_free(&z);
	mbedtls_mpi_free(&d);
	mbedtls_ecp_point_free(&peer);
	mbedtls_ecp_point_free(&q);
	mbedtls_ecp_group_free(&g);
	return r;
}


static int sign(const uint8_t h[32], struct buf *sig)
{
	mbedtls_ecp_keypair *kp = mbedtls_pk_ec(host_key);
	mbedtls_mpi r, t;
	uint8_t d[32], e[32];
	BUF(b, 80);
	int ret;

	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&t);
	ret = mbedtls_sha256(h, 32, d, 0);
	if (!ret)
		ret = mbedtls_ecdsa_sign(&kp->grp, &r, &t, &kp->d, d, 32, rng_get, NULL);
	if (!ret)
		ret = mbedtls_mpi_write_binary(&r, e, 32);
	if (!ret)
		w_mpint(&b, e, 32);
	if (!ret)
		ret = mbedtls_mpi_write_binary(&t, e, 32);
	if (!ret)
		w_mpint(&b, e, 32);
	w_cstr(sig, hostkey_name);
	w_str(sig, b.p, b.n);
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&t);
	return ret;
}


/* RFC 4253 7.2 */
static void derive(struct ssh *s, char x, uint8_t *out, size_t n)
{
	mbedtls_sha256_context c;
	uint8_t k[32];

	mbedtls_sha256_init(&c);
	mbedtls_sha256_starts(&c, 0);
	mbedtls_sha256_update(&c, s->k_mpint, s->k_len);
	mbedtls_sha256_update(&c, s->h, 32);
	mbedtls_sha256_update(&c, (uint8_t *)&x, 1);
	mbedtls_sha256_update(&c, s->session_id, 32);
	mbedtls_sha256_finish(&c, k);
	mbedtls_sha256_free(&c);
	memcpy(out, k, n);	/* 32 bytes are the most anything here needs */
	mbedtls_platform_zeroize(k, sizeof(k));
}


static void keys_free(struct keys *k)
{
	mbedtls_aes_free(&k->aes);
	mbedtls_gcm_free(&k->gcm);
	mbedtls_platform_zeroize(k, sizeof(*k));
}


static void keys_set(struct ssh *s, struct keys *k, bool c2s, uint8_t cipher, uint8_t mac)
{
	uint32_t seq = k->seq;
	uint8_t key[32];

	keys_free(k);
	mbedtls_aes_init(&k->aes);
	mbedtls_gcm_init(&k->gcm);
	k->seq = s->strict ? 0 : seq;
	k->cipher = cipher;
	k->mac = mac;
	derive(s, c2s ? 'A' : 'B', k->iv, 16);
	derive(s, c2s ? 'C' : 'D', key, 32);
	derive(s, c2s ? 'E' : 'F', k->mac_key, 32);
	if (is_gcm(k) ? mbedtls_gcm_setkey(&k->gcm, MBEDTLS_CIPHER_ID_AES, key, 8 * key_bits_8(cipher))
		      : mbedtls_aes_setkey_enc(&k->aes, key, 8 * key_bits_8(cipher)))
		s->over = true;
	k->on = true;
	mbedtls_platform_zeroize(key, sizeof(key));
}


static void got_kex_init(struct ssh *s, struct rd *r)
{
	struct rd q_c = r_str(r);
	uint8_t q_s[65];
	size_t q_s_len = 0;
	mbedtls_sha256_context c;
	BUF(sig, 160);
	BUF(b, 520);
	const uint8_t newkeys = M_NEWKEYS;

	if (r->bad || agree(s, &q_c, q_s, &q_s_len)) {
		disconnect(s, 3, "key exchange failed");
		return;
	}
	mbedtls_sha256_init(&c);
	mbedtls_sha256_starts(&c, 0);
	hash_str(&c, s->v_c, strlen(s->v_c));
	hash_str(&c, VERSION, strlen(VERSION));
	hash_str(&c, s->i_c, s->i_c_len);
	hash_str(&c, s->i_s, s->i_s_len);
	hash_str(&c, key_blob, key_blob_len);
	hash_str(&c, q_c.p, q_c.end - q_c.p);
	hash_str(&c, q_s, q_s_len);
	mbedtls_sha256_update(&c, s->k_mpint, s->k_len);
	mbedtls_sha256_finish(&c, s->h);
	mbedtls_sha256_free(&c);
	if (!s->first_kex_done)
		memcpy(s->session_id, s->h, 32);
	if (sign(s->h, &sig)) {
		disconnect(s, 3, "signing failed");
		return;
	}
	w8(&b, M_KEX_REPLY);
	w_str(&b, key_blob, key_blob_len);
	w_str(&b, q_s, q_s_len);
	w_str(&b, sig.p, sig.n);
	send_buf(s, &b);
	send_packet(s, &newkeys, 1);
	keys_set(s, &s->tx, false, s->next_tx_c, s->next_tx_m);
	s->wait_newkeys = true;
	free(s->i_c);
	free(s->i_s);
	s->i_c = s->i_s = NULL;
}


static void got_newkeys(struct ssh *s)
{
	if (!s->wait_newkeys) {
		disconnect(s, 2, "NEWKEYS out of order");
		return;
	}
	keys_set(s, &s->rx, true, s->next_rx_c, s->next_rx_m);
	mbedtls_platform_zeroize(s->k_mpint, sizeof(s->k_mpint));
	s->wait_newkeys = s->kex_got = s->kex_sent = false;
	s->first_kex_done = true;
}


/* ---- authentication ---- */

static void auth_failure(struct ssh *s)
{
	BUF(b, 40);

	w8(&b, M_AUTH_FAILURE);
	w_cstr(&b, "password");
	w8(&b, 0);
	send_buf(s, &b);
	if (s->tries >= TRIES)
		disconnect(s, 14, "too many tries");
}


static void got_auth(struct ssh *s, struct rd *r)
{
	struct rd user = r_str(r), service = r_str(r), method = r_str(r);
	char u[33], pw[65];
	size_t n;

	if (r->bad || !s->service || s->delay) {
		disconnect(s, 2, "unexpected authentication request");
		return;
	}
	if (s->authed)
		return;			/* RFC 4252 5.1: ignored */
	if (!is(&service, "ssh-connection")) {
		disconnect(s, 7, "no such service");
		return;
	}
	n = user.end - user.p;
	if (n > sizeof(u) - 1)
		n = sizeof(u) - 1;
	memcpy(u, user.p, n);
	u[n] = 0;
	if (is(&method, "password") && !r8(r)) {
		struct rd p = r_str(r);
		const uint8_t ok = M_AUTH_SUCCESS;
		bool good = false;

		if (!r->bad && (size_t)(p.end - p.p) < sizeof(pw) && !memchr(p.p, 0, p.end - p.p) && !lock_left) {
			memcpy(pw, p.p, p.end - p.p);
			pw[p.end - p.p] = 0;
			good = ssh_password_ok(pw);
			mbedtls_platform_zeroize(pw, sizeof(pw));
		}
		ssh_log(true, good, u, s->peer);
		if (good) {
			failures_ = 0;
			s->authed = true;
			s->idle = IDLE_S;
			send_packet(s, &ok, 1);
			return;
		}
		s->tries++;
		s->delay = 2;
		if (!lock_left && ++failures_ >= LOCK_AFTER) {
			failures_ = 0;
			lock_left = LOCK_S;
		}
		return;
	}
	/* "none" asks what there is; anything else there isn't */
	if (!is(&method, "none") && ++s->tries > 3 * TRIES) {
		disconnect(s, 14, "too many tries");
		return;
	}
	BUF(b, 40);
	w8(&b, M_AUTH_FAILURE);
	w_cstr(&b, "password");
	w8(&b, 0);
	send_buf(s, &b);
}


/* ---- the session ---- */

static bool pend_room(struct ssh *s, size_t n)
{
	size_t used = s->pend_len - s->pend_off;

	if (s->pend_off) {
		memmove(s->pend, s->pend + s->pend_off, used);
		s->pend_len = used;
		s->pend_off = 0;
	}
	if (used + n <= s->pend_max)
		return true;
	if (used + n > 2 * OUTPUT_MAX)
		return false;
	char *p = realloc(s->pend, used + n);
	if (!p)
		return false;
	s->pend = p;
	s->pend_max = used + n;
	return true;
}


static void put(struct ssh *s, const char *t, size_t n)
{
	size_t nl = 0;

	if (s->pty)
		for (size_t i = 0; i < n; i++)
			nl += t[i] == '\n';
	if (!pend_room(s, n + nl))
		return;
	for (size_t i = 0; i < n; i++) {
		if (s->pty && t[i] == '\n')
			s->pend[s->pend_len++] = '\r';
		s->pend[s->pend_len++] = t[i];
	}
}

static void puts_(struct ssh *s, const char *t) { put(s, t, strlen(t)); }


static void prompt(struct ssh *s)
{
	if (!s->pty)
		return;
	puts_(s, ssh_prompt());
	puts_(s, "> ");
}


static void finish(struct ssh *s, uint8_t status)
{
	s->finishing = true;
	s->status = status;
}


/* One command line; its output is queued. Returns its exit status. */
static uint8_t run(struct ssh *s, char *line)
{
	bool ok = true;
	char *out;
	size_t n;

	while (*line == ' ')
		line++;
	if (!*line)
		return 0;
	if (!strcmp(line, "exit") || !strcmp(line, "logout") || !strcmp(line, "quit")) {
		finish(s, 0);
		return 0;
	}
	out = malloc(OUTPUT_MAX);
	if (!out) {
		puts_(s, "out of memory\n");
		return 1;
	}
	n = !strcmp(line, "backup") ? ssh_backup(out, OUTPUT_MAX) : ssh_run(line, out, OUTPUT_MAX, &ok);
	put(s, out, n);
	if (n && out[n - 1] != '\n' && s->shell)
		puts_(s, "\n");
	free(out);
	return !ok;
}


static void line_done(struct ssh *s)
{
	if (s->pty)
		puts_(s, "\n");
	s->line[s->line_len] = 0;
	if (s->line_len) {
		if (s->hist && (!s->hist_n || strcmp(s->hist[s->hist_n - 1], s->line))) {
			if (s->hist_n == HISTORY) {
				memmove(s->hist, s->hist + 1, (HISTORY - 1) * sizeof(s->hist[0]));
				s->hist_n--;
			}
			strcpy(s->hist[s->hist_n++], s->line);
		}
		s->hist_at = s->hist_n;
		run(s, s->line);
	}
	s->line_len = 0;
	if (!s->finishing)
		prompt(s);
}


static void recall(struct ssh *s, int by)
{
	int at = s->hist_at + by;

	if (!s->hist || !s->pty || at < 0 || at > s->hist_n)
		return;
	while (s->line_len) {
		puts_(s, "\b \b");
		s->line_len--;
	}
	s->hist_at = at;
	if (at < s->hist_n) {
		strcpy(s->line, s->hist[at]);
		s->line_len = strlen(s->line);
		puts_(s, s->line);
	}
}


static void typed(struct ssh *s, uint8_t c)
{
	bool cr = s->last_cr;

	s->last_cr = c == '\r';
	if (s->esc == 1) {
		s->esc = c == '[' || c == 'O' ? 2 : 0;
		return;
	}
	if (s->esc == 2) {
		if (c >= 0x40 && c <= 0x7e) {
			s->esc = 0;
			if (c == 'A' || c == 'B')
				recall(s, c == 'A' ? -1 : 1);
		}
		return;
	}
	switch (c) {
	case 0x1b:
		s->esc = 1;
		break;
	case '\n':
		if (cr)
			break;
		/* fall through */
	case '\r':
		line_done(s);
		break;
	case 3:			/* ^C */
		puts_(s, "^C\n");
		s->line_len = 0;
		prompt(s);
		break;
	case 4:			/* ^D */
		if (!s->line_len)
			finish(s, 0);
		break;
	case 8:
	case 0x7f:
		if (s->line_len) {
			s->line_len--;
			if (s->pty)
				puts_(s, "\b \b");
		}
		break;
	case 0x15:		/* ^U */
		while (s->line_len) {
			s->line_len--;
			if (s->pty)
				puts_(s, "\b \b");
		}
		break;
	default:
		if (c < 0x20 || c > 0x7e || s->line_len >= LINE_MAX_)
			break;
		s->line[s->line_len++] = c;
		if (s->pty)
			put(s, (char *)&c, 1);
	}
}


static void restore_done(struct ssh *s)
{
	char msg[160];
	bool ok = ssh_restore(s->file, s->file_len, s->minutes, msg, sizeof(msg));

	puts_(s, msg);
	puts_(s, "\n");
	s->restoring = false;
	finish(s, !ok);
}


static void got_data(struct ssh *s, struct rd *r)
{
	struct rd d;
	size_t n;

	r32(r);
	d = r_str(r);
	n = d.end - d.p;
	if (r->bad || !s->open || n > s->window || s->eof_in) {
		disconnect(s, 2, "data outside the window");
		return;
	}
	s->window -= n;
	s->idle = IDLE_S;
	if (s->restoring) {
		if (s->file_len + n >= RESTORE_MAX) {
			puts_(s, "the file is too long\n");
			s->restoring = false;
			finish(s, 1);
		} else {
			memcpy(s->file + s->file_len, d.p, n);
			s->file_len += n;
		}
	} else if (s->shell) {
		while (d.p < d.end && !s->finishing)
			typed(s, *d.p++);
	}
	if (s->window < WINDOW / 2 && !s->close_sent) {
		BUF(b, 16);

		w8(&b, M_WINDOW_ADJUST);
		w32(&b, s->peer_ch);
		w32(&b, WINDOW - s->window);
		send_buf(s, &b);
		s->window = WINDOW;
	}
}


static void channel_reply(struct ssh *s, bool ok)
{
	BUF(b, 8);

	w8(&b, ok ? M_CHANNEL_SUCCESS : M_CHANNEL_FAILURE);
	w32(&b, s->peer_ch);
	send_buf(s, &b);
}


static void got_channel_request(struct ssh *s, struct rd *r)
{
	struct rd type, c;
	bool want, ok = false;
	char line[LINE_MAX_ + 2];
	bool start_exec = false;

	r32(r);
	type = r_str(r);
	want = r8(r);
	if (r->bad || !s->open) {
		disconnect(s, 2, "malformed request");
		return;
	}
	if (is(&type, "pty-req")) {
		s->pty = ok = !s->shell && !s->exec;
	} else if (is(&type, "env") || is(&type, "window-change") || is(&type, "signal")) {
		ok = !is(&type, "env");
	} else if (is(&type, "shell") && !s->shell && !s->exec) {
		s->shell = ok = true;
		if (s->pty)
			s->hist = calloc(HISTORY, sizeof(s->hist[0]));
		s->line_len = 0;
	} else if (is(&type, "exec") && !s->shell && !s->exec) {
		c = r_str(r);
		if (!r->bad && c.end - c.p <= LINE_MAX_ && !memchr(c.p, 0, c.end - c.p)) {
			memcpy(line, c.p, c.end - c.p);
			line[c.end - c.p] = 0;
			s->exec = ok = start_exec = true;
		}
	}
	if (want)
		channel_reply(s, ok);
	if (start_exec) {
		if (!strncmp(line, "restore", 7) && (!line[7] || line[7] == ' ')) {
			s->minutes = line[7] ? strtoul(line + 8, NULL, 10) : 0;
			s->file = malloc(RESTORE_MAX);
			s->restoring = s->file != NULL;
			if (!s->file)
				finish(s, 1);
		} else {
			uint8_t st = run(s, line);

			finish(s, st);
		}
	} else if (ok && s->shell && is(&type, "shell")) {
		prompt(s);
	}
}


static void got_open(struct ssh *s, struct rd *r)
{
	struct rd type = r_str(r);
	uint32_t ch = r32(r), window = r32(r), max = r32(r);
	BUF(b, 80);

	if (r->bad) {
		disconnect(s, 2, "malformed open");
		return;
	}
	if (!is(&type, "session") || s->open) {
		w8(&b, M_OPEN_FAILURE);
		w32(&b, ch);
		w32(&b, s->open ? 4 : 3);
		w_cstr(&b, s->open ? "one session per connection" : "sessions only");
		w_cstr(&b, "");
		send_buf(s, &b);
		return;
	}
	s->open = true;
	s->peer_ch = ch;
	s->peer_window = window;
	s->peer_max = max;
	s->window = WINDOW;
	w8(&b, M_OPEN_CONFIRM);
	w32(&b, ch);
	w32(&b, 0);
	w32(&b, WINDOW);
	w32(&b, WINDOW);
	send_buf(s, &b);
}


static void message(struct ssh *s, const uint8_t *d, size_t n)
{
	struct rd r = { d + 1, d + n, false };
	uint8_t t = d[0];
	bool kexing = s->kex_got || s->wait_newkeys;

	/* RFC 4253 7.1; under strict KEX the first exchange allows nothing else at all */
	if ((kexing || !s->first_kex_done) && t != M_KEXINIT && t != M_NEWKEYS && t != M_KEX_INIT && t != M_DISCONNECT) {
		if (t > M_DEBUG || (s->strict && !s->first_kex_done)) {
			disconnect(s, 2, "not during key exchange");
			return;
		}
	}
	switch (t) {
	case M_DISCONNECT:
		s->over = true;
		return;
	case M_IGNORE:
	case M_UNIMPLEMENTED:
	case M_DEBUG:
		return;
	case M_KEXINIT:
		got_kexinit(s, d, n);
		return;
	case M_KEX_INIT:
		if (!s->kex_got || !s->kex_sent || s->wait_newkeys)
			disconnect(s, 2, "key exchange out of order");
		else if (s->skip_guess)
			s->skip_guess = false;
		else
			got_kex_init(s, &r);
		return;
	case M_NEWKEYS:
		got_newkeys(s);
		return;
	case M_SERVICE_REQUEST: {
		struct rd name = r_str(&r);
		BUF(b, 40);

		if (!is(&name, "ssh-userauth")) {
			disconnect(s, 7, "no such service");
			return;
		}
		s->service = true;
		w8(&b, M_SERVICE_ACCEPT);
		w_cstr(&b, "ssh-userauth");
		send_buf(s, &b);
		return;
	}
	case M_AUTH_REQUEST:
		got_auth(s, &r);
		return;
	}
	if (!s->authed) {
		disconnect(s, 2, "not authenticated");
		return;
	}
	switch (t) {
	case M_GLOBAL_REQUEST: {
		const uint8_t no = M_REQUEST_FAILURE;

		r_str(&r);
		if (r8(&r) && !r.bad)
			send_packet(s, &no, 1);
		return;
	}
	case M_CHANNEL_OPEN:
		got_open(s, &r);
		return;
	case M_WINDOW_ADJUST: {
		uint32_t add;

		r32(&r);
		add = r32(&r);
		if (!r.bad)
			s->peer_window = s->peer_window + add < add ? 0xffffffff : s->peer_window + add;
		return;
	}
	case M_DATA:
		got_data(s, &r);
		return;
	case M_EXT_DATA:
		return;
	case M_EOF:
		s->eof_in = true;
		if (s->restoring)
			restore_done(s);
		else if (s->shell && !s->finishing)
			finish(s, 0);
		return;
	case M_CLOSE:
		s->close_got = true;
		s->restoring = false;
		if (!s->close_sent)
			finish(s, 0);
		return;
	case M_CHANNEL_REQUEST:
		got_channel_request(s, &r);
		return;
	case M_REQUEST_SUCCESS:
	case M_REQUEST_FAILURE:
	case M_CHANNEL_SUCCESS:
	case M_CHANNEL_FAILURE:
		return;
	default: {
		BUF(b, 8);

		w8(&b, M_UNIMPLEMENTED);
		w32(&b, s->rx.seq - 1);
		send_buf(s, &b);
	}
	}
}


/* ---- out, as window and connection allow ---- */

bool ssh_poll(struct ssh *s)
{
	/* RFC 4253 7.1: nothing of a channel between our KEXINIT and NEWKEYS */
	while (!s->over && s->open && !s->close_sent && !s->kex_sent && !s->kex_got) {
		size_t n = s->pend_len - s->pend_off;

		if (ssh_io_room(s->io) < WIRE(OUT_CHUNK + 32) + 1000)
			break;		/* room stays for what can't wait */
		if (n) {
			BUF(b, OUT_CHUNK + 16);

			if (n > OUT_CHUNK)
				n = OUT_CHUNK;
			if (n > s->peer_window)
				n = s->peer_window;
			if (n > s->peer_max)
				n = s->peer_max;
			if (!n && !s->close_got)
				break;
			if (n) {
				w8(&b, M_DATA);
				w32(&b, s->peer_ch);
				w_str(&b, s->pend + s->pend_off, n);
				send_buf(s, &b);
				s->pend_off += n;
				s->peer_window -= n;
				continue;
			}
		}
		if (!s->finishing)
			break;
		BUF(b, 48);
		w8(&b, M_EOF);
		w32(&b, s->peer_ch);
		send_buf(s, &b);
		b.n = 0;
		w8(&b, M_CHANNEL_REQUEST);
		w32(&b, s->peer_ch);
		w_cstr(&b, "exit-status");
		w8(&b, 0);
		w32(&b, s->status);
		send_buf(s, &b);
		b.n = 0;
		w8(&b, M_CLOSE);
		w32(&b, s->peer_ch);
		send_buf(s, &b);
		s->close_sent = true;
	}
	if (s->close_sent && s->close_got)
		s->over = true;
	return !s->over;
}


/* ---- in ---- */

/* The version line, ours first; RFC 4253 4.2. Returns how much it took. */
static size_t version_in(struct ssh *s, const uint8_t *d, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (d[i] != '\n') {
			if (s->v_len == sizeof(s->v_c) - 1) {
				s->over = true;
				return n;
			}
			s->v_c[s->v_len++] = d[i];
			continue;
		}
		if (s->v_len && s->v_c[s->v_len - 1] == '\r')
			s->v_len--;
		s->v_c[s->v_len] = 0;
		if (strncmp(s->v_c, "SSH-2.0-", 8) || strlen(s->v_c) != s->v_len) {
			s->over = true;
			return n;
		}
		s->v_done = true;
		send_kexinit(s);
		return i + 1;
	}
	return n;
}


/*
 * A packet in s->in, if one is complete: 1 taken, 0 more needed, -1 broken.
 * The length of an encrypted one is decrypted first, in place.
 */
static int packet_in(struct ssh *s)
{
	struct keys *k = &s->rx;
	size_t block = k->on ? 16 : 8, tag, aad;
	uint32_t len;
	uint8_t m[32];

	tag = !k->on ? 0 : is_gcm(k) ? 16 : 32;
	aad = k->on && (is_gcm(k) || k->mac == H_SHA256_ETM) ? 4 : 0;
	if (!s->pkt_len) {
		if (s->in_len < (aad ? 4 : block))
			return 0;
		if (k->on && !aad && mbedtls_aes_crypt_ctr(&k->aes, block, &k->nc_off, k->iv, k->stream, s->in, s->in))
			return -1;
		len = (uint32_t)s->in[0] << 24 | s->in[1] << 16 | s->in[2] << 8 | s->in[3];
		if (len < 6 || len > IN_MAX - 4 - tag || (len + 4 - aad) % block)
			return -1;
		s->pkt_len = len;
	}
	len = s->pkt_len;
	if (s->in_len < 4 + len + tag)
		return 0;
	if (k->on && is_gcm(k)) {
		if (mbedtls_gcm_auth_decrypt(&k->gcm, len, k->iv, 12, s->in, 4, s->in + 4 + len, 16, s->in + 4, s->in + 4))
			return -1;
		gcm_iv_next(k);
	} else if (k->on && aad) {
		if (mac_of(k, NULL, 0, s->in, 4 + len, m) || mbedtls_ct_memcmp(m, s->in + 4 + len, 32))
			return -1;
		if (mbedtls_aes_crypt_ctr(&k->aes, len, &k->nc_off, k->iv, k->stream, s->in + 4, s->in + 4))
			return -1;
	} else if (k->on) {
		if (mbedtls_aes_crypt_ctr(&k->aes, 4 + len - block, &k->nc_off, k->iv, k->stream, s->in + block, s->in + block))
			return -1;
		if (mac_of(k, NULL, 0, s->in, 4 + len, m) || mbedtls_ct_memcmp(m, s->in + 4 + len, 32))
			return -1;
	}
	k->seq++;
	uint8_t pad = s->in[4];
	if (pad < 4 || pad + 2u > len)
		return -1;
	uint8_t *payload = s->in + 5;
	size_t n = len - 1 - pad, whole = 4 + len + tag;
	message(s, payload, n);
	memmove(s->in, s->in + whole, s->in_len - whole);
	s->in_len -= whole;
	s->pkt_len = 0;
	return 1;
}


bool ssh_input(struct ssh *s, const uint8_t *d, size_t n)
{
	while (n && !s->over) {
		size_t k;

		if (!s->v_done) {
			k = version_in(s, d, n);
			d += k;
			n -= k;
			continue;
		}
		k = IN_MAX - s->in_len;
		if (k > n)
			k = n;
		memcpy(s->in + s->in_len, d, k);
		s->in_len += k;
		d += k;
		n -= k;
		int r;
		while (!s->over && (r = packet_in(s)) > 0)
			;
		if (!s->over && r < 0)
			disconnect(s, 2, "broken packet");
	}
	return ssh_poll(s);
}


struct ssh *ssh_new(void *io, const char *peer)
{
	struct ssh *s = calloc(1, sizeof(*s));

	if (!s || !have_key) {
		free(s);
		return NULL;
	}
	s->io = io;
	s->login_left = LOGIN_S;
	snprintf(s->peer, sizeof(s->peer), "%s", peer);
	mbedtls_aes_init(&s->rx.aes);
	mbedtls_gcm_init(&s->rx.gcm);
	mbedtls_aes_init(&s->tx.aes);
	mbedtls_gcm_init(&s->tx.gcm);
	ssh_io_write(io, (const uint8_t *)VERSION "\r\n", strlen(VERSION) + 2);
	return s;
}


void ssh_free(struct ssh *s)
{
	if (!s)
		return;
	keys_free(&s->rx);
	keys_free(&s->tx);
	free(s->i_c);
	free(s->i_s);
	free(s->hist);
	if (s->pend)
		mbedtls_platform_zeroize(s->pend, s->pend_max);
	free(s->pend);
	if (s->file)
		mbedtls_platform_zeroize(s->file, RESTORE_MAX);
	free(s->file);
	mbedtls_platform_zeroize(s, sizeof(*s));
	free(s);
}


bool ssh_second(struct ssh *s)
{
	if (s->delay && !--s->delay)
		auth_failure(s);
	if (!s->authed && s->login_left && !--s->login_left)
		disconnect(s, 11, "login timed out");
	if (s->authed && s->idle && !--s->idle)
		disconnect(s, 11, "idle too long");
	return ssh_poll(s);
}


/* ---- the host key ---- */

bool ssh_host_key_set(const uint8_t *der, size_t len)
{
	struct buf b = { key_blob, 0, sizeof(key_blob), false };
	mbedtls_ecp_keypair *kp;
	uint8_t q[65];
	size_t n;

	if (have_key)
		mbedtls_pk_free(&host_key);
	have_key = false;
	mbedtls_pk_init(&host_key);
	if (mbedtls_pk_parse_key(&host_key, der, len, NULL, 0, rng_get, NULL) || !mbedtls_pk_can_do(&host_key, MBEDTLS_PK_ECDSA))
		goto bad;
	kp = mbedtls_pk_ec(host_key);
	if (kp->grp.id != MBEDTLS_ECP_DP_SECP256R1
	    || mbedtls_ecp_point_write_binary(&kp->grp, &kp->Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, q, sizeof(q)))
		goto bad;
	w_cstr(&b, hostkey_name);
	w_cstr(&b, "nistp256");
	w_str(&b, q, n);
	key_blob_len = b.n;
	have_key = !b.bad;
	return have_key;
bad:
	mbedtls_pk_free(&host_key);
	return false;
}


bool ssh_host_key_make(uint8_t *der, size_t *len)
{
	mbedtls_pk_context k;
	uint8_t b[200];
	int n = -1;

	mbedtls_pk_init(&k);
	if (!mbedtls_pk_setup(&k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))
	    && !mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(k), rng_get, NULL))
		n = mbedtls_pk_write_key_der(&k, b, sizeof(b));	/* at the end of b */
	mbedtls_pk_free(&k);
	if (n <= 0 || (size_t)n > *len) {
		mbedtls_platform_zeroize(b, sizeof(b));
		return false;
	}
	memcpy(der, b + sizeof(b) - n, n);
	*len = n;
	mbedtls_platform_zeroize(b, sizeof(b));
	return true;
}


bool ssh_fingerprint(char *buf, size_t max)
{
	uint8_t h[32];
	unsigned char b[48];
	size_t n;

	if (!have_key || mbedtls_sha256(key_blob, key_blob_len, h, 0) || mbedtls_base64_encode(b, sizeof(b), &n, h, 32))
		return false;
	while (n && b[n - 1] == '=')
		n--;
	snprintf(buf, max, "SHA256:%.*s", (int)n, b);
	return true;
}


void ssh_lock_second(void)
{
	static uint8_t calm;

	if (lock_left)
		lock_left--;
	/* a wrong password now and then doesn't add up to an attack */
	if (failures_ && ++calm >= 60) {
		failures_--;
		calm = 0;
	}
}
