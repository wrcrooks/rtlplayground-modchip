/* The firmware's lwIP options for test_httpd_tls: as lwip_httpd's, with
 * TLS kept */
#include "../../src/lwipopts.h"
#undef LWIP_SNMP
#define LWIP_SNMP			0
#define LWIP_NETIF_LOOPBACK		1
#define LWIP_HAVE_LOOPIF		1
#define LWIP_LOOPBACK_MAX_PBUFS		0
#undef LWIP_SINGLE_NETIF
#define LWIP_SINGLE_NETIF		0
