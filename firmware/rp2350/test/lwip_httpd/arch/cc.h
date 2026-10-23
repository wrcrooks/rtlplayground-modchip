/* lwIP's port for test_snmp.c on the host */
#ifndef _ARCH_CC_H_
#define _ARCH_CC_H_
#include <stdio.h>
#include <stdlib.h>
typedef int sys_prot_t;
#define LWIP_PLATFORM_DIAG(x)		do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x)		do { fprintf(stderr, "lwIP assert: %s\n", x); abort(); } while (0)
#define LWIP_RAND()			((u32_t)rand())
#endif
