/* usm.c under AddressSanitizer: RFC 3414's and 7860's key vectors, what it refuses, and mutated messages */
#include "../src/usm.c"

#include <stdio.h>
#include <stdlib.h>

static int failures;
#define check(c, ...) do { bool ok_ = (c); printf("%s  ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); failures += !ok_; } while (0)

static uint32_t now = 1000;
static int asked;
static uint8_t last[USM_MSG_MAX];
static size_t last_len;
const char *usm_community(void) { return "inner"; }
uint32_t usm_time(void) { return now; }
uint32_t usm_uptime_cs(void) { return now * 100; }
void usm_random(uint8_t *p, size_t n) { while (n--) *p++ = random(); }
size_t usm_agent(const uint8_t *msg, size_t len, uint8_t *out, size_t max)
{
	asked++;
	memcpy(last, msg, len);
	last_len = len;
	if (len > max) return 0;
	memcpy(out, msg, len);		/* the request as its own answer */
	return len;
}

/* A v3 request as a manager would send it, through our own writer */
static size_t request(uint8_t *out, size_t max, const struct user *u, uint8_t flags, const uint8_t *pdu, size_t n)
{
	struct out o = { out, out + max, out + max, false };
	uint8_t *mark = o.p;

	put(&o, pdu, n);
	return finish(&o, wrap(&o, mark, 77, flags | F_REPORT, u, u->name, strlen(u->name), (uint8_t *)"", 0), out);
}

static uint8_t in[USM_MSG_MAX], out[USM_MSG_MAX];
static size_t rx(size_t n) { uint8_t *m = malloc(n ? n : 1); memcpy(m, in, n); size_t r = usm_receive(m, n, out, sizeof(out)); free(m); return r; }
/* The report's usmStats number, 0: no report */
static int rep(size_t n)
{
	for (size_t i = 0; i + 10 < n; i++)
		if (!memcmp(out + i, "\x2b\x06\x01\x06\x03\x0f\x01\x01", 8))
			return out[i + 8];
	return 0;
}

int main(void)
{
	static const uint8_t pdu[] = { 0xa0, 0x1a, 2, 2, 1, 5, 2, 1, 0, 2, 1, 0, 0x30, 0x0e, 0x30, 0x0c, 6, 8, 0x2b, 6, 1, 2, 1, 1, 5, 0, 5, 0 };
	uint8_t key[32], eng[12] = { [11] = 2 };
	mbedtls_md_context_t c;

	/* RFC 3414 A.3.2: SHA-1, "maplesyrup", engine 00..02 (12 octets): hash the engine by hand, as ours has 11 */
	mbedtls_md_init(&c);
	uint8_t k1[32];
	mbedtls_md_setup(&c, md_of(USM_SHA1), 0);
	mbedtls_md_starts(&c);
	for (int i = 0; i < 1048576; i++) mbedtls_md_update(&c, (uint8_t *)"maplesyrup" + i % 10, 1);
	mbedtls_md_finish(&c, k1);
	memset(engine, 0, 11);
	localize(USM_SHA1, "maplesyrup", key);
	uint8_t want[32];
	mbedtls_md_starts(&c); mbedtls_md_update(&c, k1, 20); mbedtls_md_update(&c, engine, 11); mbedtls_md_update(&c, k1, 20); mbedtls_md_finish(&c, want);
	check(!memcmp(key, want, 20), "a key is localized with the engine ID");
	mbedtls_md_starts(&c); mbedtls_md_update(&c, k1, 20); mbedtls_md_update(&c, eng, 12); mbedtls_md_update(&c, k1, 20); mbedtls_md_finish(&c, want);
	check(!memcmp(want, "\x66\x95\xfe\xbc\x92\x88\xe3\x62\x82\x23\x5f\xc7\x15\x1f\x12\x84\x97\xb3\x8f\x3f", 20), "RFC 3414 A.3.2's SHA-1 vector");
	mbedtls_md_free(&c);

	usm_engine((const uint8_t *)"\x06\x0f\x00\xdd\x3b\x3e", 5);
	check(usm_user_set("alice", USM_SHA256, "authpassword", "privpassword") && usm_user_set("bob", USM_SHA1, "authpassword", NULL), "two users");
	check(!usm_user_set("x", USM_SHA1, "1234567", NULL) && !usm_user_set("x", USM_SHA1, "12345678", "1234567") && !usm_user_set("", USM_SHA1, "12345678", NULL), "short passwords and no name are refused");
	struct user *alice = find((uint8_t *)"alice", 5), *bob = find((uint8_t *)"bob", 3);
	size_t n = request(in, sizeof(in), alice, F_AUTH | F_PRIV, pdu, sizeof(pdu)), r;
	r = rx(n);
	check(r && asked == 1 && !rep(r) && !memcmp(last, "\x30\x26\x02\x01\x01\x04\x05inner", 12), "a request reaches the agent as v2c, its answer comes back");
	for (size_t i = 0; i + sizeof(pdu) <= r; i++)
		if (!memcmp(out + i, pdu + 2, 10)) { check(false, "the answer is encrypted"); break; }
	in[n - 1] ^= 1; r = rx(n); in[n - 1] ^= 1;
	check(asked == 1 && rep(r) == S_DIGEST, "a flipped bit: wrongDigests");
	now += 200; r = rx(n);
	check(asked == 1 && rep(r) == S_WINDOW, "replayed 200 s later: notInTimeWindows");
	now -= 200; boots++; r = rx(n); boots--;
	check(asked == 1 && rep(r) == S_WINDOW, "replayed after a restart: notInTimeWindows");
	n = request(in, sizeof(in), alice, F_AUTH, pdu, sizeof(pdu)); r = rx(n);
	check(asked == 1 && rep(r) == S_LEVEL, "unencrypted by a user who encrypts: refused");
	n = request(in, sizeof(in), alice, 0, pdu, sizeof(pdu)); r = rx(n);
	check(asked == 1 && rep(r) == S_LEVEL, "unauthenticated: refused");
	n = request(in, sizeof(in), bob, 0, pdu, sizeof(pdu)); r = rx(n);
	check(asked == 1 && rep(r) == S_LEVEL, "also by one who only authenticates");
	n = request(in, sizeof(in), bob, F_AUTH, pdu, sizeof(pdu)); r = rx(n);
	check(asked == 2 && r && !rep(r), "authNoPriv for that one");
	struct user eve = *bob; strcpy(eve.name, "eve");
	n = request(in, sizeof(in), &eve, F_AUTH, pdu, sizeof(pdu)); r = rx(n);
	check(asked == 2 && rep(r) == S_USER, "an unknown user");
	eve = *alice; eve.akey[0] ^= 1;
	n = request(in, sizeof(in), &eve, F_AUTH | F_PRIV, pdu, sizeof(pdu)); r = rx(n);
	check(asked == 2 && rep(r) == S_DIGEST, "the wrong key");
	engine[10] ^= 1; n = request(in, sizeof(in), alice, 0, pdu, sizeof(pdu)); engine[10] ^= 1; r = rx(n);
	check(asked == 2 && rep(r) == S_ENGINE, "another engine: unknownEngineIDs, which is how managers learn ours");
	uint8_t resp[sizeof(pdu)]; memcpy(resp, pdu, sizeof(pdu)); resp[0] = 0xa2;
	n = request(in, sizeof(in), alice, F_AUTH | F_PRIV, resp, sizeof(resp)); r = rx(n);
	check(asked == 2 && r == 0, "a response is no request");
	check(usm_user_del("bob") && !usm_user_del("bob") && !find((uint8_t *)"bob", 3) && !find((uint8_t *)"", 0), "a user is removed");
	for (int i = 0; i < 3; i++) { char nm[4] = { 'u', '0' + i }; usm_user_set(nm, USM_SHA1, "12345678", NULL); }
	check(!usm_user_set("five", USM_SHA1, "12345678", NULL), "the fifth user doesn't fit");

	srandom(3);
	for (int i = 0; i < 200000; i++) {	/* from outside */
		n = request(in, sizeof(in), alice, random() % 4 & (F_AUTH | F_PRIV), pdu, sizeof(pdu));
		for (int k = random() % 6; k >= 0; k--) in[random() % n] = random();
		rx(random() % 4 ? n : random() % (n + 1));
	}
	int before = asked;
	for (int i = 0; i < 200000; i++) {	/* from a user */
		uint8_t p[60];
		memcpy(p, pdu, sizeof(pdu)); for (size_t k = sizeof(pdu); k < 60; k++) p[k] = random();
		for (int k = random() % 6; k >= 0; k--) p[random() % sizeof(pdu)] = random();
		n = request(in, sizeof(in), alice, F_AUTH | F_PRIV, p, random() % 60);
		rx(n);
	}
	check(asked > before, "400000 mutated messages, %d of them let through to the agent", asked - before);
	struct usm_trap t = { .community = "public", .oid = { 1, 3, 6, 1, 6, 3, 1, 1, 5, 3 }, .oid_len = 10, .port = 3, .if_admin = 1, .if_oper = 2, .text = "x" };
	bool fits = true;
	for (size_t m = 0; m < 300; m++) { uint8_t *b = malloc(m ? m : 1); fits &= usm_trap(&t, b, m) <= m; free(b); }
	t.user = "alice";
	for (size_t m = 0; m < 400; m++) { uint8_t *b = malloc(m ? m : 1); fits &= usm_trap(&t, b, m) <= m; free(b); }
	check(fits, "a notification into any buffer");
	t.user = "nobody";
	check(!usm_trap(&t, out, sizeof(out)), "none under a user that isn't there");
	printf("\n%s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
