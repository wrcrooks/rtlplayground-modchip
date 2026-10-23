/*
 * The web server, on lwIP's raw TCP API. It speaks the HTTP of
 * RTLPlayground's httpd.c, so its web UI (html/, embedded by
 * tools/embed_web.py) runs unchanged: the same files, login and session
 * cookie, JSON endpoints (written by page_impl.c), /cmd and /config.
 * Every response ends with Connection: close, as there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"

#include "bridge.h"
#include "metrics.h"
#include "net.h"
#include "plat.h"
#include "upload.h"
#include "web_assets.h"

#define CONNS			4
#define REQ_MAX			(PLAT_CONFIG_LEN + 384)	/* httpd.c's CONFIG_UPLOAD_BUF */
#define RX_TIMEOUT_S		5
#define CMD_LINE_MAX		127			/* CMD_BUF_SIZE - 1 */
#define TLS_CONNS		2			/* each holds about 25 KB of mbedTLS buffers */
#define METRICS_JSON_MAX	6144
#define WRITE_MAX		2048			/* below MBEDTLS_SSL_OUT_CONTENT_LEN: one TLS record */

enum { ST_FREE, ST_RX, ST_TX, ST_UPLOAD, ST_DRAIN };

struct conn {
	struct altcp_pcb *pcb;
	uint8_t state;
	bool tls;
	uint8_t idle_s;
	bool reboot_after;
	uint32_t drain;			/* ST_DRAIN: body bytes to read before the response goes out */
	uint16_t req_len;
	uint16_t hdr_len;		/* through the blank line, 0 while incomplete */
	uint16_t content_length;
	char req[REQ_MAX + 1];
	/* the response: resp (ours, freed when done), then body (flash, not copied) */
	uint8_t *resp;
	uint32_t resp_len, resp_off;
	const uint8_t *body;
	uint32_t body_len, body_off;
};

static struct conn conns[CONNS];
static struct altcp_pcb *listen_tls;

/*
 * A firmware upload (POST /upload), one at a time. Its body goes through
 * this ring to upload.c, a little each main loop pass (httpd_poll()); lwIP
 * is told the bytes were received only as they leave the ring, so TCP
 * flow control holds the browser back while the switch's flash catches
 * up. The ring holds the TCP window plus what came with the headers.
 */
#define UP_RING			8192
#define UP_IDLE_S		60
static struct conn *up_conn;
static uint8_t up_ring[UP_RING];
static uint16_t up_head, up_tail, up_count;
static uint32_t up_pre;		/* bytes in the ring lwIP was already told about */
static char session_id[RTL_SESSION_ID_LEN + 1];
static uint32_t last_session_use;
static bool reboot_pending;

static const char hdr_common[] =
	"Cache-Control: max-age=60, must-revalidate\r\nConnection: close\r\n"
	"Access-Control-Allow-Origin: *\r\n"
	"Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
	"style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; form-action 'self'\r\n\r\n";


static void conn_free(struct conn *c)
{
	if (c == up_conn) {
		upload_abort();	/* no reboot follows: nothing installable may stay */
		up_conn = NULL;
	}
	if (c->reboot_after)
		reboot_pending = true;
	c->reboot_after = false;
	free(c->resp);
	c->resp = NULL;
	c->body = NULL;
	c->state = ST_FREE;
	c->pcb = NULL;
}


static void conn_close(struct conn *c)
{
	struct altcp_pcb *pcb = c->pcb;

	if (pcb) {
		altcp_arg(pcb, NULL);
		altcp_recv(pcb, NULL);
		altcp_sent(pcb, NULL);
		altcp_err(pcb, NULL);
		if (altcp_close(pcb) != ERR_OK)
			altcp_abort(pcb);
	}
	conn_free(c);
}


/* Queue as much of the response as lwIP takes; close once it is all acked */
static void conn_push(struct conn *c)
{
	struct altcp_pcb *pcb = c->pcb;

	while (c->resp_off < c->resp_len) {
		uint32_t n = c->resp_len - c->resp_off;
		if (n > altcp_sndbuf(pcb))
			n = altcp_sndbuf(pcb);
		if (n > WRITE_MAX)
			n = WRITE_MAX;
		if (!n || altcp_write(pcb, c->resp + c->resp_off, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
			goto out;
		c->resp_off += n;
	}
	while (c->body_off < c->body_len) {
		uint32_t n = c->body_len - c->body_off;
		if (n > altcp_sndbuf(pcb))
			n = altcp_sndbuf(pcb);
		if (n > WRITE_MAX)
			n = WRITE_MAX;
		if (!n || altcp_write(pcb, c->body + c->body_off, n, 0) != ERR_OK)
			goto out;
		c->body_off += n;
	}
out:
	altcp_output(pcb);
	if (c->resp_off == c->resp_len && c->body_off == c->body_len && !altcp_sndqueuelen(pcb))
		conn_close(c);
}


/* The response, held until conn_push() sends it */
static void prepare(struct conn *c, const void *hdr, uint32_t len)
{
	free(c->resp);
	c->resp = malloc(len);
	c->resp_len = c->resp ? len : 0;
	c->resp_off = 0;
	if (c->resp)
		memcpy(c->resp, hdr, len);
}


static void respond(struct conn *c, const void *hdr, uint32_t len)
{
	prepare(c, hdr, len);
	c->state = ST_TX;
	conn_push(c);
}


static void respond_str(struct conn *c, const char *s)
{
	respond(c, s, strlen(s));
}


/* outbuf as page_impl.c and the command line left it, plus any rest in flash */
static void respond_outbuf(struct conn *c)
{
	uint32_t len = slen + cont_len;

	free(c->resp);
	c->resp = malloc(len);
	if (!c->resp) {
		respond_str(c, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
		return;
	}
	memcpy(c->resp, outbuf, slen);
	c->resp_len = slen;
	while (cont_len)
		c->resp_len += rtl_http_cont(c->resp + c->resp_len, cont_len);
	c->resp_off = 0;
	c->state = ST_TX;
	conn_push(c);
}


static void send_not_found(struct conn *c)
{
	respond_str(c, "HTTP/1.1 404 Not found\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n"
		    "<!DOCTYPE HTML PUBLIC>\n<title>404 Not Found</title>\n<h1>Not Found</h1>\n");
}


static void send_bad_request(struct conn *c)
{
	respond_str(c, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n"
		    "<!DOCTYPE HTML PUBLIC>\n<title>400 Bad Request</title>\n<h1>Bad Request</h1>\n");
}


static void send_to_login(struct conn *c)
{
	respond_str(c, "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: login.html\r\n\r\n");
}


static void send_unauthorized(struct conn *c)
{
	respond_str(c, "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
}


static void send_ok(struct conn *c)
{
	respond_str(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
}


/* The value of header name (lower case, with the colon), or NULL */
static char *header(struct conn *c, const char *name)
{
	size_t n = strlen(name);
	char *end = c->req + c->hdr_len;
	/* By length, not as a string: handle_request() ends the request line
	 * at the path with a NUL, and the headers lie beyond it */
	char *p = memchr(c->req, '\n', c->hdr_len);

	while (p && ++p < end) {
		if ((size_t)(end - p) > n && !strncasecmp(p, name, n)) {
			p += n;
			while (*p == ' ' || *p == '\t')
				p++;
			return p;
		}
		p = memchr(p, '\n', end - p);
	}
	return NULL;
}


static bool authenticated(struct conn *c)
{
	char *v = header(c, "cookie:");
	uint32_t now = plat_uptime_s();

	if (!v || !session_id[0] || now - last_session_use > session_timeout)
		return false;
	/* the "session" cookie, among any others, in any order */
	while (*v && *v != '\r') {
		if (!strncmp(v, "session=", 8) && (v == header(c, "cookie:") || v[-1] == ' ' || v[-1] == ';')) {
			v += 8;
			if (strncmp(v, session_id, RTL_SESSION_ID_LEN))
				return false;
			v += RTL_SESSION_ID_LEN;
			if (*v && *v != ';' && *v != '\r' && *v != ' ')
				return false;
			last_session_use = now;
			return true;
		}
		v++;
	}
	return false;
}


static void serve_asset(struct conn *c, const struct web_asset *a)
{
	char hdr[512];
	int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: %s; charset=UTF-8\r\n%s"
			 "Content-Length: %lu\r\n%s", a->mime, a->gzip ? "Content-Encoding: gzip\r\n" : "",
			 (unsigned long)a->len, hdr_common);

	c->body = a->data;
	c->body_len = a->len;
	c->body_off = 0;
	respond(c, hdr, n);
}


/* A response we build here: header, then body (taken over and freed) */
static void respond_body(struct conn *c, const char *mime, uint8_t *body, size_t len)
{
	char hdr[160];
	int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", mime, (unsigned long)len);

	free(c->resp);
	c->resp = malloc(n + len);
	if (!c->resp) {
		free(body);
		respond_str(c, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
		return;
	}
	memcpy(c->resp, hdr, n);
	memcpy(c->resp + n, body, len);
	free(body);
	c->resp_len = n + len;
	c->resp_off = 0;
	c->state = ST_TX;
	conn_push(c);
}


/* The dashboard's data, metrics.c */
static bool handle_metrics(struct conn *c, const char *path)
{
	size_t len;
	uint8_t *body;

	if (!strcmp(path, "/metrics.json")) {
		body = malloc(METRICS_JSON_MAX);
		if (!body)
			return false;
		len = metrics_json((char *)body, METRICS_JSON_MAX);
		respond_body(c, "application/json", body, len);
		return true;
	}
	if (!strcmp(path, "/history.bin")) {
		body = metrics_history(&len);
		if (!body)
			return false;
		respond_body(c, "application/octet-stream", body, len);
		return true;
	}
	return false;
}


static void handle_get(struct conn *c, char *target)
{
	char path[64] = "";
	size_t n = strcspn(target, "?");
	bool auth = authenticated(c);

	if (n < sizeof(path)) {
		memcpy(path, target, n);
		path[n] = 0;
		for (const struct web_asset *a = web_assets; a->path; a++) {
			if (strcmp(a->path, path))
				continue;
			if (!auth && strcmp(path, "/login.html") && strcmp(path, "/favicon.ico"))
				send_to_login(c);
			else
				serve_asset(c, a);
			return;
		}
	}
	if (!auth) {
		send_unauthorized(c);
		return;
	}
	if (handle_metrics(c, path))
		return;
	if (!strcmp(path, "/reset")) {
		c->reboot_after = true;
		send_ok(c);
		return;
	}
	if (rtl_http_get(target))
		respond_outbuf(c);
	else
		send_not_found(c);
}


static void handle_login(struct conn *c, char *body)
{
	if (strncmp(body, "pwd=", 4) || !rtl_password_ok(body + 4)) {
		send_to_login(c);
		return;
	}
	for (int i = 0; i < RTL_SESSION_ID_LEN; i++)
		session_id[i] = "0123456789abcdef"[plat_random32() & 0xf];
	session_id[RTL_SESSION_ID_LEN] = 0;
	last_session_use = plat_uptime_s();

	char hdr[160];
	int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: index.html\r\n"
			 "Set-Cookie: session=%s; SameSite=Strict\r\n\r\n", session_id);
	respond(c, hdr, n);
}


static const char *memfind(const char *p, const char *end, const char *s)
{
	size_t n = strlen(s);

	for (; p + n <= end; p++)
		if (!memcmp(p, s, n))
			return p;
	return NULL;
}


/*
 * POST /config, multipart/form-data: the part with a filename holds the
 * configuration. Checked as config_take() in httpd.c does: it must fit the
 * sector with its terminator, and no line may outgrow the command buffer.
 */
static void handle_config(struct conn *c, char *body, uint16_t len)
{
	static uint8_t sector[PLAT_CONFIG_LEN];
	char delim[80];
	char *ct = header(c, "content-type:");
	const char *end = body + len;

	if (!ct || strncmp(ct, "multipart/form-data; boundary=", 30)) {
		send_bad_request(c);
		return;
	}
	size_t bl = strcspn(ct + 30, "\r");
	if (bl == 0 || bl > sizeof(delim) - 5) {
		send_bad_request(c);
		return;
	}
	memcpy(delim, "\r\n--", 4);
	memcpy(delim + 4, ct + 30, bl);
	delim[4 + bl] = 0;

	/* The first delimiter has no CRLF in front of it */
	const char *part = memfind(body, end, delim + 2);
	while (part) {
		const char *hdr = part + strlen(delim + 2);
		const char *data = memfind(hdr, end, "\r\n\r\n");
		if (!data)
			break;
		data += 4;
		const char *next = memfind(data, end, delim);
		if (!next)
			break;
		if (memfind(hdr, data, "filename")) {
			size_t n = next - data;
			int run = 0;

			if (n + 1 > sizeof(sector)) {
				send_bad_request(c);
				return;
			}
			for (size_t i = 0; i < n; i++) {
				if (data[i] == '\n')
					run = 0;
				else if (++run >= CMD_LINE_MAX) {
					send_bad_request(c);
					return;
				}
			}
			memset(sector, 0xff, sizeof(sector));
			memcpy(sector, data, n);
			sector[n] = 0;
			if (plat_config_write(sector, sizeof(sector)))
				send_ok(c);
			else
				respond_str(c, "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n");
			return;
		}
		part = next + 2;
	}
	send_bad_request(c);
}


static void handle_request(struct conn *c)
{
	char *body = c->req + c->hdr_len;
	char *target, *sp;

	body[c->content_length] = 0;
	if (!strncmp(c->req, "GET ", 4)) {
		target = c->req + 4;
	} else if (!strncmp(c->req, "POST ", 5)) {
		target = c->req + 5;
	} else {
		send_bad_request(c);
		return;
	}
	sp = strpbrk(target, " \r");
	if (!sp) {
		send_bad_request(c);
		return;
	}
	*sp = 0;

	if (c->req[0] == 'G') {
		handle_get(c, target);
		return;
	}
	if (!strcmp(target, "/login")) {
		char *ct = header(c, "content-type:");
		if (!ct || strncmp(ct, "application/x-www-form-urlencoded", 33))
			send_bad_request(c);
		else
			handle_login(c, body);
		return;
	}
	if (!authenticated(c)) {
		send_unauthorized(c);
		return;
	}
	if (!strcmp(target, "/cmd")) {
		rtl_http_cmd(body);
		respond_outbuf(c);
	} else if (!strcmp(target, "/config")) {
		handle_config(c, body, c->content_length);
	} else {
		send_not_found(c);
	}
}


static void up_put(const uint8_t *p, size_t n)
{
	while (n--) {
		up_ring[up_head] = *p++;
		up_head = (up_head + 1) % UP_RING;
		up_count++;
	}
}


/* The plain-text verdicts of RTLPlayground's /upload: "OK: ..." or "NO: ..." */
static int verdict(char *r, size_t max, int status, const char *text)
{
	return snprintf(r, max, "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nConnection: close\r\n"
			"Content-Length: %u\r\n\r\n%s\n", status == 200 ? "200 OK" : status == 400 ? "400 Bad Request"
			: status == 401 ? "401 Unauthorized" : "503 Service Unavailable", (unsigned)strlen(text) + 1, text);
}


static void upload_verdict(struct conn *c, int status, const char *text)
{
	char r[220];

	respond(c, r, verdict(r, sizeof(r), status, text));
}


/* A refusal before the upload starts: sent once the browser has sent the
 * rest of its body, as it reads the answer only then (and closing on unread
 * data could reset the connection under it) */
static void upload_refuse(struct conn *c, int status, const char *text)
{
	char *cl = header(c, "content-length:"), r[220];
	uint32_t total = cl ? strtoul(cl, NULL, 10) : 0, have = c->req_len - c->hdr_len;
	int n = verdict(r, sizeof(r), status, text);

	if (total <= have) {
		respond(c, r, n);
		return;
	}
	prepare(c, r, n);
	c->drain = total - have;
	c->state = ST_DRAIN;
}


/*
 * POST /upload, as RTLPlayground's web UI sends it: a multipart body with
 * the image. It goes to the bridge's staging area; once it is in and reads
 * back intact, the verdict goes out and the switch reboots to install it.
 */
static void start_upload(struct conn *c)
{
	char boundary[80], msg[140];
	char *ct = header(c, "content-type:"), *cl = header(c, "content-length:");
	size_t bl;

	if (!authenticated(c)) {
		upload_refuse(c, 401, "NO: not logged in");
		return;
	}
	if (up_conn) {
		upload_refuse(c, 503, "NO: another upload is in progress");
		return;
	}
	if (!bridge_is_up()) {
		upload_refuse(c, 503, "NO: the switch's bridge firmware isn't answering");
		return;
	}
	if (!ct || strncmp(ct, "multipart/form-data; boundary=", 30) || !cl) {
		upload_refuse(c, 400, "NO: expected a multipart upload with a length");
		return;
	}
	bl = strcspn(ct + 30, "\r;");
	if (bl >= sizeof(boundary))
		bl = sizeof(boundary) - 1;
	memcpy(boundary, ct + 30, bl);
	boundary[bl] = 0;
	if (!upload_begin(strtoul(cl, NULL, 10), boundary, bridge_hello.image_size, bridge_hello.upload_start,
			  bridge_hello.flash_size)) {
		snprintf(msg, sizeof(msg), "NO: %s", upload_error());
		upload_refuse(c, 400, msg);
		return;
	}
	printf("upload: staging a %lu byte image at 0x%lx\n", (unsigned long)bridge_hello.image_size,
	       (unsigned long)bridge_hello.upload_start);
	up_conn = c;
	c->state = ST_UPLOAD;
	up_head = up_tail = up_count = 0;
	up_put((const uint8_t *)c->req + c->hdr_len, c->req_len - c->hdr_len);
	up_pre = c->req_len - c->hdr_len;	/* acknowledged with the headers */
}


/* Each main loop pass: a step of the upload, at most one flash operation */
bool httpd_poll(void)
{
	struct conn *c = up_conn;
	char msg[140];

	if (!c || c->state != ST_UPLOAD)
		return false;
	if (upload_body_left()) {
		if (!up_count)
			return false;
		size_t n = up_count < UP_RING - up_tail ? up_count : UP_RING - up_tail;
		uint32_t used = upload_feed(up_ring + up_tail, n), ack = used;
		up_tail = (up_tail + used) % UP_RING;
		up_count -= used;
		if (up_pre) {
			uint32_t d = ack < up_pre ? ack : up_pre;
			up_pre -= d;
			ack -= d;
		}
		if (ack)
			altcp_recved(c->pcb, ack);
		return true;
	}
	if (upload_state() != UP_DONE && upload_state() != UP_FAILED && !upload_finish()
	    && upload_state() != UP_FAILED)
		return true;	/* the last page, the read-back: more next pass */
	up_conn = NULL;
	if (upload_state() == UP_DONE) {
		printf("upload: image staged and verified, rebooting to install it\n");
		c->reboot_after = true;
		upload_verdict(c, 200, "OK: checksum verified, rebooting");
	} else {
		printf("upload: %s\n", upload_error());
		snprintf(msg, sizeof(msg), "NO: %s", upload_error());
		upload_verdict(c, 400, msg);
	}
	return true;
}


static err_t on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct conn *c = arg;
	(void)err;

	if (!c)
		return ERR_OK;
	if (!p) {
		conn_close(c);
		return ERR_OK;
	}
	if (c->state == ST_UPLOAD) {
		if (p->tot_len > UP_RING - up_count) {
			/* More than the window allows: give up rather than lose bytes */
			pbuf_free(p);
			altcp_arg(pcb, NULL);
			altcp_abort(pcb);
			conn_free(c);
			return ERR_ABRT;
		}
		for (struct pbuf *q = p; q; q = q->next)
			up_put(q->payload, q->len);
		pbuf_free(p);
		c->idle_s = 0;
		return ERR_OK;	/* acknowledged as httpd_poll() uses it */
	}
	altcp_recved(pcb, p->tot_len);
	if (c->state == ST_DRAIN) {
		c->drain = p->tot_len < c->drain ? c->drain - p->tot_len : 0;
		pbuf_free(p);
		c->idle_s = 0;
		if (!c->drain) {
			c->state = ST_TX;
			conn_push(c);
		}
		return ERR_OK;
	}
	if (c->state != ST_RX) {
		pbuf_free(p);
		return ERR_OK;
	}
	c->idle_s = 0;
	if (c->req_len + p->tot_len > REQ_MAX) {
		pbuf_free(p);
		c->req_len = 0;
		send_bad_request(c);
		return ERR_OK;
	}
	pbuf_copy_partial(p, c->req + c->req_len, p->tot_len, 0);
	c->req_len += p->tot_len;
	c->req[c->req_len] = 0;
	pbuf_free(p);

	if (!c->hdr_len) {
		char *e = strstr(c->req, "\r\n\r\n");
		if (!e)
			return ERR_OK;
		c->hdr_len = e + 4 - c->req;
		if (!strncmp(c->req, "POST /upload", 12) && (c->req[12] == ' ' || c->req[12] == '?')) {
			start_upload(c);
			return ERR_OK;
		}
		char *cl = header(c, "content-length:");
		unsigned long v = cl ? strtoul(cl, NULL, 10) : 0;
		if (v + c->hdr_len > REQ_MAX) {
			send_bad_request(c);
			return ERR_OK;
		}
		c->content_length = v;
		if (!strncmp(c->req, "POST ", 5) && !cl) {
			respond_str(c, "HTTP/1.1 411 Length Required\r\nConnection: close\r\n\r\n");
			return ERR_OK;
		}
	}
	if (c->req_len >= c->hdr_len + c->content_length)
		handle_request(c);
	return ERR_OK;
}


static err_t on_sent(void *arg, struct altcp_pcb *pcb, u16_t len)
{
	struct conn *c = arg;
	(void)pcb;
	(void)len;

	if (c && c->state == ST_TX) {
		c->idle_s = 0;
		conn_push(c);
	}
	return ERR_OK;
}


static void on_err(void *arg, err_t err)
{
	struct conn *c = arg;
	(void)err;

	if (c)
		conn_free(c);	/* lwIP has freed the pcb */
}


static err_t on_accept(void *arg, struct altcp_pcb *pcb, err_t err)
{
	bool tls = arg != NULL;
	int n_tls = 0;

	if (err != ERR_OK || !pcb)
		return ERR_VAL;
	for (int i = 0; i < CONNS; i++)
		n_tls += conns[i].state != ST_FREE && conns[i].tls;
	for (int i = 0; i < CONNS && !(tls && n_tls >= TLS_CONNS); i++) {
		struct conn *c = &conns[i];
		if (c->state != ST_FREE)
			continue;
		c->pcb = pcb;
		c->tls = tls;
		c->state = ST_RX;
		c->idle_s = 0;
		c->reboot_after = false;
		c->drain = 0;
		c->req_len = c->hdr_len = c->content_length = 0;
		c->resp = NULL;
		c->resp_len = c->resp_off = 0;
		c->body = NULL;
		c->body_len = c->body_off = 0;
		altcp_arg(pcb, c);
		altcp_recv(pcb, on_recv);
		altcp_sent(pcb, on_sent);
		altcp_err(pcb, on_err);
		altcp_setprio(pcb, TCP_PRIO_MIN);
		return ERR_OK;
	}
	altcp_abort(pcb);
	return ERR_ABRT;
}


static struct altcp_pcb *listen_on(struct altcp_pcb *pcb, u16_t port, void *arg)
{
	struct altcp_pcb *l;

	if (!pcb)
		return NULL;
	if (altcp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK || !(l = altcp_listen_with_backlog(pcb, CONNS))) {
		altcp_close(pcb);
		return NULL;
	}
	altcp_arg(l, arg);
	altcp_accept(l, on_accept);
	return l;
}


void httpd_init(void)
{
	listen_on(altcp_tcp_new_ip_type(IPADDR_TYPE_V4), 80, NULL);
}


/* HTTPS on port 443 with the key and certificate in conf (tls.c) */
bool httpd_tls_start(struct altcp_tls_config *conf)
{
	httpd_tls_stop();
	listen_tls = listen_on(altcp_tls_new(conf, IPADDR_TYPE_V4), 443, (void *)1);
	return listen_tls != NULL;
}


/* Closes the listener and every TLS connection, so the config can be freed */
void httpd_tls_stop(void)
{
	if (listen_tls) {
		altcp_close(listen_tls);
		listen_tls = NULL;
	}
	for (int i = 0; i < CONNS; i++) {
		struct conn *c = &conns[i];
		if (c->state == ST_FREE || !c->tls)
			continue;
		if (c->pcb) {
			altcp_arg(c->pcb, NULL);
			altcp_abort(c->pcb);
		}
		conn_free(c);
	}
}


void httpd_second(void)
{
	for (int i = 0; i < CONNS; i++) {
		struct conn *c = &conns[i];
		if (c->state == ST_FREE)
			continue;
		if (++c->idle_s > (c->state == ST_RX ? RX_TIMEOUT_S : c->state == ST_UPLOAD ? UP_IDLE_S : 30)) {
			if (c->pcb) {
				altcp_arg(c->pcb, NULL);
				altcp_abort(c->pcb);
			}
			conn_free(c);
		}
	}
	if (reboot_pending) {
		reboot_pending = false;
		plat_reboot();
	}
}
