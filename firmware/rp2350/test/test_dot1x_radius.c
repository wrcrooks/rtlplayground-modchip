/*
 * dot1x.c against a real RADIUS server: EAP-MD5 accepted and rejected, MAB
 * accepted and rejected. run_dot1x_radius.sh starts FreeRADIUS in Docker with
 * the users this expects and runs it; the server is at 127.0.0.1:1812, secret
 * testing123.
 */
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dot1x.h"

static int failures, checks;
#define check(c, ...) do { checks++; if (c) { printf("PASS  "); } else { printf("FAIL  "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

static int sock;
static const uint8_t sw_mac[6] = { 0x06, 0x0f, 0x00, 0xdd, 0x3b, 0x3e };

uint16_t dot1x_hw_ports(void) { return 0x1f8; }
uint8_t dot1x_hw_front_port(uint8_t port) { return port - 2; }
bool dot1x_hw_link(uint8_t port) { (void)port; return false; }
void dot1x_hw_random(uint8_t *p, uint8_t n) { FILE *f = fopen("/dev/urandom", "rb"); fread(p, 1, n, f); fclose(f); }
void dot1x_hw_nas_ip(uint8_t ip[4]) { ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1; }
const uint8_t *dot1x_hw_mac(void) { return sw_mac; }
void dot1x_hw_log(const char *msg, uint8_t port) { printf("      log: %s %u\n", msg, port); }
void dot1x_hw_gate(uint16_t blocked, bool on) { (void)blocked; (void)on; }

void dot1x_hw_radius_tx(const uint8_t *p, uint16_t len)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1812) };
	a.sin_addr.s_addr = htonl(0x7f000001);
	sendto(sock, p, len, 0, (struct sockaddr *)&a, sizeof(a));
}

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
		struct pollfd pf = { sock, POLLIN, 0 };
		while (poll(&pf, 1, 100) > 0) {
			ssize_t n = recv(sock, buf, sizeof(buf), 0);
			if (n > 0)
				dot1x_radius_rx(buf, n);
			while (pending_port >= 0)
				supplicant();
		}
	}
}

int main(void)
{
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	dot1x_cfg.enabled = true;
	strcpy(dot1x_cfg.secret, "testing123");
	dot1x_cfg.mode[4] = DOT1X_AUTO;
	dot1x_cfg.mode[6] = DOT1X_AUTO;
	dot1x_cfg.mab[6] = true;
	dot1x_init();
	dot1x_apply();

	user = "alice"; password = "secret";
	dot1x_link(4, true);
	run(3);
	check(successes == 1 && dot1x_port_status(4)->authorized, "FreeRADIUS: EAP-MD5 alice accepted");

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
