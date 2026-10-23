/*
 * Storage for the 8051 SFRs that RTLPlayground's modules read and write
 * (rtl837x_sfr.h declares them; sdcc_shim.h turns the declarations into
 * plain externs). On the 8051 the 16-bit views overlap the bytes:
 * SFR_DATA_U16 is A6:A7, that is SFR_DATA_8 high and SFR_DATA_0 low. A
 * little-endian halfword at the address of SFR_DATA_0 is exactly that, so
 * the symbols are laid out to alias the same way.
 */
	.bss
	.balign 4

	.global SFR_DATA_U16, SFR_DATA_0, SFR_DATA_8
	.global SFR_DATA_U16_UPPER, SFR_DATA_16, SFR_DATA_24
SFR_DATA_U16:
SFR_DATA_0:		.space 1
SFR_DATA_8:		.space 1
SFR_DATA_U16_UPPER:
SFR_DATA_16:		.space 1
SFR_DATA_24:		.space 1

	.global SFR_REG_ADDR_U16, SFR_REG_ADDRL, SFR_REG_ADDRH
SFR_REG_ADDR_U16:
SFR_REG_ADDRL:		.space 1
SFR_REG_ADDRH:		.space 1

	.global SFR_SMI_REG_U16, SFR_SMI_REG_L, SFR_SMI_REG_H
SFR_SMI_REG_U16:
SFR_SMI_REG_L:		.space 1
SFR_SMI_REG_H:		.space 1

	/* Written only by rtl837x_phy.c's RTL8224 paths, which run on the
	 * RTL8373 alone; status 0 lets their busy-wait loops fall through. */
	.global SFR_SMI_DEV, SFR_SMI_PHYMASK, SFR_EXEC_GO, SFR_EXEC_STATUS, SFR_93, SFR_94
SFR_SMI_DEV:		.space 1
SFR_SMI_PHYMASK:	.space 1
SFR_EXEC_GO:		.space 1
SFR_EXEC_STATUS:	.space 1
SFR_93:			.space 1
SFR_94:			.space 1

	/* 8051.h stand-ins, see stub/8051.h */
	.global PSBANK, EA, SP
PSBANK:			.space 1
EA:			.space 1
SP:			.space 1

	.section .note.GNU-stack, "", %progbits
