#ifndef _UNIFI_PROTO_H_
#define _UNIFI_PROTO_H_

/*
 * The UniFi inform protocol, the parts with no network in them (unifi.c has
 * those): packets, the controller's JSON, and its switch configuration
 * (system_cfg) as RTLPlayground commands.
 *
 * A packet: "TNBU", version 1, the device's MAC, flags, a 16-byte IV, data
 * version 1, the body's length, then the body: JSON, zlib, then AES-128-CBC,
 * or AES-GCM with the IV as its 16-byte nonce and the 40-byte header as
 * additional data, the tag after the ciphertext.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNIFI_HDR		40
#define UNIFI_DEFAULT_KEY	"ba86f2bbe107c7c57eb5f2690775c712"	/* MD5("ubnt") */
#define UNIFI_PORTS		10	/* the most a model below has */
#define UNIFI_VLANS		32

/* The packet for json, in out; its length, or -1: out too small */
long unifi_seal(uint8_t *out, size_t max, const uint8_t mac[6], const uint8_t key[16], bool gcm,
		const uint8_t iv[16], const char *json, size_t len);
/* The JSON in a packet, NUL-terminated in out; its length, or -1: not one, a wrong key, damaged, too long */
long unifi_open(const uint8_t *pkt, size_t len, const uint8_t key[16], char *out, size_t max);

/* A top-level member of a JSON object: a string, unescaped, or an integer */
bool unifi_json_str(const char *json, const char *name, char *out, size_t max);
bool unifi_json_int(const char *json, const char *name, long *v);
/* The value of a "key=value" line, as in mgmt_cfg */
bool unifi_kv(const char *text, const char *key, char *out, size_t max);
bool unifi_hex16(const char *hex, uint8_t key[16]);

/* The model claimed, and which front-panel port each of its ports is (0: none here) */
struct unifi_model {
	const char *code, *name;
	uint8_t ports;
	uint8_t front[UNIFI_PORTS + 1];		/* by UniFi port, from 1 */
	bool sfp[UNIFI_PORTS + 1];
};
/* For a switch with that many copper and SFP ports, front-panel order copper first */
void unifi_model_pick(uint8_t copper, uint8_t sfp, struct unifi_model *m);

enum { UNIFI_DEFAULT, UNIFI_UNTAGGED, UNIFI_TAGGED, UNIFI_EXCLUDE };

enum { UNIFI_STORM_B, UNIFI_STORM_M, UNIFI_STORM_U };

/* What of system_cfg is applied; -1: not given */
struct unifi_sw {
	struct {
		int8_t on, rstp;
		int32_t prio;				/* 0..61440 */
	} stp;
	uint8_t nvlans;
	struct {
		uint16_t id;
		uint8_t mode;				/* UNIFI_UNTAGGED or _TAGGED: of ports not named */
		uint8_t port[UNIFI_PORTS + 1];		/* UNIFI_* */
	} vlan[UNIFI_VLANS];
	struct {
		uint16_t pvid;				/* 0: not given */
		uint16_t speed;				/* Mbit/s when autoneg is off, 0: autoneg */
		bool full;
		char name[24];
		int8_t stp;				/* the port in STP: 0, 1 */
		int8_t edge;				/* 0 off, 1 on, 2 auto */
		int8_t guard;				/* BPDU guard: 0, 1 */
		bool isolated;				/* not to other isolated ports */
		uint32_t storm[3];			/* UNIFI_STORM_*, packets/s; 0: no limit */
		uint32_t egress_kbps;			/* 0: no limit */
	} port[UNIFI_PORTS + 1];
};

/* system_cfg's switch lines into sw; false: none, or more VLANs than UNIFI_VLANS */
bool unifi_parse(const char *system_cfg, struct unifi_sw *sw);

/*
 * The commands that take the switch from before (NULL: unknown) to sw, one per
 * line. VLANs only before has are deleted; a speed goes back to "auto" only
 * when before had it forced. Returns the length, or -1: out too small.
 */
long unifi_commands(const struct unifi_sw *sw, const struct unifi_sw *before, const struct unifi_model *m,
		    char *out, size_t max);

#endif
