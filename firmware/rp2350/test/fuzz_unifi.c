/*
 * libFuzzer over what unifi.c reads from the network: make fuzz-unifi
 *
 * Until it's adopted the switch uses the default key, MD5("ubnt"), so anyone
 * who can answer its informs gets data through unifi_open(). The first byte
 * picks what the rest is:
 *   0  a packet, opened with the default key
 *   1  a packet's contents: sealed here (flags from the next byte), so the
 *      fuzzer reaches the padding, inflate and the end of unifi_open()
 *   2  a zlib stream, unpacked into a buffer of the size the next byte says
 *   3  a reply's JSON, read as unifi.c's handle() reads it
 *   4  two system_cfg texts (before NUL after), into commands, with the copper
 *      and SFP port counts and the output size from the next three bytes
 *   5  an LLDPDU's TLVs, on the port the next byte says; then the neighbours shown
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"

#include "inflate.h"
#include "unifi_proto.h"
#include "lldp_nbr.h"

#define TEXT_MAX	16384		/* unifi.c's sizes */
#define CMDS_MAX	2048

static const uint8_t KEY[16] = {0xba, 0x86, 0xf2, 0xbb, 0xe1, 0x07, 0xc7, 0xc5,
				0x7e, 0xb5, 0xf2, 0x69, 0x07, 0x75, 0xc7, 0x12};

static char *text(const uint8_t *d, size_t n)
{
	char *t = malloc(n + 1);

	memcpy(t, d, n);
	t[n] = 0;
	return t;
}

static void open_packet(const uint8_t *d, size_t n)
{
	char *out = malloc(TEXT_MAX);
	uint8_t *p = malloc(n ? n : 1);		/* opened in place */

	memcpy(p, d, n);
	unifi_open(p, n, KEY, out, TEXT_MAX);
	free(p);
	free(out);
}

/* flags: bit 0 zlib, 1 GCM, 2 PKCS#7 padding added here (else the data's own) */
static void sealed(const uint8_t *d, size_t n)
{
	if (!n)
		return;
	uint8_t f = d[0];
	d++, n--;
	size_t pad = (f & 2) ? 0 : (f & 4) ? 16 - n % 16 : 0;
	size_t body = n + pad + ((f & 2) ? 16 : 0);
	uint8_t *p = calloc(1, UNIFI_HDR + body + 16);

	memcpy(p, "TNBU", 4);
	p[7] = 1;
	p[15] = 1 | ((f & 1) ? 2 : 0) | ((f & 2) ? 8 : 0);
	memset(p + 16, 0x5a, 16);
	p[35] = 1;
	p[36] = body >> 24, p[37] = body >> 16, p[38] = body >> 8, p[39] = body;
	memcpy(p + UNIFI_HDR, d, n);
	memset(p + UNIFI_HDR + n, (int)pad, pad);
	if (f & 2) {
		mbedtls_gcm_context c;

		mbedtls_gcm_init(&c);
		mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, KEY, 128);
		mbedtls_gcm_crypt_and_tag(&c, MBEDTLS_GCM_ENCRYPT, n, p + 16, 16, p, UNIFI_HDR,
					  p + UNIFI_HDR, p + UNIFI_HDR, 16, p + UNIFI_HDR + n);
		mbedtls_gcm_free(&c);
	} else {
		mbedtls_aes_context c;
		uint8_t iv[16];
		size_t m = (n + pad) & ~(size_t)15;	/* the rest stays clear: a wrong length */

		memcpy(iv, p + 16, 16);
		mbedtls_aes_init(&c);
		mbedtls_aes_setkey_enc(&c, KEY, 128);
		if (m)
			mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, m, iv, p + UNIFI_HDR, p + UNIFI_HDR);
		mbedtls_aes_free(&c);
	}
	open_packet(p, UNIFI_HDR + body);
	free(p);
}

static void inflate(const uint8_t *d, size_t n)
{
	if (!n)
		return;
	size_t max = (size_t)d[0] * 64 + 1;
	uint8_t *out = malloc(max);

	zlib_inflate(d + 1, n - 1, out, max);
	free(out);
}

static void commands(struct unifi_sw *sw, struct unifi_sw *before, uint8_t copper, uint8_t sfp, size_t max)
{
	struct unifi_model m;
	char *out = malloc(max);

	unifi_model_pick(copper, sfp, &m);
	if (unifi_commands(sw, before, &m, out, max) >= 0 && memchr(out, 0, max) == NULL)
		abort();	/* said it fitted, but no terminator */
	free(out);
}

/* As handle() in unifi.c */
static void reply(const uint8_t *d, size_t n)
{
	char *j = text(d, n), type[24], s[96], version[24] = "";
	uint8_t k[16];
	long v;

	if (unifi_json_str(j, "_type", type, sizeof(type))) {
		unifi_json_int(j, "interval", &v);
		char *mgmt = malloc(1024), *sys;

		if (unifi_json_str(j, "mgmt_cfg", mgmt, 1024)) {
			if (unifi_kv(mgmt, "authkey", s, sizeof(s)))
				unifi_hex16(s, k);
			unifi_kv(mgmt, "use_aes_gcm", s, sizeof(s));
			unifi_kv(mgmt, "cfgversion", version, sizeof(version));
		}
		if (unifi_json_str(j, "cfgversion", s, sizeof(s)) && strlen(s) < sizeof(version))
			strcpy(version, s);
		free(mgmt);
		if (unifi_json_str(j, "cmd", type, sizeof(type)) && unifi_json_str(j, "key", s, sizeof(s)))
			unifi_hex16(s, k);
		unifi_json_str(j, "uri", s, sizeof(s));
		/* last, as it's unescaped in place */
		if ((sys = unifi_json_str_here(j, "system_cfg"))) {
			struct unifi_sw *sw = malloc(sizeof(*sw));

			if (unifi_parse(sys, sw))
				commands(sw, NULL, 8, 2, CMDS_MAX);
			free(sw);
		}
	}
	free(j);
}

static void config(const uint8_t *d, size_t n)
{
	if (n < 3)
		return;
	uint8_t copper = d[0] % 12, sfp = d[1] % 4;
	size_t max = (size_t)d[2] * 8 + 1;
	char *t = text(d + 3, n - 3), *after = t + strlen(t);
	struct unifi_sw *sw = malloc(sizeof(*sw)), *before = malloc(sizeof(*before));

	after = after < t + n - 3 ? after + 1 : after;	/* past the NUL, if there's one */
	bool b = unifi_parse(t, before);
	if (unifi_parse(after, sw)) {
		commands(sw, b ? before : NULL, copper, sfp, max);
		commands(sw, b ? before : NULL, copper, sfp, CMDS_MAX);
	}
	free(sw);
	free(before);
	free(t);
}

static void lldp(const uint8_t *d, size_t n)
{
	char s[700];

	if (!n)
		return;
	lldp_nbr_rx(d[0] % 12, d + 1, n - 1 > 0xffff ? 0xffff : n - 1, 1000);
	lldp_nbr_show(s, d[0] / 12 + 1, 1000);
	lldp_nbr_show(s, sizeof(s), 1000);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	if (!n)
		return 0;
	switch (d[0] % 6) {
	case 0: open_packet(d + 1, n - 1); break;
	case 1: sealed(d + 1, n - 1); break;
	case 2: inflate(d + 1, n - 1); break;
	case 3: reply(d + 1, n - 1); break;
	case 4: config(d + 1, n - 1); break;
	case 5: lldp(d + 1, n - 1); break;
	}
	return 0;
}
