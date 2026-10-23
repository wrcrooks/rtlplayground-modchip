#ifndef _MBEDTLS_CONFIG_H_
#define _MBEDTLS_CONFIG_H_

/*
 * mbedTLS for the web server's HTTPS: a TLS 1.2 server with an ECDSA P-256
 * key and a self-signed certificate made on the device (tls.c).
 * ECDHE over X25519 or P-256; AES-GCM or ChaCha20-Poly1305.
 */

/* Platform: the RP2350's TRNG feeds the entropy pool (pico_mbedtls) */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT	/* mbedtls_ms_time() in tls.c */

/* The SDK's altcp_tls_mbedtls.c reads fields mbedTLS 3 marks private */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* Randomness */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* Hashes and ciphers */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C

/* Public key */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C

/* Certificates: parsing ours, and writing it in the first place */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_CACHE_C		/* session resumption, see lwipopts.h */

/* A browser may send full 16 KiB records; ours stay small (httpd.c writes 2 KiB at a time) */
#define MBEDTLS_SSL_IN_CONTENT_LEN	16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN	4096

#define MBEDTLS_SSL_CIPHERSUITES \
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, \
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384

#endif
