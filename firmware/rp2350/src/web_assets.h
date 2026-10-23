#ifndef _WEB_ASSETS_H_
#define _WEB_ASSETS_H_

#include <stdint.h>

/* RTLPlayground's html/ files, gzipped at build time by tools/embed_web.py */
struct web_asset {
	const char *path;
	const char *mime;
	const uint8_t *data;
	uint32_t len;
	uint8_t gzip;
};

extern const struct web_asset web_assets[];

#endif
