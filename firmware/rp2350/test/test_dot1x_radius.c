/*
 * dot1x.c against a real RADIUS server: EAP-MD5 accepted and rejected, MAB
 * accepted and rejected, a VLAN from the server. run_dot1x_radius.sh starts FreeRADIUS in Docker with
 * the users this expects and runs it; the server is at 127.0.0.1:1812, secret
 * testing123. Built with RADSEC, the same over TLS through radsec.c, to port
 * 2083: test_radsec cert <dir> makes our certificate, test_radsec <dir>
 * <server's fingerprint> runs the test.
 */
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dot1x.h"
#ifdef RADSEC
#include <stdlib.h>
#include <time.h>
#include "lwip/init.h"
#include "psa/crypto.h"
#include "events.h"
#include "plat.h"
#include "radsec.h"
#include "relay.h"
#include "tls_cert.h"
#include "tls_client.h"
#endif

static int failures, checks;
#define check(c, ...) do { checks++; if (c) { printf("PASS  "); } else { printf("FAIL  "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

static int sock;
static uint16_t vlan_set;
static const uint8_t sw_mac[6] = { 0x06, 0x0f, 0x00, 0xdd, 0x3b, 0x3e };

uint16_t dot1x_hw_ports(void) { return 0x1f8; }
uint8_t dot1x_hw_front_port(uint8_t port) { return port - 2; }
bool dot1x_hw_link(uint8_t port) { (void)port; return false; }
void dot1x_hw_random(uint8_t *p, uint8_t n) { FILE *f = fopen("/dev/urandom", "rb"); fread(p, 1, n, f); fclose(f); }
void dot1x_hw_nas_ip(uint8_t ip[4]) { ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1; }
const uint8_t *dot1x_hw_mac(void) { return sw_mac; }
void dot1x_hw_log(const char *msg, uint8_t port) { printf("      log: %s %u\n", msg, port); }
bool dot1x_hw_vlan(uint8_t port, uint16_t vlan) { (void)port; vlan_set = vlan; return true; }
void dot1x_hw_gate(uint16_t blocked, uint16_t learn, bool on) { (void)blocked; (void)learn; (void)on; }

#ifdef RADSEC
static uint8_t key[1024], cert[1024];
static size_t key_len = sizeof(key), cert_len = sizeof(cert);
bool tls_identity(struct tls_identity *id)
{
	id->key = key; id->key_len = key_len; id->cert = cert; id->cert_len = cert_len;
	return true;
}
u32_t sys_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
uint32_t plat_uptime_s(void) { return sys_now() / 1000; }
const struct ip6_addr *net_ip6_gw(struct netif *n, const struct ip6_addr *d) { return NULL; }
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
void net_dhcp_append(struct dhcp_msg *msg, unsigned char type, unsigned short *len) { }
static void on_event(const struct event *e) { printf("      event: %s\n", e->text); }

static void file(const char *dir, const char *name, uint8_t *b, size_t *n, bool out)
{
	char path[300];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, out ? "wb" : "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	*n = out ? fwrite(b, 1, *n, f) : fread(b, 1, *n, f);
	fclose(f);
}

void dot1x_hw_radius_tx(const uint8_t *p, uint16_t len) { radsec_tx(p, len); }
#else
void dot1x_hw_radius_tx(const uint8_t *p, uint16_t len)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1812) };
	a.sin_addr.s_addr = htonl(0x7f000001);
	sendto(sock, p, len, 0, (struct sockaddr *)&a, sizeof(a));
}

#endif

/* The supplicant: EAP-MD5 */
static const char *user, *password;
static uint8_t smac[6] = { 0x02, 0, 0, 0, 0, 0x04 };
static int successes, eap_failures;
static uint8_t pending[1600];
static uint16_t pending_len;
static int pending_port = -1;

void dot1x_hw_eapol_tx(uint8_t port, const uint8_t *pdu, uint16_t len)
{
	/* answered from the main loop, not from inside dot1x.c */
	memcpy(pending, pdu, len);
	pending_len = len;
	pending_port = port;
}

static void supplicant(void)
{
	uint8_t f[200], *r = f + 4, *e = pending + 4;
	int port = pending_port;

	pending_port = -1;
	if (port != 4 || pending[1] != 0)	/* the device on port 6 has no supplicant */
		return;
	if (e[0] == 3) { successes++; return; }
	if (e[0] == 4) { eap_failures++; return; }
	r[0] = 2; r[1] = e[1];
	uint16_t n;
	if (e[4] == 1) {
		n = strlen(user);
		r[4] = 1; memcpy(r + 5, user, n); n += 5;
	} else if (e[4] == 4) {
		uint8_t buf[200];
		buf[0] = e[1];
		memcpy(buf + 1, password, strlen(password));
		memcpy(buf + 1 + strlen(password), e + 6, e[5]);
		r[4] = 4; r[5] = 16;
		dot1x_md5(buf, 1 + strlen(password) + e[5], NULL, 0, r + 6);
		n = 22;
	} else {
		r[4] = 3; r[5] = 4; n = 6;	/* NAK: we only do MD5 */
	}
	r[2] = n >> 8; r[3] = n;
	f[0] = 2; f[1] = 0; f[2] = n >> 8; f[3] = n;
	dot1x_eapol_rx(port, smac, f, n + 4);
}

/* Run for up to s seconds of protocol time; the server answers in real time */
static void run(int s)
{
	uint8_t buf[2000];

	for (int t = 0; t < s * 10; t++) {
		if (t % 10 == 0)
			dot1x_second();
		while (pending_port >= 0)
			supplicant();
#ifdef RADSEC
		(void)buf;
		if (t % 10 == 0)
			radsec_second();
		for (int k = 0; k < 100; k++) {
			relay_poll();
			while (pending_port >= 0)
				supplicant();
			usleep(1000);
		}
		continue;
#else
		struct pollfd pf = { sock, POLLIN, 0 };
		while (poll(&pf, 1, 100) > 0) {
			ssize_t n = recv(sock, buf, sizeof(buf), 0);
			if (n > 0)
				dot1x_radius_rx(buf, n);
			while (pending_port >= 0)
				supplicant();
		}
#endif
	}
}

int main(int argc, char **argv)
{
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	dot1x_cfg.enabled = true;
#ifdef RADSEC
	psa_crypto_init();
	if (argc > 2 && !strcmp(argv[1], "cert")) {
		tls_make_cert("testswitch", key, &key_len, cert, &cert_len);
		file(argv[2], "key.der", key, &key_len, true);
		file(argv[2], "cert.der", cert, &cert_len, true);
		return 0;
	}
	file(argv[1], "key.der", key, &key_len, false);
	file(argv[1], "cert.der", cert, &cert_len, false);
	lwip_init();
	netif_set_default(netif_list);
	event_listen(on_event);
	relay_out(2083, 2083);
	memcpy(dot1x_cfg.radius_ip, "\x7f\0\0\x01", 4);
	dot1x_cfg.radsec = plat_radsec(argv[2]);
	dot1x_cfg.radius_port = 2083;
	if (argc > 3) {		/* a server that isn't the one */
		dot1x_cfg.mode[4] = DOT1X_AUTO;
		dot1x_init();
		dot1x_apply();
		user = "alice"; password = "secret";
		dot1x_link(4, true);
		run(12);
		check(!dot1x_port_status(4)->authorized && !successes, "another certificate: nobody gets in");
		return failures ? 1 : 0;
	}
#else
	(void)argc; (void)argv;
	strcpy(dot1x_cfg.secret, "testing123");
#endif
	dot1x_cfg.radius_vlan = true;
	dot1x_cfg.mode[4] = DOT1X_AUTO;
	dot1x_cfg.mode[6] = DOT1X_AUTO;
	dot1x_cfg.mab[6] = true;
	dot1x_init();
	dot1x_apply();

	user = "alice"; password = "secret";
	dot1x_link(4, true);
	for (int t = 0; t < 12 && !successes; t++) {
		run(1);
		if (successes)
			printf("      after %d s\n", t + 1);
	}
	check(successes && dot1x_port_status(4)->authorized, "FreeRADIUS: EAP-MD5 alice accepted");

	check(!vlan_set, "no VLAN asked for");
	user = "bob";
	dot1x_restart(4);
	run(3);
	check(dot1x_port_status(4)->authorized && vlan_set == 42, "FreeRADIUS: bob goes into VLAN 42");
	user = "alice";

	password = "wrong";
	dot1x_restart(4);
	run(3);
	check(eap_failures == 1 && !dot1x_port_status(4)->authorized && dot1x_port_status(4)->state == DOT1X_S_HELD,
	      "FreeRADIUS: a wrong password rejected");

	uint8_t dev[6] = { 0x02, 0, 0, 0, 0, 0x03 };
	dot1x_link(6, true);
	dot1x_seen(6, dev);
	run(DOT1X_TX_PERIOD * DOT1X_MAB_AFTER + 3);
	check(dot1x_port_status(6)->authorized && dot1x_port_status(6)->method == DOT1X_M_MAB,
	      "FreeRADIUS: MAB for 02:00:00:00:00:03 accepted");

	uint8_t other[6] = { 0x02, 0, 0, 0, 0, 0x09 };
	dot1x_link(6, false);
	dot1x_link(6, true);
	dot1x_seen(6, other);
	run(DOT1X_TX_PERIOD * DOT1X_MAB_AFTER + 3);
	check(!dot1x_port_status(6)->authorized && dot1x_port_status(6)->state == DOT1X_S_HELD,
	      "FreeRADIUS: MAB for an unknown MAC rejected");

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
