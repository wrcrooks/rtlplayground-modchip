/* The RP2350's TRNG and clock for mbedTLS on the host, where a test has none of its own (libmbed.a) */
#include <stdio.h>
#include <time.h>

#include "mbedtls/platform_time.h"

__attribute__((weak)) int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
	FILE *f = fopen("/dev/urandom", "rb");

	(void)data;
	*olen = f ? fread(out, 1, len, f) : 0;
	if (f)
		fclose(f);
	return *olen == len ? 0 : -1;
}

__attribute__((weak)) mbedtls_ms_time_t mbedtls_ms_time(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (mbedtls_ms_time_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
