/* Random numbers for keys, see rng.h */

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "rng.h"

int rng_get(void *unused, unsigned char *p, size_t n)
{
	static mbedtls_entropy_context entropy;
	static mbedtls_ctr_drbg_context drbg;
	static int ready;

	(void)unused;
	if (!ready) {
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"modchip", 7)) {
			mbedtls_ctr_drbg_free(&drbg);
			mbedtls_entropy_free(&entropy);
			return -1;
		}
		ready = 1;
	}
	while (n) {
		size_t k = n > 256 ? 256 : n;
		if (mbedtls_ctr_drbg_random(&drbg, p, k))
			return -1;
		p += k;
		n -= k;
	}
	return 0;
}
