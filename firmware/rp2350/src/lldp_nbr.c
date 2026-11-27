/* LLDP neighbours, see lldp_nbr.h */
#include <stdio.h>
#include <string.h>

#include "lldp_nbr.h"

static struct lldp_nbr nbr[LLDP_NBR_PORTS + 1];

/* Printable text of a TLV value, NUL-terminated */
static void text(char *out, size_t max, const uint8_t *v, uint16_t n)
{
	size_t k = 0;

	for (uint16_t i = 0; i < n && k + 1 < max; i++)
		out[k++] = v[i] >= 0x20 && v[i] < 0x7f ? v[i] : '?';
	out[k] = 0;
}

static void mac(char *out, size_t max, const uint8_t *v)
{
	snprintf(out, max, "%02x:%02x:%02x:%02x:%02x:%02x", v[0], v[1], v[2], v[3], v[4], v[5]);
}

void lldp_nbr_rx(uint8_t front, const uint8_t *tlv, uint16_t len, uint32_t now)
{
	struct lldp_nbr n;
	bool chassis = false, port = false, ttl_seen = false;
	uint16_t ttl = 0;

	if (front < 1 || front > LLDP_NBR_PORTS)
		return;
	memset(&n, 0, sizeof(n));
	for (uint16_t i = 0; i + 2 <= len;) {
		uint8_t type = tlv[i] >> 1;
		uint16_t l = (tlv[i] & 1) << 8 | tlv[i + 1];
		const uint8_t *v = tlv + i + 2;

		if (i + 2 + l > len)
			return;		/* runs past the frame: not one */
		i += 2 + l;
		if (type == 0)
			break;
		switch (type) {
		case 1:		/* chassis ID: subtype, ID */
			if (l < 2)
				return;
			n.chassis_mac = v[0] == 4 && l == 7;
			if (n.chassis_mac)
				mac(n.chassis, sizeof(n.chassis), v + 1);
			else
				text(n.chassis, sizeof(n.chassis), v + 1, l - 1);
			chassis = true;
			break;
		case 2:		/* port ID: subtype, ID */
			if (l < 2)
				return;
			if (v[0] == 3 && l == 7)
				mac(n.port, sizeof(n.port), v + 1);
			else
				text(n.port, sizeof(n.port), v + 1, l - 1);
			port = true;
			break;
		case 3:
			if (l < 2)
				return;
			ttl = v[0] << 8 | v[1];
			ttl_seen = true;
			break;
		case 4:
			text(n.port_desc, sizeof(n.port_desc), v, l);
			break;
		case 5:
			text(n.name, sizeof(n.name), v, l);
			break;
		case 8:		/* management address: length, subtype, address, ... */
			if (l >= 6 && v[0] == 5 && v[1] == 1 && !(n.ip[0] | n.ip[1] | n.ip[2] | n.ip[3]))
				memcpy(n.ip, v + 2, 4);
			break;
		}
	}
	/* the three a LLDPDU must start with */
	if (!chassis || !port || !ttl_seen)
		return;
	if (!ttl) {	/* shutdown: forget it */
		memset(&nbr[front], 0, sizeof(nbr[front]));
		return;
	}
	n.expires = now + ttl;
	if (!n.expires)
		n.expires = 1;
	nbr[front] = n;
}

const struct lldp_nbr *lldp_nbr_get(uint8_t front, uint32_t now)
{
	if (front < 1 || front > LLDP_NBR_PORTS || !nbr[front].expires)
		return NULL;
	if ((int32_t)(nbr[front].expires - now) <= 0) {
		memset(&nbr[front], 0, sizeof(nbr[front]));
		return NULL;
	}
	return &nbr[front];
}

size_t lldp_nbr_show(char *out, size_t max, uint32_t now)
{
	size_t k = 0;
	int w;

	if (!max)
		return 0;
	out[0] = 0;
	for (uint8_t p = 1; p <= LLDP_NBR_PORTS; p++) {
		const struct lldp_nbr *n = lldp_nbr_get(p, now);

		if (!n)
			continue;
		w = snprintf(out + k, max - k, "port %u: %s, port %s%s%s%s", p, n->name[0] ? n->name : n->chassis,
			     n->port, n->port_desc[0] ? " (" : "", n->port_desc, n->port_desc[0] ? ")" : "");
		if (w < 0 || (size_t)w >= max - k)
			break;
		k += w;
		if (n->ip[0] | n->ip[1] | n->ip[2] | n->ip[3])
			w = snprintf(out + k, max - k, ", %u.%u.%u.%u", n->ip[0], n->ip[1], n->ip[2], n->ip[3]);
		else
			w = 0;
		if (w < 0 || (size_t)w >= max - k)
			break;
		k += w;
		w = snprintf(out + k, max - k, ", %lu s left\n", (unsigned long)(n->expires - now));
		if (w < 0 || (size_t)w >= max - k)
			break;
		k += w;
	}
	if (!k)
		k = snprintf(out, max, "no LLDP neighbours (they are heard while \"lldp on\")\n");
	return k < max ? k : max - 1;
}

void lldp_nbr_clear(void)
{
	memset(nbr, 0, sizeof(nbr));
}
