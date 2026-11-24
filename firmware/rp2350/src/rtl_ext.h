#ifndef _RTL_EXT_H_
#define _RTL_EXT_H_

/* Helpers for commands of our own (rtl_ext.c), after rtl837x_common.h and cmd_parser.h */
#include <stdbool.h>
#include <stdint.h>

extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[];
extern __xdata uint8_t atoi_results_u8;
uint8_t cmd_compare(uint8_t start, __code const uint8_t *cmd);
uint8_t cmd_parse_port_separator(uint8_t idx);

void ext_error(__code const char *msg);
const char *ext_rest(uint8_t w);	/* the rest of the line from word w */
const char *ext_word(uint8_t w, char *buf, uint8_t max);	/* word w alone; NULL if it doesn't fit */
/* A decimal number at *s, up to max, then sep (0: the end of the word); moves *s past it */
bool ext_num(const char **s, unsigned max, char sep, unsigned *v);
bool ext_word_num(uint8_t w, unsigned min, unsigned max, unsigned *v);
bool ext_ip(const char *s, unsigned a[4]);
/* Runs a command line outside the command history; not from within a command */
bool ext_run(const char *fmt, ...);

bool rtl_guard_cmd(void);	/* rtl_guard.c */
bool rtl_mcast_cmd(void);	/* rtl_mcast.c */

#endif
