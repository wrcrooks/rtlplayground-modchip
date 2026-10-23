/* Stand-in for SDCC's 8051.h: the few core SFRs RTLPlayground's portable
 * modules name. Storage in src/rtl_sfr.S; nothing acts on them here. */
#ifndef _STUB_8051_H
#define _STUB_8051_H
#include <stdbool.h>
#include <stdint.h>
extern volatile uint8_t PSBANK;
extern volatile bool EA;
extern volatile uint8_t SP;
#endif
