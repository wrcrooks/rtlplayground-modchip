# ACL on the RTL8372/RTL8373: reverse engineering notes

RTLPlayground has no ACL support, and the chip's ACL registers aren't documented anywhere public.
The stock firmware of both switches here (XikeStor SKS3200M, MokerLink POE-2G040210GSM) carries
Realtek's driver for them: `..\dal\rtl8373\dal_rtl8373_acl.c`, with the service layer
`..\src\sal\salacl.c` above it. The driver's error strings say:

- "invalid ACL rulenum";
- "entry type isn't an ingress ACL rule";
- "block belongs to ingress ACL";
- "invalid ingress ACL selection priority";
- "ACL entry is already exit";
- "ACL entry is empty".

The stock web UI doesn't expose rules. Its only mention of ACLs is as one source in the QoS
priority selection. So the layout has to come from the code, then from experiments on a switch.

This page is the work so far. Tools are in `docs/re/`. Nothing from the stock images is in this
repository: run the tools on your own switch's flash dump.

## The stock image

`docs/re/stock_re.py` handles the layout:

- **The image.** It is laid out as RTLPlayground's `tools/imagebuilder.c` writes one:
  - a 2-byte header;
  - the common area (code 0x0000-0x3fff) from file offset 2;
  - bank n's window (code 0x4000-0xffff) from file offset (n − 1) × 0xc000 + 0x4000.

  `stock_re.py banks` writes one 64 KiB image per bank, for `r2 -a 8051`.
- **Bank switching.** It is Keil C51 code with banking through SFR 0x96 (PSBANK), as RTLPlayground's.
  - A table of stubs at code 0x1100 has one per bank. Each saves PSBANK, sets the bank, jumps
    through DPTR and restores it.
  - Callers reach a banked function through a trampoline, `mov dptr, #target; ljmp stub`.
  - `stock_re.py tramp` lists all 603 trampolines.
- **Calling convention.** Long arguments go through XDATA, stored with `lcall 0x263c` (R4-R7 →
  @DPTR). The register helpers:

  | Trampoline | Bank 10 | Use |
  |---|---|---|
  | 0x1b41 | 0x6148 | register write: address in R6:R7, value at XDATA 0x9179 |
  | 0x1b2f | 0x5f27 | register read into the generic pointer at XDATA 0x917d |
  | 0x1b47 | 0x6163 | register read into the generic pointer in R1-R3 |
  | 0x1b29 | 0x5e8d | register bits: mask at 0x9179, value at 0x917d (the ACL code uses it on 0x4810/0x4818) |

- **Pointers.** The driver's file-name strings sit in another bank than the code that passes them
  (as Keil "code" pointers, R3 = 0xff), so they're never printed. Don't use them to place code.

## What the ACL driver does (bank 14)

`stock_re.py tbl` lists every access to TBL_CTRL (0x5cac):

- banks 9 and 10 have the L2 and VLAN tables;
- **bank 14 holds the ACL driver's seven table functions**, at code 0x9795-0xa1a4, next to the code
  that names `dal_rtl8373_acl.c` (0xb841).

They use the table engine RTLPlayground already drives for L2 and VLANs:

- **The engine.** TBL_CTRL = entry << 16 | table type << 8 | command (bit 1 write, bit 0 execute),
  polled until bit 0 clears.
- **Data.** Written to DATA_IN at 0x5cb8, read from DATA_OUT at 0x5ccc. Here it is **five 32-bit
  words**, at 0x5cb8-0x5cc8 and 0x5ccc-0x5cdc.
- **Table type 1: the rules.** Entries 0-95 (the functions refuse an index above 0x5f).
  - A rule's data half is at entry i, and its care (mask) half at entry **0x80 + i**: two of the
    functions OR 0x80 into the index.
  - 96 rules with separate data and care halves is the RTL8367C's ACL design.
- **Table type 2: the actions.** Entries 0-95, read and written plainly.
- **Rule buffer.** The read functions unpack a rule into a 42-byte buffer (XDATA 0x913c). The field
  layout is in that unpacking: the next thing to work through.
- **Registers.** The same bank touches 0x4810, 0x4818 and 0x481c with the bits helper:
  - 0x4818 with a per-port mask (1 << port), and 0x4810 once at init;
  - these are probably the per-port ACL enable and a global switch. Not confirmed.

## Raw access on the mod chip

The RP2350 firmware has three debug commands for the two tables (`firmware/rp2350/src/rtl_ext.c`).
They aren't configuration, so nothing is saved:

```
acl rule <0-95>                         data (entry i) and care (entry 0x80 + i), five words each
acl rule <0-95> <d0>..<d4> <c0>..<c4>   write both halves (hex words), care first
acl action <0-95> [<w0>..<w4>]          the action entry
```

The simulator checks them against a model of the two tables. On a switch they're untried.

**Careful.** A rule that matches everything and drops can cut off the management path. On the mod
chip that only stops the switch's own traffic; the RP2350 console over SWD keeps working, and
`acl rule <i> 0 0 0 0 0 0 0 0 0 0` clears a rule. Work with a console attached.

## Next steps

1. **Read what stock leaves in the tables.** `acl rule` on all 96 entries of a switch running RTLPlayground shows
   what's set at reset. On stock firmware the same reads over T3 (the RP2350 reads registers there)
   show what Realtek's init writes. `init.c` mentions an `aclAllPortMask`, so stock may install rules
   at boot.
2. **Decode the unpacking.** The read functions in bank 14, from 0x9795, turn the five words into the
   42-byte rule buffer, and the write functions do the reverse. This gives the fields:
   - per-port enable;
   - template or field selectors;
   - the "valid" bit;
   - which word holds what.

   Then the action word: drop, redirect, mirror, priority, the policer index.
3. **Find the templates.** On the RTL8367C, fixed-width fields are filled per rule from one of five
   templates of field selectors, in registers. "block belongs to ingress ACL" points to blocks of
   rules with their own templates, as on the RTL9300.
4. **Confirm on a switch.**
   - Write a rule that matches a destination MAC with a drop action, and see the frames stop.
   - Then move the match to a VLAN, an EtherType, an IP and a port, one at a time.
5. **Then add a real `acl` command set** to RTLPlayground (on `feat/modchip` first, then upstream),
   built on what is confirmed.
