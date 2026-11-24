/*
 * Between the host's sockets and the lwIP in a test: relay_in() listens on
 * the host and connects to a port in lwIP, relay_out() the other way round.
 * So programs like ssh or mosquitto talk to the firmware's own code.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"

struct relay {
	int fd;
	struct tcp_pcb *pcb;
	bool up, fd_eof, pcb_eof;
	struct pbuf *in;
};
static struct relay relays[16];
static int relay_listen_fd = -1;
static uint16_t relay_in_port, relay_out_port;

static struct relay *relay_new(int fd, struct tcp_pcb *pcb)
{
	for (int i = 0; i < 16; i++) {
		if (relays[i].fd > 0 || relays[i].pcb)
			continue;
		memset(&relays[i], 0, sizeof(relays[i]));
		relays[i].fd = fd;
		relays[i].pcb = pcb;
		return &relays[i];
	}
	return NULL;
}

static void relay_err(void *arg, err_t err) { ((struct relay *)arg)->pcb = NULL; }

static err_t relay_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct relay *r = arg;

	if (!p)
		r->pcb_eof = true;
	else if (r->in)
		pbuf_cat(r->in, p);
	else
		r->in = p;
	return ERR_OK;
}

static err_t relay_connected(void *arg, struct tcp_pcb *pcb, err_t err) { ((struct relay *)arg)->up = true; return ERR_OK; }

static void relay_wire(struct relay *r)
{
	tcp_arg(r->pcb, r);
	tcp_recv(r->pcb, relay_recv);
	tcp_err(r->pcb, relay_err);
	tcp_nagle_disable(r->pcb);
}

static void relay_in(uint16_t host_port, uint16_t lwip_port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(host_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int one = 1;

	relay_in_port = lwip_port;
	relay_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(relay_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(relay_listen_fd, (struct sockaddr *)&a, sizeof(a)) || listen(relay_listen_fd, 8)) {
		perror("relay");
		exit(1);
	}
	fcntl(relay_listen_fd, F_SETFL, O_NONBLOCK);
}

static err_t relay_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(relay_out_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		close(fd);
		tcp_abort(pcb);
		return ERR_ABRT;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	struct relay *r = relay_new(fd, pcb);
	r->up = true;
	relay_wire(r);
	return ERR_OK;
}

static __attribute__((unused)) void relay_out(uint16_t lwip_port, uint16_t host_port)
{
	struct tcp_pcb *p = tcp_new();

	relay_out_port = host_port;
	tcp_bind(p, IP_ADDR_ANY, lwip_port);
	tcp_accept(tcp_listen(p), relay_accept);
}

static void relay_end(struct relay *r)
{
	if (r->pcb) {
		tcp_arg(r->pcb, NULL);
		tcp_recv(r->pcb, NULL);
		tcp_err(r->pcb, NULL);
		if (tcp_close(r->pcb) != ERR_OK)
			tcp_abort(r->pcb);
	}
	if (r->in)
		pbuf_free(r->in);
	close(r->fd);
	memset(r, 0, sizeof(*r));
}

static void relay_poll(void)
{
	ip_addr_t lo;
	int fd;

	IP_ADDR4(&lo, 127, 0, 0, 1);
	while (relay_listen_fd >= 0 && (fd = accept(relay_listen_fd, NULL, NULL)) >= 0) {
		struct relay *r = relay_new(fd, tcp_new());

		fcntl(fd, F_SETFL, O_NONBLOCK);
		relay_wire(r);
		tcp_connect(r->pcb, &lo, relay_in_port, relay_connected);
	}
	for (int i = 0; i < 16; i++) {
		struct relay *r = &relays[i];
		uint8_t b[1024];
		ssize_t n;

		if (r->fd <= 0)
			continue;
		while (r->in) {
			n = write(r->fd, r->in->payload, r->in->len);
			if (n <= 0)
				break;
			if (r->pcb)
				tcp_recved(r->pcb, n);
			r->in = pbuf_free_header(r->in, n);
		}
		if (!r->pcb || r->pcb_eof) {
			if (!r->in)
				relay_end(r);
			continue;
		}
		while (r->up && r->pcb && !r->fd_eof && tcp_sndbuf(r->pcb) > sizeof(b) && tcp_sndqueuelen(r->pcb) < TCP_SND_QUEUELEN - 4) {
			n = read(r->fd, b, sizeof(b));
			if (n == 0 || (n < 0 && errno != EAGAIN)) {
				r->fd_eof = true;
				relay_end(r);
			}
			if (n <= 0)
				break;
			tcp_write(r->pcb, b, n, TCP_WRITE_FLAG_COPY);
			tcp_output(r->pcb);
		}
	}
	for (struct netif *nf = netif_list; nf; nf = nf->next)
		netif_poll(nf);
	sys_check_timeouts();
}
