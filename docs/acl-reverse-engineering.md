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

## Findings from emulation (no hardware)

`docs/re/emu51.py` is an 8051 emulator for the stock image. It models:
- the register engine (SFRs 0xa0-0xa7) and the table engine;
- the bank-save stack (SFR 0xbb, which has to be a stack);
- the UART, whose debug prints are kept in `Stock.uart`;
- Keil's data init (`init_data()`). The image holds a boot stage at 0x0000 and the application, with
  its own reset code at 0x1000.

It runs the stock driver's functions as they are. `docs/re/probes/` holds the probes; set `STOCK_BIN`
to your dump. Set XDATA 0xb325 = 1 (the "switch initialised" flag) and call the init at bank 14
0xa1b4 first.

**Bank 14 functions:**

| Address | Function |
|---|---|
| 0xa1b4 | init |
| 0xa33c | add a field to a rule config |
| 0xa51a | add a rule (index, config, action, rule count) |
| 0xba2b | get a rule |
| 0xc4ea | delete a rule |
| 0x9795 / 0x9aa6 | raw rule read / write |
| 0x9e8b / 0xa017 | raw action read / write |

**What init writes:**
- 0x4810 = 1;
- 0x4820-0x4844 = 0 (ten registers, purpose unknown);
- 0x4848 + 4·i = 0xff, one register per rule. It holds the action groups the rule uses: rule add
  writes it with bits 0x01-0x80, one per group.

**Rule entry, five words, TCAM-encoded.**
- **Encoding.** For care bit c and value bit d, the data entry (i) holds ¬(d∧c) and the care entry
  (0x80 + i) holds d∨¬c. Care-entry word 4 bit 21 is the valid bit.
- **Words 0-3.** Eight 16-bit field slots: word n holds slot 2n+1 in bits 31:16 and slot 2n in 15:0.
- **Word 4.** Bits 10:0 hold A: bits 2:0 are the template index, bits 3-10 eight care-tag bits.
  Bits 20:11 hold B, probably the active port mask; still to confirm.

**API structures**, as RTL8367C's `rtk_filter_*`, big-endian, with 3-byte Keil pointers:
- **Rule config.**
  - +0: the field list pointer;
  - +3/+7, +11/+15, +19/+23: care tags 0-2 as (value, mask) pairs of 32-bit words;
  - +0xb7..0xba and +0xbf..0xc2: more care-tag masks (A bits 6-10);
  - +0xab and +0xaf: the active-port value and mask (32-bit). They go through `rtk_switch`'s port
    conversion, which needs `halCtrl->valid_portmask` set. Not done yet;
  - +0xc3: invert.
- **Field node.**
  - +0: the type (u32, 0-42);
  - +4: the data type;
  - then value and mask: a MAC has its value at +8..13 and its mask at +14..19;
  - +0x48: the slot count, +0x4c: slot entries (filled by 0xa33c), +0x6c: next.
- **Type to field-code table** (XDATA 0xa7ab, 8 codes per type; counts at 0xa780). Some codes:

  | Codes | Field |
  |---|---|
  | 00-02 | DMAC |
  | 03-05 | SMAC |
  | 06 | EtherType? |
  | 10-11 / 12-13 | IPv4 SIP / DIP |
  | 14-17 | L3/L4 fields |
  | 20-27 / 30-37 | IPv6 addresses |
  | 40-47 | ranges, VID |
  | 43/44 | tags |

  The order matches the RTL8367C field type list closely.
- **Action struct.** A byte of enable flags per action, then parameters:
  - enables 0-3 select word 0 bits 1:0 (C-VLAN group);
  - enables 4-7 select word 0 bits 19:18 (S-VLAN group);
  - enable 10 (drop?) sets word 1 bit 3;
  - enables 12-19 set word 1 bits 16-20;
  - enable 21 sets word 1 bit 31;
  - enables 22, 27-29 set word 2 bits 4-7;
  - enables 23-26 set word 0 bit 3 and bits 17:16.

  The parameter offsets aren't probed yet.

**Next without hardware:**
1. Set `valid_portmask` (find `halCtrl` in XDATA, from 0xaea9 onward) and probe the active-port bytes.
2. Probe action parameters with each enable set.
3. Probe each field type's value offsets.
4. Write a field-type table for an `acl` command set.

### More emulation results (partial runs, `docs/re/results/`)

**Active port mask.** `emu51.Stock.setup_switch()` sets the HAL control pointer (XDATA 0xb326 → the
static struct at 0xa980, whose `valid_portmask` at +0x135 is 0x3f8). With it, the config's active
port value and mask (+0xab and +0xaf, 32-bit) reach the rule.

- Rule word 4 bits 20:11 (field B) are a 10-bit port mask, in the TCAM encoding like everything else.
- API ports 3, 4, 5 and 9 give bits 7, 8, 3 and 9. That is the stock firmware's own port numbering;
  ports 6-8 come out empty, since this start-up doesn't fully set up its port table.

**Field types** (`results/field-types.txt`, all 43 types, complete run). Byte offsets are in the field node:

| Types | Slots | Value | Mask | Likely field |
|---|---|---|---|---|
| 0, 1 | 3 | 8-13 | 14-19 | MAC (DMAC, SMAC) |
| 2, 15-18, 21-26 | 1 | 10-11 | 14-15 | 16-bit, such as EtherType, L4 ports |
| 3, 4 | 1 | split | split | tags: probably several subfields (VID, priority, CFI), more than one value range |
| 5-8 | 2 | 16-19 | 20-23 | IPv4 address |
| 9, 10, 13, 14, 19, 20 | 1 | 11 | 15 | 8-bit, such as ToS, protocol, next header |
| 11, 12 | 8 | 8-23 | 24-39 | IPv6 address |
| 27-42 | 1 | 10-11 | 14-15 | 16-bit; sixteen of them, probably the user-defined field selectors (RTL8367C has 16) |

Types 34-42 land in slots 6, 7, 2, 3, 4, 5, 6, 7, 7: the slot follows the template the type falls in,
not the type alone.

**Action parameters** (`results/action-params.txt`, all 30 enables).
- **Words 0 and 1 hold indexes.** Each action group's parameter is mostly a 32-bit word in the action
  struct, from +30, with only its low byte significant: larger values are refused (error 1).
- **What each parameter sets:**
  - enables 0-1: a 16-bit value at +30 into action word 0 bits 15:4 (a VLAN or policer index);
  - enables 4-5: +34 into word 0 bits 31:20;
  - enable 3: +52; enable 7: +56; enable 8: +36 into word 1 bit 0;
  - enables 9-10: +40 or +44 into word 1 bits 4+;
  - enables 11-12: +48 or +60 into word 1 bits 10+.
- **Error 4** for enables 13-15 and 19 at +64: probably port masks, which need the port table as well.
- **The rest (complete run):**
  - enable 22: +71, low nibble, into word 2 bits 3:0;
  - enables 16-18, 20-21 and 23-29 take no parameter in +30..+0x6f: their effect is the fixed bits
    the enable alone sets.
