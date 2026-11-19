/*
 * ACL rules: match frames as they enter front ports and drop, permit, trap,
 * redirect or copy them. Compiled with sdcc_shim.h, like the modules.
 *
 *   acl                                       the rules, as the lines that make them
 *   acl <1-96> ports <p,p,...>|all            the front ports the rule watches (all by default)
 *   acl <1-96> dmac|smac <mac> [<mask>]|any
 *   acl <1-96> ethertype <hex>|any
 *   acl <1-96> src|dst <a.b.c.d>[/<0-32>]|any IPv4 addresses
 *   acl <1-96> proto <0-255>|tcp|udp|icmp|any IPv4 protocol, IPv6 next header
 *   acl <1-96> sport|dport <0-65535>|any      TCP or UDP port
 *   acl <1-96> icmp-type <0-255>|any
 *   acl <1-96> drop|permit|trap
 *   acl <1-96> redirect|copy <p,p,...|cpu>    only to those ports, or to them as well
 *   acl <1-96> delete
 *
 * A rule takes effect once it has an action. The lowest numbered matching
 * rule decides. A rule matches either addresses at layer 2 (dmac, smac,
 * ethertype) or at layers 3 and 4 (the rest), not both: each comes from one
 * of the chip's field templates. Frames the switch sends itself aren't
 * matched, so a rule can't cut off the management CPU's own replies, only
 * what reaches it.
 *
 * The layout, worked out from the stock firmware and checked on a switch, is
 * in docs/acl-reverse-engineering.md:
 * - rule i is table 1 entry i (data, X) and 0x80 + i (care, Y), TCAM-coded:
 *   a bit that must be d has X = !d, Y = d; a bit that doesn't matter has
 *   X = Y = 1. Words 0-3 are eight 16-bit slots, filled per the rule's
 *   template; word 4 holds the template (bits 2:0), the ingress port, one
 *   bit per port (bits 20:11), and the valid bit (21, in Y).
 * - its action is table 2 entry i; this file only uses action group 5,
 *   forwarding: word 1 bits 20:17 a code, bits 30:21 a port mask.
 * - 0x4848 + 4i: the action groups rule i uses.
 * - 0x4818: ACL on, per port; 0x481c: frames no rule matches pass, per port.
 * - 0x4820-0x4844: the templates, two registers each (slots 0-3 in the
 *   second, slot 0 in its low byte, then 4-7), the stock firmware's.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "cmd_parser.h"

#include "rtl_acl.h"

extern __code const struct machine machine;
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[];
uint8_t cmd_compare(uint8_t start, __code const uint8_t *cmd);

#define ACL_ENABLE		0x4818
#define ACL_UNMATCH_PERMIT	0x481c
#define ACL_TEMPLATE		0x4820
#define ACL_GROUPS		0x4848
#define ACL_GROUP_FWD		0x20

/* Forwarding codes, action word 1 bits 20:17 */
#define FWD_COPY		0	/* as usual, and to the mask's ports */
#define FWD_REDIRECT		1	/* only to the mask's ports: none is a drop */
#define FWD_TRAP		3	/* to the CPU only */

static const uint32_t templates[10] = {
	0x4f060504, 0x03020100,		/* 0: DMAC, SMAC, EtherType */
	0x40363534, 0x13121110,		/* 1: IPv4 SIP, DIP, ToS/protocol, L4 ports */
	0x48474645, 0x44431312,
	0x4e4d4c4b, 0x4a491110,
	0x33424107, 0x08323130,
};

enum { F_DMAC, F_SMAC, F_ETYPE, F_SRC, F_DST, F_PROTO, F_SPORT, F_DPORT, F_ICMP };
#define L2_FIELDS	(1 << F_DMAC | 1 << F_SMAC | 1 << F_ETYPE)

enum { A_NONE, A_DROP, A_PERMIT, A_TRAP, A_REDIRECT, A_COPY };
static const char *const action_names[] = { "", "drop", "permit", "trap", "redirect", "copy" };

struct acl_rule {
	uint16_t fields;		/* F_* present */
	uint8_t action;
	uint16_t ports;			/* logical ports watched, 0: every front port */
	uint16_t to;			/* redirect, copy: logical ports */
	uint8_t mac[2][6], mac_mask[2][6];
	uint16_t etype;
	uint32_t ip[2];
	uint8_t ip_len[2];
	uint8_t proto, icmp;
	uint16_t l4[2];
};

static struct acl_rule rules[ACL_ENTRIES];
static bool hw_ready;			/* templates written, table cleared */


static uint16_t front_ports(void)
{
	uint16_t m = 0;

	for (uint8_t i = machine.min_port; i <= machine.max_port; i++)
		m |= 1 << i;
	return m;
}


static void reg_put(uint16_t reg, uint32_t v)
{
	REG_SET(reg, v);
}


/* Bits shift.. of word w must be v where m is set */
static void put(uint32_t *x, uint32_t *y, uint8_t w, uint8_t shift, uint8_t width, uint32_t v, uint32_t m)
{
	for (uint8_t b = 0; b < width; b++) {
		uint32_t bit = (uint32_t)1 << (shift + b);
		if (!(m >> b & 1))
			continue;
		if (v >> b & 1)
			x[w] &= ~bit;
		else
			y[w] &= ~bit;
	}
}


static void slot(uint32_t *x, uint32_t *y, uint8_t s, uint16_t v, uint16_t m)
{
	put(x, y, s >> 1, (s & 1) << 4, 16, v, m);
}


static void encode(const struct acl_rule *r, uint32_t *x, uint32_t *y, uint32_t *act)
{
	for (uint8_t i = 0; i < ACL_WORDS; i++)
		x[i] = y[i] = 0xffffffff;
	if (r->fields & L2_FIELDS) {
		put(x, y, 4, 0, 3, 0, 7);
		for (uint8_t k = 0; k < 2; k++) {
			const uint8_t *a = r->mac[k], *m = r->mac_mask[k];
			if (!(r->fields >> (F_DMAC + k) & 1))
				continue;
			/* slots 0-2 the destination, 3-5 the source; low bits first */
			for (uint8_t s = 0; s < 3; s++)
				slot(x, y, 3 * k + s, a[4 - 2 * s] << 8 | a[5 - 2 * s], m[4 - 2 * s] << 8 | m[5 - 2 * s]);
		}
		if (r->fields >> F_ETYPE & 1)
			slot(x, y, 6, r->etype, 0xffff);
	} else {
		put(x, y, 4, 0, 3, 1, 7);
		for (uint8_t k = 0; k < 2; k++) {
			uint32_t m = r->ip_len[k] ? 0xffffffff << (32 - r->ip_len[k]) : 0;
			if (!(r->fields >> (F_SRC + k) & 1))
				continue;
			/* slots 0-1 the source, 2-3 the destination; low half first */
			slot(x, y, 2 * k, r->ip[k], m);
			slot(x, y, 2 * k + 1, r->ip[k] >> 16, m >> 16);
		}
		if (r->fields >> F_PROTO & 1)
			slot(x, y, 4, r->proto, 0x00ff);
		if (r->fields >> F_SPORT & 1)
			slot(x, y, 5, r->l4[0], 0xffff);
		if (r->fields >> F_ICMP & 1)
			slot(x, y, 5, r->icmp << 8, 0xff00);
		if (r->fields >> F_DPORT & 1)
			slot(x, y, 6, r->l4[1], 0xffff);
	}
	/* The ingress port sets its own bit: every other port's must be clear */
	put(x, y, 4, 11, 10, 0, ~(r->ports ? r->ports : front_ports()) & 0x3ff);
	x[4] &= 0x003fffff;
	y[4] &= 0x003fffff;

	for (uint8_t i = 0; i < ACL_WORDS; i++)
		act[i] = 0;
	switch (r->action) {
	case A_DROP:
		act[1] = (uint32_t)FWD_REDIRECT << 17;
		break;
	case A_PERMIT:
		act[1] = (uint32_t)FWD_COPY << 17;
		break;
	case A_TRAP:
		act[1] = (uint32_t)FWD_TRAP << 17;
		break;
	case A_REDIRECT:
		act[1] = (uint32_t)FWD_REDIRECT << 17 | (uint32_t)(r->to & 0x3ff) << 21;
		break;
	case A_COPY:
		act[1] = (uint32_t)FWD_COPY << 17 | (uint32_t)(r->to & 0x3ff) << 21;
		break;
	}
}


static bool hw_init(void)
{
	uint32_t zero[ACL_WORDS] = { 0 };

	if (hw_ready)
		return true;
	for (uint8_t i = 0; i < 10; i++)
		reg_put(ACL_TEMPLATE + 4 * i, templates[i]);
	/* The table holds whatever it powered up with: no valid bit, as seen
	 * so far, but make sure */
	for (uint8_t i = 0; i < ACL_ENTRIES; i++)
		if (!acl_op(0x80 | i, TBL_ACL_RULE, true, zero))
			return false;
	hw_ready = true;
	return true;
}


/* Rule i into the table as it now stands; false if the engine doesn't answer */
static bool hw_write(uint8_t i)
{
	uint32_t x[ACL_WORDS], y[ACL_WORDS], act[ACL_WORDS], zero[ACL_WORDS] = { 0 };
	uint16_t on = 0;

	if (!hw_init())
		return false;
	/* Invalid while it changes */
	if (!acl_op(0x80 | i, TBL_ACL_RULE, true, zero))
		return false;
	if (rules[i].action != A_NONE) {
		encode(&rules[i], x, y, act);
		if (!acl_op(i, TBL_ACL_ACTION, true, act))
			return false;
		reg_put(ACL_GROUPS + 4 * i, ACL_GROUP_FWD);
		if (!acl_op(i, TBL_ACL_RULE, true, x) || !acl_op(0x80 | i, TBL_ACL_RULE, true, y))
			return false;
	}
	for (uint8_t k = 0; k < ACL_ENTRIES; k++)
		if (rules[k].action != A_NONE)
			on = front_ports();
	reg_put(ACL_UNMATCH_PERMIT, on);
	reg_put(ACL_ENABLE, on);
	return true;
}


void rtl_acl_reset(void)
{
	memset(rules, 0, sizeof(rules));
	hw_ready = false;
}


/* ---- parsing ---- */

/* Word w of the command line, up to its space */
static const char *word(uint8_t w, char *b, uint8_t n)
{
	const char *p = (const char *)&cmd_buffer[cmd_words_b[w]];
	uint8_t k = 0;

	while (*p && *p != ' ' && k < n - 1)
		b[k++] = *p++;
	b[k] = 0;
	return b;
}


static bool dec(const char *s, uint32_t max, uint32_t *v)
{
	*v = 0;
	if (!*s)
		return false;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return false;
		*v = *v * 10 + (*s - '0');
		if (*v > max)
			return false;
	}
	return true;
}


static bool hex(const char *s, uint32_t max, uint32_t *v)
{
	*v = 0;
	if (s[0] == '0' && (s[1] | 0x20) == 'x')
		s += 2;
	if (!*s)
		return false;
	for (; *s; s++) {
		uint8_t c = *s | 0x20;
		if (*s >= '0' && *s <= '9')
			*v = *v << 4 | (*s - '0');
		else if (c >= 'a' && c <= 'f')
			*v = *v << 4 | (c - 'a' + 10);
		else
			return false;
		if (*v > max)
			return false;
	}
	return true;
}


static bool mac(const char *s, uint8_t *a)
{
	for (uint8_t i = 0; i < 6; i++) {
		uint32_t v;
		char b[3] = { s[0], s[1], 0 };
		if (!s[0] || !s[1] || !hex(b, 0xff, &v))
			return false;
		a[i] = v;
		s += 2;
		if (i < 5 && *s != ':' && *s != '-')
			return false;
		s += i < 5;
	}
	return !*s;
}


/* a.b.c.d[/len] */
static bool ipv4(const char *s, uint32_t *ip, uint8_t *len)
{
	uint32_t v = 0, part, l = 32;
	char b[4];

	for (uint8_t i = 0; i < 4; i++) {
		uint8_t k = 0;
		while (*s >= '0' && *s <= '9' && k < 3)
			b[k++] = *s++;
		b[k] = 0;
		if (!dec(b, 255, &part) || (i < 3 && *s++ != '.'))
			return false;
		v = v << 8 | part;
	}
	if (*s == '/' && !dec(s + 1, 32, &l))
		return false;
	if (*s && *s != '/')
		return false;
	*ip = v;
	*len = l;
	return true;
}


/* p,p,... as front port digits; cpu for the CPU port where allowed */
static bool port_list(const char *s, bool cpu, uint16_t *m)
{
	*m = 0;
	while (*s) {
		if (cpu && !strncmp(s, "cpu", 3)) {
			*m |= 1 << CPU_PORT;
			s += 3;
		} else {
			uint8_t p = *s - '1';
			if (p > 8)
				return false;
			p = machine.phys_to_log_port[p];
			if (p < machine.min_port || p > machine.max_port)
				return false;
			*m |= 1 << p;
			s++;
		}
		if (*s == ',')
			s++;
		else if (*s)
			return false;
	}
	return *m != 0;
}


static void print_ports(char *b, size_t n, uint16_t m)
{
	size_t k = strlen(b);
	bool first = true;

	for (uint8_t i = 0; i <= CPU_PORT && k + 5 < n; i++) {
		if (!(m >> i & 1))
			continue;
		if (i == CPU_PORT)
			k += snprintf(b + k, n - k, "%scpu", first ? "" : ",");
		else
			k += snprintf(b + k, n - k, "%s%u", first ? "" : ",", machine.log_to_phys_port[i]);
		first = false;
	}
}


static void line(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void line(char *b, size_t n, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, n, fmt, ap);
	va_end(ap);
	print_string_x(b);
}


/* A rule as the lines that make it */
static void print_rule(uint8_t i)
{
	const struct acl_rule *r = &rules[i];
	char b[96];
	unsigned n = i + 1;

	if (r->ports) {
		snprintf(b, sizeof(b), "acl %u ports ", n);
		print_ports(b, sizeof(b), r->ports);
		strcat(b, "\n");
		print_string_x(b);
	}
	for (uint8_t k = 0; k < 2; k++) {
		const uint8_t *a = r->mac[k], *m = r->mac_mask[k];
		if (!(r->fields >> (F_DMAC + k) & 1))
			continue;
		snprintf(b, sizeof(b), "acl %u %s %02x:%02x:%02x:%02x:%02x:%02x", n, k ? "smac" : "dmac",
			 a[0], a[1], a[2], a[3], a[4], a[5]);
		if (memcmp(m, "\xff\xff\xff\xff\xff\xff", 6))
			snprintf(b + strlen(b), sizeof(b) - strlen(b), " %02x:%02x:%02x:%02x:%02x:%02x",
				 m[0], m[1], m[2], m[3], m[4], m[5]);
		strcat(b, "\n");
		print_string_x(b);
	}
	if (r->fields >> F_ETYPE & 1)
		line(b, sizeof(b), "acl %u ethertype %04x\n", n, r->etype);
	for (uint8_t k = 0; k < 2; k++)
		if (r->fields >> (F_SRC + k) & 1)
			line(b, sizeof(b), "acl %u %s %lu.%lu.%lu.%lu/%u\n", n, k ? "dst" : "src",
			     (unsigned long)(r->ip[k] >> 24), (unsigned long)(r->ip[k] >> 16 & 0xff),
			     (unsigned long)(r->ip[k] >> 8 & 0xff), (unsigned long)(r->ip[k] & 0xff), r->ip_len[k]);
	if (r->fields >> F_PROTO & 1)
		line(b, sizeof(b), "acl %u proto %u\n", n, r->proto);
	if (r->fields >> F_SPORT & 1)
		line(b, sizeof(b), "acl %u sport %u\n", n, r->l4[0]);
	if (r->fields >> F_DPORT & 1)
		line(b, sizeof(b), "acl %u dport %u\n", n, r->l4[1]);
	if (r->fields >> F_ICMP & 1)
		line(b, sizeof(b), "acl %u icmp-type %u\n", n, r->icmp);
	if (r->action == A_REDIRECT || r->action == A_COPY) {
		snprintf(b, sizeof(b), "acl %u %s ", n, action_names[r->action]);
		print_ports(b, sizeof(b), r->to);
		strcat(b, "\n");
		print_string_x(b);
	} else if (r->action != A_NONE) {
		line(b, sizeof(b), "acl %u %s\n", n, action_names[r->action]);
	}
}


static void acl_error(__code const char *msg)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: ");
	print_string(msg);
}


/* Sets field f, or clears it for "any"; false if the rule can't hold it */
static bool field(struct acl_rule *r, uint8_t f, bool any)
{
	uint16_t after = any ? r->fields & ~(1 << f) : r->fields | 1 << f;

	if (!any && (after & L2_FIELDS) && (after & ~L2_FIELDS)) {
		acl_error("a rule matches dmac, smac and ethertype, or IP fields, not both\n");
		return false;
	}
	if (!any && (after >> F_SPORT & 1) && (after >> F_ICMP & 1)) {
		acl_error("a rule matches sport or icmp-type, not both\n");
		return false;
	}
	r->fields = after;
	return true;
}


bool rtl_acl_cmd(void)
{
	char w[24], v[24];
	uint32_t n, x;
	struct acl_rule *r, keep;
	bool any;

	if (cmd_words_len == 1) {
		for (uint8_t i = 0; i < ACL_ENTRIES; i++)
			if (rules[i].action != A_NONE || rules[i].fields || rules[i].ports)
				print_rule(i);
		return true;
	}
	if (cmd_words_len < 3 || !dec(word(1, w, sizeof(w)), ACL_ENTRIES, &n) || !n) {
		acl_error("acl <1-96> ports|dmac|smac|ethertype|src|dst|proto|sport|dport|icmp-type <value>|any, "
			  "acl <1-96> drop|permit|trap|redirect <ports>|copy <ports>|delete\n");
		return true;
	}
	r = &rules[n - 1];
	keep = *r;
	word(2, w, sizeof(w));
	if (cmd_words_len > 3)
		word(3, v, sizeof(v));
	else
		v[0] = 0;
	any = !strcmp(v, "any");

	if (cmd_words_len == 3 && !strcmp(w, "delete")) {
		memset(r, 0, sizeof(*r));
	} else if (cmd_words_len == 3 && !strcmp(w, "drop")) {
		r->action = A_DROP;
	} else if (cmd_words_len == 3 && !strcmp(w, "permit")) {
		r->action = A_PERMIT;
	} else if (cmd_words_len == 3 && !strcmp(w, "trap")) {
		r->action = A_TRAP;
	} else if (cmd_words_len == 4 && (!strcmp(w, "redirect") || !strcmp(w, "copy"))) {
		if (!port_list(v, true, &r->to)) {
			acl_error("acl <n> redirect|copy <port>[,<port>...], cpu for the CPU\n");
			return true;
		}
		r->action = w[0] == 'r' ? A_REDIRECT : A_COPY;
	} else if (cmd_words_len == 4 && !strcmp(w, "ports")) {
		uint16_t m = 0;
		if (strcmp(v, "all") && !port_list(v, false, &m)) {
			acl_error("acl <n> ports <port>[,<port>...]|all\n");
			return true;
		}
		r->ports = m;
	} else if ((cmd_words_len == 4 || cmd_words_len == 5) && (!strcmp(w, "dmac") || !strcmp(w, "smac"))) {
		uint8_t k = w[0] == 's', a[6], m[6];
		memset(m, 0xff, 6);
		if (!any && (!mac(v, a) || (cmd_words_len == 5 && !mac(word(4, v, sizeof(v)), m)))) {
			acl_error("acl <n> dmac|smac <aa:bb:cc:dd:ee:ff> [<mask>]|any\n");
			return true;
		}
		if (!field(r, F_DMAC + k, any))
			return true;
		if (!any) {
			memcpy(r->mac[k], a, 6);
			memcpy(r->mac_mask[k], m, 6);
		}
	} else if (cmd_words_len == 4 && !strcmp(w, "ethertype")) {
		if (!any && !hex(v, 0xffff, &x)) {
			acl_error("acl <n> ethertype <hex>|any\n");
			return true;
		}
		if (!field(r, F_ETYPE, any))
			return true;
		r->etype = x;
	} else if (cmd_words_len == 4 && (!strcmp(w, "src") || !strcmp(w, "dst"))) {
		uint8_t k = w[0] == 'd', len;
		uint32_t ip;
		if (!any && !ipv4(v, &ip, &len)) {
			acl_error("acl <n> src|dst <a.b.c.d>[/<0-32>]|any\n");
			return true;
		}
		if (!field(r, F_SRC + k, any))
			return true;
		r->ip[k] = ip;
		r->ip_len[k] = len;
	} else if (cmd_words_len == 4 && !strcmp(w, "proto")) {
		if (!strcmp(v, "tcp"))
			x = 6;
		else if (!strcmp(v, "udp"))
			x = 17;
		else if (!strcmp(v, "icmp"))
			x = 1;
		else if (!any && !dec(v, 255, &x)) {
			acl_error("acl <n> proto <0-255>|tcp|udp|icmp|any\n");
			return true;
		}
		if (!field(r, F_PROTO, any))
			return true;
		r->proto = x;
	} else if (cmd_words_len == 4 && (!strcmp(w, "sport") || !strcmp(w, "dport"))) {
		uint8_t k = w[0] == 'd';
		if (!any && !dec(v, 65535, &x)) {
			acl_error("acl <n> sport|dport <0-65535>|any\n");
			return true;
		}
		if (!field(r, F_SPORT + k, any))
			return true;
		r->l4[k] = x;
	} else if (cmd_words_len == 4 && !strcmp(w, "icmp-type")) {
		if (!any && !dec(v, 255, &x)) {
			acl_error("acl <n> icmp-type <0-255>|any\n");
			return true;
		}
		if (!field(r, F_ICMP, any))
			return true;
		r->icmp = x;
	} else {
		acl_error("acl <1-96> ports|dmac|smac|ethertype|src|dst|proto|sport|dport|icmp-type <value>|any, "
			  "acl <1-96> drop|permit|trap|redirect <ports>|copy <ports>|delete\n");
		return true;
	}
	/* Untouched rules stay out of the table; the rest go in as they stand */
	if (!memcmp(&keep, r, sizeof(keep)))
		return true;
	if (!hw_write(n - 1))
		acl_error("the table engine doesn't answer\n");
	return true;
}
