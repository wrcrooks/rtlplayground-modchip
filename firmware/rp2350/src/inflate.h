#ifndef _INFLATE_H_
#define _INFLATE_H_

/*
 * zlib (RFC 1950) streams: unpacking one in a single call (RFC 1951 inflate),
 * and packing data into one without compressing it (stored blocks), which is
 * all a UniFi inform needs.
 */

#include <stddef.h>
#include <stdint.h>

/* Returns the length unpacked, or -1: damaged, a wrong checksum or out too small */
long zlib_inflate(const uint8_t *in, size_t len, uint8_t *out, size_t max);

/* Returns the length packed, or -1: out too small (needs len + 6 + 5 per 65535 bytes) */
long zlib_store(const uint8_t *in, size_t len, uint8_t *out, size_t max);

#endif
