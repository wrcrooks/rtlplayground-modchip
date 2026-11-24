#ifndef _SSH_H_
#define _SSH_H_

/*
 * The command line over SSH: an SSH-2 server (RFC 4251-4254) with one session
 * per connection, password authentication with the web UI's password, any
 * user name. Key exchange curve25519-sha256 or ecdh-sha2-nistp256 with strict
 * KEX, an ecdsa-sha2-nistp256 host key, AES-GCM or AES-CTR with
 * hmac-sha2-256(-etm). ssh.c is the protocol, ssh_lwip.c the connections.
 *
 * In a session: RTLPlayground's commands, "exit", "backup" (the saved
 * configuration), and as "ssh <switch> restore [<minutes>] < file" a
 * configuration restored as the web UI does, with a rollback.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ssh;

struct ssh *ssh_new(void *io, const char *peer);
void ssh_free(struct ssh *s);
/* Bytes from the peer; false: the connection is over (after what is queued) */
bool ssh_input(struct ssh *s, const uint8_t *d, size_t n);
/* More output may fit; false: over */
bool ssh_poll(struct ssh *s);
bool ssh_second(struct ssh *s);
void ssh_lock_second(void);	/* once a second, for all: guessing is slowed down */

/* The host key, DER (tls_cert.c's kind of key) */
bool ssh_host_key_set(const uint8_t *der, size_t len);
bool ssh_host_key_make(uint8_t *der, size_t *len);
/* "SHA256:..." as ssh prints it */
bool ssh_fingerprint(char *buf, size_t max);

/* ---- supplied by ssh_lwip.c or the test ---- */
size_t ssh_io_room(void *io);
void ssh_io_write(void *io, const uint8_t *d, size_t n);	/* never more than room */
bool ssh_password_ok(const char *password);
/* Runs a command line; its output, never more than max - 1 */
size_t ssh_run(char *line, char *out, size_t max, bool *ok);
size_t ssh_backup(char *out, size_t max);
/* text is a whole file; a message for the user in out */
bool ssh_restore(char *text, size_t len, unsigned minutes, char *out, size_t max);
const char *ssh_prompt(void);
void ssh_log(bool login, bool ok, const char *user, const char *peer);

#endif
