#ifndef _TLS_CLIENT_H_
#define _TLS_CLIENT_H_

/*
 * TLS towards a server that is known by its certificate's SHA-256
 * fingerprint: no clock and no list of authorities needed. The handshake
 * fails on any other certificate, before anything is sent.
 */

#include <stdbool.h>
#include <stdint.h>

#define TLS_PIN_LEN	32

struct altcp_tls_config;

/* identity: our own key and certificate (tls.c) for a server that asks for one.
 * pin must stay where it is as long as the configuration is used. tls12: for
 * servers that don't get on with us otherwise, to be tried after a failure. */
struct altcp_tls_config *tls_client_config(const uint8_t *pin, bool identity, bool tls12);
/* 64 hex digits, with or without colons */
bool tls_pin_parse(const char *s, uint8_t pin[TLS_PIN_LEN]);

/* tls.c: our key and certificate, DER; made if there are none. False: no luck */
struct tls_identity {
	const uint8_t *key, *cert;
	uint16_t key_len, cert_len;
};
bool tls_identity(struct tls_identity *id);

/* For a server's configuration: resumption by tickets, TLS 1.3's way */
void tls_tickets(struct altcp_tls_config *conf);

#endif
