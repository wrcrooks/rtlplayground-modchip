/*
 * Firmware uploads for the switch, see upload.h
 */

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "upload.h"

#define PREAMBLE_MAX		1024
#define DELIM_MAX		80
#define READ_CHUNK		512
#define CRC_RESIDUE		0xb001	/* CRC-16/ARC over an image that carries its CRC */

static enum upload_state state;
static char error[96];
static uint32_t body_left, image_size, stage, image_seen, page_addr, erased_end, verify_pos;
static uint16_t crc, crc_back;
static char delim[DELIM_MAX];	/* "\r\n--" boundary */
static size_t delim_len, delim_seen;
static char pre[PREAMBLE_MAX + 1];
static size_t pre_len;
static uint8_t page[UPLOAD_PAGE];
static uint8_t head[2];		/* the image's first bytes */
static size_t page_len;


static void fail(const char *fmt, unsigned long a, unsigned long b)
{
	snprintf(error, sizeof(error), fmt, a, b);
	state = UP_FAILED;
	/* An intact image left in the staging area would be installed at the
	 * next boot of the 8051: make sure nothing valid stays behind */
	upload_flash_erase(stage);
}


bool upload_begin(uint32_t content_length, const char *boundary, uint32_t size, uint32_t addr, uint32_t flash_size)
{
	size_t bl = strlen(boundary);

	error[0] = 0;
	state = UP_IDLE;
	if (!bl || bl > DELIM_MAX - 5) {
		snprintf(error, sizeof(error), "no multipart boundary");
		return false;
	}
	if (!size || size % UPLOAD_SECTOR || addr % UPLOAD_SECTOR || addr + size > flash_size) {
		snprintf(error, sizeof(error), "the switch's flash has no room for a staged image");
		return false;
	}
	if (content_length <= size) {
		snprintf(error, sizeof(error), "body of %lu bytes is too short for a %lu byte image",
			 (unsigned long)content_length, (unsigned long)size);
		return false;
	}
	memcpy(delim, "\r\n--", 4);
	memcpy(delim + 4, boundary, bl);
	delim_len = 4 + bl;
	delim_seen = 0;
	body_left = content_length;
	image_size = size;
	stage = addr;
	image_seen = 0;
	page_addr = addr;
	erased_end = addr;
	page_len = 0;
	pre_len = 0;
	crc = 0;
	verify_pos = 0;
	crc_back = 0;
	state = UP_PREAMBLE;
	return true;
}


/* The page in hand to flash: its sector erased first. One operation. */
static bool flush_page(void)
{
	if (page_addr >= erased_end) {
		if (!upload_flash_erase(erased_end)) {
			fail("erasing the switch's flash at 0x%lx failed", erased_end, 0);
			return false;
		}
		erased_end += UPLOAD_SECTOR;
		return true;	/* the write comes next time */
	}
	if (!upload_flash_write(page_addr, page, page_len)) {
		fail("writing the switch's flash at 0x%lx failed", page_addr, 0);
		return false;
	}
	page_addr += page_len;
	page_len = 0;
	return true;
}


size_t upload_feed(const uint8_t *p, size_t n)
{
	size_t used = 0;

	if (n > body_left)
		n = body_left;
	switch (state) {
	case UP_PREAMBLE:
		/* "--boundary\r\n", the part's headers, a blank line */
		while (used < n) {
			if (pre_len == PREAMBLE_MAX) {
				fail("multipart part headers longer than %lu bytes", PREAMBLE_MAX, 0);
				goto out;
			}
			pre[pre_len++] = p[used++];
			if (pre_len >= 4 && !memcmp(pre + pre_len - 4, "\r\n\r\n", 4)) {
				pre[pre_len] = 0;
				if (strncmp(pre, delim + 2, delim_len - 2)) {
					fail("the body doesn't start with the multipart boundary", 0, 0);
					goto out;
				}
				state = UP_DATA;
				break;
			}
		}
		break;
	case UP_DATA:
		if (page_len == UPLOAD_PAGE) {
			flush_page();
			return 0;
		}
		used = UPLOAD_PAGE - page_len;
		if (used > n)
			used = n;
		if (used > image_size - image_seen)
			used = image_size - image_seen;
		for (size_t i = 0; i < used && image_seen + i < sizeof(head); i++)
			head[image_seen + i] = p[i];
		memcpy(page + page_len, p, used);
		page_len += used;
		crc = link_crc16(crc, p, used);
		image_seen += used;
		if (image_seen == image_size)
			state = UP_EPILOGUE;
		break;
	case UP_EPILOGUE:
		/* The image must end exactly where the closing delimiter starts */
		while (used < n && delim_seen < delim_len) {
			if (p[used] != (uint8_t)delim[delim_seen]) {
				fail("the image is longer than the %lu bytes of the bridge's layout", image_size, 0);
				goto out;
			}
			used++;
			delim_seen++;
		}
		used = n;	/* the final "--" and the CRLF */
		break;
	default:
		/* Failed or done: drain the rest. A browser reads the verdict
		 * only once it has sent the whole body. */
		used = n;
		break;
	}
out:
	body_left -= used;
	return used;
}


bool upload_finish(void)
{
	uint8_t buf[READ_CHUNK];

	if (state == UP_PREAMBLE || state == UP_DATA) {
		fail(state == UP_DATA ? "the image is %lu bytes, the bridge's layout takes %lu"
				      : "no file in the upload", image_seen, image_size);
		return false;
	}
	if (state != UP_EPILOGUE)
		return state == UP_DONE;
	if (delim_seen < delim_len) {
		fail("the upload ends without its closing boundary", 0, 0);
		return false;
	}
	if (page_len) {
		flush_page();
		return false;
	}
	if (crc != CRC_RESIDUE) {
		fail("bad image CRC (0x%04lx), not used", crc, 0);
		return false;
	}
	/* The 8051's code starts 00 40; the boot code of both RTLPlayground and
	 * the bridge looks for it before it checks a staged image at all */
	if (head[0] != 0x00 || head[1] != 0x40) {
		fail("not a switch firmware image (it starts %02lx%02lx, not 0040)", head[0], head[1]);
		return false;
	}
	/* Read the staged image back: what the 8051 will check at boot */
	if (verify_pos < image_size) {
		uint32_t n = image_size - verify_pos < READ_CHUNK ? image_size - verify_pos : READ_CHUNK;
		if (!upload_flash_read(stage + verify_pos, buf, n)) {
			fail("reading the staged image back failed at 0x%lx", stage + verify_pos, 0);
			return false;
		}
		crc_back = link_crc16(crc_back, buf, n);
		verify_pos += n;
		return false;
	}
	if (crc_back != CRC_RESIDUE) {
		fail("the staged image reads back wrong (CRC 0x%04lx)", crc_back, 0);
		return false;
	}
	state = UP_DONE;
	return true;
}


enum upload_state upload_state(void)
{
	return state;
}


const char *upload_error(void)
{
	return error;
}


uint32_t upload_body_left(void)
{
	return body_left;
}


/* The upload ends without a reboot to install it (the client went away,
 * or a request failed): nothing in the staging area may stay installable */
void upload_abort(void)
{
	if (state == UP_IDLE)
		return;
	if (state != UP_FAILED)
		snprintf(error, sizeof(error), "upload cancelled");
	upload_flash_erase(stage);
	state = UP_IDLE;
}
