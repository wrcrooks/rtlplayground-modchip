/*
 * unifi_proto.c and inflate.c. run_unifi.py feeds vectors on stdin and checks
 * what comes back on stdout:
 *   Z <zlib hex> <plain hex>         must inflate to plain
 *   X <hex>                          damaged: must be refused
 *   P <key> <packet hex> <json hex>  must open to json
 *   S <key> <gcm 0|1> <json hex>     prints "S <packet hex>", sealed
 * Then the configuration in unifi/: printed as commands, which the script compares.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"
#include "unifi_proto.h"
#include "lldp_nbr.h"

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = 0;
	unsigned b;

	while (h[0] && h[1] && sscanf(h, "%2x", &b) == 1) {
		out[n++] = b;
		h += 2;
	}
	return n;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	static char buf[16384];
	size_t n;

	if (!f) {
		perror(path);
		exit(2);
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return strdup(buf);
}

static void json_checks(void)
{
	const char *j = "{\"_type\" : \"setparam\", \"n\":{\"_type\":\"no\",\"a\":[1,{\"x\":\"}\"},\"]\"]},"
		"\"mgmt_cfg\":\"a=1\\nauthkey=00112233445566778899aabbccddeeff\\nuse_aes_gcm=true\\n\","
		"\"interval\":10,\"s\":\"\\u0041\\\"\\\\\\/\",\"t\":true,\"cfgversion\":\"12\"}";
	char s[200], v[64];
	long i;
	uint8_t k[16];

	CHECK(unifi_json_str(j, "_type", s, sizeof(s)) && !strcmp(s, "setparam"), "_type %s", s);
	CHECK(unifi_json_str(j, "s", s, sizeof(s)) && !strcmp(s, "A\"\\/"), "escapes %s", s);
	CHECK(unifi_json_int(j, "interval", &i) && i == 10, "interval");
	CHECK(unifi_json_int(j, "cfgversion", &i) && i == 12, "int in a string");
	CHECK(!unifi_json_str(j, "x", s, sizeof(s)), "nested members are not top-level");
	CHECK(!unifi_json_str(j, "interval", s, sizeof(s)), "a number is no string");
	CHECK(!unifi_json_str(j, "_type", s, 4), "too long for out");
	CHECK(unifi_json_str("{\"a\":\"\\u004a\\u004B\"}", "a", s, sizeof(s)) && !strcmp(s, "JK"), "\\u in either case");
	CHECK(!unifi_json_str("{\"a\":\"\\u+041\"}", "a", s, sizeof(s)) && !unifi_json_str("{\"a\":\"\\u 041\"}", "a", s, sizeof(s)),
	      "\\u takes only hex digits");
	{
		/* found by fuzz_unifi: a short \u at the end read past it */
		static const char t[] = "{\"_\":\"\\u0";
		char *h = malloc(sizeof(t));

		memcpy(h, t, sizeof(t));
		CHECK(!unifi_json_str(h, "x", s, sizeof(s)), "a short \\u at the end");
		free(h);
	}
	CHECK(unifi_json_str(j, "mgmt_cfg", s, sizeof(s)), "mgmt_cfg");
	CHECK(unifi_kv(s, "authkey", v, sizeof(v)) && unifi_hex16(v, k) && k[15] == 0xff, "authkey %s", v);
	CHECK(unifi_kv(s, "use_aes_gcm", v, sizeof(v)) && !strcmp(v, "true"), "use_aes_gcm");
	CHECK(!unifi_kv(s, "authke", v, sizeof(v)), "a key's prefix is not the key");
	CHECK(!unifi_hex16("00112233445566778899aabbccddeegg", k), "bad hex");
	CHECK(!unifi_json_str("{\"a\":\"x", "a", s, sizeof(s)), "unterminated");
	CHECK(!unifi_json_str("[1]", "a", s, sizeof(s)), "not an object");
	CHECK(!unifi_json_str("{\"a\" 1}", "a", s, sizeof(s)), "no colon");
	{
		char h[] = "{\"x\":1,\"a\":\"one\\ntwo \\u0041\\\"\",\"b\":2}", plain[] = "{\"a\":\"abc\"}", *v;
		CHECK((v = unifi_json_str_here(h, "a")) && !strcmp(v, "one\ntwo A\""), "in place: %s", v ? v : "-");
		CHECK((v = unifi_json_str_here(plain, "a")) && !strcmp(v, "abc"), "in place, no escapes");
		char bad[] = "{\"a\":\"x\\q\"}", open_[] = "{\"a\":\"x";
		CHECK(!unifi_json_str_here(bad, "a") && !unifi_json_str_here(open_, "a"), "in place: bad ones refused");
	}
}

static void model_checks(void)
{
	struct unifi_model m;

	unifi_model_pick(4, 2, &m);
	CHECK(!strcmp(m.code, "USWED36") && m.ports == 10 && m.front[4] == 4 && !m.front[5] && m.front[9] == 5
	      && m.front[10] == 6 && m.sfp[9] && !m.sfp[4], "4 + 2");
	unifi_model_pick(4, 1, &m);
	CHECK(!strcmp(m.code, "USWED35") && m.ports == 5 && m.front[5] == 5 && m.sfp[5], "4 + 1");
	unifi_model_pick(8, 1, &m);
	CHECK(!strcmp(m.code, "USWED36") && m.front[8] == 8 && m.front[9] == 9 && !m.front[10], "8 + 1");
}

/* An LLDPDU's TLVs into d: type, length, value ...; returns the length */
static uint16_t tlv(uint8_t *d, uint16_t n, uint8_t type, const void *v, uint16_t len)
{
	d[n] = type << 1 | len >> 8;
	d[n + 1] = len;
	memcpy(d + n + 2, v, len);
	return n + 2 + len;
}

static void lldp(void)
{
	uint8_t d[400];
	uint16_t n = 0;
	const struct lldp_nbr *x;
	char s[600];

	lldp_nbr_clear();
	n = tlv(d, n, 1, "\x04\x0a\x0b\x0c\x0d\x0e\x0f", 7);
	n = tlv(d, n, 2, "\x05ge-0/0/7", 9);
	n = tlv(d, n, 3, "\x00\x78", 2);
	n = tlv(d, n, 4, "uplink", 6);
	n = tlv(d, n, 5, "core\x01sw", 7);
	n = tlv(d, n, 8, "\x05\x01\x0a\x00\x00\x02\x02\x00\x00\x00\x01\x00", 12);
	n = tlv(d, n, 0, "", 0);
	lldp_nbr_rx(3, d, n, 1000);
	x = lldp_nbr_get(3, 1000);
	CHECK(x && x->chassis_mac && !strcmp(x->chassis, "0a:0b:0c:0d:0e:0f") && !strcmp(x->port, "ge-0/0/7")
	      && !strcmp(x->port_desc, "uplink") && !strcmp(x->name, "core?sw") && x->ip[0] == 10 && x->ip[3] == 2,
	      "LLDP: a neighbour, unprintable bytes as ?");
	CHECK(lldp_nbr_get(3, 1119) && !lldp_nbr_get(3, 1120) && !lldp_nbr_get(3, 1000), "its TTL, then gone");
	lldp_nbr_rx(0, d, n, 1000);
	lldp_nbr_rx(11, d, n, 1000);
	CHECK(!lldp_nbr_get(0, 1000) && !lldp_nbr_get(11, 1000), "ports outside 1..10 ignored");
	lldp_nbr_rx(1, d, n - 3, 1000);	/* cut in the management address */
	CHECK(!lldp_nbr_get(1, 1000), "a TLV running past the frame: ignored");
	n = 0;
	n = tlv(d, n, 1, "\x07sw1", 4);
	n = tlv(d, n, 2, "\x03\x01\x02\x03\x04\x05\x06", 7);
	lldp_nbr_rx(2, d, n, 1000);
	CHECK(!lldp_nbr_get(2, 1000), "no TTL: ignored");
	n = tlv(d, n, 3, "\x00\x10", 2);
	static const char longname[] = "a-system-name-much-longer-than-thirty-one-characters";
	n = tlv(d, n, 5, longname, sizeof(longname) - 1);
	lldp_nbr_rx(2, d, n, 1000);
	x = lldp_nbr_get(2, 1000);
	CHECK(x && !x->chassis_mac && !strcmp(x->chassis, "sw1") && !strcmp(x->port, "01:02:03:04:05:06")
	      && strlen(x->name) == 31 && !memcmp(x->name, longname, 31), "a local chassis ID, a MAC port ID, a long name cut");
	lldp_nbr_show(s, sizeof(s), 1000);
	CHECK(strstr(s, "port 2: a-system-name") && strstr(s, ", 16 s left\n"), "shown: %s", s);
	n = 0;
	n = tlv(d, n, 1, "\x07sw1", 4);
	n = tlv(d, n, 2, "\x07p1", 3);
	n = tlv(d, n, 3, "\x00\x00", 2);
	lldp_nbr_rx(2, d, n, 1001);
	CHECK(!lldp_nbr_get(2, 1001), "TTL 0: forgotten");
	lldp_nbr_show(s, 20, 1000);
	CHECK(strlen(s) < 20, "show stays in its buffer");
	lldp_nbr_clear();
	lldp_nbr_show(s, sizeof(s), 1000);
	CHECK(strstr(s, "no LLDP neighbours"), "none: says so");
}

int main(int argc, char **argv)
{
	(void)argc;
	setvbuf(stdout, NULL, _IOLBF, 0);
	lldp();
	static uint8_t a[70000], b[70000];
	static char line[300000], out[70000];
	struct unifi_model m;
	static struct unifi_sw sw, before;
	long n;

	json_checks();
	model_checks();
	while (fgets(line, sizeof(line), stdin)) {
		char key[40], h1[150000], h2[150000];
		int gcm;

		if (sscanf(line, "Z %149999s %149999s", h1, h2) == 2) {
			size_t zl = unhex(h1, a), pl = unhex(h2, b);
			n = zlib_inflate(a, zl, (uint8_t *)out, sizeof(out));
			CHECK(n == (long)pl && !memcmp(out, b, pl), "inflate %zu bytes: %ld", pl, n);
			CHECK(pl == 0 || zlib_inflate(a, zl, (uint8_t *)out, pl - 1) < 0, "inflate into too small a buffer");
			n = zlib_store(b, pl, a, sizeof(a));
			CHECK(n > 0 && zlib_inflate(a, n, (uint8_t *)out, sizeof(out)) == (long)pl && !memcmp(out, b, pl), "store");
		} else if (sscanf(line, "X %149999s", h1) == 1) {
			size_t zl = unhex(h1, a);
			CHECK(zlib_inflate(a, zl, (uint8_t *)out, sizeof(out)) < 0, "damaged stream taken: %.40s", h1);
		} else if (sscanf(line, "P %39s %149999s %149999s", key, h1, h2) == 3) {
			uint8_t k[16];
			size_t pl = unhex(h1, a), jl = unhex(h2, b);
			unifi_hex16(key, k);
			n = unifi_open(a, pl, k, out, sizeof(out));
			CHECK(n == (long)jl && !memcmp(out, b, jl), "open: %ld", n);
			unhex(h1, a);	/* opened in place */
			a[pl - 1] ^= 1;
			CHECK(unifi_open(a, pl, k, out, sizeof(out)) < 0, "a changed packet is refused");
		} else if (sscanf(line, "S %39s %d %149999s", key, &gcm, h2) == 3) {
			uint8_t k[16], iv[16];
			size_t jl = unhex(h2, b);
			unifi_hex16(key, k);
			for (int i = 0; i < 16; i++)
				iv[i] = rand();
			n = unifi_seal(a, sizeof(a), (const uint8_t *)"\x02\x00\x5e\x10\x00\x01", k, gcm, iv, (char *)b, jl);
			CHECK(n > 0, "seal");
			printf("S ");
			for (long i = 0; i < n; i++)
				printf("%02x", a[i]);
			printf("\n");
			/* the JSON already where seal wants it: the same packet */
			static uint8_t c[sizeof(a)];
			memcpy(c + UNIFI_SEAL_AT, b, jl);
			CHECK(unifi_seal(c, sizeof(c), (const uint8_t *)"\x02\x00\x5e\x10\x00\x01", k, gcm, iv,
					 (char *)c + UNIFI_SEAL_AT, jl) == n && !memcmp(a, c, n), "sealed in place");
			CHECK(unifi_open(a, n, k, out, sizeof(out)) == (long)jl, "own packet opens");
		}
	}

	/* the configuration, as captured from UniFi Network 10.6.109 */
	unifi_model_pick(4, 2, &m);
	char *c1 = slurp(argv[1]), *c2 = slurp(argv[2]);
	CHECK(unifi_parse(c1, &before), "adopted.cfg");
	CHECK(unifi_parse(c2, &sw), "ports.cfg");
	free(c1);
	free(c2);
	n = unifi_commands(&before, NULL, &m, out, sizeof(out));
	printf("C adopted\n%s", out);
	n = unifi_commands(&sw, &before, &m, out, sizeof(out));
	printf("C ports\n%s", out);
	CHECK(unifi_commands(&sw, &before, &m, out, 20) < 0, "out too small");
	n = unifi_commands(&before, &sw, &m, out, sizeof(out));
	printf("C back\n%s", out);
	if (argc > 3) {
		/* STP, isolation, storm control, egress limits: from adopted.cfg, and back */
		static struct unifi_sw f;
		char *c3 = slurp(argv[3]);
		CHECK(unifi_parse(c3, &f), "features.cfg");
		free(c3);
		unifi_commands(&f, &before, &m, out, sizeof(out));
		printf("C features\n%s", out);
		unifi_commands(&before, &f, &m, out, sizeof(out));
		printf("C unfeatures\n%s", out);
	}
	CHECK(!unifi_parse("switch.vlan.1.id=1\n", &sw) || sw.nvlans == 1, "one VLAN");
	CHECK(!unifi_parse("nothing=here\n", &sw), "no switch lines");
	printf("C end\n%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
