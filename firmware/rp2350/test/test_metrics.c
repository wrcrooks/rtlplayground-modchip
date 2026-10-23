/*
 * metrics.c on the host: an hour and a bit of simulated time, six ports
 * with known traffic, then checks of the history ring and /history.bin, and
 * /metrics.json written to build/ for run_metrics.py to parse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "metrics.h"
#include "plat.h"

struct bridge_hello bridge_hello;
struct bridge_stats bridge_stats;
static uint32_t now_ms = 5000;
static int failures;

static void check(int cond, const char *what)
{
	printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
	failures += !cond;
}

bool bridge_is_up(void) { return true; }
uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_uptime_s(void) { return now_ms / 1000; }
uint8_t rtl_port_count(void) { return 6; }

/* Port n: (100 * n) Mb/s in, half that out, 1500-byte frames; port 2 also
 * 3 errors and 1 discard a second; port 4 down */
uint8_t rtl_ports_read(struct rtl_port *p, uint8_t max)
{
	double t = now_ms / 1000.0;

	for (int i = 0; i < 6 && i < max; i++) {
		int n = i + 1;
		memset(&p[i], 0, sizeof(p[i]));
		snprintf(p[i].name, sizeof(p[i].name), n == 1 ? "uplink \"core\"" : "Port %d", n);
		p[i].enabled = true;
		p[i].sfp = n >= 5;
		p[i].max_mbps = n >= 5 ? 10000 : 2500;
		p[i].link = n == 4 ? 0 : 3;
		p[i].in_octets = (uint64_t)(t * 100e6 * n / 8);
		p[i].out_octets = (uint64_t)(t * 50e6 * n / 8);
		p[i].in_ucast = (uint64_t)(t * 100e6 * n / 8 / 1500);
		p[i].out_ucast = (uint64_t)(t * 50e6 * n / 8 / 1500);
		p[i].in_bcast = (uint64_t)t;
		if (n == 2) {
			p[i].in_errors = (uint32_t)(t * 3);
			p[i].in_discards = (uint32_t)t;
		}
	}
	return 6;
}

void rtl_status_read(struct rtl_status *s)
{
	memset(s, 0, sizeof(*s));
	s->chip_temp = 45 * 128 + 64;	/* 45.5 degC */
	s->mac_entries = 17;
	s->stp_on = true;
	s->stp_root_port = 0xff;
	s->stp_tc = 2;
	s->igmp_on = true;
	s->igmp_groups = 3;
	s->igmp_joins = 9;
	s->igmp_leaves = 4;
}

uint8_t rtl_sfp_count(void) { return 2; }

void rtl_sfp_read(uint8_t slot, struct rtl_sfp *d)
{
	memset(d, 0, sizeof(*d));
	if (slot)
		return;
	d->present = d->ddm = true;
	strcpy(d->vendor, "FS");
	strcpy(d->model, "SFP-10GSR-85");
	d->temp = 35 * 256 + 128;	/* 35.5 degC */
	d->vcc = 33000;			/* 3.3 V */
	d->bias = 3100;			/* 6.2 mA */
	d->tx_power = 5012;		/* 501.2 uW: -3.0 dBm */
	d->rx_power = 2512;		/* 251.2 uW: -6.0 dBm */
}

uint8_t rtl_link_events(struct rtl_link_event *out, uint8_t max)
{
	if (max < 1)
		return 0;
	out[0] = (struct rtl_link_event){ .seq = 1, .t_ms = 4000, .port = 4, .link = 0 };
	return 1;
}

void plat_sys_read(struct plat_sys *s)
{
	s->load_pm = 125;
	s->heap_free = 200 * 1024;
	s->heap_total = 270 * 1024;
	s->temp = 382;
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "build";
	char path[256];
	size_t len;

	/* The bridge link: 9766 bytes a second in, 4883 out (10 % and 5 %) */
	for (int s = 0; s < 3700; s++) {
		bridge_stats.uart_rx_bytes += 9766;
		bridge_stats.uart_tx_bytes += 4883;
		bridge_stats.rx_frames += 20;
		now_ms += 1000;
		metrics_second();
	}

	uint8_t *h = metrics_history(&len);
	check(h && !memcmp(h, "MCH1", 4) && h[4] == 1 && h[5] == 6, "history header: version 1, 6 ports");
	uint16_t n = u16(h + 8), size = u16(h + 10);
	check(u16(h + 6) == METRICS_PERIOD_S && n == METRICS_HISTORY, "a full hour of 10 s samples");
	check(size == 36 + 6 * 24 && len == 16 + (size_t)n * size, "sample size and file length");
	const uint8_t *s = h + 16 + (n - 1) * size;
	check(u32(h + 12) == now_ms / 1000 && u32(s) <= now_ms / 1000 && u32(s) + METRICS_PERIOD_S > now_ms / 1000,
	      "newest sample is the latest");
	check(u32(h + 16 + size) - u32(h + 16) == METRICS_PERIOD_S, "samples 10 s apart, oldest first");
	check((int16_t)u16(s + 4) == 455 && (int16_t)u16(s + 6) == 382, "temperatures in 0.1 degC");
	check(u16(s + 8) == 125 && u16(s + 10) == 100, "main loop load 12.5 %, bridge link load 10 %");
	check(u16(s + 12) == 17 && u16(s + 14) == 3 && u16(s + 16) == 259 && u16(s + 18) == 20,
	      "MAC entries, IGMP groups, heap used per mille, CPU-port frames/s");
	int rates_ok = 1;
	for (int i = 0; i < 6; i++) {
		const uint8_t *p = s + 36 + i * 24;
		long rx = u32(p), tx = u32(p + 4), rxp = u32(p + 8);
		rates_ok &= rx >= 99999 * (i + 1) && rx <= 100001 * (i + 1) && tx >= 49999 * (i + 1) && tx <= 50001 * (i + 1);
		rates_ok &= rxp >= 8332 * (i + 1) && rxp <= 8335 * (i + 1) + 1;
		rates_ok &= p[20] == (i == 3 ? 0 : 3);
	}
	check(rates_ok, "per-port kb/s, packets/s and link match the simulated traffic");
	check(u32(s + 20) == 0 && u32(s + 24) == 6 && u32(s + 28) == 0 && u32(s + 32) == 0,
	      "all ports: 6 broadcast packets/s in, no multicast");
	const uint8_t *p2 = s + 36 + 24;
	check(u16(p2 + 16) == 30 && u16(p2 + 18) == 10, "port 2: 30 errors and 10 discards per 10 s");
	free(h);

	static char json[8192];
	len = metrics_json(json, sizeof(json));
	snprintf(path, sizeof(path), "%s/metrics.json", dir);
	FILE *f = fopen(path, "w");
	fwrite(json, 1, len, f);
	fclose(f);
	check(len > 0 && len < sizeof(json) - 1 && json[len - 1] == '}', "metrics.json written");
	h = metrics_history(&len);
	snprintf(path, sizeof(path), "%s/history.bin", dir);
	f = fopen(path, "wb");
	fwrite(h, 1, len, f);
	fclose(f);
	free(h);

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
