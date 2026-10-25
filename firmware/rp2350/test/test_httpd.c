/*
 * The web server on the host: httpd.c and upload.c on lwIP with the
 * firmware's options, a TCP client in the same stack over the loopback
 * interface, and the switch's flash simulated. Logs in and uploads
 * firmware images as RTLPlayground's web UI does: the good one must be
 * staged intact and answered "OK" before the reboot; bad ones, and uploads
 * without a login, must get their "NO" after the whole body, not a reset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/altcp_tls.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#include "bridge.h"
#include "link.h"
#include "metrics.h"
#include "net.h"
#include "plat.h"
#include "upload.h"
#include "web_assets.h"

#define FLASH_SIZE	0x80000
#define STAGE		0x3f000
#define IMAGE		0x3f000
#define BOUNDARY	"----WebKitFormBoundary7MA4YWxkTrZu0gW"

/* ---- what httpd.c uses from the rest of the firmware ---- */
struct bridge_hello bridge_hello = { .flash_size = FLASH_SIZE, .image_size = IMAGE, .upload_start = STAGE };
uint8_t outbuf[2500];
uint16_t slen, cont_len, session_timeout = 200;
const struct web_asset web_assets[] = { { 0 } };
static int reboots, flash_ops;
static uint8_t flash[FLASH_SIZE];

bool bridge_is_up(void) { return true; }
uint32_t plat_uptime_s(void) { return time(NULL); }
uint32_t plat_random32(void) { return rand(); }
void plat_reboot(void) { reboots++; }
bool plat_config_write(const uint8_t *src, uint32_t len) { (void)src; (void)len; return true; }
bool rtl_http_get(char *path)
{
	if (strcmp(path, "/status.json"))
		return false;
	slen = snprintf((char *)outbuf, sizeof(outbuf), "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n[]");
	return true;
}
void rtl_http_cmd(char *body)
{
	slen = snprintf((char *)outbuf, sizeof(outbuf), "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nran: %s\n", body);
}
uint16_t rtl_http_cont(uint8_t *dst, uint16_t max) { (void)dst; (void)max; return 0; }
bool rtl_password_ok(const char *v) { return !strncmp(v, "1234", 4) && (!v[4] || v[4] == '&'); }
/* No TLS on the host unless TEST_HTTPD_TLS, no DHCP options to keep */
#ifndef TEST_HTTPD_TLS
struct altcp_pcb *altcp_tls_wrap(struct altcp_tls_config *config, struct altcp_pcb *inner) { (void)config; (void)inner; return NULL; }
#endif
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
size_t metrics_json(char *buf, size_t max) { return snprintf(buf, max, "{}"); }
uint8_t *metrics_history(size_t *len) { *len = 0; return NULL; }

u32_t sys_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

bool upload_flash_erase(uint32_t addr)
{
	flash_ops++;
	if (addr < STAGE || addr + UPLOAD_SECTOR > FLASH_SIZE)
		return false;	/* the bridge refuses writes below upload_start */
	memset(flash + addr, 0xff, UPLOAD_SECTOR);
	return true;
}

bool upload_flash_write(uint32_t addr, const uint8_t *p, uint16_t n)
{
	flash_ops++;
	if (addr < STAGE || (addr % UPLOAD_PAGE) + n > UPLOAD_PAGE)
		return false;
	for (int i = 0; i < n; i++)
		flash[addr + i] &= p[i];
	return true;
}

bool upload_flash_read(uint32_t addr, uint8_t *p, uint16_t n)
{
	flash_ops++;
	memcpy(p, flash + addr, n);
	return true;
}

/* ---- a client in the same lwIP ---- */
static struct {
	struct tcp_pcb *pcb;
	const uint8_t *tx;
	size_t tx_len, tx_off;
	char rx[8192];
	size_t rx_len;
	bool done, reset;
} cl;
static int failures;

static void check(int cond, const char *what)
{
	printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
	failures += !cond;
}

static void c_send(void)
{
	while (cl.pcb && cl.tx_off < cl.tx_len && tcp_sndbuf(cl.pcb)) {
		size_t n = cl.tx_len - cl.tx_off;
		if (n > tcp_sndbuf(cl.pcb))
			n = tcp_sndbuf(cl.pcb);
		if (tcp_write(cl.pcb, cl.tx + cl.tx_off, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
			break;
		cl.tx_off += n;
	}
	if (cl.pcb)
		tcp_output(cl.pcb);
}

static err_t c_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	(void)arg;
	(void)err;
	if (!p) {
		cl.done = true;
		tcp_close(pcb);
		cl.pcb = NULL;
		return ERR_OK;
	}
	cl.rx_len += pbuf_copy_partial(p, cl.rx + cl.rx_len, sizeof(cl.rx) - 1 - cl.rx_len, 0);
	cl.rx[cl.rx_len] = 0;
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}

static err_t c_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	(void)arg;
	(void)pcb;
	(void)len;
	c_send();
	return ERR_OK;
}

static void c_err(void *arg, err_t err)
{
	(void)arg;
	(void)err;
	cl.reset = cl.done = true;
	cl.pcb = NULL;
}

static err_t c_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	(void)arg;
	(void)pcb;
	(void)err;
	c_send();
	return ERR_OK;
}

#ifdef TEST_HTTPD_TLS
#include "mbedtls/platform_time.h"
#include "tls_cert.h"

/* The RP2350's TRNG and clock, as pico_mbedtls and tls.c provide them */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
	FILE *f = fopen("/dev/urandom", "rb");

	(void)data;
	*olen = f ? fread(out, 1, len, f) : 0;
	if (f)
		fclose(f);
	return *olen == len ? 0 : -1;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (mbedtls_ms_time_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool tls_c_gone;
static void tls_c_err(void *arg, err_t err)
{
	(void)arg;
	(void)err;
	tls_c_gone = true;
}
#endif

/* One request: returns the response; the main loop runs meanwhile */
static const char *request(const uint8_t *req, size_t len)
{
	ip_addr_t lo;
	uint32_t t0 = sys_now(), last_second = sys_now();

	memset(&cl, 0, sizeof(cl));
	cl.tx = req;
	cl.tx_len = len;
	cl.pcb = tcp_new();
	tcp_recv(cl.pcb, c_recv);
	tcp_sent(cl.pcb, c_sent);
	tcp_err(cl.pcb, c_err);
	IP_ADDR4(&lo, 127, 0, 0, 1);
	tcp_connect(cl.pcb, &lo, 80, c_connected);
	while (!cl.done && sys_now() - t0 < 60000) {
		netif_poll_all();
		sys_check_timeouts();
		httpd_poll();
		c_send();
		if (sys_now() - last_second >= 1000) {
			last_second = sys_now();
			httpd_second();
		}
	}
	return cl.rx;
}

static char hdrs[1024];
static uint8_t req[IMAGE * 2 + 8192];

static size_t upload_request(const uint8_t *img, size_t n, const char *cookie)
{
	char head[512];
	int hn = sprintf(head, "--%s\r\nContent-Disposition: form-data; name=\"uploadedfile\"; filename=\"fw.bin\"\r\n"
			 "Content-Type: application/octet-stream\r\n\r\n", BOUNDARY);
	char tail[128];
	int tn = sprintf(tail, "\r\n--%s--\r\n", BOUNDARY);
	int h = sprintf(hdrs, "POST /upload HTTP/1.1\r\nHost: switch\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
			"Content-Length: %zu\r\n%s%s%s\r\n", BOUNDARY, hn + n + tn, cookie ? "Cookie: " : "",
			cookie ? cookie : "", cookie ? "\r\n" : "");
	memcpy(req, hdrs, h);
	memcpy(req + h, head, hn);
	memcpy(req + h + hn, img, n);
	memcpy(req + h + hn + n, tail, tn);
	return h + hn + n + tn;
}

static void make_image(uint8_t *img, size_t n, unsigned seed)
{
	srand(seed);
	for (size_t i = 0; i < n - 2; i++)
		img[i] = rand();
	img[0] = 0x00;	/* the 8051's code starts 00 40 */
	img[1] = 0x40;
	uint16_t c = ~link_crc16(0, img, n - 2);
	img[n - 2] = c;
	img[n - 1] = c >> 8;
}

static uint8_t img[0x80000];	/* up to the standard layout's image, for the wrong-layout case */

int main(void)
{
	char cookie[64] = "";
	const char *r;

	lwip_init();
	netif_set_default(netif_list);	/* the loopback interface */
	httpd_init();
	memset(flash, 0x5a, sizeof(flash));

	r = request((const uint8_t *)"GET /status.json HTTP/1.1\r\n\r\n", 29);
	check(!strncmp(r, "HTTP/1.1 401", 12), "no session: 401");

	const char *login = "POST /login HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
			    "Content-Length: 8\r\n\r\npwd=1234";
	r = request((const uint8_t *)login, strlen(login));
	const char *sc = strstr(r, "Set-Cookie: session=");
	check(!strncmp(r, "HTTP/1.1 302", 12) && sc, "login: 302 with a session cookie");
	if (sc)
		sprintf(cookie, "session=%.12s", sc + 20);

	/* The session cookie opens the JSON pages and the command line */
	static char rq[512];
	int k = sprintf(rq, "GET /status.json HTTP/1.1\r\nCookie: lang=en; %s\r\n\r\n", cookie);
	r = request((const uint8_t *)rq, k);
	check(!strncmp(r, "HTTP/1.1 200", 12), "GET with the session cookie: 200");
	k = sprintf(rq, "POST /cmd HTTP/1.1\r\nCookie: %s\r\nContent-Length: 11\r\n\r\nvlan 10 1 2", cookie);
	r = request((const uint8_t *)rq, k);
	check(strstr(r, "200 OK") && strstr(r, "ran: vlan 10 1 2"), "POST /cmd with the session cookie runs the command");
	k = sprintf(rq, "GET /status.json HTTP/1.1\r\nCookie: session=000000000000\r\n\r\n");
	r = request((const uint8_t *)rq, k);
	check(!strncmp(r, "HTTP/1.1 401", 12), "a wrong session cookie: 401");

	/* Not logged in: the refusal comes after the whole body */
	make_image(img, IMAGE, 1);
	size_t n = upload_request(img, IMAGE, NULL);
	r = request(req, n);
	check(!cl.reset && strstr(r, "401 Unauthorized") && strstr(r, "NO: not logged in") && cl.tx_off == n,
	      "upload without a login: 401 once the body is sent, no reset");

	/* A good image */
	flash_ops = 0;
	uint32_t t0 = sys_now();
	n = upload_request(img, IMAGE, cookie);
	r = request(req, n);
	check(!cl.reset && strstr(r, "200 OK") && strstr(r, "OK: checksum verified, rebooting"), "a good image: 200 OK");
	check(!memcmp(flash + STAGE, img, IMAGE), "  staged intact");
	check(flash[STAGE - 1] == 0x5a, "  nothing below the staging area touched");
	printf("      %zu bytes, %d flash operations, %u ms\n", n, flash_ops, sys_now() - t0);
	for (int i = 0; i < 3 && !reboots; i++)
		httpd_second();
	check(reboots == 1, "  and the reboot follows the answer");

	/* A bad CRC, after the good one: no longer installable */
	make_image(img, IMAGE, 2);
	img[12345] ^= 0x40;
	n = upload_request(img, IMAGE, cookie);
	r = request(req, n);
	check(!cl.reset && strstr(r, "400 Bad Request") && strstr(r, "NO: bad image CRC"), "a bad CRC: 400 with the reason");
	check(link_crc16(0, flash + STAGE, IMAGE) != 0xb001, "  and nothing installable left in the staging area");

	/* The other layout's image */
	make_image(img, 0x80000 - 0x1000, 3);
	n = upload_request(img, 0x80000 - 0x1000, cookie);
	r = request(req, n);
	check(!cl.reset && strstr(r, "400 Bad Request") && strstr(r, "longer"), "an image for the other layout: 400");

	/* A client that gives up halfway */
	make_image(img, IMAGE, 4);
	n = upload_request(img, IMAGE, cookie);
	memset(&cl, 0, sizeof(cl));
	cl.tx = req;
	cl.tx_len = n / 2;
	{
		ip_addr_t lo;
		IP_ADDR4(&lo, 127, 0, 0, 1);
		cl.pcb = tcp_new();
		tcp_recv(cl.pcb, c_recv);
		tcp_sent(cl.pcb, c_sent);
		tcp_err(cl.pcb, c_err);
		tcp_connect(cl.pcb, &lo, 80, c_connected);
		uint32_t s = sys_now();
		while (cl.tx_off < cl.tx_len && sys_now() - s < 10000) {
			netif_poll_all();
			sys_check_timeouts();
			httpd_poll();
			c_send();
		}
		tcp_abort(cl.pcb);
		for (int i = 0; i < 1000; i++) {
			netif_poll_all();
			sys_check_timeouts();
			httpd_poll();
		}
	}
	check(flash[STAGE] == 0xff && link_crc16(0, flash + STAGE, IMAGE) != 0xb001,
	      "a client gone halfway: the staging area is invalidated");
	r = request(req, upload_request(img, IMAGE, cookie));
	check(strstr(r, "200 OK") != NULL, "  and the next upload goes through");

#ifdef TEST_HTTPD_TLS
	/* HTTPS on, off and on again, as "https on|off" and the System page's
	 * switch do it, the second time with a client connected on 443. Closing
	 * the TLS listener used to trip lwIP's "invalid socket state" assert. */
	static uint8_t key[1024], crt[1024];
	size_t kl = sizeof(key), crl = sizeof(crt);
	check(tls_make_cert("sim-switch", key, &kl, crt, &crl) == 0, "TLS: key and certificate made");
	for (int round = 0; round < 2; round++) {
		struct altcp_tls_config *conf = altcp_tls_create_config_server_privkey_cert(key, kl, NULL, 0, crt, crl);
		check(conf && httpd_tls_start(conf), round ? "TLS: listening on 443 again" : "TLS: listening on 443");
		struct tcp_pcb *c = NULL;
		if (round) {
			ip_addr_t lo;
			IP_ADDR4(&lo, 127, 0, 0, 1);
			c = tcp_new();
			tcp_err(c, tls_c_err);
			tcp_connect(c, &lo, 443, NULL);
			for (int i = 0; i < 100; i++) {
				netif_poll_all();
				sys_check_timeouts();
				httpd_poll();
			}
			check(c->state == ESTABLISHED, "  a client connects on 443");
		}
		httpd_tls_stop();
		altcp_tls_free_config(conf);
		for (int i = 0; i < 100; i++) {
			netif_poll_all();
			sys_check_timeouts();
			httpd_poll();
		}
		check(true, round ? "  HTTPS off with the client connected" : "  HTTPS off");
		if (c && !tls_c_gone)
			tcp_abort(c);
		if (round)
			check(tls_c_gone, "  and the client's connection is reset");
	}
	r = request((const uint8_t *)"GET /status.json HTTP/1.1\r\n\r\n", 29);
	check(!strncmp(r, "HTTP/1.1 401", 12), "TLS: HTTP on port 80 still answers");
#endif

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
