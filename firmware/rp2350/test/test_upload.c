/*
 * upload.c on the host: multipart bodies as a browser sends them, into a
 * simulated NOR flash (erase to 0xff, writes only clear bits and stay
 * within a page). A good image must land intact; bad, short, long and
 * malformed ones must fail and leave nothing installable behind. With a
 * path argument, that image file (a real bridge or RTLPlayground build)
 * is uploaded too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "upload.h"

#define FLASH_SIZE	0x80000		/* a W25Q40, as on the MokerLink */
#define STAGE		0x3f000		/* the compact layout's staging area */
#define IMAGE		0x3f000		/* and its image size */
#define BOUNDARY	"----WebKitFormBoundaryAb12Cd34Ef56Gh78"

static uint8_t flash[FLASH_SIZE];
static int ops, ops_this_call, max_ops_per_call, fail_write_at = -1;
static int failures;

static void check(int cond, const char *what)
{
	printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
	failures += !cond;
}

static void op(void)
{
	ops++;
	if (++ops_this_call > max_ops_per_call)
		max_ops_per_call = ops_this_call;
}

bool upload_flash_erase(uint32_t addr)
{
	op();
	if (addr % UPLOAD_SECTOR || addr + UPLOAD_SECTOR > FLASH_SIZE)
		return false;
	memset(flash + addr, 0xff, UPLOAD_SECTOR);
	return true;
}

bool upload_flash_write(uint32_t addr, const uint8_t *p, uint16_t n)
{
	op();
	if (fail_write_at >= 0 && addr >= (uint32_t)fail_write_at)
		return false;
	if (!n || n > UPLOAD_PAGE || (addr % UPLOAD_PAGE) + n > UPLOAD_PAGE || addr + n > FLASH_SIZE)
		return false;
	for (int i = 0; i < n; i++)
		flash[addr + i] &= p[i];	/* NOR: an unerased byte stays wrong */
	return true;
}

bool upload_flash_read(uint32_t addr, uint8_t *p, uint16_t n)
{
	op();
	if (n > 512 || addr + n > FLASH_SIZE)
		return false;
	memcpy(p, flash + addr, n);
	return true;
}

/* An image with its CRC set as RTLPlayground's crc_calculator does: the
 * complement of CRC-16/ARC over the rest, low byte first */
static void make_image(uint8_t *img, size_t n, unsigned seed)
{
	srand(seed);
	for (size_t i = 0; i < n - 2; i++)
		img[i] = rand();
	img[0] = 0x00;	/* the 8051's code starts 00 40 */
	img[1] = 0x40;
	uint16_t c = ~link_crc16(0, img, n - 2);
	img[n - 2] = c;
	img[n - 1] = c >> 8;
}

static size_t make_body(uint8_t *body, const uint8_t *img, size_t n, const char *boundary)
{
	size_t len = sprintf((char *)body, "--%s\r\nContent-Disposition: form-data; name=\"uploadedfile\"; "
			     "filename=\"bridge.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n", boundary);
	memcpy(body + len, img, n);
	len += n;
	len += sprintf((char *)body + len, "\r\n--%s--\r\n", BOUNDARY);
	return len;
}

/* As httpd.c drives it: whatever arrived, in pieces of varying size */
static bool run(const uint8_t *body, size_t len, uint32_t image)
{
	size_t off = 0;
	int guard = 0;

	if (!upload_begin(len, BOUNDARY, image, STAGE, FLASH_SIZE))
		return false;
	srand(len);
	/* Fed to the end even after a failure: the verdict waits for the whole body */
	while (upload_body_left() && guard++ < 10000000) {
		size_t avail = 1 + rand() % 1460;
		if (avail > len - off)
			avail = len - off;
		ops_this_call = 0;
		off += upload_feed(body + off, avail);
	}
	while (upload_state() == UP_EPILOGUE && guard++ < 10000000) {
		ops_this_call = 0;
		upload_finish();
	}
	if (upload_state() == UP_PREAMBLE || upload_state() == UP_DATA)
		upload_finish();
	return upload_state() == UP_DONE;
}

static uint8_t img[IMAGE + 4096], body[IMAGE + 8192];

static int staged_installable(void)
{
	return link_crc16(0, flash + STAGE, IMAGE) == 0xb001;
}

int main(int argc, char **argv)
{
	size_t len;

	memset(flash, 0x5a, sizeof(flash));	/* old contents, not erased */
	make_image(img, IMAGE, 1);
	len = make_body(body, img, IMAGE, BOUNDARY);
	ops = max_ops_per_call = 0;
	check(run(body, len, IMAGE), "a good image is staged");
	check(!memcmp(flash + STAGE, img, IMAGE), "the staging area holds the image exactly");
	check(max_ops_per_call <= 1, "never more than one flash operation per call");
	printf("      %d flash operations: %d erases, %d page writes, %d reads\n", ops,
	       IMAGE / UPLOAD_SECTOR, IMAGE / UPLOAD_PAGE, IMAGE / 512);
	check(ops == IMAGE / UPLOAD_SECTOR + IMAGE / UPLOAD_PAGE + IMAGE / 512, "one erase per sector, one write per page, a full read-back");
	check(flash[STAGE + IMAGE] == 0x5a && flash[STAGE - 1] == 0x5a, "nothing outside the staging area is touched");

	/* Cancelled after success (the client went away before the reboot) */
	upload_abort();
	check(!staged_installable(), "an abort leaves nothing installable");

	/* A bad CRC */
	make_image(img, IMAGE, 2);
	img[1000] ^= 1;
	len = make_body(body, img, IMAGE, BOUNDARY);
	check(!run(body, len, IMAGE) && strstr(upload_error(), "CRC"), "a bad CRC is refused");
	check(!staged_installable(), "  and leaves nothing installable");

	/* A good CRC on something that isn't switch firmware */
	make_image(img, IMAGE, 7);
	img[1] = 0x41;
	uint16_t c2 = ~link_crc16(0, img, IMAGE - 2);
	img[IMAGE - 2] = c2;
	img[IMAGE - 1] = c2 >> 8;
	len = make_body(body, img, IMAGE, BOUNDARY);
	check(!run(body, len, IMAGE) && strstr(upload_error(), "not a switch firmware"), "an image that doesn't start 00 40 is refused");
	check(!staged_installable(), "  and leaves nothing installable");

	/* Too long: the other layout's 512 KiB image */
	static uint8_t big[0x80000];
	make_image(big, 0x40000, 3);
	len = make_body(body, big, 0x40000, BOUNDARY);
	check(!run(body, len, IMAGE) && strstr(upload_error(), "longer"), "an image too long for the layout is refused");
	check(upload_body_left() == 0, "  and the rest of the body is drained for the verdict");
	check(!staged_installable(), "  and leaves nothing installable");

	/* Too short */
	make_image(img, IMAGE - 4096, 4);
	len = make_body(body, img, IMAGE - 4096, BOUNDARY);
	check(!run(body, len, IMAGE) && (strstr(upload_error(), "takes") || strstr(upload_error(), "short")), "a short image is refused");

	/* Not this upload's boundary */
	make_image(img, IMAGE, 5);
	len = make_body(body, img, IMAGE, "----SomethingElse");
	check(!run(body, len, IMAGE) && strstr(upload_error(), "boundary"), "a body that doesn't start with the boundary is refused");

	/* A flash write failing halfway */
	make_image(img, IMAGE, 6);
	len = make_body(body, img, IMAGE, BOUNDARY);
	fail_write_at = STAGE + 0x10000;
	check(!run(body, len, IMAGE) && strstr(upload_error(), "writing"), "a failing flash write ends the upload");
	fail_write_at = -1;
	check(!staged_installable(), "  and leaves nothing installable");

	/* No room: a staging area past the end of the flash */
	check(!upload_begin(len, BOUNDARY, IMAGE, FLASH_SIZE - 4096, FLASH_SIZE), "no upload without room for it");

	/* A real build, if given */
	if (argc > 1) {
		FILE *f = fopen(argv[1], "rb");
		size_t n = f ? fread(big, 1, sizeof(big), f) : 0;
		if (f)
			fclose(f);
		len = make_body(body, big, n, BOUNDARY);
		check(n == IMAGE && run(body, len, IMAGE) && !memcmp(flash + STAGE, big, n), argv[1]);
	}

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
