#ifndef _USM_H_
#define _USM_H_

/*
 * SNMPv3 with the user-based security model (RFC 3412, 3414, 3826, 7860) in
 * front of the SNMPv2c agent: a verified, decrypted request reaches the agent
 * as a v2c message, and its response goes back under the same user. SHA-1 or
 * SHA-256 authenticates, AES-128 encrypts; a user's level is the least its
 * messages may have. Also the notifications: coldStart, linkUp/linkDown and
 * events, as v2c or v3 traps.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USM_USERS	4
#define USM_NAME_MAX	32
#define USM_MSG_MAX	1600

enum usm_auth { USM_SHA1, USM_SHA256 };

/* Keys follow from the engine ID: set that first, users after */
void usm_engine(const uint8_t mac[6], uint32_t boots);
const uint8_t *usm_engine_id(uint8_t *len);
/* priv NULL: authentication only. False: table full, or a password under 8 characters */
bool usm_user_set(const char *name, enum usm_auth auth, const char *auth_pw, const char *priv_pw);
bool usm_user_del(const char *name);
void usm_users_clear(void);
/* The i-th user's name, NULL past the last */
const char *usm_user(int i, enum usm_auth *auth, bool *priv);

/* A v3 message: the reply's length in out, 0 for none */
size_t usm_receive(uint8_t *msg, size_t len, uint8_t *out, size_t max);

/* A notification's bytes, under community (v2c) or user (v3) */
struct usm_trap {
	const char *community, *user;
	uint32_t oid[14];
	uint8_t oid_len;
	int port;			/* ifIndex, 0: none */
	int if_admin, if_oper;		/* with port, 0: none */
	const char *text;		/* NULL: none */
};
size_t usm_trap(const struct usm_trap *t, uint8_t *out, size_t max);

/* ---- supplied by the agent and the platform ---- */
/* A v2c message to the agent, under its own community; its response */
size_t usm_agent(const uint8_t *msg, size_t len, uint8_t *out, size_t max);
const char *usm_community(void);
uint32_t usm_time(void);	/* s since boots was counted up */
uint32_t usm_uptime_cs(void);
void usm_random(uint8_t *p, size_t n);

#endif
