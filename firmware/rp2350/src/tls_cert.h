#ifndef _TLS_CERT_H_
#define _TLS_CERT_H_

#include <stddef.h>
#include <stdint.h>

/*
 * A new ECDSA P-256 key and a self-signed certificate for it, named
 * CN=<cn>, O=RTLPlayground mod chip, both DER. *key_len and *cert_len give
 * the room in key and cert and come back as the lengths. 0 on success.
 * Only mbedTLS here, so the host tests run it (test/test_tls.c).
 */
int tls_make_cert(const char *cn, uint8_t *key, size_t *key_len, uint8_t *cert, size_t *cert_len);

#endif
