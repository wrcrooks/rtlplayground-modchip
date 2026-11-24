#ifndef _EXTRAS_H_
#define _EXTRAS_H_

#include <stddef.h>

/* /features.json: the state of what the mod chip adds (extras.c) */
size_t features_json(char *buf, size_t max);
/* /dot1x.json (dot1x_plat.c): more than RTLPlayground's output buffer holds */
size_t dot1x_json(char *buf, size_t max);

#endif
