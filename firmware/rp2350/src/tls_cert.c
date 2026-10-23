/*
 * The web server's key and self-signed certificate, see tls_cert.h
 */

#include <stdio.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#include "tls_cert.h"

#define DER_BUF		1024

int tls_make_cert(const char *cn, uint8_t *key_out, size_t *key_len, uint8_t *cert_out, size_t *cert_len)
{
	static uint8_t buf[DER_BUF];
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_pk_context key;
	mbedtls_x509write_cert crt;
	char subject[96];
	uint8_t serial[16];
	int n, ret = -1;

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_pk_init(&key);
	mbedtls_x509write_crt_init(&crt);

	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const uint8_t *)"modchip-tls", 11)
	    || mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))
	    || mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg)
	    || mbedtls_ctr_drbg_random(&drbg, serial, sizeof(serial)))
		goto out;
	serial[0] &= 0x7f;	/* a positive number */
	snprintf(subject, sizeof(subject), "CN=%s,O=RTLPlayground mod chip", cn);

	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&crt, &key);
	mbedtls_x509write_crt_set_issuer_key(&crt, &key);
	if (mbedtls_x509write_crt_set_subject_name(&crt, subject)
	    || mbedtls_x509write_crt_set_issuer_name(&crt, subject)
	    || mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))
	    /* The switch has no clock until NTP syncs: a wide validity */
	    || mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959")
	    || mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)
	    || mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE))
		goto out;

	/* Both writers fill the buffer from its end */
	n = mbedtls_x509write_crt_der(&crt, buf, sizeof(buf), mbedtls_ctr_drbg_random, &drbg);
	if (n <= 0 || (size_t)n > *cert_len)
		goto out;
	memcpy(cert_out, buf + sizeof(buf) - n, n);
	*cert_len = n;
	n = mbedtls_pk_write_key_der(&key, buf, sizeof(buf));
	if (n <= 0 || (size_t)n > *key_len)
		goto out;
	memcpy(key_out, buf + sizeof(buf) - n, n);
	*key_len = n;
	ret = 0;
out:
	memset(buf, 0, sizeof(buf));
	mbedtls_x509write_crt_free(&crt);
	mbedtls_pk_free(&key);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	return ret;
}
