/* SNMPv3's user-based security and the notifications, see usm.h */

#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#include "usm.h"

#define T_INT		0x02
#define T_OCTETS	0x04
#define T_NULL		0x05
#define T_OID		0x06
#define T_SEQ		0x30
#define T_COUNTER	0x41
#define T_TICKS		0x43
#define T_GET		0xa0
#define T_GETBULK	0xa5
#define T_REPORT	0xa8
#define T_TRAP2		0xa7

#define F_AUTH		1
#define F_PRIV		2
#define F_REPORT	4
#define WINDOW_S	150
#define SCOPED_OVERHEAD	64

struct user {
	char name[USM_NAME_MAX + 1];	/* empty: free */
	enum usm_auth auth;
	bool priv;
	uint8_t akey[32], pkey[32];
	uint8_t from[32];		/* a hash of the passwords, to know them again */
};

static struct user users[USM_USERS];
static uint8_t engine[11];
static uint32_t boots;
static uint32_t stats[6];
static uint64_t salt;

/* The usmStats counters, 1.3.6.1.6.3.15.1.1.<n>.0 */
enum { S_LEVEL = 1, S_WINDOW, S_USER, S_ENGINE, S_DIGEST, S_DECRYPT };

/* ---- reading BER ---- */

struct in { const uint8_t *p, *end; };

static bool get(struct in *s, uint8_t tag, struct in *v)
{
	size_t n;

	if (s->end - s->p < 2 || *s->p != tag)
		return false;
	n = s->p[1];
	s->p += 2;
	if (n & 0x80) {
		uint8_t k = n & 0x7f;
		if (!k || k > 3 || s->end - s->p < k)
			return false;
		for (n = 0; k; k--)
			n = n << 8 | *s->p++;
	}
	if (n > (size_t)(s->end - s->p))
		return false;
	v->p = s->p;
	v->end = s->p += n;
	return true;
}


static bool get_int(struct in *s, uint32_t *v)
{
	struct in i;

	if (!get(s, T_INT, &i) || i.p == i.end || (*i.p & 0x80))
		return false;		/* none here is negative */
	if (i.end - i.p > 5 || (i.end - i.p == 5 && *i.p))
		return false;
	for (*v = 0; i.p < i.end; i.p++)
		*v = *v << 8 | *i.p;
	return true;
}


/* ---- writing BER, from the back ---- */

struct out { uint8_t *start, *p, *end; bool full; };

static void put(struct out *o, const void *d, size_t n)
{
	if ((size_t)(o->p - o->start) < n) {
		o->full = true;
		return;
	}
	o->p -= n;
	memcpy(o->p, d, n);
}


/* The tag and length of what was written since mark */
static void put_head(struct out *o, uint8_t tag, const uint8_t *mark)
{
	size_t n = mark - o->p;
	uint8_t h[4] = { tag, 0x82, n >> 8, n };

	if (n < 128) {
		h[1] = n;
		put(o, h, 2);
	} else {
		put(o, h, 4);
	}
}


static void put_str(struct out *o, uint8_t tag, const void *d, size_t n)
{
	uint8_t *mark = o->p;

	put(o, d, n);
	put_head(o, tag, mark);
}


static void put_uint(struct out *o, uint8_t tag, uint32_t v)
{
	uint8_t *mark = o->p, b;

	do {
		b = v;
		put(o, &b, 1);
		v >>= 8;
	} while (v);
	if (b & 0x80)
		put(o, "", 1);
	put_head(o, tag, mark);
}


static void put_oid(struct out *o, const uint32_t *oid, size_t n)
{
	uint8_t *mark = o->p, b;

	while (n-- > 2) {
		uint32_t v = oid[n];
		b = v & 0x7f;
		put(o, &b, 1);
		while (v >>= 7) {
			b = 0x80 | (v & 0x7f);
			put(o, &b, 1);
		}
	}
	b = oid[0] * 40 + oid[1];
	put(o, &b, 1);
	put_head(o, T_OID, mark);
}


/* ---- users and keys ---- */

static const mbedtls_md_info_t *md_of(enum usm_auth a)
{
	return mbedtls_md_info_from_type(a == USM_SHA256 ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA1);
}


static uint8_t mac_len(enum usm_auth a)
{
	return a == USM_SHA256 ? 24 : 12;
}


/* RFC 3414 A.2: a megabyte of the password, then with the engine ID around */
static bool localize(enum usm_auth a, const char *pw, uint8_t key[32])
{
	mbedtls_md_context_t c;
	size_t n = strlen(pw), at = 0, hl = mbedtls_md_get_size(md_of(a));
	uint8_t block[64];
	bool ok;

	mbedtls_md_init(&c);
	ok = !mbedtls_md_setup(&c, md_of(a), 0) && !mbedtls_md_starts(&c);
	for (uint32_t i = 0; ok && i < 1048576; i += 64) {
		for (int k = 0; k < 64; k++)
			block[k] = pw[at++ % n];
		ok = !mbedtls_md_update(&c, block, 64);
	}
	ok = ok && !mbedtls_md_finish(&c, key) && !mbedtls_md_starts(&c) && !mbedtls_md_update(&c, key, hl)
		&& !mbedtls_md_update(&c, engine, sizeof(engine)) && !mbedtls_md_update(&c, key, hl)
		&& !mbedtls_md_finish(&c, key);
	mbedtls_md_free(&c);
	mbedtls_platform_zeroize(block, sizeof(block));
	return ok;
}


void usm_engine(const uint8_t mac[6], uint32_t b)
{
	/* RFC 3411: lwIP's enterprise number, format 3, the MAC */
	static const uint8_t head[5] = { 0x80, 0x00, 0x67, 0x0d, 0x03 };

	memcpy(engine, head, 5);
	memcpy(engine + 5, mac, 6);
	boots = b;
	usm_random((uint8_t *)&salt, sizeof(salt));
}


const uint8_t *usm_engine_id(uint8_t *len)
{
	*len = sizeof(engine);
	return engine;
}


static struct user *find(const uint8_t *name, size_t n)
{
	for (int i = 0; i < USM_USERS; i++)
		if (n && strlen(users[i].name) == n && !memcmp(users[i].name, name, n))
			return &users[i];
	return NULL;
}


bool usm_user_set(const char *name, enum usm_auth auth, const char *auth_pw, const char *priv_pw)
{
	struct user u, *slot;

	if (!*name || strlen(name) > USM_NAME_MAX || strlen(auth_pw) < 8 || strlen(auth_pw) > 64
	    || (priv_pw && strlen(priv_pw) > 64) || (priv_pw && strlen(priv_pw) < 8))
		return false;
	slot = find((const uint8_t *)name, strlen(name));
	for (int i = 0; !slot && i < USM_USERS; i++)
		if (!users[i].name[0])
			slot = &users[i];
	if (!slot)
		return false;
	memset(&u, 0, sizeof(u));
	/* A configuration that runs again need not wait for the keys again */
	{
		char all[200];
		size_t n = 0;

		n += strlen(strcpy(all, auth == USM_SHA256 ? "2" : "1")) + 1;
		n += strlen(strcpy(all + n, auth_pw)) + 1;
		if (priv_pw)
			n += strlen(strcpy(all + n, priv_pw)) + 1;
		mbedtls_sha256((uint8_t *)all, n, u.from, 0);
		mbedtls_platform_zeroize(all, sizeof(all));
	}
	if (slot->name[0] && !memcmp(slot->from, u.from, sizeof(u.from)))
		return true;
	strcpy(u.name, name);
	u.auth = auth;
	u.priv = priv_pw != NULL;
	if (!localize(auth, auth_pw, u.akey) || (priv_pw && !localize(auth, priv_pw, u.pkey)))
		return false;
	*slot = u;
	mbedtls_platform_zeroize(&u, sizeof(u));
	return true;
}


bool usm_user_del(const char *name)
{
	struct user *u = find((const uint8_t *)name, strlen(name));

	if (u)
		mbedtls_platform_zeroize(u, sizeof(*u));
	return u != NULL;
}


void usm_users_clear(void)
{
	mbedtls_platform_zeroize(users, sizeof(users));
}


const char *usm_user(int i, enum usm_auth *auth, bool *priv)
{
	for (int k = 0; k < USM_USERS; k++)
		if (users[k].name[0] && !i--) {
			*auth = users[k].auth;
			*priv = users[k].priv;
			return users[k].name;
		}
	return NULL;
}


/* ---- messages ---- */

static bool crypt_cfb(const struct user *u, int mode, uint32_t b, uint32_t t, const uint8_t *priv, uint8_t *d, size_t n)
{
	mbedtls_aes_context c;
	uint8_t iv[16] = { b >> 24, b >> 16, b >> 8, b, t >> 24, t >> 16, t >> 8, t };
	size_t off = 0;
	int r;

	memcpy(iv + 8, priv, 8);
	mbedtls_aes_init(&c);
	r = mbedtls_aes_setkey_enc(&c, u->pkey, 128);
	if (!r)
		r = mbedtls_aes_crypt_cfb128(&c, mode, n, &off, iv, d, d);
	mbedtls_aes_free(&c);
	return !r;
}


/* A whole v3 message around what o holds: a PDU */
static size_t wrap(struct out *o, const uint8_t *mark, uint32_t msg_id, uint8_t flags, const struct user *u,
		   const void *uname, size_t ulen, const uint8_t *ctx, size_t ctx_len)
{
	uint32_t now = usm_time();
	uint8_t priv[8], digest[32], *auth_at = NULL, *m;
	const uint8_t zero[24] = { 0 };

	put_str(o, T_OCTETS, ctx, ctx_len);
	put_str(o, T_OCTETS, engine, sizeof(engine));
	put_head(o, T_SEQ, mark);
	if (flags & F_PRIV) {
		if (o->full)
			return 0;
		salt++;
		memcpy(priv, &salt, 8);
		if (!crypt_cfb(u, MBEDTLS_AES_ENCRYPT, boots, now, priv, o->p, mark - o->p))
			return 0;
		put_head(o, T_OCTETS, mark);
	}

	/* msgSecurityParameters */
	m = o->p;
	put_str(o, T_OCTETS, priv, flags & F_PRIV ? 8 : 0);
	if (flags & F_AUTH) {
		uint8_t *a = o->p;
		put(o, zero, mac_len(u->auth));
		auth_at = o->p;
		put_head(o, T_OCTETS, a);
	} else {
		put_str(o, T_OCTETS, "", 0);
	}
	put_str(o, T_OCTETS, uname, ulen);
	put_uint(o, T_INT, now);
	put_uint(o, T_INT, boots);
	put_str(o, T_OCTETS, engine, sizeof(engine));
	put_head(o, T_SEQ, m);
	put_head(o, T_OCTETS, m);

	/* msgGlobalData */
	m = o->p;
	put_uint(o, T_INT, 3);
	put_str(o, T_OCTETS, &flags, 1);
	put_uint(o, T_INT, USM_MSG_MAX);
	put_uint(o, T_INT, msg_id);
	put_head(o, T_SEQ, m);
	put_uint(o, T_INT, 3);
	put_head(o, T_SEQ, mark);
	if (o->full)
		return 0;
	if (flags & F_AUTH) {
		if (mbedtls_md_hmac(md_of(u->auth), u->akey, mbedtls_md_get_size(md_of(u->auth)), o->p, mark - o->p, digest))
			return 0;
		memcpy(auth_at, digest, mac_len(u->auth));
	}
	return mark - o->p;
}


static size_t finish(struct out *o, size_t n, uint8_t *out)
{
	if (n)
		memmove(out, o->p, n);
	return n;
}


static size_t report(uint8_t *out, size_t max, uint8_t stat, uint32_t msg_id, uint32_t req_id, uint8_t in_flags,
		     const struct user *u, const struct in *uname)
{
	const uint32_t oid[] = { 1, 3, 6, 1, 6, 3, 15, 1, 1, stat, 0 };
	struct out o = { out, out + max, out + max, false };
	uint8_t *mark = o.p;

	stats[stat - 1]++;
	if (!(in_flags & F_REPORT))
		return 0;
	put_uint(&o, T_COUNTER, stats[stat - 1]);
	put_oid(&o, oid, 11);
	put_head(&o, T_SEQ, mark);
	put_head(&o, T_SEQ, mark);
	put_uint(&o, T_INT, 0);
	put_uint(&o, T_INT, 0);
	put_uint(&o, T_INT, req_id);
	put_head(&o, T_REPORT, mark);
	/* signed only where the time is what it is about (RFC 3414 3.2) */
	return finish(&o, wrap(&o, mark, msg_id, stat == S_WINDOW ? F_AUTH : 0, u, uname->p, uname->end - uname->p,
			       (const uint8_t *)"", 0), out);
}


size_t usm_receive(uint8_t *msg, size_t len, uint8_t *out, size_t max)
{
	static uint8_t v2[USM_MSG_MAX], reply[USM_MSG_MAX];
	struct in m = { msg, msg + len }, s, h, flags, sp, q, eng, uname, auth, priv, scoped, ctx_eng, ctx, pdu;
	uint32_t version, msg_id, size, model, b, t, req_id = 0;
	uint8_t f, digest[32], got[24];
	struct user *u;
	size_t n;

	if (!get(&m, T_SEQ, &s) || !get_int(&s, &version) || version != 3
	    || !get(&s, T_SEQ, &h) || !get_int(&h, &msg_id) || !get_int(&h, &size) || size < 484
	    || !get(&h, T_OCTETS, &flags) || flags.end - flags.p != 1 || !get_int(&h, &model) || model != 3
	    || !get(&s, T_OCTETS, &sp) || !get(&sp, T_SEQ, &q) || !get(&q, T_OCTETS, &eng) || !get_int(&q, &b)
	    || !get_int(&q, &t) || !get(&q, T_OCTETS, &uname) || !get(&q, T_OCTETS, &auth) || !get(&q, T_OCTETS, &priv)
	    || uname.end - uname.p > USM_NAME_MAX)
		return 0;
	f = *flags.p;
	if ((f & (F_AUTH | F_PRIV)) == F_PRIV)
		return 0;
	len = s.end - msg;	/* what follows the message isn't signed */

	if (eng.end - eng.p != sizeof(engine) || memcmp(eng.p, engine, sizeof(engine))) {
		/* discovery: echo the request's id where it can be read */
		if (!(f & F_PRIV) && get(&s, T_SEQ, &scoped) && get(&scoped, T_OCTETS, &ctx_eng)
		    && get(&scoped, T_OCTETS, &ctx) && scoped.p < scoped.end && get(&scoped, *scoped.p, &pdu))
			get_int(&pdu, &req_id);
		return report(out, max, S_ENGINE, msg_id, req_id, f, NULL, &uname);
	}
	u = find(uname.p, uname.end - uname.p);
	if (!u)
		return report(out, max, S_USER, msg_id, 0, f, NULL, &uname);
	if (!(f & F_AUTH) || (u->priv && !(f & F_PRIV)) || (!u->priv && (f & F_PRIV)))
		return report(out, max, S_LEVEL, msg_id, 0, f, NULL, &uname);

	n = mac_len(u->auth);
	if ((size_t)(auth.end - auth.p) != n)
		return report(out, max, S_DIGEST, msg_id, 0, f, NULL, &uname);
	memcpy(got, auth.p, n);
	memset(msg + (auth.p - msg), 0, n);
	if (mbedtls_md_hmac(md_of(u->auth), u->akey, mbedtls_md_get_size(md_of(u->auth)), msg, len, digest)
	    || mbedtls_ct_memcmp(digest, got, n))
		return report(out, max, S_DIGEST, msg_id, 0, f, NULL, &uname);
	if (b != boots || t > usm_time() + WINDOW_S || t + WINDOW_S < usm_time())
		return report(out, max, S_WINDOW, msg_id, 0, f, u, &uname);

	if (f & F_PRIV) {
		if (!get(&s, T_OCTETS, &scoped) || priv.end - priv.p != 8
		    || !crypt_cfb(u, MBEDTLS_AES_DECRYPT, b, t, priv.p, msg + (scoped.p - msg), scoped.end - scoped.p))
			return report(out, max, S_DECRYPT, msg_id, 0, f, NULL, &uname);
		s = scoped;
	}
	if (!get(&s, T_SEQ, &scoped) || !get(&scoped, T_OCTETS, &ctx_eng) || !get(&scoped, T_OCTETS, &ctx)
	    || ctx.end - ctx.p > 32 || scoped.p == scoped.end)
		return f & F_PRIV ? report(out, max, S_DECRYPT, msg_id, 0, f, NULL, &uname) : 0;
	/* requests only: a notification is none of the agent's business */
	if (*scoped.p < T_GET || *scoped.p > T_GETBULK || *scoped.p == 0xa2)
		return 0;

	/* to the agent as v2c, and back */
	struct out o = { v2, v2 + sizeof(v2), v2 + sizeof(v2), false };
	uint8_t *mark = o.p;
	struct in pdu_in = { scoped.p, scoped.end };

	if (!get(&pdu_in, *scoped.p, &pdu))
		return 0;
	put(&o, scoped.p, pdu.end - scoped.p);
	put_str(&o, T_OCTETS, usm_community(), strlen(usm_community()));
	put_uint(&o, T_INT, 1);
	put_head(&o, T_SEQ, mark);
	if (o.full)
		return 0;
	n = usm_agent(o.p, mark - o.p, reply, sizeof(reply));
	struct in r = { reply, reply + n }, body, community;
	if (!n || !get(&r, T_SEQ, &body) || !get_int(&body, &version) || !get(&body, T_OCTETS, &community)
	    || body.p == body.end)
		return 0;
	o = (struct out){ out, out + max, out + max, false };
	mark = o.p;
	put(&o, body.p, body.end - body.p);
	return finish(&o, wrap(&o, mark, msg_id, f & (F_AUTH | F_PRIV), u, uname.p, uname.end - uname.p, ctx.p,
			       ctx.end - ctx.p), out);
}


/* ---- notifications ---- */

static void put_bind(struct out *o, const uint32_t *oid, size_t n, const uint8_t *mark)
{
	put_oid(o, oid, n);
	put_head(o, T_SEQ, mark);
}


size_t usm_trap(const struct usm_trap *t, uint8_t *out, size_t max)
{
	static const uint32_t sys_uptime[] = { 1, 3, 6, 1, 2, 1, 1, 3, 0 };
	static const uint32_t trap_oid[] = { 1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0 };
	static const uint32_t text_oid[] = { 1, 3, 6, 1, 4, 1, 8072, 9999, 9999, 837, 1, 0 };
	uint32_t if_oid[] = { 1, 3, 6, 1, 2, 1, 2, 2, 1, 0, t->port };
	static uint32_t id;
	struct out o = { out, out + max, out + max, false };
	uint8_t *mark = o.p, *m;

	if (t->text) {
		m = o.p;
		put_str(&o, T_OCTETS, t->text, strlen(t->text));
		put_bind(&o, text_oid, 12, m);
	}
	if (t->port) {
		const int val[] = { t->port, t->if_admin, t->if_oper };
		const uint8_t col[] = { 1, 7, 8 };	/* ifIndex, ifAdminStatus, ifOperStatus */
		for (int i = 2; i >= 0; i--) {
			if (!val[i])
				continue;
			m = o.p;
			put_uint(&o, T_INT, val[i]);
			if_oid[9] = col[i];
			put_bind(&o, if_oid, 11, m);
		}
	}
	m = o.p;
	put_oid(&o, t->oid, t->oid_len);
	put_bind(&o, trap_oid, 11, m);
	m = o.p;
	put_uint(&o, T_TICKS, usm_uptime_cs());
	put_bind(&o, sys_uptime, 9, m);
	put_head(&o, T_SEQ, mark);
	put_uint(&o, T_INT, 0);
	put_uint(&o, T_INT, 0);
	if (!id)
		usm_random((uint8_t *)&id, sizeof(id));
	id = (id + 1) & 0x7fffffff;
	put_uint(&o, T_INT, id);
	put_head(&o, T_TRAP2, mark);
	if (t->user) {
		const struct user *u = find((const uint8_t *)t->user, strlen(t->user));

		if (!u)
			return 0;
		return finish(&o, wrap(&o, mark, id, u->priv ? F_AUTH | F_PRIV : F_AUTH, u, u->name, strlen(u->name),
				       (const uint8_t *)"", 0), out);
	}
	put_str(&o, T_OCTETS, t->community, strlen(t->community));
	put_uint(&o, T_INT, 1);
	put_head(&o, T_SEQ, mark);
	return finish(&o, o.full ? 0 : mark - o.p, out);
}
