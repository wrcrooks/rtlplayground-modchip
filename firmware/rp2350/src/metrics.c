/*
 * The dashboard's data, see metrics.h.
 *
 * /history.bin, little endian:
 *   header, 16 bytes: "MCH1", version 1, port count, period (s, u16),
 *     samples (u16), sample size (u16), uptime now (s, u32)
 *   samples, oldest first, each:
 *     uptime (s, u32), switch temp (0.1 degC, i16), mod chip temp (0.1 degC,
 *     i16), main loop load (per mille, u16), bridge link load (per mille,
 *     u16, the busier direction), MAC entries (u16), IGMP groups (u16),
 *     heap used (per mille, u16), CPU-port frames/s (u16), and over all
 *     ports: multicast and broadcast packets/s in, then out (u32 each;
 *     unicast is the ports' totals less these)
 *     then per port: rx kb/s, tx kb/s, rx packets/s, tx packets/s (u32
 *     each), errors in the period (u16), discards in the period (u16),
 *     link (u8, 0 down or 1 + speed code), 3 bytes pad
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "metrics.h"
#include "plat.h"

#define LIVE_MAX_AGE_MS		900	/* polls faster than this share one read */
#define SFP_MAX_AGE_MS		30000	/* SFP diagnostics: slow I2C through the 8051 */
#define HDR_SIZE		16
#define SAMPLE_HDR		36
#define SAMPLE_PORT		24
#define BRIDGE_BYTES_PER_S	(MODCHIP_LINK_BAUD / 10)

#ifndef MODCHIP_LINK_BAUD
#define MODCHIP_LINK_BAUD	976562
#endif

static struct rtl_port ports[RTL_MAX_PORTS];
static uint8_t n_ports;
static uint32_t ports_at;
static bool ports_valid;
static struct rtl_status status;
static struct rtl_sfp sfp[2];
static uint32_t sfp_at;
static bool sfp_valid;

/* The last second, for the live view */
static uint16_t link_pm, frames_ps;
static uint32_t last_uart_rx, last_uart_tx, last_frames;

/* History */
static uint8_t *ring;
static uint8_t ring_ports;
static uint16_t ring_head, ring_count;
static uint32_t sample_at_s;
static uint64_t prev[RTL_MAX_PORTS][4];	/* in octets, out octets, in packets, out packets */
static uint64_t prev_err[RTL_MAX_PORTS], prev_dis[RTL_MAX_PORTS];
static uint64_t prev_nu[4];		/* all ports: in multicast, in broadcast, out multicast, out broadcast */
static uint32_t prev_ms;
static bool have_prev;
static uint32_t link_rx_acc, link_tx_acc;

static const uint16_t speed_mbps[] = { 0, 10, 100, 1000, 500, 10000, 2500, 5000 };

static void refresh(bool force)
{
	uint32_t now = plat_ms();

	if (!bridge_is_up()) {
		n_ports = 0;
		ports_valid = sfp_valid = false;
		return;
	}
	if (force || !ports_valid || now - ports_at >= LIVE_MAX_AGE_MS) {
		n_ports = rtl_ports_read(ports, RTL_MAX_PORTS);
		rtl_status_read(&status);
		ports_at = now;
		ports_valid = true;
	}
	if (!sfp_valid || now - sfp_at >= SFP_MAX_AGE_MS) {
		for (uint8_t i = 0; i < rtl_sfp_count() && i < 2; i++)
			rtl_sfp_read(i, &sfp[i]);
		sfp_at = now;
		sfp_valid = true;
	}
}


static uint64_t in_pkts(const struct rtl_port *p)
{
	return p->in_ucast + p->in_mcast + p->in_bcast;
}


static uint64_t out_pkts(const struct rtl_port *p)
{
	return p->out_ucast + p->out_mcast + p->out_bcast;
}


static uint64_t errs(const struct rtl_port *p)
{
	return (uint64_t)p->in_errors + p->out_errors;
}


static uint64_t discards(const struct rtl_port *p)
{
	return (uint64_t)p->in_discards + p->out_discards;
}


static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v;
	p[1] = v >> 8;
}


static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}


static uint32_t sat32(uint64_t v)
{
	return v > 0xffffffffu ? 0xffffffffu : v;
}


static uint16_t sat16(uint64_t v)
{
	return v > 0xffff ? 0xffff : v;
}


static void take_sample(void)
{
	struct plat_sys sys;
	uint32_t now = plat_ms();

	refresh(true);
	if (!n_ports)
		return;
	if (!ring) {
		ring_ports = n_ports;
		ring = calloc(METRICS_HISTORY, SAMPLE_HDR + ring_ports * SAMPLE_PORT);
		if (!ring)
			return;
	}
	uint32_t dt = now - prev_ms;
	if (have_prev && dt >= 1000) {
		size_t size = SAMPLE_HDR + ring_ports * SAMPLE_PORT;
		uint8_t *s = ring + ring_head * size;

		plat_sys_read(&sys);
		memset(s, 0, size);
		put32(s, plat_uptime_s());
		put16(s + 4, (int16_t)(status.chip_temp * 10 / 128));
		put16(s + 6, sys.temp);
		put16(s + 8, sys.load_pm);
		uint32_t busier = link_rx_acc > link_tx_acc ? link_rx_acc : link_tx_acc;
		put16(s + 10, sat16((uint64_t)busier * 1000 * 1000 / dt / BRIDGE_BYTES_PER_S));
		put16(s + 12, status.mac_entries);
		put16(s + 14, status.igmp_groups);
		put16(s + 16, sys.heap_total ? sat16((uint64_t)(sys.heap_total - sys.heap_free) * 1000 / sys.heap_total) : 0);
		put16(s + 18, frames_ps);
		uint64_t nu[4] = { 0 };
		for (uint8_t i = 0; i < n_ports; i++) {
			nu[0] += ports[i].in_mcast;
			nu[1] += ports[i].in_bcast;
			nu[2] += ports[i].out_mcast;
			nu[3] += ports[i].out_bcast;
		}
		for (int k = 0; k < 4; k++)
			put32(s + 20 + 4 * k, sat32((nu[k] - prev_nu[k]) * 1000 / dt));
		for (uint8_t i = 0; i < ring_ports && i < n_ports; i++) {
			const struct rtl_port *p = &ports[i];
			uint8_t *o = s + SAMPLE_HDR + i * SAMPLE_PORT;
			put32(o, sat32((p->in_octets - prev[i][0]) * 8 / dt));		/* bits/ms = kb/s */
			put32(o + 4, sat32((p->out_octets - prev[i][1]) * 8 / dt));
			put32(o + 8, sat32((in_pkts(p) - prev[i][2]) * 1000 / dt));
			put32(o + 12, sat32((out_pkts(p) - prev[i][3]) * 1000 / dt));
			put16(o + 16, sat16(errs(p) - prev_err[i]));
			put16(o + 18, sat16(discards(p) - prev_dis[i]));
			o[20] = p->link;
		}
		ring_head = (ring_head + 1) % METRICS_HISTORY;
		if (ring_count < METRICS_HISTORY)
			ring_count++;
	}
	memset(prev_nu, 0, sizeof(prev_nu));
	for (uint8_t i = 0; i < n_ports; i++) {
		prev_nu[0] += ports[i].in_mcast;
		prev_nu[1] += ports[i].in_bcast;
		prev_nu[2] += ports[i].out_mcast;
		prev_nu[3] += ports[i].out_bcast;
		prev[i][0] = ports[i].in_octets;
		prev[i][1] = ports[i].out_octets;
		prev[i][2] = in_pkts(&ports[i]);
		prev[i][3] = out_pkts(&ports[i]);
		prev_err[i] = errs(&ports[i]);
		prev_dis[i] = discards(&ports[i]);
	}
	prev_ms = now;
	have_prev = true;
	link_rx_acc = link_tx_acc = 0;
}


void metrics_second(void)
{
	uint32_t rx = bridge_stats.uart_rx_bytes - last_uart_rx;
	uint32_t tx = bridge_stats.uart_tx_bytes - last_uart_tx;

	last_uart_rx = bridge_stats.uart_rx_bytes;
	last_uart_tx = bridge_stats.uart_tx_bytes;
	link_rx_acc += rx;
	link_tx_acc += tx;
	link_pm = sat16((uint64_t)(rx > tx ? rx : tx) * 1000 / BRIDGE_BYTES_PER_S);
	frames_ps = sat16(bridge_stats.rx_frames - last_frames);
	last_frames = bridge_stats.rx_frames;

	if (!bridge_is_up()) {
		have_prev = false;
		return;
	}
	uint32_t t = plat_uptime_s();
	if (!have_prev || t - sample_at_s >= METRICS_PERIOD_S) {
		sample_at_s = t;
		take_sample();
	}
}

/* ---- /metrics.json ---- */

struct out {
	char *buf;
	size_t max, len;
};

static void add(struct out *o, const char *fmt, ...)
{
	va_list ap;

	if (o->len >= o->max)
		return;
	va_start(ap, fmt);
	int n = vsnprintf(o->buf + o->len, o->max - o->len, fmt, ap);
	va_end(ap);
	if (n > 0)
		o->len += (size_t)n < o->max - o->len ? (size_t)n : o->max - o->len;
}


static void add_str(struct out *o, const char *s)
{
	add(o, "\"");
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			add(o, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			add(o, "\\u%04x", *s);
		else
			add(o, "%c", *s);
	}
	add(o, "\"");
}


static const char *b(bool v)
{
	return v ? "true" : "false";
}


size_t metrics_json(char *buf, size_t max)
{
	struct out o = { buf, max, 0 };
	struct plat_sys sys;
	struct rtl_link_event ev[32];

	refresh(false);
	plat_sys_read(&sys);
	add(&o, "{\"t\":%lu,\"uptime\":%lu,\"period\":%u,\"bridge_up\":%s,\"ports\":[",
	    (unsigned long)plat_ms(), (unsigned long)plat_uptime_s(), METRICS_PERIOD_S, b(bridge_is_up()));
	for (uint8_t i = 0; i < n_ports; i++) {
		const struct rtl_port *p = &ports[i];
		add(&o, "%s{\"n\":%u,\"name\":", i ? "," : "", i + 1);
		add_str(&o, p->name);
		add(&o, ",\"en\":%s,\"sfp\":%s,\"max\":%u,\"link\":%u,\"mbps\":%u,\"c\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%lu,%lu]}",
		    b(p->enabled), b(p->sfp), p->max_mbps, p->link, p->link < 8 ? speed_mbps[p->link] : 0,
		    (unsigned long long)p->in_octets, (unsigned long long)p->out_octets,
		    (unsigned long long)p->in_ucast, (unsigned long long)p->in_mcast, (unsigned long long)p->in_bcast,
		    (unsigned long long)p->out_ucast, (unsigned long long)p->out_mcast, (unsigned long long)p->out_bcast,
		    (unsigned long)p->in_discards, (unsigned long)p->out_discards,
		    (unsigned long)p->in_errors, (unsigned long)p->out_errors);
	}
	add(&o, "],\"switch\":{\"temp\":%.1f,\"mac\":%u},", status.chip_temp / 128.0, status.mac_entries);
	add(&o, "\"stp\":{\"on\":%s,\"root_port\":%u,\"tc\":%lu,\"root_changes\":%lu},",
	    b(status.stp_on), status.stp_root_port, (unsigned long)status.stp_tc, (unsigned long)status.stp_root_changes);
	add(&o, "\"lacp\":{\"on\":%s,\"up\":%u,\"changes\":%lu},",
	    b(status.lacp_on), status.lacp_ports_up, (unsigned long)status.lacp_changes);
	add(&o, "\"igmp\":{\"on\":%s,\"groups\":%u,\"joins\":%lu,\"leaves\":%lu},",
	    b(status.igmp_on), status.igmp_groups, (unsigned long)status.igmp_joins, (unsigned long)status.igmp_leaves);
	add(&o, "\"sfp\":[");
	for (uint8_t i = 0; i < rtl_sfp_count() && i < 2 && bridge_is_up(); i++) {
		const struct rtl_sfp *d = &sfp[i];
		add(&o, "%s{\"slot\":%u,\"present\":%s,\"los\":%s,\"vendor\":", i ? "," : "", i + 1, b(d->present), b(d->los));
		add_str(&o, d->vendor);
		add(&o, ",\"model\":");
		add_str(&o, d->model);
		add(&o, ",\"ddm\":%s", b(d->ddm));
		if (d->ddm)
			add(&o, ",\"temp\":%.2f,\"vcc\":%.3f,\"bias\":%.2f,\"tx_uw\":%.1f,\"rx_uw\":%.1f",
			    d->temp / 256.0, d->vcc / 10000.0, d->bias * 0.002, d->tx_power / 10.0, d->rx_power / 10.0);
		add(&o, "}");
	}
	add(&o, "],\"modchip\":{\"load\":%.1f,\"heap_free\":%lu,\"heap_total\":%lu,\"temp\":%.1f},",
	    sys.load_pm / 10.0, (unsigned long)sys.heap_free, (unsigned long)sys.heap_total, sys.temp / 10.0);
	const struct bridge_stats *bs = &bridge_stats;
	add(&o, "\"bridge\":{\"link_load\":%.1f,\"frames_ps\":%u,\"requests\":%lu,\"timeouts\":%lu,\"link_errors\":%lu,"
	    "\"events_dropped\":%lu,\"rx_overruns\":%lu,\"resets\":%lu,\"rx_frames\":%lu,\"tx_frames\":%lu,"
	    "\"uart_rx\":%lu,\"uart_tx\":%lu,\"uptime\":%lu,\"rx_dropped\":%u,\"bridge_link_errors\":%u},",
	    link_pm / 10.0, frames_ps, (unsigned long)bs->requests, (unsigned long)bs->timeouts,
	    (unsigned long)bs->link_errors, (unsigned long)bs->events_dropped, (unsigned long)bs->rx_overruns,
	    (unsigned long)bs->resets, (unsigned long)bs->rx_frames, (unsigned long)bs->tx_frames,
	    (unsigned long)bs->uart_rx_bytes, (unsigned long)bs->uart_tx_bytes, (unsigned long)bs->uptime_s,
	    bs->bridge_rx_dropped, bs->bridge_link_errors);
	struct plat_t3 t3;
	plat_t3_status(&t3);
	add(&o, "\"t3\":{\"up\":%s,\"enabled\":%s,\"khz\":%u,\"reads\":%lu,\"writes\":%lu,\"errors\":%lu},",
	    b(t3.up), b(t3.enabled), t3.khz, (unsigned long)t3.reads, (unsigned long)t3.writes, (unsigned long)t3.errors);
	add(&o, "\"events\":[");
	uint8_t n = rtl_link_events(ev, 32);
	for (uint8_t i = 0; i < n; i++)
		add(&o, "%s{\"seq\":%lu,\"t\":%lu,\"port\":%u,\"link\":%u,\"mbps\":%u}", i ? "," : "",
		    (unsigned long)ev[i].seq, (unsigned long)ev[i].t_ms, ev[i].port, ev[i].link,
		    ev[i].link < 8 ? speed_mbps[ev[i].link] : 0);
	add(&o, "]}");
	return o.len;
}

/* ---- /history.bin ---- */

uint8_t *metrics_history(size_t *len)
{
	size_t size = SAMPLE_HDR + ring_ports * SAMPLE_PORT;
	uint8_t *out;

	*len = HDR_SIZE + ring_count * size;
	out = malloc(*len);
	if (!out)
		return NULL;
	memcpy(out, "MCH1", 4);
	out[4] = 1;
	out[5] = ring_ports;
	put16(out + 6, METRICS_PERIOD_S);
	put16(out + 8, ring_count);
	put16(out + 10, size);
	put32(out + 12, plat_uptime_s());
	for (uint16_t k = 0; k < ring_count; k++) {
		uint16_t idx = (ring_head + METRICS_HISTORY - ring_count + k) % METRICS_HISTORY;
		memcpy(out + HDR_SIZE + k * size, ring + idx * size, size);
	}
	return out;
}
