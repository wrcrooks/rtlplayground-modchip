#ifndef _RNG_H_
#define _RNG_H_

#include <stddef.h>

/* mbedTLS's f_rng: a DRBG seeded from the RP2350's TRNG */
int rng_get(void *unused, unsigned char *p, size_t n);

#endif
