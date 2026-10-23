#ifndef _UPLOAD_H_
#define _UPLOAD_H_

/*
 * Firmware uploads for the switch: the multipart body of RTLPlayground's
 * POST /upload, streamed into the bridge's staging area. Any image in the
 * running bridge's layout is accepted - a new bridge build, or
 * RTLPlayground to go back - if its CRC-16 holds (0xb001 over the whole
 * image, as RTLPlayground's build sets it and its boot code checks) and it
 * starts 00 40, as the 8051's code does and the boot code looks for. After
 * a REBOOT the 8051 checks it again and installs it.
 *
 * The flash goes one operation per upload_feed() call (an erase or a page
 * write), so the main loop keeps running through an upload that takes
 * minutes on the switch's slow flash. No hardware here; the flash access
 * is upload_flash_*() (upload_bridge.c on the device, test/test_upload.c
 * on the host).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPLOAD_PAGE		256
#define UPLOAD_SECTOR		4096

enum upload_state {
	UP_IDLE,
	UP_PREAMBLE,		/* the part's headers, up to the blank line */
	UP_DATA,		/* the image */
	UP_EPILOGUE,		/* the closing delimiter and whatever follows */
	UP_DONE,		/* image staged and read back intact */
	UP_FAILED,
};

/* Start an upload: the request's Content-Length, the boundary from its
 * Content-Type, the image size and staging address from HELLO */
bool upload_begin(uint32_t content_length, const char *boundary, uint32_t image_size,
		  uint32_t stage_addr, uint32_t flash_size);

/* Takes up to n bytes of the body; returns how many it used. Does at most
 * one flash operation. Call again with the rest. */
size_t upload_feed(const uint8_t *p, size_t n);

/* Once the whole body is in: the last page, then a read-back check. True
 * if the image is staged and intact; at most one flash operation per call,
 * false with upload_state() still UP_EPILOGUE meaning call again. */
bool upload_finish(void);

enum upload_state upload_state(void);
const char *upload_error(void);	/* why it failed */
uint32_t upload_body_left(void);	/* body bytes not yet fed */
void upload_abort(void);	/* no reboot follows: invalidates the staging area */

/* The flash, by the switch's addresses */
bool upload_flash_erase(uint32_t addr);				/* one sector */
bool upload_flash_write(uint32_t addr, const uint8_t *p, uint16_t n);	/* within a page */
bool upload_flash_read(uint32_t addr, uint8_t *p, uint16_t n);	/* n up to 512 */

#endif
