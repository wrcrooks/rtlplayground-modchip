/*
 * upload.c's flash access: the switch's SPI flash, through the bridge's
 * FLASH_ERASE, FLASH_WRITE and FLASH_READ. The bridge allows erases and
 * writes only at or above the running image's end (upload_start).
 */

#include "bridge.h"
#include "upload.h"

static void put_addr(uint8_t *p, uint32_t a)
{
	p[0] = a;
	p[1] = a >> 8;
	p[2] = a >> 16;
	p[3] = a >> 24;
}


bool upload_flash_erase(uint32_t addr)
{
	uint8_t p[4];

	put_addr(p, addr);
	return bridge_request(REQ_FLASH_ERASE, p, 4, NULL, 0, NULL, 0, NULL) == ST_OK;
}


bool upload_flash_write(uint32_t addr, const uint8_t *data, uint16_t n)
{
	uint8_t p[4];

	put_addr(p, addr);
	return bridge_request(REQ_FLASH_WRITE, p, 4, data, n, NULL, 0, NULL) == ST_OK;
}


bool upload_flash_read(uint32_t addr, uint8_t *data, uint16_t n)
{
	uint8_t p[6];
	size_t got;

	put_addr(p, addr);
	p[4] = n;
	p[5] = n >> 8;
	return bridge_request(REQ_FLASH_READ, p, 6, NULL, 0, data, n, &got) == ST_OK && got == n;
}
