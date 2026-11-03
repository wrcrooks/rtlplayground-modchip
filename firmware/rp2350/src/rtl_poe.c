/*
 * RTLPlayground's PoE interface (rtl8238.h) on the mod chip. Only the 8051
 * reaches the RTL8238B, over bit-banged I2C on GPIO46/47, and it loads the
 * controller's image at boot; these functions are its POE_* requests. The
 * console's poe command, the configuration's "poe port <n> on|off" lines and
 * /poe.json (page_poe.c) call them unchanged.
 *
 * Compiled with sdcc_shim.h, like the modules. Without a PoE controller
 * (machine.pse_rtl8238, and HELLO_POE from the bridge) they report nothing
 * and change nothing.
 */

#include "rtl837x_common.h"
#include "machine.h"
#include "rtl8238.h"
#include "bridge.h"

extern __code const struct machine machine;

__xdata struct pse_port pse_ports[PSE_PORTS];
__xdata uint8_t pse_loaded;

#define POE_PORT_BYTES	8	/* det cls on en i_raw:2 v_raw:2 */


static bool poe_present(void)
{
	return machine.pse_rtl8238 && bridge_is_up() && (bridge_hello.flags & HELLO_POE);
}


bool pse_read_ports(void) __banked
{
	uint8_t r[2 + PSE_PORTS * POE_PORT_BYTES];
	size_t n;

	if (!poe_present()) {
		pse_loaded = 0;
		return false;
	}
	if (bridge_request(REQ_POE_READ, NULL, 0, NULL, 0, r, sizeof(r), &n) != ST_OK || n < 2
	    || r[1] > PSE_PORTS || n != 2 + r[1] * POE_PORT_BYTES)
		return false;
	pse_loaded = r[0];
	for (uint8_t i = 0; i < r[1]; i++) {
		const uint8_t *q = r + 2 + i * POE_PORT_BYTES;
		pse_ports[i].det = q[0];
		pse_ports[i].cls = q[1];
		pse_ports[i].on = q[2];
		pse_ports[i].en = q[3];
		pse_ports[i].i_raw = q[4] | q[5] << 8;
		pse_ports[i].v_raw = q[6] | q[7] << 8;
	}
	return pse_loaded && r[1] == PSE_PORTS;
}


/* The 8051 logs what it wrote, which arrives as a LOG message */
bool pse_port_set(uint8_t port, bool on) __banked
{
	uint8_t p[2] = { port, on };

	if (!poe_present())
		return false;
	return bridge_request(REQ_POE_PORT, p, 2, NULL, 0, NULL, 0, NULL) == ST_OK;
}


/* Reloads the controller's image; the 8051 logs its progress */
void pse_load(void) __banked
{
	uint8_t r[1];
	size_t n;

	if (!poe_present()) {
		print_string("PSE: no RTL8238B on this switch\n");
		return;
	}
	print_string("PSE: reloading the controller, about 10 s\n");
	if (bridge_request(REQ_POE_LOAD, NULL, 0, NULL, 0, r, sizeof(r), &n) != ST_OK || n != 1) {
		print_string("PSE: the bridge didn't answer the load\n");
		return;
	}
	pse_loaded = r[0];
}


/*
 * The poe command without arguments. RTLPlayground's own prints the chip's
 * registers; the bridge only returns the decoded readings, so this prints
 * those, converted as the web UI does.
 */
void pse_status(void) __banked
{
	if (!poe_present()) {
		print_string("PSE: no RTL8238B on this switch\n");
		return;
	}
	if (!pse_read_ports()) {
		print_string(pse_loaded ? "PSE: no answer from the controller\n"
			     : "PSE: controller not running (no image loaded)\n");
		return;
	}
	print_string("PSE: controller running\n");
	for (uint8_t i = 0; i < PSE_PORTS; i++) {
		if (machine.pse_channel[i] > 3)
			continue;
		struct pse_port *p = &pse_ports[i];
		uint16_t mv = (uint32_t)(p->v_raw & 0x7fff) * 1000 / 512;
		uint16_t ma = (uint32_t)p->i_raw * 125 / 4096;
		print_string("  port "); write_char('1' + i);
		print_string(p->en ? (p->on ? ": powered" : ": on, not powered") : ": off");
		print_string(", detect "); itoa(p->det);		/* 4 valid, 6 open */
		print_string(", class "); itoa(p->cls == 0xc ? 0 : p->cls);	/* 0xc is class 0 */
		print_string(", "); itoa_short(mv); print_string(" mV, ");
		itoa_short(ma); print_string(" mA\n");
	}
}
