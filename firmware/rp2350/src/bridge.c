/*
 * Host side of the 8051 bridge link, see bridge.h
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"

#include "bridge.h"
#include "link.h"

#define UART			UART_INSTANCE(MODCHIP_BRIDGE_UART)

#define RX_RING_SIZE		16384	/* power of 2; 168 ms of line time */
#define EVENT_SLOTS		24

#define TIMEOUT_MS		150	/* 100 ms, the protocol's figure, plus a full frame's transfer */
#define TIMEOUT_SLOW_MS		500	/* 64 table entries, about 1.3 KB back; SerDes setup */
#define TIMEOUT_FLASH_MS	3000	/* a sector erase on a W25Q40 takes up to 400 ms, slow writes more */
#define TRIES			3

#define PING_MISSES		3	/* the bridge drops the host after 3 s without a request */
#define RESET_HOLDOFF_S		30	/* the 8051 needs a few seconds to boot and initialise */
#define SILENT_RESET_S		20	/* no beacon and no answer for this long: reset, once seen */
#define RESET_PULSE_MS		200

struct bridge_hello bridge_hello;
struct bridge_stats bridge_stats;

static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;	/* written by the ISR */
static uint16_t rx_tail;

static struct link_rx rx;
/* The first bytes after power-up, for "bridge raw" */
#define RAW_KEEP		4096
static uint8_t raw_keep[RAW_KEEP];
static uint16_t raw_n;
static bool hold;		/* "bridge hold": send nothing, only listen */
static uint8_t tx_buf[LINK_ENC_MAX];
static uint8_t seq;

static struct {
	uint16_t len;
	uint8_t msg[LINK_MSG_MAX];
} events[EVENT_SLOTS];
static uint8_t ev_head, ev_tail, ev_count;
static uint8_t ev_cur[LINK_MSG_MAX];

/* The reply being waited for */
static bool waiting;
static uint8_t wait_type, wait_seq;
static bool reply_in;
static uint8_t reply_status;
static uint8_t *reply_buf;
static size_t reply_max, reply_len;

static bool running;
static bool up;
static bool seen;		/* a beacon or a reply since power-up: a bridge is fitted */
static bool hello_now;
static uint8_t ping_misses;
static uint32_t silent_s;
static uint32_t since_reset_s = RESET_HOLDOFF_S;


static void uart_isr(void)
{
	uart_hw_t *hw = uart_get_hw(UART);

	while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
		uint32_t dr = hw->dr;
		uint16_t next = (rx_head + 1) & (RX_RING_SIZE - 1);

		if (dr & (UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_FE_BITS))
			bridge_stats.rx_overruns++;
		if (next == rx_tail) {
			bridge_stats.rx_overruns++;
			continue;
		}
		rx_ring[rx_head] = dr;
		rx_head = next;
		if (raw_n < RAW_KEEP)
			raw_keep[raw_n++] = dr;
		bridge_stats.uart_rx_bytes++;
	}
}


/*
 * Wiring check: GP0 and GP1 as plain inputs with pull-downs for 2 s,
 * counting edges. An idle UART output reads high; an open wire reads low.
 * The bridge beacons each second, so a connected, running one shows edges.
 */
void bridge_probe(void)
{
	static const uint pins[2] = { MODCHIP_BRIDGE_TX_PIN, MODCHIP_BRIDGE_RX_PIN };
	unsigned edges[2] = { 0, 0 };
	bool idle[2], last[2];

	for (int i = 0; i < 2; i++) {
		gpio_set_function(pins[i], GPIO_FUNC_SIO);
		gpio_set_dir(pins[i], false);
		gpio_pull_down(pins[i]);
	}
	sleep_us(200);
	for (int i = 0; i < 2; i++)
		idle[i] = last[i] = gpio_get(pins[i]);
	absolute_time_t end = make_timeout_time_ms(2000);
	while (!time_reached(end))
		for (int i = 0; i < 2; i++) {
			bool v = gpio_get(pins[i]);
			if (v != last[i]) {
				edges[i]++;
				last[i] = v;
			}
		}
	for (int i = 0; i < 2; i++)
		printf("GP%u (%s): idles %s with a pull-down, %u edges in 2 s\n", pins[i],
		       i ? "UART RX, to T5 pin 1" : "UART TX, to T5 pin 3", idle[i] ? "high" : "low", edges[i]);
	gpio_set_function(MODCHIP_BRIDGE_TX_PIN, UART_FUNCSEL_NUM(UART, MODCHIP_BRIDGE_TX_PIN));
	gpio_set_function(MODCHIP_BRIDGE_RX_PIN, UART_FUNCSEL_NUM(UART, MODCHIP_BRIDGE_RX_PIN));
	gpio_pull_up(MODCHIP_BRIDGE_RX_PIN);
	link_rx_reset(&rx);
}


static void hexdump(const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i += 16) {
		printf("%04x ", i);
		for (unsigned j = i; j < i + 16; j++)
			j < n ? printf(" %02x", p[j]) : printf("   ");
		printf("  ");
		for (unsigned j = i; j < i + 16 && j < n; j++)
			putchar_raw(p[j] >= 0x20 && p[j] < 0x7f ? p[j] : '.');
		printf("\n");
	}
}


/* "bridge raw": the first bytes received since power-up, and the frame
 * being decoded now */
void bridge_raw(void)
{
	printf("first %u bytes received since power-up:\n", raw_n);
	hexdump(raw_keep, raw_n);
	printf("frame being decoded: %u bytes, %u left in block, %s\n", rx.len, rx.left,
	       rx.bad ? "bad" : "ok");
	hexdump(rx.buf, rx.len < 256 ? rx.len : 256);
}


void bridge_hold(bool on)
{
	hold = on;
	printf("bridge %s\n", on ? "on hold: no HELLO, only listening" : "active");
}


void bridge_init(void)
{
	uart_init(UART, MODCHIP_BRIDGE_BAUD);
	uart_set_format(UART, 8, 1, UART_PARITY_NONE);
	uart_set_hw_flow(UART, false, false);
	uart_set_fifo_enabled(UART, true);
	gpio_set_function(MODCHIP_BRIDGE_TX_PIN, UART_FUNCSEL_NUM(UART, MODCHIP_BRIDGE_TX_PIN));
	gpio_set_function(MODCHIP_BRIDGE_RX_PIN, UART_FUNCSEL_NUM(UART, MODCHIP_BRIDGE_RX_PIN));
	gpio_pull_up(MODCHIP_BRIDGE_RX_PIN);

	rx_head = rx_tail = 0;
	raw_n = 0;
	link_rx_reset(&rx);
	ev_head = ev_tail = ev_count = 0;
	irq_set_exclusive_handler(UART_IRQ_NUM(UART), uart_isr);
	irq_set_enabled(UART_IRQ_NUM(UART), true);
	uart_set_irqs_enabled(UART, true, false);	/* RX and RX timeout */

	gpio_init(MODCHIP_RESET_PIN);
	gpio_put(MODCHIP_RESET_PIN, 0);
	gpio_set_dir(MODCHIP_RESET_PIN, GPIO_OUT);

	up = false;
	hello_now = true;
	running = true;
}


/* Back to high impedance with the input buffers off: the switch lost power */
void bridge_deinit(void)
{
	if (!running)
		return;
	running = false;
	uart_set_irqs_enabled(UART, false, false);
	irq_set_enabled(UART_IRQ_NUM(UART), false);
	uart_deinit(UART);
	gpio_deinit(MODCHIP_BRIDGE_TX_PIN);
	gpio_deinit(MODCHIP_BRIDGE_RX_PIN);
	gpio_disable_pulls(MODCHIP_BRIDGE_RX_PIN);
	gpio_set_input_enabled(MODCHIP_BRIDGE_TX_PIN, false);
	gpio_set_input_enabled(MODCHIP_BRIDGE_RX_PIN, false);
	if (up) {
		up = false;
		bridge_on_down();
	}
}


bool bridge_is_up(void)
{
	return up;
}


static void queue_event(const uint8_t *msg, uint16_t len)
{
	if (ev_count == EVENT_SLOTS) {
		bridge_stats.events_dropped++;
		return;
	}
	events[ev_head].len = len;
	memcpy(events[ev_head].msg, msg, len);
	ev_head = (ev_head + 1) % EVENT_SLOTS;
	ev_count++;
}


static void handle_msg(void)
{
	uint8_t type = rx.buf[0];

	seen = true;
	silent_s = 0;
	if (type & REPLY) {
		if (!waiting || reply_in || type != (wait_type | REPLY) || rx.buf[1] != wait_seq || rx.len < 3)
			return;		/* late reply to a request that timed out */
		reply_status = rx.buf[2];
		reply_len = rx.len - 3;
		if (reply_len > reply_max)
			reply_len = reply_max;
		if (reply_buf)
			memcpy(reply_buf, rx.buf + 3, reply_len);
		reply_in = true;
		return;
	}
	queue_event(rx.buf, rx.len);
}


void bridge_service(void)
{
	while (rx_tail != rx_head) {
		uint8_t b = rx_ring[rx_tail];

		rx_tail = (rx_tail + 1) & (RX_RING_SIZE - 1);
		switch (link_rx_byte(&rx, b)) {
		case LINK_RX_MSG:
			handle_msg();
			link_rx_reset(&rx);
			break;
		case LINK_RX_ERROR:
			bridge_stats.link_errors++;
			break;
		}
	}
}


static int request_once(uint8_t type, const void *p1, size_t n1, const void *p2, size_t n2,
			void *reply, size_t rmax, size_t *rlen, uint32_t timeout_ms)
{
	struct link_tx tx;
	size_t len;

	seq++;
	link_tx_begin(&tx, tx_buf, type, seq);
	if (p1)
		link_tx_put_buf(&tx, p1, n1);
	if (p2)
		link_tx_put_buf(&tx, p2, n2);
	len = link_tx_end(&tx);

	waiting = true;
	wait_type = type;
	wait_seq = seq;
	reply_in = false;
	reply_buf = reply;
	reply_max = rmax;
	reply_len = 0;

	/* A delimiter first, so the 8051 discards a message cut short earlier */
	uart_putc_raw(UART, 0);
	uart_write_blocking(UART, tx_buf, len);
	bridge_stats.uart_tx_bytes += len + 1;
	if (type == REQ_TX_FRAME)
		bridge_stats.tx_frames++;

	absolute_time_t until = make_timeout_time_ms(timeout_ms + len / 64);
	while (!reply_in && !time_reached(until)) {
		bridge_service();
		watchdog_update();	/* a bounded wait: see WATCHDOG_MS in main.c */
		if (!reply_in)
			tight_loop_contents();
	}
	waiting = false;
	if (!reply_in) {
		bridge_stats.timeouts++;
		return BRIDGE_TIMEOUT;
	}
	if (rlen)
		*rlen = reply_len;
	return reply_status;
}


int bridge_request(uint8_t type, const void *p1, size_t n1, const void *p2, size_t n2,
		   void *reply, size_t rmax, size_t *rlen)
{
	uint32_t timeout = TIMEOUT_MS;
	int tries = TRIES;
	int r = BRIDGE_TIMEOUT;

	if (!running)
		return BRIDGE_TIMEOUT;
	if (type == REQ_FLASH_ERASE || type == REQ_FLASH_WRITE)
		timeout = TIMEOUT_FLASH_MS;
	else if (type == REQ_TBL_READ || type == REQ_FLASH_ID || type == REQ_SDS_CONFIG)
		timeout = TIMEOUT_SLOW_MS;
	/* A frame sent twice is worse than a frame lost; the peer retransmits */
	if (type == REQ_TX_FRAME || type == REQ_REBOOT)
		tries = 1;
	bridge_stats.requests++;
	while (tries--) {
		r = request_once(type, p1, n1, p2, n2, reply, rmax, rlen, timeout);
		if (r != BRIDGE_TIMEOUT)
			break;
	}
	return r;
}


static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}


static bool parse_hello(const uint8_t *p, size_t n)
{
	struct bridge_hello *h = &bridge_hello;

	if (n < 61 || p[0] != BRIDGE_PROTO_VERSION)
		return false;
	h->version = p[0];
	h->chip_id = get32(p + 1);
	memcpy(h->mac, p + 5, 6);
	h->flash_size = get32(p + 11);
	h->image_size = get32(p + 15);
	h->upload_start = get32(p + 19);
	h->cpu_port = p[23];
	h->min_port = p[24];
	h->max_port = p[25];
	h->n_sfp = p[26];
	h->flags = p[27];
	memcpy(h->machine, p + 28, 30);
	h->machine[30] = 0;
	n -= 58;
	if (n >= sizeof(h->version_sw))
		n = sizeof(h->version_sw) - 1;
	memcpy(h->version_sw, p + 58, n);
	h->version_sw[n] = 0;
	return true;
}


static void go_down(void)
{
	if (!up)
		return;
	up = false;
	bridge_on_down();
}


static void try_hello(void)
{
	static uint8_t buf[128];
	size_t n;

	hello_now = false;
	if (request_once(REQ_HELLO, NULL, 0, NULL, 0, buf, sizeof(buf), &n, TIMEOUT_MS) != ST_OK)
		return;
	if (!parse_hello(buf, n)) {
		printf("bridge: HELLO with an unknown layout or protocol version %u\n", buf[0]);
		return;
	}
	up = true;
	ping_misses = 0;
	bridge_stats.uptime_s = 0;
	printf("bridge: %s, chip %08lx, bridge firmware %s\n", bridge_hello.machine,
	       (unsigned long)bridge_hello.chip_id, bridge_hello.version_sw);
	bridge_on_up();
}


static void reset_switch(void)
{
	if (since_reset_s < RESET_HOLDOFF_S)
		return;
	printf("bridge: not answering, resetting the switch\n");
	gpio_put(MODCHIP_RESET_PIN, 1);
	sleep_ms(RESET_PULSE_MS);
	gpio_put(MODCHIP_RESET_PIN, 0);
	bridge_stats.resets++;
	since_reset_s = 0;
	silent_s = 0;
}


void bridge_second(void)
{
	uint8_t buf[8];
	size_t n;

	if (!running)
		return;
	since_reset_s++;
	silent_s++;
	if (hold)
		return;
	if (!up) {
		try_hello();
		if (!up && seen && silent_s >= SILENT_RESET_S)
			reset_switch();
		return;
	}
	if (request_once(REQ_PING, NULL, 0, NULL, 0, buf, sizeof(buf), &n, TIMEOUT_MS) != ST_OK || n < 8) {
		if (++ping_misses >= PING_MISSES) {
			go_down();
			reset_switch();
		}
		return;
	}
	ping_misses = 0;
	uint32_t uptime = get32(buf);
	if (uptime < bridge_stats.uptime_s) {
		/* The 8051 restarted between two pings and lost its state */
		printf("bridge: restarted\n");
		go_down();
		hello_now = true;
	}
	bridge_stats.uptime_s = uptime;
	bridge_stats.bridge_rx_dropped = buf[4] | buf[5] << 8;
	bridge_stats.bridge_link_errors = buf[6] | buf[7] << 8;
}


bool bridge_dispatch(void)
{
	bool any = ev_count != 0;

	bridge_service();
	any |= ev_count != 0;
	while (ev_count) {
		uint16_t len = events[ev_tail].len;

		memcpy(ev_cur, events[ev_tail].msg, len);
		ev_tail = (ev_tail + 1) % EVENT_SLOTS;
		ev_count--;

		const uint8_t *p = ev_cur + 2;
		uint16_t n = len - 2;
		switch (ev_cur[0]) {
		case MSG_BEACON:
			/* Beacons mean the bridge has no host: it restarted or timed us out */
			go_down();
			hello_now = true;
			break;
		case MSG_RX_FRAME:
			bridge_stats.rx_frames++;
			if (up && n > 8)
				bridge_on_rx_frame(p, ev_cur + 10, n - 8);
			break;
		case MSG_LINK:
			if (n >= 12)
				bridge_on_link(get32(p), get32(p + 4), get32(p + 8));
			break;
		case MSG_SFP:
			if (n >= 1)
				bridge_on_sfp(p[0]);
			break;
		case MSG_LOG:
			bridge_on_log((const char *)p, n);
			break;
		}
		bridge_service();
	}
	if (hello_now && !up && running)
		try_hello();
	return any;
}
