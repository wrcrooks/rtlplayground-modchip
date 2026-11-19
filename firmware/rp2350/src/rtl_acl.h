#ifndef _RTL_ACL_H_
#define _RTL_ACL_H_

/* ACL rules (rtl_acl.c) and the raw table access under them (rtl_ext.c) */

#include <stdbool.h>
#include <stdint.h>

#define ACL_ENTRIES	96
#define ACL_WORDS	5
#define TBL_ACL_RULE	1
#define TBL_ACL_ACTION	2

/* One table entry through TBL_CTRL: false if the engine doesn't answer */
bool acl_op(uint16_t entry, uint8_t type, bool write, uint32_t *w);

/* acl <1-96> ... and the bare acl: true if it took the command */
bool rtl_acl_cmd(void);
/* The 8051 started over, with the registers at their defaults: forget the
 * rules, which the config's acl lines set up again */
void rtl_acl_reset(void);

#endif
