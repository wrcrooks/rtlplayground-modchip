/* zlib streams, see inflate.h. Inflate after RFC 1951's own description. */

#include <string.h>

#include "inflate.h"

struct huff {
	uint16_t count[16];		/* codes of each length */
	uint16_t sym[288];		/* symbols ordered by code */
};

struct in {
	const uint8_t *p, *end;
	uint32_t bits;
	int nbits;
	uint8_t *out;
	size_t n, max;
	int bad;
};

static int bits(struct in *s, int need)
{
	uint32_t v;

	while (s->nbits < need) {
		if (s->p == s->end) {
			s->bad = 1;
			return 0;
		}
		s->bits |= (uint32_t)*s->p++ << s->nbits;
		s->nbits += 8;
	}
	v = s->bits & ((1u << need) - 1);
	s->bits >>= need;
	s->nbits -= need;
	return v;
}

/* Canonical codes from lengths; -1 when over-subscribed */
static int build(struct huff *h, const uint8_t *len, int n)
{
	uint16_t offs[16];
	int left = 1;

	memset(h->count, 0, sizeof(h->count));
	for (int i = 0; i < n; i++)
		h->count[len[i]]++;
	h->count[0] = 0;
	for (int l = 1; l < 16; l++) {
		left = (left << 1) - h->count[l];
		if (left < 0)
			return -1;
	}
	offs[1] = 0;
	for (int l = 1; l < 15; l++)
		offs[l + 1] = offs[l] + h->count[l];
	for (int i = 0; i < n; i++)
		if (len[i])
			h->sym[offs[len[i]]++] = i;
	return 0;
}

static int decode(struct in *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0;

	for (int l = 1; l < 16; l++) {
		code |= bits(s, 1);
		if (s->bad)
			return -1;
		int count = h->count[l];
		if (code - first < count)
			return h->sym[index + code - first];
		index += count;
		first = (first + count) << 1;
		code <<= 1;
	}
	s->bad = 1;
	return -1;
}

static const uint16_t len_base[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
	67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
	1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
	11, 11, 12, 12, 13, 13 };

static int codes(struct in *s, const struct huff *lit, const struct huff *dist)
{
	for (;;) {
		int sym = decode(s, lit);

		if (sym < 0)
			return -1;
		if (sym < 256) {
			if (s->n == s->max)
				return -1;
			s->out[s->n++] = sym;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29)
				return -1;
			size_t len = len_base[sym] + bits(s, len_extra[sym]);
			int d = decode(s, dist);
			if (d < 0 || d >= 30)
				return -1;
			size_t back = dist_base[d] + bits(s, dist_extra[d]);
			if (s->bad || back > s->n || len > s->max - s->n)
				return -1;
			for (; len; len--, s->n++)
				s->out[s->n] = s->out[s->n - back];
		}
	}
}

static int fixed(struct in *s)
{
	static struct huff lit, dist;
	static int built;

	if (!built) {
		uint8_t l[288];
		int i;

		for (i = 0; i < 144; i++) l[i] = 8;
		for (; i < 256; i++) l[i] = 9;
		for (; i < 280; i++) l[i] = 7;
		for (; i < 288; i++) l[i] = 8;
		build(&lit, l, 288);
		memset(l, 5, 30);
		build(&dist, l, 30);
		built = 1;
	}
	return codes(s, &lit, &dist);
}

static int dynamic(struct in *s)
{
	static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	struct huff lit, dist, lens;
	uint8_t l[320];
	int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
	int i;

	if (s->bad || nlen > 286 || ndist > 30)
		return -1;
	memset(l, 0, 19);
	for (i = 0; i < ncode; i++)
		l[order[i]] = bits(s, 3);
	if (s->bad || build(&lens, l, 19))
		return -1;
	for (i = 0; i < nlen + ndist;) {
		int sym = decode(s, &lens), rep, v = 0;

		if (sym < 0)
			return -1;
		if (sym < 16) {
			l[i++] = sym;
			continue;
		}
		if (sym == 16) {
			if (!i)
				return -1;
			v = l[i - 1];
			rep = 3 + bits(s, 2);
		} else if (sym == 17) {
			rep = 3 + bits(s, 3);
		} else {
			rep = 11 + bits(s, 7);
		}
		if (s->bad || i + rep > nlen + ndist)
			return -1;
		while (rep--)
			l[i++] = v;
	}
	if (!l[256] || build(&lit, l, nlen) || build(&dist, l + nlen, ndist))
		return -1;
	return codes(s, &lit, &dist);
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
	uint32_t a = 1, b = 0;

	while (n) {
		size_t k = n < 5552 ? n : 5552;

		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

long zlib_inflate(const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
	struct in s = { .p = in + 2, .end = in + len, .out = out, .max = max };
	int last;

	if (len < 6 || (in[0] & 0x0f) != 8 || (in[0] >> 4) > 7 || (in[0] << 8 | in[1]) % 31 || (in[1] & 0x20))
		return -1;
	s.end -= 4;
	do {
		last = bits(&s, 1);
		int type = bits(&s, 2);
		if (s.bad)
			return -1;
		if (type == 0) {
			s.bits = 0;
			s.nbits = 0;
			if (s.end - s.p < 4)
				return -1;
			unsigned n = s.p[0] | s.p[1] << 8;
			if ((n ^ (s.p[2] | s.p[3] << 8)) != 0xffff)
				return -1;
			s.p += 4;
			if ((size_t)(s.end - s.p) < n || n > s.max - s.n)
				return -1;
			memcpy(s.out + s.n, s.p, n);
			s.p += n;
			s.n += n;
		} else if (type == 1) {
			if (fixed(&s))
				return -1;
		} else if (type == 2) {
			if (dynamic(&s))
				return -1;
		} else {
			return -1;
		}
	} while (!last);
	const uint8_t *a = in + len - 4;
	if (adler32(out, s.n) != ((uint32_t)a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3]))
		return -1;
	return s.n;
}

long zlib_store(const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
	size_t n = 0, blocks = len ? (len + 65534) / 65535 : 1;
	uint32_t a = adler32(in, len);

	if (max < len + 6 + 5 * blocks)
		return -1;
	out[n++] = 0x78;
	out[n++] = 0x01;
	for (size_t b = 0; b < blocks; b++) {
		size_t k = len - b * 65535 > 65535 ? 65535 : len - b * 65535;

		out[n++] = b == blocks - 1;
		out[n++] = k;
		out[n++] = k >> 8;
		out[n++] = ~k;
		out[n++] = ~k >> 8;
		memcpy(out + n, in + b * 65535, k);
		n += k;
	}
	out[n++] = a >> 24;
	out[n++] = a >> 16;
	out[n++] = a >> 8;
	out[n++] = a;
	return n;
}
