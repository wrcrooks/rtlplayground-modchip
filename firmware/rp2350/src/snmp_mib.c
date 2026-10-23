/*
 * The agent's MIB: lwIP's MIB-2 groups (system, at, ip, icmp, tcp, udp,
 * snmp) with an interfaces group and IF-MIB ifXTable that list the
 * switch's ports, with the ASIC's counters, rather than lwIP's one netif.
 * No Pico SDK here, so the host test runs it (test/test_snmp.c).
 */

#include <stdio.h>
#include <string.h>

#include "lwip/apps/snmp.h"
#include "lwip/apps/snmp_core.h"
#include "lwip/apps/snmp_mib2.h"
#include "lwip/apps/snmp_scalar.h"
#include "lwip/apps/snmp_table.h"
#include "lwip/sys.h"

#include "bridge.h"
#include "plat.h"
#include "snmp_modchip.h"

#define MAX_PORTS		10
#define PORTS_MAX_AGE_MS	1000

static struct rtl_port ports[MAX_PORTS];
static uint8_t n_ports;
static uint32_t ports_at;
static bool ports_valid;

static void ports_refresh(void)
{
	uint32_t now = sys_now();

	if (ports_valid && now - ports_at < PORTS_MAX_AGE_MS)
		return;
	n_ports = bridge_is_up() ? rtl_ports_read(ports, MAX_PORTS) : 0;
	ports_at = now;
	ports_valid = true;
}


static uint32_t speed_mbps(uint8_t link)
{
	static const uint16_t mbps[] = { 0, 10, 100, 1000, 500, 10000, 2500, 5000 };

	return link < sizeof(mbps) / sizeof(mbps[0]) ? mbps[link] : 0;
}


static const struct snmp_oid_range if_oid_ranges[] = { { 1, MAX_PORTS } };

static snmp_err_t if_get_cell_instance(const u32_t *column, const u32_t *row_oid, u8_t row_oid_len,
				       struct snmp_node_instance *cell)
{
	(void)column;
	if (!snmp_oid_in_range(row_oid, row_oid_len, if_oid_ranges, LWIP_ARRAYSIZE(if_oid_ranges)))
		return SNMP_ERR_NOSUCHINSTANCE;
	ports_refresh();
	if (row_oid[0] > n_ports)
		return SNMP_ERR_NOSUCHINSTANCE;
	cell->reference.u32 = row_oid[0];
	return SNMP_ERR_NOERROR;
}


static snmp_err_t if_get_next_cell_instance(const u32_t *column, struct snmp_obj_id *row_oid,
					    struct snmp_node_instance *cell)
{
	struct snmp_next_oid_state state;
	u32_t result[LWIP_ARRAYSIZE(if_oid_ranges)];

	(void)column;
	ports_refresh();
	snmp_next_oid_init(&state, row_oid->id, row_oid->len, result, LWIP_ARRAYSIZE(if_oid_ranges));
	for (u32_t i = 1; i <= n_ports; i++) {
		u32_t test[1] = { i };
		snmp_next_oid_check(&state, test, 1, (void *)(uintptr_t)i);
	}
	if (state.status != SNMP_NEXT_OID_STATUS_SUCCESS)
		return SNMP_ERR_NOSUCHINSTANCE;
	snmp_oid_assign(row_oid, state.next_oid, state.next_oid_len);
	cell->reference.u32 = (u32_t)(uintptr_t)state.reference;
	return SNMP_ERR_NOERROR;
}


static s16_t put_str(void *value, const char *s)
{
	size_t n = strlen(s);

	memcpy(value, s, n);
	return n;
}


static s16_t put_u32(void *value, u32_t v)
{
	*(u32_t *)value = v;
	return sizeof(u32_t);
}


static s16_t put_u64(void *value, u64_t v)
{
	*(u64_t *)value = v;
	return sizeof(u64_t);
}


/* IF-MIB ifTable, RFC 2863 */
static s16_t if_get_value(struct snmp_node_instance *instance, void *value)
{
	u32_t idx = instance->reference.u32;
	const struct rtl_port *p = &ports[idx - 1];
	char descr_buf[16];

	switch (SNMP_TABLE_GET_COLUMN_FROM_OID(instance->instance_oid.id)) {
	case 1: return put_u32(value, idx);					/* ifIndex */
	case 2:									/* ifDescr */
		snprintf(descr_buf, sizeof(descr_buf), "Port %lu", (unsigned long)idx);
		return put_str(value, descr_buf);
	case 3: return put_u32(value, 6);					/* ifType: ethernetCsmacd */
	case 4: return put_u32(value, 1500);					/* ifMtu */
	case 5: {								/* ifSpeed */
		u32_t m = speed_mbps(p->link);
		return put_u32(value, m >= 4295 ? 0xffffffffu : m * 1000000u);
	}
	case 6: memcpy(value, rtl_mac(), 6); return 6;				/* ifPhysAddress */
	case 7: return put_u32(value, p->enabled ? 1 : 2);			/* ifAdminStatus */
	case 8: return put_u32(value, p->link ? 1 : 2);			/* ifOperStatus */
	case 9: return put_u32(value, 0);					/* ifLastChange */
	case 10: return put_u32(value, p->in_octets);
	case 11: return put_u32(value, p->in_ucast);
	case 12: return put_u32(value, p->in_mcast + p->in_bcast);		/* ifInNUcastPkts */
	case 13: return put_u32(value, p->in_discards);
	case 14: return put_u32(value, p->in_errors);
	case 15: return put_u32(value, 0);					/* ifInUnknownProtos */
	case 16: return put_u32(value, p->out_octets);
	case 17: return put_u32(value, p->out_ucast);
	case 18: return put_u32(value, p->out_mcast + p->out_bcast);		/* ifOutNUcastPkts */
	case 19: return put_u32(value, p->out_discards);
	case 20: return put_u32(value, p->out_errors);
	}
	return 0;
}


static const struct snmp_table_col_def if_cols[] = {
	{ 1, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 2, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 3, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 4, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 5, SNMP_ASN1_TYPE_GAUGE, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 6, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 7, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 8, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 9, SNMP_ASN1_TYPE_TIMETICKS, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 10, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 11, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 12, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 13, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 14, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 15, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 16, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 17, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 18, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 19, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 20, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
};

static const struct snmp_table_node if_table = SNMP_TABLE_CREATE(2, if_cols, if_get_cell_instance,
	if_get_next_cell_instance, if_get_value, NULL, NULL);

static s16_t if_number_get(struct snmp_node_instance *instance, void *value)
{
	(void)instance;
	ports_refresh();
	return put_u32(value, n_ports);
}

static const struct snmp_scalar_node if_number = SNMP_SCALAR_CREATE_NODE_READONLY(1, SNMP_ASN1_TYPE_INTEGER, if_number_get);

static const struct snmp_node *const interfaces_nodes[] = { &if_number.node.node, &if_table.node.node };
static const struct snmp_tree_node interfaces = SNMP_CREATE_TREE_NODE(2, interfaces_nodes);


/* IF-MIB ifXTable: names, the 64-bit counters, the speed in Mb/s */
static s16_t ifx_get_value(struct snmp_node_instance *instance, void *value)
{
	u32_t idx = instance->reference.u32;
	const struct rtl_port *p = &ports[idx - 1];
	char name[16];

	switch (SNMP_TABLE_GET_COLUMN_FROM_OID(instance->instance_oid.id)) {
	case 1:									/* ifName */
		snprintf(name, sizeof(name), "port%lu", (unsigned long)idx);
		return put_str(value, name);
	case 2: return put_u32(value, p->in_mcast);
	case 3: return put_u32(value, p->in_bcast);
	case 4: return put_u32(value, p->out_mcast);
	case 5: return put_u32(value, p->out_bcast);
	case 6: return put_u64(value, p->in_octets);
	case 7: return put_u64(value, p->in_ucast);
	case 8: return put_u64(value, p->in_mcast);
	case 9: return put_u64(value, p->in_bcast);
	case 10: return put_u64(value, p->out_octets);
	case 11: return put_u64(value, p->out_ucast);
	case 12: return put_u64(value, p->out_mcast);
	case 13: return put_u64(value, p->out_bcast);
	case 15: return put_u32(value, speed_mbps(p->link));		/* ifHighSpeed */
	case 17: return put_u32(value, 1);				/* ifConnectorPresent: true */
	case 18: return put_str(value, p->name);			/* ifAlias */
	}
	return 0;
}


static const struct snmp_table_col_def ifx_cols[] = {
	{ 1, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 2, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 3, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 4, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 5, SNMP_ASN1_TYPE_COUNTER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 6, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 7, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 8, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 9, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 10, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 11, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 12, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 13, SNMP_ASN1_TYPE_COUNTER64, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 15, SNMP_ASN1_TYPE_GAUGE, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 17, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
	{ 18, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
};

static const struct snmp_table_node ifx_table = SNMP_TABLE_CREATE(1, ifx_cols, if_get_cell_instance,
	if_get_next_cell_instance, ifx_get_value, NULL, NULL);
static const struct snmp_node *const ifmib_objects_nodes[] = { &ifx_table.node.node };
static const struct snmp_tree_node ifmib_objects = SNMP_CREATE_TREE_NODE(1, ifmib_objects_nodes);
static const struct snmp_node *const ifmib_nodes[] = { &ifmib_objects.node };
static const struct snmp_tree_node ifmib = SNMP_CREATE_TREE_NODE(31, ifmib_nodes);


/* MIB-2 from lwIP's groups, with the interfaces above (snmp_mib2.c does the same with its own) */
extern const struct snmp_scalar_array_node snmp_mib2_snmp_root;
extern const struct snmp_tree_node snmp_mib2_udp_root;
extern const struct snmp_tree_node snmp_mib2_tcp_root;
extern const struct snmp_scalar_array_node snmp_mib2_icmp_root;
extern const struct snmp_scalar_array_node snmp_mib2_system_node;
extern const struct snmp_tree_node snmp_mib2_at_root;
extern const struct snmp_tree_node snmp_mib2_ip_root;

static const struct snmp_node *const mib2_nodes[] = {
	&snmp_mib2_system_node.node.node,
	&interfaces.node,
	&snmp_mib2_at_root.node,
	&snmp_mib2_ip_root.node,
	&snmp_mib2_icmp_root.node.node,
	&snmp_mib2_tcp_root.node,
	&snmp_mib2_udp_root.node,
	&snmp_mib2_snmp_root.node.node,
	&ifmib.node,
};
static const struct snmp_tree_node mib2_root = SNMP_CREATE_TREE_NODE(1, mib2_nodes);
static const u32_t mib2_base[] = { 1, 3, 6, 1, 2, 1 };
static const struct snmp_mib modchip_mib2 = SNMP_MIB_CREATE(mib2_base, &mib2_root.node);
const struct snmp_mib *const snmp_modchip_mibs[] = { &modchip_mib2 };

