/*
 * link.c against the 8051 bridge's own frames, firmware/micropython/bridge_vectors.txt
 * (from RTLPlayground's test_bridge_link --emit). Built and run by test/Makefile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

static int failures;

static void check(int cond, const char *name, int k)
{
	printf("%s  %s %d\n", cond ? "PASS" : "FAIL", name, k);
	failures += !cond;
}

/* As vector() in RTLPlayground's test/test_bridge_link.c */
static size_t vector(int k, uint8_t *out)
{
	static const size_t lens[] = { 0, 1, 1, 253, 254, 255, 508, 509, 300, 1557 };
	uint32_t x = 12345 + k;

	for (size_t i = 0; i < lens[k]; i++) {
		x = x * 1103515245u + 12345u;
		if (k == 1 || k == 8)
			out[i] = 0;
		else if (k >= 3 && k <= 7)
			out[i] = 1 + (x >> 16) % 255;
		else
			out[i] = (x >> 16) & 0xff;
	}
	return lens[k];
}

static int feed(struct link_rx *rx, const uint8_t *p, size_t n)
{
	int got = 0;

	for (size_t i = 0; i < n; i++) {
		int r = link_rx_byte(rx, p[i]);
		if (r == LINK_RX_MSG)
			got++;
	}
	return got;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "../micropython/bridge_vectors.txt";
	FILE *f = fopen(path, "r");
	static char line[2 * LINK_ENC_MAX + 8];
	static uint8_t frame[LINK_ENC_MAX], enc[LINK_ENC_MAX], payload[LINK_MSG_MAX];
	static struct link_rx rx;
	int k = 0;

	if (!f) {
		perror(path);
		return 2;
	}
	check(link_crc16(0xffff, (const uint8_t *)"123456789", 9) == 0x4b37, "CRC-16/MODBUS check value", 0);

	while (k < 10 && fscanf(f, "%s", line) == 1) {
		size_t flen = strlen(line) / 2;
		for (size_t i = 0; i < flen; i++)
			sscanf(line + 2 * i, "%2hhx", &frame[i]);

		size_t n = vector(k, payload);
		size_t elen = link_encode(enc, 0x10 + k, k, payload, n);
		check(elen == flen && !memcmp(enc, frame, flen), "encodes as the bridge does, vector", k);

		link_rx_reset(&rx);
		int got = feed(&rx, frame, flen);
		check(got == 1 && rx.len == n + 2 && rx.buf[0] == 0x10 + k && rx.buf[1] == k
		      && !memcmp(rx.buf + 2, payload, n), "decodes the bridge's frame, vector", k);

		frame[1] ^= frame[1] != 0x01 ? 0x01 : 0x03;
		uint32_t errs = rx.errors;
		check(feed(&rx, frame, flen) == 0 && rx.errors == errs + 1, "refuses a corrupted frame, vector", k);

		k++;
	}
	fclose(f);
	check(k == 10, "ten vectors read", k);

	/* Back to back messages, and a lone delimiter between them */
	size_t a = link_encode(enc, 0x81, 7, (const uint8_t *)"\x00\x01\x00", 3);
	enc[a++] = 0;
	a += link_encode(enc + a, 0x82, 8, NULL, 0);
	link_rx_reset(&rx);
	check(feed(&rx, enc, a) == 2 && rx.buf[0] == 0x82 && rx.len == 2, "two messages in one stream", 0);

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
