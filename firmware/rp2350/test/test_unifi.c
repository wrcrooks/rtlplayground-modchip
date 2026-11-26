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
	CHECK(unifi_json_str(j, "mgmt_cfg", s, sizeof(s)), "mgmt_cfg");
	CHECK(unifi_kv(s, "authkey", v, sizeof(v)) && unifi_hex16(v, k) && k[15] == 0xff, "authkey %s", v);
	CHECK(unifi_kv(s, "use_aes_gcm", v, sizeof(v)) && !strcmp(v, "true"), "use_aes_gcm");
	CHECK(!unifi_kv(s, "authke", v, sizeof(v)), "a key's prefix is not the key");
	CHECK(!unifi_hex16("00112233445566778899aabbccddeegg", k), "bad hex");
	CHECK(!unifi_json_str("{\"a\":\"x", "a", s, sizeof(s)), "unterminated");
	CHECK(!unifi_json_str("[1]", "a", s, sizeof(s)), "not an object");
	CHECK(!unifi_json_str("{\"a\" 1}", "a", s, sizeof(s)), "no colon");
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

int main(int argc, char **argv)
{
	(void)argc;
	setvbuf(stdout, NULL, _IOLBF, 0);
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
	CHECK(!unifi_parse("switch.vlan.1.id=1\n", &sw) || sw.nvlans == 1, "one VLAN");
	CHECK(!unifi_parse("nothing=here\n", &sw), "no switch lines");
	printf("C end\n%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
