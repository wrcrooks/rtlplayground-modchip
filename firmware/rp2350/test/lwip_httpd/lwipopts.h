/* The firmware's lwIP options for test_httpd.c on the host: altcp as the
 * web server uses it, TLS left out, and a loopback interface for a client
 * in the same stack */
#include "../../src/lwipopts.h"
#undef LWIP_ALTCP_TLS_MBEDTLS
#define LWIP_ALTCP_TLS_MBEDTLS		0
#undef LWIP_SNMP
#define LWIP_SNMP			0
#define LWIP_NETIF_LOOPBACK		1
#define LWIP_HAVE_LOOPIF		1
#define LWIP_LOOPBACK_MAX_PBUFS		0
#undef LWIP_SINGLE_NETIF
#define LWIP_SINGLE_NETIF		0
