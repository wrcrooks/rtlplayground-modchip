#ifndef _SAFETY_H_
#define _SAFETY_H_

/*
 * Ways back from a change that cuts the switch off: a timed restart, and a
 * rollback, which returns to the configuration saved when it was started
 * unless it is confirmed in time. A restart before that rolls back too.
 * Also the check a configuration file gets before it is restored.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAFETY_MAX_MIN	1440

void safety_boot(void);			/* before the configuration is read */
void safety_second(void);
void safety_reboot_in(uint16_t minutes);	/* 0: cancel */
uint32_t safety_reboot_left(void);	/* s, 0: none */
bool safety_rollback_start(uint16_t minutes);
void safety_rollback_confirm(void);
uint32_t safety_rollback_left(void);

/* The saved configuration as a file; its length */
size_t config_backup(char *buf, size_t max);
/* Checks text (changed) and saves it, with a rollback in minutes unless 0; the
 * caller restarts the switch. A verdict in msg, either way. */
bool config_restore(char *text, size_t len, unsigned minutes, char *msg, size_t max);

/*
 * A configuration file: comments ('#') and blank lines go, CRLF becomes LF,
 * and each line must be a command the switch knows and can take. The result
 * replaces text. Returns the new length, or -1 with the reason in err.
 */
int config_check(char *text, size_t len, char *err, size_t err_len);

#endif
