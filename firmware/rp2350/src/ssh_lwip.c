/* SSH's connections, host key and settings, see ssh.h. Command "ssh ..." is in rtl_ext.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/tcp.h"

#include "events.h"
#include "net.h"
#include "plat.h"
#include "safety.h"
#include "ssh.h"
#include "store.h"

#define CONNS		2
#define HEAP_MIN	(32 * 1024)	/* the key exchange, and a command's output */
#define OUT_MAX		6144

struct sconn {
	struct tcp_pcb *pcb;
	struct ssh *s;
	bool over;
	uint8_t linger;			/* s left to get the rest out */
	size_t out_len;
	uint8_t out[OUT_MAX];
};

static struct sconn *conns[CONNS];
static struct tcp_pcb *listener;
static bool want;
static int reboot_in;

/* ---- for ssh.c ---- */

size_t ssh_io_room(void *io)
{
	return OUT_MAX - ((struct sconn *)io)->out_len;
}


void ssh_io_write(void *io, const uint8_t *d, size_t n)
{
	struct sconn *c = io;

	memcpy(c->out + c->out_len, d, n);
	c->out_len += n;
}


bool ssh_password_ok(const char *password)
{
	return rtl_password_plain_ok(password);
}


size_t ssh_run(char *line, char *out, size_t max, bool *ok)
{
	return rtl_run(line, out, max, ok);
}


size_t ssh_backup(char *out, size_t max)
{
	return config_backup(out, max);
}


bool ssh_restore(char *text, size_t len, unsigned minutes, char *out, size_t max)
{
	if (!config_restore(text, len, minutes, out, max))
		return false;
	reboot_in = 3;
	return true;
}


const char *ssh_prompt(void)
{
	return hostname;
}


void ssh_log(bool login, bool ok, const char *user, const char *peer)
{
	char u[33];
	size_t n = 0;

	/* the user's name is whatever the peer sent */
	for (; *user && n < sizeof(u) - 1; user++)
		u[n++] = *user > ' ' && *user < 0x7f ? *user : '?';
	u[n] = 0;
	if (login)
		event(ok ? EV_LOGIN : EV_LOGIN_FAILED, 0, 0, "SSH login%s from %s (%s)", ok ? "" : " failed", peer, u);
}


/* ---- connections ---- */

static void conn_drop(struct sconn *c)
{
	for (int i = 0; i < CONNS; i++)
		if (conns[i] == c)
			conns[i] = NULL;
	if (c->pcb) {
		tcp_arg(c->pcb, NULL);
		tcp_recv(c->pcb, NULL);
		tcp_sent(c->pcb, NULL);
		tcp_err(c->pcb, NULL);
	}
	ssh_free(c->s);
	free(c);
}


/* Returns false when c is gone */
static bool conn_push(struct sconn *c)
{
	struct tcp_pcb *pcb = c->pcb;

	while (1) {
		size_t n = c->out_len, k = tcp_sndbuf(pcb);

		if (n > k)
			n = k;
		if (n && tcp_write(pcb, c->out, n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
			memmove(c->out, c->out + n, c->out_len - n);
			c->out_len -= n;
		} else {
			n = 0;
		}
		if (!c->over && !ssh_poll(c->s))
			c->over = true;
		if (!n)
			break;
	}
	tcp_output(pcb);
	if (c->over && !c->out_len) {
		conn_drop(c);
		if (tcp_close(pcb) != ERR_OK)
			tcp_abort(pcb);		/* no callback of lwIP's closes the pcb it was given */
		return false;
	}
	return true;
}


static void on_err(void *arg, err_t err)
{
	struct sconn *c = arg;

	(void)err;
	if (c) {
		c->pcb = NULL;
		conn_drop(c);
	}
}


static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct sconn *c = arg;

	(void)err;
	if (!c) {
		if (p) {
			tcp_recved(pcb, p->tot_len);
			pbuf_free(p);
		}
		return ERR_OK;
	}
	if (!p) {
		conn_drop(c);
		if (tcp_close(pcb) != ERR_OK) {
			tcp_abort(pcb);
			return ERR_ABRT;
		}
		return ERR_OK;
	}
	for (struct pbuf *q = p; q && !c->over; q = q->next)
		if (!ssh_input(c->s, q->payload, q->len))
			c->over = true;
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	/* pushed, and perhaps closed, from ssh_conn_poll(): not under lwIP's feet */
	return ERR_OK;
}


static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct sconn *c;
	int i;

	(void)arg;
	if (err != ERR_OK || !pcb)
		return ERR_VAL;
	for (i = 0; i < CONNS && conns[i]; i++)
		;
	c = i < CONNS && plat_heap_free() >= HEAP_MIN ? calloc(1, sizeof(*c)) : NULL;
	if (c)
		c->s = ssh_new(c, ipaddr_ntoa(&pcb->remote_ip));
	if (!c || !c->s) {
		free(c);
		tcp_abort(pcb);
		return ERR_ABRT;
	}
	c->pcb = pcb;
	c->linger = 5;
	conns[i] = c;
	tcp_arg(pcb, c);
	tcp_recv(pcb, on_recv);
	tcp_err(pcb, on_err);
	tcp_nagle_disable(pcb);
	return ERR_OK;
}


static void stop(void)
{
	if (listener)
		tcp_close(listener);
	listener = NULL;
	for (int i = 0; i < CONNS; i++) {
		struct sconn *c = conns[i];
		struct tcp_pcb *pcb;

		if (!c)
			continue;
		pcb = c->pcb;
		conn_drop(c);
		if (pcb)
			tcp_abort(pcb);
	}
}


static bool key_ready(void)
{
	size_t n = sizeof(store.ssh_key);

	if (store.ssh_key_len && ssh_host_key_set(store.ssh_key, store.ssh_key_len))
		return true;
	printf("ssh: making a host key\n");
	if (!ssh_host_key_make(store.ssh_key, &n))
		return false;
	store.ssh_key_len = n;
	return store_save() && ssh_host_key_set(store.ssh_key, n);
}


static void start(void)
{
	struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_ANY), *l;

	if (!p)
		return;
	if (tcp_bind(p, IP_ANY_TYPE, 22) != ERR_OK || !(l = tcp_listen_with_backlog(p, 2))) {
		tcp_close(p);
		return;
	}
	listener = l;
	tcp_accept(listener, on_accept);
}


/* Each main loop pass */
void ssh_conn_poll(void)
{
	for (int i = 0; i < CONNS; i++)
		if (conns[i])
			conn_push(conns[i]);
}


void ssh_conn_second(void)
{
	ssh_lock_second();
	if (reboot_in && !--reboot_in)
		plat_reboot();
	if (want && !listener)
		start();
	if (!want && listener)
		stop();			/* not from the command: it may have come over SSH */
	for (int i = 0; i < CONNS; i++) {
		struct sconn *c = conns[i];
		struct tcp_pcb *pcb;

		if (!c)
			continue;
		if (!c->over && !ssh_second(c->s))
			c->over = true;
		if (c->over && !c->linger--) {
			pcb = c->pcb;
			conn_drop(c);
			tcp_abort(pcb);
		}
	}
}


/* ---- settings ---- */

bool plat_ssh_enable(bool on)
{
	if (on && !key_ready())
		return false;
	want = on;
	return true;
}


bool plat_ssh_newkey(void)
{
	store.ssh_key_len = 0;
	return key_ready();
}


size_t plat_ssh_status(char *buf, size_t max)
{
	char fp[64] = "none yet";

	if (!ssh_fingerprint(fp, sizeof(fp)) && store.ssh_key_len && ssh_host_key_set(store.ssh_key, store.ssh_key_len))
		ssh_fingerprint(fp, sizeof(fp));
	return app(buf, max, 0, "SSH %s, host key %s\n", want ? "on" : "off", fp);
}
