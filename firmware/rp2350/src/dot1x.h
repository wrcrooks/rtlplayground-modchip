#ifndef _DOT1X_H_
#define _DOT1X_H_

/*
 * 802.1X port authentication: the switch as authenticator, a RADIUS server
 * as authentication server (EAP relayed per RFC 3579), and MAC
 * authentication (MAB) for devices without a supplicant. dot1x.c is the
 * protocol, with no platform code; dot1x_plat.c ties it to the switch and
 * lwIP. Ports are logical port numbers.
 */

#include <stdbool.h>
#include <stdint.h>

#define DOT1X_PORTS		10
#define DOT1X_SECRET_MAX	64
#define DOT1X_IDENTITY_MAX	64

/* Port control, per port */
#define DOT1X_FORCE_AUTH	0	/* no authentication: the default */
#define DOT1X_AUTO		1	/* authenticate whatever is connected */
#define DOT1X_FORCE_UNAUTH	2	/* blocked */

/* Port states */
#define DOT1X_S_OFF		0	/* force-auth, or 802.1X off */
#define DOT1X_S_DOWN		1	/* auto, link down */
#define DOT1X_S_CONNECTING	2	/* asking for an identity */
#define DOT1X_S_AUTHENTICATING	3	/* talking to the RADIUS server */
#define DOT1X_S_AUTHENTICATED	4
#define DOT1X_S_HELD		5	/* after a failure, the quiet period */
#define DOT1X_S_BLOCKED		6	/* force-unauth */
#define DOT1X_S_GUEST		7	/* open in the guest VLAN */
#define DOT1X_S_AUTH_FAIL	8	/* open in the auth-fail VLAN */

#define DOT1X_M_NONE		0
#define DOT1X_M_EAP		1
#define DOT1X_M_MAB		2

/* Timing, seconds (802.1X-2004 defaults where it has one) */
#define DOT1X_TX_PERIOD		30	/* between EAP Request/Identity */
#define DOT1X_MAB_AFTER		3	/* requests without an answer before MAB */
#define DOT1X_SUPP_TIMEOUT	30
#define DOT1X_QUIET_PERIOD	60
#define DOT1X_SERVER_TIMEOUT	5	/* per RADIUS try */
#define DOT1X_SERVER_TRIES	3
#define DOT1X_FAIL_AFTER	3	/* rejections before the auth-fail VLAN */
#define DOT1X_AUTH_FAIL_RETRY	3600	/* until another try from there, without reauth */

struct dot1x_config {
	bool enabled;
	uint8_t radius_ip[4];
	uint16_t radius_port;
	char secret[DOT1X_SECRET_MAX];
	bool radsec;			/* over TLS, radsec.c */
	uint8_t mode[DOT1X_PORTS];
	bool mab[DOT1X_PORTS];
	uint32_t reauth_s;		/* 0: no periodic re-authentication */
	bool radius_vlan;		/* take the VLAN an Access-Accept names */
	uint16_t guest_vlan[DOT1X_PORTS];	/* for devices that don't speak 802.1X, 0: none */
	uint16_t fail_vlan[DOT1X_PORTS];	/* for devices the server rejects, 0: none */
};

struct dot1x_status {
	uint8_t state;
	uint8_t method;			/* DOT1X_M_* of the last success */
	bool authorized;
	bool mac_valid;
	uint8_t mac[6];			/* the supplicant's, or the MAB device's */
	char identity[DOT1X_IDENTITY_MAX];
	uint32_t successes, failures;
	uint32_t reauth_left;
	uint16_t vlan;			/* the port was put in, 0: its own */
};

extern struct dot1x_config dot1x_cfg;

void dot1x_init(void);
/* After a configuration change: states, the EAPOL trap and the port gates */
void dot1x_apply(void);
void dot1x_link(uint8_t port, bool up);
/* An EAPOL PDU (from its version byte) from src on port */
void dot1x_eapol_rx(uint8_t port, const uint8_t *src, const uint8_t *pdu, uint16_t len);
/* Any other frame from an unauthorized port: the MAC for MAB */
void dot1x_seen(uint8_t port, const uint8_t *src);
void dot1x_radius_rx(const uint8_t *p, uint16_t len);
void dot1x_second(void);
/* Start over on port (dot1x port <n> restart) */
void dot1x_restart(uint8_t port);
/* Ports that may only exchange EAPOL */
uint16_t dot1x_blocked(void);
const struct dot1x_status *dot1x_port_status(uint8_t port);
const char *dot1x_state_name(uint8_t state);

/* ---- supplied by the platform ---- */
/* Valid logical ports, bit per port */
uint16_t dot1x_hw_ports(void);
uint8_t dot1x_hw_front_port(uint8_t port);
/* The link as last reported: links only report changes, so a port set to
 * auto with its link already up needs this */
bool dot1x_hw_link(uint8_t port);
/* An EAPOL PDU to the supplicant on port */
void dot1x_hw_eapol_tx(uint8_t port, const uint8_t *pdu, uint16_t len);
void dot1x_hw_radius_tx(const uint8_t *p, uint16_t len);
/* Fence off the ports in blocked, which learn no addresses but for one on those
 * in learn; on: trap EAPOL to the CPU */
void dot1x_hw_gate(uint16_t blocked, uint16_t learn, bool on);
/* Put port in vlan, untagged and alone; 0: back to its own. False if the
 * VLAN doesn't exist, with the port in its own. */
bool dot1x_hw_vlan(uint8_t port, uint16_t vlan);
void dot1x_hw_random(uint8_t *p, uint8_t n);
void dot1x_hw_nas_ip(uint8_t ip[4]);
const uint8_t *dot1x_hw_mac(void);
void dot1x_hw_log(const char *msg, uint8_t port);

/* HMAC-MD5 and MD5 (dot1x.c, over md5.c), exported for the tests */
void dot1x_md5(const uint8_t *a, uint16_t na, const uint8_t *b, uint16_t nb, uint8_t out[16]);
void dot1x_hmac_md5(const uint8_t *key, uint16_t nkey, const uint8_t *m, uint16_t nm, uint8_t out[16]);

#endif
