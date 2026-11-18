#ifndef _MD5_H_
#define _MD5_H_

#include <stdint.h>

/* MD5 (RFC 1321), md5.c: for RADIUS only */
struct md5 {
	uint32_t h[4];
	uint64_t len;
	uint8_t buf[64];
};

void md5_init(struct md5 *c);
void md5_update(struct md5 *c, const uint8_t *p, uint32_t n);
void md5_final(struct md5 *c, uint8_t out[16]);

#endif
