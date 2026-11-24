/* RADIUS over TLS, see radsec.h */

#include <stdio.h>
#include <string.h>

#include "lwip/altcp_tls.h"
#include "lwip/ip_addr.h"

#include "dot1x.h"
#include "events.h"
#include "plat.h"
#include "radsec.h"
#include "tls_client.h"

#define PACKET_MAX	4096		/* RFC 2865's */
#define OUT_MAX		4096
#define CONNECT_S	10
#define RETRY_S		3		/* below dot1x.c's between its tries */

static bool on;
static uint8_t pin[TLS_PIN_LEN];
static struct altcp_tls_config *conf;
static struct altcp_pcb *pcb;
static enum { DOWN, CONNECTING, UP } state;
static uint8_t timer;			/* CONNECTING: s left; DOWN: s before another try */
static bool restart, tls12, was_up, failed;
static uint8_t out[OUT_MAX], in[PACKET_MAX];
static uint16_t out_len, in_len;

bool radsec_on(void)
{
	return on;
}


static void down(const char *why)
{
	if (why && !failed)
		event(EV_DOT1X, 0, 0, "RADIUS over TLS: %s", why);
	failed = why != NULL;
	if (why && state == CONNECTING && !was_up)
		tls12 = !tls12;		/* see tls_client.c */
	was_up = false;
	state = DOWN;
	timer = why ? RETRY_S : 0;
	out_len = in_len = 0;		/* dot1x.c sends again */
}


static void drop(const char *why)
{
	struct altcp_pcb *p = pcb;

	pcb = NULL;
	if (p) {
		altcp_arg(p, NULL);
		altcp_recv(p, NULL);
		altcp_err(p, NULL);
		if (altcp_close(p) != ERR_OK)
			altcp_abort(p);
	}
	down(why);
}


static void push(void)
{
	uint16_t n = out_len, room;

	if (state != UP)
		return;
	room = altcp_sndbuf(pcb);
	if (n > room)
		n = room;
	if (n && altcp_write(pcb, out, n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
		memmove(out, out + n, out_len - n);
		out_len -= n;
		altcp_output(pcb);
	}
}


static void on_err(void *arg, err_t err)
{
	(void)arg; (void)err;
	if (!pcb)
		return;
	pcb = NULL;			/* gone already, or altcp closes it */
	down(state == UP ? "connection lost" : "no connection");
}


static err_t on_connected(void *arg, struct altcp_pcb *p, err_t err)
{
	(void)arg; (void)p;
	if (err != ERR_OK)
		return ERR_OK;
	state = UP;			/* the handshake is done, the certificate the right one */
	was_up = true;
	failed = false;
	event(EV_DOT1X, 0, 0, "RADIUS over TLS: connected");
	push();
	return ERR_OK;
}


static err_t on_recv(void *arg, struct altcp_pcb *p, struct pbuf *b, err_t err)
{
	(void)arg; (void)err;
	if (!b) {
		/* the server's idle timeout, for one: the next request connects anew */
		pcb = NULL;
		altcp_arg(p, NULL);
		altcp_recv(p, NULL);
		altcp_err(p, NULL);
		down(state == UP ? NULL : "no connection");
		if (altcp_close(p) != ERR_OK) {
			altcp_abort(p);
			return ERR_ABRT;
		}
		return ERR_OK;
	}
	altcp_recved(p, b->tot_len);
	for (uint16_t off = 0; off < b->tot_len && pcb; ) {
		uint16_t n = pbuf_copy_partial(b, in + in_len, sizeof(in) - in_len, off);

		off += n;
		in_len += n;
		while (in_len >= 4) {
			uint16_t len = in[2] << 8 | in[3];

			if (len < 20 || len > sizeof(in)) {
				/* no way to find the next one; closed from radsec_second() */
				restart = true;
				in_len = 0;
				pbuf_free(b);
				return ERR_OK;
			}
			if (in_len < len)
				break;
			dot1x_radius_rx(in, len);
			memmove(in, in + len, in_len - len);
			in_len -= len;
		}
	}
	pbuf_free(b);
	return ERR_OK;
}


static void connect_(void)
{
	static bool conf12;
	ip_addr_t ip;

	if (conf && conf12 != tls12) {
		altcp_tls_free_config(conf);
		conf = NULL;
	}
	conf12 = tls12;
	if (!conf)
		conf = tls_client_config(pin, true, tls12);
	pcb = conf ? altcp_tls_new(conf, IPADDR_TYPE_V4) : NULL;
	if (!pcb) {
		down("out of memory");
		return;
	}
	altcp_recv(pcb, on_recv);
	altcp_err(pcb, on_err);
	IP_ADDR4(&ip, dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1], dot1x_cfg.radius_ip[2], dot1x_cfg.radius_ip[3]);
	state = CONNECTING;
	timer = CONNECT_S;
	if (altcp_connect(pcb, &ip, dot1x_cfg.radius_port ? dot1x_cfg.radius_port : RADSEC_PORT, on_connected) != ERR_OK)
		drop("no connection");
}


void radsec_tx(const uint8_t *p, uint16_t len)
{
	if (state == DOWN && timer)
		return;			/* dot1x.c sends again */
	if (out_len + len > sizeof(out))
		return;
	memcpy(out + out_len, p, len);
	out_len += len;
	if (state == DOWN)
		connect_();
	else
		push();
}


void radsec_second(void)
{
	if (restart) {
		restart = false;
		tls12 = false;
		drop(NULL);
		failed = false;
		if (conf)
			altcp_tls_free_config(conf);
		conf = NULL;
	}
	if (state == CONNECTING && !--timer)
		drop("no connection");
	else if (state == DOWN && timer)
		timer--;
	else if (state == UP)
		push();
}


bool plat_radsec(const char *fingerprint)
{
	uint8_t p[TLS_PIN_LEN] = { 0 };

	if (fingerprint && !tls_pin_parse(fingerprint, p))
		return false;
	if (on != (fingerprint != NULL) || memcmp(pin, p, sizeof(p)))
		restart = true;
	on = fingerprint != NULL;
	memcpy(pin, p, sizeof(p));
	return true;
}


void plat_radsec_restart(void)
{
	restart = true;
}
