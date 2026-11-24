/* TLS to a server known by its fingerprint, see tls_client.h */

#include <string.h>

#include "lwip/altcp_tls.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "psa/crypto.h"

#include "rng.h"
#include "tls_client.h"

/*
 * In place of a chain to an authority: the server's own certificate (depth 0)
 * must be the one. TLS itself proves that the server holds its key.
 */
static int verify(void *pin, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
	uint8_t h[TLS_PIN_LEN];

	if (depth == 0) {
		if (mbedtls_sha256(crt->raw.p, crt->raw.len, h, 0) || mbedtls_ct_memcmp(h, pin, sizeof(h)))
			return MBEDTLS_ERR_X509_FATAL_ERROR;
	}
	*flags = 0;
	return 0;
}


/*
 * mbedTLS offers RSA-PSS, as TLS 1.3 needs it, and then can't check it in TLS
 * 1.2, where a server with an RSA key may use it whatever we'd prefer. So for
 * TLS 1.2 servers with RSA keys there is a configuration without both.
 */
static const uint16_t sig_algs_12[] = {
	MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256, MBEDTLS_TLS1_3_SIG_ECDSA_SECP384R1_SHA384,
	MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA256, MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA384, MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA512,
	MBEDTLS_TLS1_3_SIG_NONE
};

struct altcp_tls_config *tls_client_config(const uint8_t *pin, bool identity, bool tls12)
{
	struct altcp_tls_config *c;
	struct tls_identity id;

	if (psa_crypto_init() != PSA_SUCCESS)	/* TLS 1.3 */
		return NULL;
	if (identity) {
		if (!tls_identity(&id))
			return NULL;
		c = altcp_tls_create_config_client_2wayauth(NULL, 0, id.key, id.key_len, NULL, 0, id.cert, id.cert_len);
	} else {
		c = altcp_tls_create_config_client(NULL, 0);
	}
	if (!c)
		return NULL;
	/* altcp_tls_mbedtls.c: the mbedTLS configuration comes first. OPTIONAL,
	 * as there is no authority to require, but verify()'s error is fatal. */
	mbedtls_ssl_config *ssl = (mbedtls_ssl_config *)c;
	mbedtls_ssl_conf_authmode(ssl, MBEDTLS_SSL_VERIFY_OPTIONAL);
	/* no resumption; a ticket would also hold up what follows it until altcp's next poll */
	mbedtls_ssl_conf_session_tickets(ssl, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
	if (tls12) {
		mbedtls_ssl_conf_sig_algs(ssl, sig_algs_12);
		mbedtls_ssl_conf_max_tls_version(ssl, MBEDTLS_SSL_VERSION_TLS1_2);
	}
	mbedtls_ssl_conf_verify(ssl, verify, (void *)pin);
	return c;
}


/* One ticket key for all: altcp's own option leaks one with each configuration */
void tls_tickets(struct altcp_tls_config *conf)
{
	static mbedtls_ssl_ticket_context t;
	static bool ready;

	if (!ready) {
		mbedtls_ssl_ticket_init(&t);
		if (mbedtls_ssl_ticket_setup(&t, rng_get, NULL, MBEDTLS_CIPHER_AES_256_GCM, 3600)) {
			mbedtls_ssl_ticket_free(&t);
			return;
		}
		ready = true;
	}
	mbedtls_ssl_conf_session_tickets_cb((mbedtls_ssl_config *)conf, mbedtls_ssl_ticket_write, mbedtls_ssl_ticket_parse, &t);
}


bool tls_pin_parse(const char *s, uint8_t pin[TLS_PIN_LEN])
{
	int n = 0;

	for (; *s && *s != ' '; s++) {
		int v;

		if (*s == ':' && !(n & 1))
			continue;
		if (*s >= '0' && *s <= '9')
			v = *s - '0';
		else if ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'f')
			v = (*s | 0x20) - 'a' + 10;
		else
			return false;
		if (n == 2 * TLS_PIN_LEN)
			return false;
		pin[n / 2] = n & 1 ? pin[n / 2] | v : v << 4;
		n++;
	}
	return n == 2 * TLS_PIN_LEN;
}
