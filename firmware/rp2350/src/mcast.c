/*
 * IGMP and MLD snooping, see mcast.h (RFC 4541, 2236, 3376, 2710, 3810).
 *
 * A group is known by its address, the switch by its MAC, which 32 IPv4
 * groups share: an entry's ports are those of all its groups and the VLAN's
 * router ports. Source filters (IGMPv3, MLDv2) count as membership of the
 * group: the switch does not filter by source. Link-local groups (224.0.0.x,
 * ff02::) always flood: routing protocols and IPv6 neighbor discovery live
 * there. So does unknown IPv6 multicast; unknown IPv4 multicast can be kept
 * to the router ports, with entries for the link-local groups.
 */

#include <string.h>

#include "events.h"
#include "mcast.h"

#define ROBUSTNESS	2
#define RESPONSE_S	10
#define LEAVE_S		3	/* after a leave, until the port is taken out */
#define STARTUP_S	(mc_cfg.interval / 4)
#define MEMBER_S	(ROBUSTNESS * mc_cfg.interval + RESPONSE_S)
#define OTHER_QUERIER_S	(ROBUSTNESS * mc_cfg.interval + RESPONSE_S / 2)

struct group {
	uint16_t vlan;			/* 0: free */
	bool v6;
	uint8_t addr[16];
	uint16_t left[MC_PORTS];	/* s of membership left */
};

struct router {
	uint16_t vlan;			/* 0: free */
	uint8_t port;
	uint16_t left;
};

struct vlan {
	uint16_t id;			/* 0: free */
	uint16_t query_in;		/* s to our next general query */
	uint8_t startup;
	uint16_t other_left[2];		/* IPv4, IPv6: another querier's time left */
	uint8_t other[2][16];
};

struct mc_config mc_cfg;
static struct group groups[MC_GROUPS];
static struct router routers[MC_ROUTERS];
static struct vlan vlans[MC_VLANS];
static uint16_t unknown_now;
static bool unknown_set;
static bool full_said;

static const uint8_t local4[] = { 1, 2, 5, 6, 9, 10, 13, 18, 22, 102, 251, 252 };

static uint16_t get16(const uint8_t *p)
{
	return p[0] << 8 | p[1];
}


static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}


static uint32_t sum16(const uint8_t *p, uint16_t n, uint32_t s)
{
	for (; n > 1; n -= 2, p += 2)
		s += get16(p);
	if (n)
		s += p[0] << 8;
	return s;
}


static uint16_t fold(uint32_t s)
{
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s;
}


/* ICMPv6 checksum state: the pseudo header of src, dst (adjacent) and length */
static uint32_t pseudo6(const uint8_t *addrs, uint16_t len)
{
	return sum16(addrs, 32, (uint32_t)len + 58);
}


/* ---- tables ---- */

static void group_mac(const struct group *g, uint8_t *mac)
{
	if (g->v6) {
		mac[0] = mac[1] = 0x33;
		memcpy(mac + 2, g->addr + 12, 4);
	} else {
		mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
		mac[3] = g->addr[1] & 0x7f;
		mac[4] = g->addr[2];
		mac[5] = g->addr[3];
	}
}


static uint16_t group_ports(const struct group *g)
{
	uint16_t m = 0;

	for (int i = 0; i < MC_PORTS; i++)
		if (g->left[i])
			m |= 1 << i;
	return m;
}


static uint16_t router_ports(uint16_t vlan)
{
	uint16_t m = mc_cfg.routers;

	for (int i = 0; i < MC_ROUTERS; i++)
		if (routers[i].vlan == vlan)
			m |= mc_hw_lag(routers[i].port);
	return m & mc_hw_ports();
}


/* The switch's entry for a group's MAC: all its groups' ports */
static void write_entry(uint16_t vlan, const uint8_t *mac)
{
	uint16_t ports = 0;
	uint8_t m[6];

	for (int i = 0; i < MC_GROUPS; i++) {
		if (groups[i].vlan != vlan)
			continue;
		group_mac(&groups[i], m);
		if (!memcmp(m, mac, 6))
			ports |= group_ports(&groups[i]);
	}
	/* no entry without members: unknown is unknown */
	mc_hw_entry(vlan, mac, ports ? ports | router_ports(vlan) : 0);
}


static void write_group(const struct group *g)
{
	uint8_t mac[6];

	group_mac(g, mac);
	write_entry(g->vlan, mac);
}


static void unknown_update(void)
{
	uint16_t m = mc_cfg.routers;

	for (int i = 0; i < MC_ROUTERS; i++)
		if (routers[i].vlan)
			m |= mc_hw_lag(routers[i].port);
	if (unknown_set && m == unknown_now)
		return;
	unknown_set = true;
	unknown_now = m;
	mc_hw_mode(mc_cfg.on, mc_cfg.restrict_unknown, m);
}


static void vlan_groups_update(uint16_t vlan)
{
	for (int i = 0; i < MC_GROUPS; i++)
		if (groups[i].vlan == vlan)
			write_group(&groups[i]);
	unknown_update();
}


/* The link-local groups as entries of their own, where unknown ones don't flood */
static void local_entries(uint16_t vlan, bool on)
{
	uint16_t members = 0, tagged;
	uint8_t mac[6] = { 0x01, 0x00, 0x5e, 0, 0, 0 };

	if (on && !mc_hw_vlan(vlan, &members, &tagged))
		return;
	for (unsigned i = 0; i < sizeof(local4); i++) {
		mac[5] = local4[i];
		mc_hw_entry(vlan, mac, members & mc_hw_ports());
	}
}


static struct vlan *vlan_of(uint16_t id)
{
	struct vlan *f = NULL;

	for (int i = 0; i < MC_VLANS; i++) {
		if (vlans[i].id == id)
			return &vlans[i];
		if (!vlans[i].id && !f)
			f = &vlans[i];
	}
	if (f) {
		memset(f, 0, sizeof(*f));
		f->id = id;
		f->startup = ROBUSTNESS;
		f->query_in = 1;
		if (mc_cfg.restrict_unknown)
			local_entries(id, true);
	}
	return f;
}


static struct group *group_of(uint16_t vlan, bool v6, const uint8_t *addr, bool make)
{
	struct group *f = NULL;
	uint8_t n = v6 ? 16 : 4;

	for (int i = 0; i < MC_GROUPS; i++) {
		struct group *g = &groups[i];
		if (g->vlan == vlan && g->v6 == v6 && !memcmp(g->addr, addr, n))
			return g;
		if (!g->vlan && !f)
			f = g;
	}
	if (!make)
		return NULL;
	if (!f) {
		if (!full_said)
			event(EV_SYSTEM, 0, 0, "Multicast snooping: the group table is full");
		full_said = true;
		return NULL;
	}
	memset(f, 0, sizeof(*f));
	f->vlan = vlan;
	f->v6 = v6;
	memcpy(f->addr, addr, n);
	return f;
}


static bool snoopable(bool v6, const uint8_t *a)
{
	if (v6)
		return a[0] == 0xff && (a[1] & 0x0f) > 2;	/* wider than link-local */
	return (a[0] & 0xf0) == 0xe0 && !(a[0] == 224 && !a[1] && !a[2]);
}


static void join(uint8_t port, uint16_t vlan, bool v6, const uint8_t *addr)
{
	uint16_t lag = mc_hw_lag(port), was;
	struct group *g;

	if (!snoopable(v6, addr) || !(g = group_of(vlan, v6, addr, true)))
		return;
	vlan_of(vlan);
	was = group_ports(g);
	for (int i = 0; i < MC_PORTS; i++)
		if ((lag >> i) & 1)
			g->left[i] = MEMBER_S;
	if (group_ports(g) != was)
		write_group(g);
}


static bool is_querier(uint16_t vlan, bool v6)
{
	struct vlan *v = vlan_of(vlan);

	return mc_cfg.querier && v && !v->other_left[v6];
}

static void query(uint16_t vlan, bool v6, const uint8_t *group);

static void leave(uint8_t port, uint16_t vlan, bool v6, const uint8_t *addr)
{
	uint16_t lag = mc_hw_lag(port);
	struct group *g;

	if (!snoopable(v6, addr) || !(g = group_of(vlan, v6, addr, false)))
		return;
	/* another member behind the port answers the querier's question in time */
	for (int i = 0; i < MC_PORTS; i++)
		if (((lag >> i) & 1) && g->left[i] > LEAVE_S)
			g->left[i] = LEAVE_S;
	if (is_querier(vlan, v6))
		query(vlan, v6, addr);
}


/* ---- out ---- */

/* To those of ports that are in the VLAN, tagged where they are */
static void relay(uint16_t ports, uint16_t vlan, const uint8_t *f, uint16_t len)
{
	uint16_t members, tagged, up = mc_hw_up();

	if (!mc_hw_vlan(vlan, &members, &tagged))
		return;
	ports &= members & mc_hw_ports() & up;
	/* one port of an aggregation */
	for (int i = 0; i < MC_PORTS; i++)
		if ((ports >> i) & 1)
			ports &= ~(mc_hw_lag(i) & ~(1 << i));
	if (ports & tagged)
		mc_hw_tx(ports & tagged, vlan, true, f, len);
	if (ports & ~tagged)
		mc_hw_tx(ports & ~tagged, vlan, false, f, len);
}


static void link_local6(uint8_t *a)
{
	const uint8_t *m = mc_hw_mac();

	memset(a, 0, 16);
	a[0] = 0xfe; a[1] = 0x80;
	memcpy(a + 8, m, 3);
	a[8] ^= 2;
	a[11] = 0xff; a[12] = 0xfe;
	memcpy(a + 13, m + 3, 3);
}


/* A general query, or one for a group that somebody left */
static void query(uint16_t vlan, bool v6, const uint8_t *group)
{
	static const uint8_t all4[4] = { 224, 0, 0, 1 };
	static const uint8_t all6[16] = { 0xff, 2, [15] = 1 };
	uint8_t f[14 + 40 + 8 + 28];
	uint16_t len;

	memset(f, 0, sizeof(f));
	memcpy(f + 6, mc_hw_mac(), 6);
	if (!v6) {
		const uint8_t *dst = group ? group : all4;
		uint8_t *ip = f + 14, *q = ip + 24;

		if (!memcmp(mc_hw_ip(), "\0\0\0\0", 4))
			return;		/* no address yet: nobody would believe it */
		f[0] = 0x01; f[2] = 0x5e;
		f[3] = dst[1] & 0x7f; f[4] = dst[2]; f[5] = dst[3];
		f[12] = 0x08;
		ip[0] = 0x46;
		ip[1] = 0xc0;
		put16(ip + 2, 24 + 12);
		ip[8] = 1;
		ip[9] = 2;
		memcpy(ip + 12, mc_hw_ip(), 4);
		memcpy(ip + 16, dst, 4);
		ip[20] = 0x94; ip[21] = 4;	/* Router Alert */
		put16(ip + 10, fold(sum16(ip, 24, 0)));
		q[0] = 0x11;
		q[1] = group ? 10 : 10 * RESPONSE_S;
		if (group)
			memcpy(q + 4, group, 4);
		q[8] = ROBUSTNESS;
		q[9] = mc_cfg.interval < 128 ? mc_cfg.interval : 127;
		put16(q + 2, fold(sum16(q, 12, 0)));
		len = 14 + 24 + 12;
	} else {
		const uint8_t *dst = group ? group : all6;
		uint8_t *ip = f + 14, *h = ip + 40, *q = h + 8;

		f[0] = f[1] = 0x33;
		memcpy(f + 2, dst + 12, 4);
		f[12] = 0x86; f[13] = 0xdd;
		ip[0] = 0x60;
		put16(ip + 4, 8 + 28);
		ip[6] = 0;			/* hop-by-hop */
		ip[7] = 1;
		link_local6(ip + 8);
		memcpy(ip + 24, dst, 16);
		h[0] = 58;
		h[2] = 5; h[3] = 2;		/* Router Alert: MLD */
		h[6] = 1; h[7] = 0;		/* PadN */
		q[0] = 130;
		put16(q + 4, group ? 1000 : 1000 * RESPONSE_S);
		if (group)
			memcpy(q + 8, group, 16);
		q[24] = ROBUSTNESS;
		q[25] = mc_cfg.interval < 128 ? mc_cfg.interval : 127;
		put16(q + 2, fold(sum16(q, 28, pseudo6(ip + 8, 28))));
		len = 14 + 40 + 8 + 28;
	}
	relay(0xffff, vlan, f, len);
}


/* ---- in ---- */

static void router_seen(uint8_t port, uint16_t vlan)
{
	struct router *r = NULL;

	for (int i = 0; i < MC_ROUTERS; i++) {
		if (routers[i].vlan == vlan && routers[i].port == port) {
			routers[i].left = OTHER_QUERIER_S;
			return;
		}
		if (!routers[i].vlan && !r)
			r = &routers[i];
	}
	if (!r)
		return;
	r->vlan = vlan;
	r->port = port;
	r->left = OTHER_QUERIER_S;
	vlan_groups_update(vlan);
}


static void query_in(uint8_t port, uint16_t vlan, bool v6, const uint8_t *src)
{
	static const uint8_t zero[16];
	uint8_t own[16], n = v6 ? 16 : 4;
	struct vlan *v = vlan_of(vlan);

	/* A source of 0.0.0.0 is a proxy's, no querier */
	if (!memcmp(src, zero, n))
		return;
	router_seen(port, vlan);
	if (!v)
		return;
	if (v6)
		link_local6(own);
	else
		memcpy(own, mc_hw_ip(), 4);
	if (!memcmp(src, own, n))
		return;
	/* the lowest address queries */
	if (memcmp(src, own, n) < 0 || !memcmp(own, zero, n)) {
		if (!v->other_left[v6] || memcmp(src, v->other[v6], n) <= 0) {
			memcpy(v->other[v6], src, n);
			v->other_left[v6] = OTHER_QUERIER_S;
		}
	}
}


/* IGMPv3 and MLDv2 group records from r to end */
static void records(uint8_t port, uint16_t vlan, bool v6, const uint8_t *r, const uint8_t *end, uint16_t n)
{
	uint8_t alen = v6 ? 16 : 4;

	while (n-- && end - r >= 4 + alen) {
		uint16_t srcs = get16(r + 2);
		uint32_t size = 4 + alen + (uint32_t)srcs * alen + r[1] * 4u;

		if (size > (uint32_t)(end - r))
			return;
		switch (r[0]) {
		case 1:		/* MODE_IS_INCLUDE */
		case 3:		/* CHANGE_TO_INCLUDE */
			if (srcs)
				join(port, vlan, v6, r + 4);
			else
				leave(port, vlan, v6, r + 4);
			break;
		case 2:		/* MODE_IS_EXCLUDE */
		case 4:		/* CHANGE_TO_EXCLUDE */
			join(port, vlan, v6, r + 4);
			break;
		case 5:		/* ALLOW_NEW_SOURCES */
			if (srcs)
				join(port, vlan, v6, r + 4);
			break;
		}
		r += size;
	}
}


/* true: relayed to the router ports; false: to the whole VLAN */
static void igmp_in(uint8_t port, uint16_t vlan, const uint8_t *f, uint16_t len, const uint8_t *src,
		    const uint8_t *m, uint16_t n)
{
	if (n < 8 || fold(sum16(m, n, 0)))
		return;
	switch (m[0]) {
	case 0x11:
		query_in(port, vlan, false, src);
		relay(~mc_hw_lag(port), vlan, f, len);
		return;
	case 0x12:
	case 0x16:
		join(port, vlan, false, m + 4);
		break;
	case 0x17:
		leave(port, vlan, false, m + 4);
		break;
	case 0x22:
		records(port, vlan, false, m + 8, m + n, get16(m + 6));
		break;
	}
	relay(router_ports(vlan) & ~mc_hw_lag(port), vlan, f, len);
}


static void mld_in(uint8_t port, uint16_t vlan, const uint8_t *f, uint16_t len, const uint8_t *ip,
		   const uint8_t *m, uint16_t n)
{
	if (n < 8 || fold(sum16(m, n, pseudo6(ip + 8, n))))
		return;
	switch (m[0]) {
	case 130:
		if (n < 24 || ip[8] != 0xfe || (ip[9] & 0xc0) != 0x80)
			return;
		query_in(port, vlan, true, ip + 8);
		relay(~mc_hw_lag(port), vlan, f, len);
		return;
	case 131:
		if (n >= 24)
			join(port, vlan, true, m + 8);
		break;
	case 132:
		if (n >= 24)
			leave(port, vlan, true, m + 8);
		break;
	case 143:
		records(port, vlan, true, m + 8, m + n, get16(m + 6));
		break;
	}
	relay(router_ports(vlan) & ~mc_hw_lag(port), vlan, f, len);
}


bool mc_rx(uint8_t port, uint16_t vlan, const uint8_t *f, uint16_t len)
{
	const uint8_t *ip = f + 14;

	if (!mc_cfg.on || port >= MC_PORTS || !vlan || vlan > 4094 || len < 14 + 20 || !(f[0] & 1))
		return false;
	if (f[12] == 0x08 && f[13] == 0x00) {
		uint16_t hl = (ip[0] & 0x0f) * 4, tl = get16(ip + 2);

		if (ip[0] >> 4 != 4 || ip[9] != 2)
			return false;
		/* a fragment is none of the protocol's messages */
		if (hl < 20 || tl < hl || tl > len - 14 || (get16(ip + 6) & 0x3fff))
			return true;
		igmp_in(port, vlan, f, 14 + tl, ip + 12, ip + hl, tl - hl);
		return true;
	}
	if (f[12] == 0x86 && f[13] == 0xdd && len >= 14 + 48) {
		uint16_t pl = get16(ip + 4), hl;
		const uint8_t *h = ip + 40;

		/* MLD always comes behind a hop-by-hop header */
		if (ip[0] >> 4 != 6 || ip[6] != 0 || h[0] != 58)
			return false;
		hl = (h[1] + 1) * 8;
		if (pl > len - 54 || hl + 4 > pl)
			return false;
		if (!(h[hl] >= 130 && h[hl] <= 132) && h[hl] != 143)
			return false;
		if (ip[7] != 1)
			return true;
		mld_in(port, vlan, f, 54 + pl, ip, h + hl, pl - hl);
		return true;
	}
	return false;
}


/* ---- time and configuration ---- */

static void drop_ports(struct group *g, uint16_t ports)
{
	uint16_t was = group_ports(g);

	for (int i = 0; i < MC_PORTS; i++)
		if ((ports >> i) & 1)
			g->left[i] = 0;
	if (group_ports(g) == was)
		return;
	write_group(g);
	if (!group_ports(g))
		g->vlan = 0;
}


void mc_port_down(uint8_t port)
{
	bool changed = false;

	if (!mc_cfg.on || port >= MC_PORTS)
		return;
	/* an aggregation lives on */
	if (mc_hw_lag(port) & ~(1 << port) & mc_hw_up())
		return;
	for (int i = 0; i < MC_GROUPS; i++)
		if (groups[i].vlan)
			drop_ports(&groups[i], 1 << port);
	for (int i = 0; i < MC_ROUTERS; i++)
		if (routers[i].vlan && routers[i].port == port) {
			uint16_t vlan = routers[i].vlan;
			routers[i].vlan = 0;
			vlan_groups_update(vlan);
			changed = true;
		}
	if (changed)
		unknown_update();
}


void mc_second(void)
{
	if (!mc_cfg.on)
		return;
	for (int i = 0; i < MC_GROUPS; i++) {
		struct group *g = &groups[i];
		uint16_t was;

		if (!g->vlan)
			continue;
		was = group_ports(g);
		for (int p = 0; p < MC_PORTS; p++)
			if (g->left[p])
				g->left[p]--;
		if (group_ports(g) == was)
			continue;
		write_group(g);
		if (!group_ports(g))
			g->vlan = 0;
	}
	for (int i = 0; i < MC_ROUTERS; i++)
		if (routers[i].vlan && !--routers[i].left) {
			uint16_t vlan = routers[i].vlan;
			routers[i].vlan = 0;
			vlan_groups_update(vlan);
		}

	uint16_t own[MC_PORTS];
	unsigned n = mc_hw_vlans(own, MC_PORTS);
	for (unsigned i = 0; i < n; i++)
		vlan_of(own[i]);
	for (int i = 0; i < MC_VLANS; i++) {
		struct vlan *v = &vlans[i];

		if (!v->id)
			continue;
		for (int k = 0; k < 2; k++)
			if (v->other_left[k] && !--v->other_left[k])
				v->query_in = 1;	/* the querier is gone: take over */
		if (!mc_cfg.querier || (v->query_in && --v->query_in))
			continue;
		if (!v->other_left[0])
			query(v->id, false, NULL);
		if (!v->other_left[1])
			query(v->id, true, NULL);
		v->query_in = v->startup ? STARTUP_S : mc_cfg.interval;
		if (v->startup)
			v->startup--;
	}
}


static void forget(void)
{
	for (int i = 0; i < MC_GROUPS; i++)
		if (groups[i].vlan)
			drop_ports(&groups[i], 0xffff);
	for (int i = 0; i < MC_VLANS; i++)
		if (vlans[i].id)
			local_entries(vlans[i].id, false);
	memset(groups, 0, sizeof(groups));
	memset(routers, 0, sizeof(routers));
	memset(vlans, 0, sizeof(vlans));
	unknown_set = false;
	full_said = false;
}


static bool was_on;

void mc_reset(void)
{
	/* the switch came up with an empty table */
	memset(groups, 0, sizeof(groups));
	memset(routers, 0, sizeof(routers));
	memset(vlans, 0, sizeof(vlans));
	memset(&mc_cfg, 0, sizeof(mc_cfg));
	mc_cfg.interval = 125;
	unknown_set = full_said = was_on = false;
}


void mc_apply(void)
{
	if (!mc_cfg.on) {
		if (was_on) {
			forget();
			mc_hw_mode(false, false, 0);
		}
		was_on = false;
		return;
	}
	was_on = true;
	unknown_set = false;
	unknown_update();
	for (int i = 0; i < MC_VLANS; i++)
		if (vlans[i].id) {
			local_entries(vlans[i].id, mc_cfg.restrict_unknown);
			vlans[i].query_in = 1;
			vlan_groups_update(vlans[i].id);
		}
}


unsigned mc_group_count(void)
{
	unsigned n = 0;

	for (int i = 0; i < MC_GROUPS; i++)
		n += groups[i].vlan != 0;
	return n;
}


static size_t ports_json(char *buf, size_t max, size_t n, uint16_t m)
{
	bool comma = false;

	n = app(buf, max, n, "[");
	for (int i = 0; i < MC_PORTS; i++)
		if ((m >> i) & 1) {
			n = app(buf, max, n, "%s%u", comma ? "," : "", mc_hw_front(i));
			comma = true;
		}
	return app(buf, max, n, "]");
}


size_t mc_json(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "{\"on\":%d,\"querier\":%d,\"interval\":%u,\"restrict\":%d,\"routers\":",
		       mc_cfg.on, mc_cfg.querier, mc_cfg.interval, mc_cfg.restrict_unknown);
	bool comma = false;

	n = ports_json(buf, max, n, mc_cfg.routers);
	n = app(buf, max, n, ",\"vlans\":[");
	for (int i = 0; i < MC_VLANS; i++) {
		if (!vlans[i].id)
			continue;
		n = app(buf, max, n, "%s{\"vlan\":%u,\"querier4\":%d,\"querier6\":%d,\"routers\":", comma ? "," : "", vlans[i].id,
			mc_cfg.querier && !vlans[i].other_left[0], mc_cfg.querier && !vlans[i].other_left[1]);
		n = ports_json(buf, max, n, router_ports(vlans[i].id));
		n = app(buf, max, n, "}");
		comma = true;
	}
	n = app(buf, max, n, "],\"total\":%u,\"groups\":[", mc_group_count());
	comma = false;
	for (int i = 0; i < MC_GROUPS && n + 160 < max; i++) {
		const struct group *g = &groups[i];

		if (!g->vlan)
			continue;
		n = app(buf, max, n, "%s{\"vlan\":%u,\"group\":\"", comma ? "," : "", g->vlan);
		if (g->v6)
			for (int k = 0; k < 16; k += 2)
				n = app(buf, max, n, "%s%x", k ? ":" : "", get16(g->addr + k));
		else
			n = app(buf, max, n, "%u.%u.%u.%u", g->addr[0], g->addr[1], g->addr[2], g->addr[3]);
		n = app(buf, max, n, "\",\"ports\":");
		n = ports_json(buf, max, n, group_ports(g));
		n = app(buf, max, n, "}");
		comma = true;
	}
	return app(buf, max, n, "]}");
}
