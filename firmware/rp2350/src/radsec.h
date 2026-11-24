#ifndef _RADSEC_H_
#define _RADSEC_H_

/*
 * RADIUS over TLS (RFC 6614), in place of dot1x_udp.c's UDP when "dot1x
 * radsec" names the server's certificate. Both sides show one: ours is the
 * web server's ("https cert" prints it, for the RADIUS server to trust).
 * The connection is made for the first request and kept.
 */

#include <stdbool.h>
#include <stdint.h>

#define RADSEC_PORT	2083

bool radsec_on(void);
void radsec_tx(const uint8_t *p, uint16_t len);
void radsec_second(void);

#endif
