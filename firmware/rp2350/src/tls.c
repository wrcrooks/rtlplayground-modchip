/*
 * HTTPS: the key and self-signed certificate, made on the device and kept
 * in their own flash sector, below the configuration's. A new key comes
 * with "https newcert", or the first "https on" on a blank sector. The
 * certificate names the switch by its hostname at the time it was made;
 * browsers warn about it as about any self-signed one, so check its
 * fingerprint ("https" prints it) the first time.
 */

#include <stdio.h>
#include <string.h>

#include "pico/flash.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/sha256.h"

#include "net.h"
#include "tls_cert.h"
#include "plat.h"

#define TLS_OFFSET		(PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)
#define TLS_MAGIC		"MCTLS1"
#define DER_MAX			1024

struct tls_store {
	char magic[8];
	uint16_t key_len, cert_len;
	uint8_t key[DER_MAX];
	uint8_t cert[DER_MAX];
};

/* mbedTLS's clock for TLS session timing, MBEDTLS_PLATFORM_MS_TIME_ALT */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return to_ms_since_boot(get_absolute_time());
}

static struct altcp_tls_config *tls_conf;
static bool enabled;
static uint8_t sector[FLASH_SECTOR_SIZE];

static const struct tls_store *stored(void)
{
	const struct tls_store *s = (const struct tls_store *)(XIP_BASE + TLS_OFFSET);

	if (memcmp(s->magic, TLS_MAGIC, sizeof(TLS_MAGIC)) || !s->key_len || s->key_len > DER_MAX
	    || !s->cert_len || s->cert_len > DER_MAX)
		return NULL;
	return s;
}


static void store_program(void *p)
{
	(void)p;
	flash_range_erase(TLS_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(TLS_OFFSET, sector, FLASH_SECTOR_SIZE);
}


/* A P-256 key and a certificate for it, signed by itself, into the sector */
static bool make_cert(void)
{
	struct tls_store *s = (struct tls_store *)sector;
	size_t key_len = DER_MAX, cert_len = DER_MAX;
	bool ok;

	printf("https: making a key and certificate\n");
	memset(sector, 0xff, sizeof(sector));
	memcpy(s->magic, TLS_MAGIC, sizeof(TLS_MAGIC));
	ok = tls_make_cert(hostname[0] ? hostname : "rtlplayground", s->key, &key_len, s->cert, &cert_len) == 0;
	if (ok) {
		s->key_len = key_len;
		s->cert_len = cert_len;
		ok = flash_safe_execute(store_program, NULL, 100) == PICO_OK;
	}
	memset(sector, 0, sizeof(sector));
	if (!ok)
		printf("https: making the certificate failed\n");
	return ok;
}


static bool start(void)
{
	const struct tls_store *s = stored();

	if (!s && (!make_cert() || !(s = stored())))
		return false;
	tls_conf = altcp_tls_create_config_server_privkey_cert(s->key, s->key_len, NULL, 0, s->cert, s->cert_len);
	if (!tls_conf) {
		printf("https: the stored key or certificate doesn't load\n");
		return false;
	}
	if (!httpd_tls_start(tls_conf)) {
		altcp_tls_free_config(tls_conf);
		tls_conf = NULL;
		return false;
	}
	return true;
}


static void stop(void)
{
	httpd_tls_stop();
	if (tls_conf) {
		altcp_tls_free_config(tls_conf);
		tls_conf = NULL;
	}
}


bool plat_https_enable(bool on)
{
	if (on == enabled)
		return true;
	if (on && !start())
		return false;
	if (!on)
		stop();
	enabled = on;
	return true;
}


bool plat_https_enabled(void)
{
	return enabled;
}


bool plat_https_newcert(void)
{
	if (enabled)
		stop();
	bool ok = make_cert();
	if (enabled && !start())
		enabled = false;
	return ok;
}


void plat_https_status(char *fp, size_t len)
{
	const struct tls_store *s = stored();
	uint8_t h[32];

	if (!s) {
		snprintf(fp, len, "none yet");
		return;
	}
	mbedtls_sha256(s->cert, s->cert_len, h, 0);
	fp[0] = 0;
	for (int i = 0; i < 32 && 3 * i + 3 <= (int)len; i++)
		snprintf(fp + 3 * i, len - 3 * i, i < 31 ? "%02X:" : "%02X", h[i]);
}
