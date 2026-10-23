/* The firmware's lwIP options for test_snmp.c on the host, without HTTPS */
#include "../../src/lwipopts.h"
#undef LWIP_ALTCP
#define LWIP_ALTCP			0
#undef LWIP_ALTCP_TLS
#define LWIP_ALTCP_TLS			0
#undef LWIP_ALTCP_TLS_MBEDTLS
#define LWIP_ALTCP_TLS_MBEDTLS		0
