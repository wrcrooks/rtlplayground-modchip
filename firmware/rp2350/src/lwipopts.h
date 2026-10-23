#ifndef _LWIPOPTS_H_
#define _LWIPOPTS_H_

/* lwIP without an OS: everything runs from the main loop, nothing from an ISR */
#define NO_SYS				1
#define LWIP_SOCKET			0
#define LWIP_NETCONN			0
#define SYS_LIGHTWEIGHT_PROT		0
#define LWIP_TIMERS			1

#define MEM_LIBC_MALLOC			0
#define MEM_ALIGNMENT			4
#define MEM_SIZE			(48 * 1024)
#define MEMP_NUM_PBUF			32
#define PBUF_POOL_SIZE			24
#define MEMP_NUM_TCP_PCB		8
#define MEMP_NUM_TCP_PCB_LISTEN		2
#define MEMP_NUM_TCP_SEG		32
#define MEMP_NUM_UDP_PCB		8
#define MEMP_NUM_ARP_QUEUE		8

#define LWIP_IPV4			1
#define LWIP_IPV6			0
#define LWIP_ARP			1
#define LWIP_ETHERNET			1
#define LWIP_ICMP			1
#define LWIP_RAW			0
#define LWIP_UDP			1
#define LWIP_TCP			1
#define LWIP_IGMP			0
#define LWIP_DNS			0	/* RTLPlayground's dns.c resolves */
#define LWIP_DHCP			1
#define LWIP_DHCP_DOES_ACD_CHECK	0
#define LWIP_AUTOIP			0
#define LWIP_NETIF_HOSTNAME		1
#define LWIP_NETIF_STATUS_CALLBACK	1
/* lwIP's MIB-2 groups count through the stats */
#define LWIP_STATS			1
#define MIB2_STATS			1
#define LWIP_STATS_DISPLAY		0
#define LWIP_SINGLE_NETIF		1

/* The bridge link carries about 97 KB/s, so a few full segments in flight
 * keep it busy; the ASIC strips nothing, so the full MSS fits a frame. */
#define TCP_MSS				1460
#define TCP_WND				(4 * TCP_MSS)
#define TCP_SND_BUF			(8 * TCP_MSS)
#define TCP_SND_QUEUELEN		((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_TCP_KEEPALIVE		0

#define ETH_PAD_SIZE			0

/*
 * DHCP: with LWIP_DNS off, lwIP neither asks for nor keeps the DNS servers.
 * Ask for option 6 anyway and take it through the parse hook, for dns.c.
 */
#define LWIP_DHCP_PROVIDE_DNS_SERVERS	0
#define DHCP_ADD_EXTRA_REQUEST_OPTIONS	, 6
struct pbuf;
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset);
#define LWIP_HOOK_DHCP_PARSE_OPTION(netif, dhcp, state, msg, msg_type, option, len, pbuf, offset) \
	net_dhcp_option(option, len, pbuf, offset)

/* HTTPS: the web server runs on altcp, plain or through mbedTLS */
#define LWIP_ALTCP			1
#define LWIP_ALTCP_TLS			1
#define LWIP_ALTCP_TLS_MBEDTLS		1
#define ALTCP_MBEDTLS_AUTHMODE		MBEDTLS_SSL_VERIFY_NONE

/* SNMPv2c; snmp.c owns the UDP socket in place of snmp_raw.c */
#define LWIP_SNMP			1
#define SNMP_USE_RAW			1
#define SNMP_LWIP_MIB2			1
#define SNMP_LWIP_MIB2_SYSDESC		"RTLPlayground mod chip"
#define SNMP_LWIP_ENTERPRISE_OID	0
#define SNMP_MAX_VALUE_SIZE		128

#define LWIP_DEBUG			0

#endif
