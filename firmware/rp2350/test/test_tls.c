/*
 * HTTPS on the host: the firmware's mbedTLS configuration (with the client
 * side added) and its certificate code, tls_cert.c. Makes a key and a
 * certificate as "https on" does, loads them as altcp_tls does, and runs a
 * handshake and a request for each cipher suite and curve the server
 * offers, against a client that verifies the certificate and the name.
 * Built by test/Makefile.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "tls_cert.h"

static int failures;

static void check(int cond, const char *what, const char *detail, int err)
{
	printf("%s  %s", cond ? "PASS" : "FAIL", what);
	if (detail)
		printf(": %s", detail);
	if (err)
		printf(" (-0x%04x)", -err);
	printf("\n");
	failures += !cond;
}

/* The RP2350's TRNG and clock, as pico_mbedtls and tls.c provide them */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
	FILE *f = fopen("/dev/urandom", "rb");

	(void)data;
	*olen = f ? fread(out, 1, len, f) : 0;
	if (f)
		fclose(f);
	return *olen == len ? 0 : -1;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (mbedtls_ms_time_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* An in-memory connection: a byte queue each way, an end holds both */
struct queue { unsigned char buf[65536]; size_t len; };
struct end { struct queue *out, *in; };

static int end_send(void *ctx, const unsigned char *p, size_t n)
{
	struct queue *q = ((struct end *)ctx)->out;

	if (n > sizeof(q->buf) - q->len)
		n = sizeof(q->buf) - q->len;
	if (!n)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	memcpy(q->buf + q->len, p, n);
	q->len += n;
	return n;
}

static int end_recv(void *ctx, unsigned char *p, size_t n)
{
	struct queue *q = ((struct end *)ctx)->in;

	if (!q->len)
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (n > q->len)
		n = q->len;
	memcpy(p, q->buf, n);
	memmove(q->buf, q->buf + n, q->len - n);
	q->len -= n;
	return n;
}

static int pending(int r)
{
	return r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE;
}

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt crt;
static mbedtls_pk_context key;

/* One connection with the given suite and group (0: the client's defaults) */
static void session(const char *what, int suite, uint16_t group)
{
	static struct queue c2s, s2c;
	struct end srv_end = { &s2c, &c2s }, cli_end = { &c2s, &s2c };
	mbedtls_ssl_config sconf, cconf;
	mbedtls_ssl_context srv, cli;
	int suites[2] = { suite, 0 };
	/* TLS 1.2 checks the certificate's curve against the same list:
	 * P-256 always goes along, as browsers offer it */
	uint16_t groups[3] = { group, MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, MBEDTLS_SSL_IANA_TLS_GROUP_NONE };
	static const char req[] = "GET /status.json HTTP/1.1\r\n\r\n";
	static unsigned char big[6000], back[6000];
	unsigned char buf[1024];
	int rs = 1, rc = 1, n;

	c2s.len = s2c.len = 0;
	mbedtls_ssl_config_init(&sconf);
	mbedtls_ssl_config_init(&cconf);
	mbedtls_ssl_init(&srv);
	mbedtls_ssl_init(&cli);
	mbedtls_ssl_config_defaults(&sconf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	mbedtls_ssl_config_defaults(&cconf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	mbedtls_ssl_conf_rng(&sconf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_rng(&cconf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_own_cert(&sconf, &crt, &key);
	mbedtls_ssl_conf_authmode(&cconf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&cconf, &crt, NULL);
	if (suite)
		mbedtls_ssl_conf_ciphersuites(&cconf, suites);
	if (group)
		mbedtls_ssl_conf_groups(&cconf, groups);
	if (mbedtls_ssl_setup(&srv, &sconf) || mbedtls_ssl_setup(&cli, &cconf)
	    || mbedtls_ssl_set_hostname(&cli, "sim-switch")) {
		check(0, what, "setup", 0);
		goto out;
	}
	mbedtls_ssl_set_bio(&srv, &srv_end, end_send, end_recv, NULL);
	mbedtls_ssl_set_bio(&cli, &cli_end, end_send, end_recv, NULL);

	for (int i = 0; i < 100 && (rs || rc); i++) {
		if (rc)
			rc = mbedtls_ssl_handshake(&cli);
		if (rs)
			rs = mbedtls_ssl_handshake(&srv);
		if ((rc && !pending(rc)) || (rs && !pending(rs)))
			break;
	}
	if (rc || rs) {
		check(0, what, "handshake", pending(rc) ? rs : rc);
		goto out;
	}

	/* A request, and a response written as the web server writes: 2 KiB records */
	mbedtls_ssl_write(&cli, (const unsigned char *)req, sizeof(req) - 1);
	n = mbedtls_ssl_read(&srv, buf, sizeof(buf));
	bool ok = n == (int)sizeof(req) - 1 && !memcmp(buf, req, n);
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = i * 7;
	for (size_t off = 0; off < sizeof(big); off += 2048) {
		size_t len = sizeof(big) - off < 2048 ? sizeof(big) - off : 2048;
		ok &= mbedtls_ssl_write(&srv, big + off, len) == (int)len;
	}
	size_t got = 0;
	while (got < sizeof(back) && (n = mbedtls_ssl_read(&cli, back + got, sizeof(back) - got)) > 0)
		got += n;
	ok &= got == sizeof(big) && !memcmp(back, big, got);

	char detail[128];
	snprintf(detail, sizeof(detail), "%s, %s, certificate verified", mbedtls_ssl_get_ciphersuite(&cli),
		 mbedtls_ssl_get_version(&cli));
	check(ok && mbedtls_ssl_get_verify_result(&cli) == 0, what, detail, 0);
out:
	mbedtls_ssl_free(&srv);
	mbedtls_ssl_free(&cli);
	mbedtls_ssl_config_free(&sconf);
	mbedtls_ssl_config_free(&cconf);
}

int main(void)
{
	static uint8_t der_key[1024], der_cert[1024];
	size_t key_len = sizeof(der_key), cert_len = sizeof(der_cert);
	char name[256];
	int r;

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&crt);
	mbedtls_pk_init(&key);
	mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0);

	r = tls_make_cert("sim-switch", der_key, &key_len, der_cert, &cert_len);
	check(r == 0 && key_len < 200 && cert_len < 600, "key and certificate made", NULL, r);
	r = mbedtls_x509_crt_parse_der(&crt, der_cert, cert_len);
	check(r == 0, "certificate parses", NULL, r);
	mbedtls_x509_dn_gets(name, sizeof(name), &crt.subject);
	check(!strcmp(name, "CN=sim-switch, O=RTLPlayground mod chip"), "subject", name, 0);
	r = mbedtls_pk_parse_key(&key, der_key, key_len, NULL, 0, mbedtls_ctr_drbg_random, &drbg);
	check(r == 0 && mbedtls_pk_get_type(&key) == MBEDTLS_PK_ECKEY, "key parses, ECDSA", NULL, r);

	session("client defaults", 0, 0);
	session("AES-128-GCM", MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0);
	session("ChaCha20-Poly1305", MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, 0);
	session("AES-256-GCM", MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, 0);
	session("X25519 offered first", 0, MBEDTLS_SSL_IANA_TLS_GROUP_X25519);
	session("P-256 only", 0, MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1);

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
