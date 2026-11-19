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
- 0x4820-0x4844: the five templates (see "On a switch" below; an earlier run without the data init
  read zeros here);
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

## On a switch (2026-11-18)

The MokerLink with the Beetle stand-in, running modchip 76a8bc5. The host is on front port 1 (ASIC
port 4) and pings the switch. `docs/re/hw/acl_hw.py` repeats every check below over the USB
console (`docs/re/hw/sw.py`), and clears the table afterwards.

**State at reset under RTLPlayground.**
- The rule table holds random contents, with no valid bit set anywhere.
- 0x4848-0x49c4 (one per rule) are 0xff; 0x4810-0x4844 are 0.

**Registers.**

| Register | Role | Confirmed by |
|---|---|---|
| 0x4818 | ACL enable, a bit per ASIC port (bit 9 is the CPU port) | with no valid rule, the port drops everything |
| 0x481c | unmatched frames pass, a bit per port | with it set, an empty table passes |
| 0x4820-0x4844 | templates: 8 field codes each, 2 registers per template | field rules match once written |
| 0x4848 + 4·i | rule i's action groups (bit per group) | only the groups set apply |
| 0x4810 | reads back 0 after a write of 1; no effect seen | |

- **Stock's init sets 0x4818 and 0x481c to 0x1f8 (ports 3-8).** That means ACL on and unmatched
  frames passing on every front port. The CPU port isn't included.
- **Templates.** Template t uses registers 0x4824 + 8t (slots 0-3, slot 0 in the low byte) and
  0x4820 + 8t (slots 4-7). Stock's codes, from XDATA 0xa903:

  | Template | Slots 0-7 |
  |---|---|
  | 0 | 00 01 02 03 04 05 06 4f |
  | 1 | 10 11 12 13 34 35 36 40 |
  | 2 | 12 13 43 44 45 46 47 48 |
  | 3 | 10 11 49 4a 4b 4c 4d 4e |
  | 4 | 30 31 32 08 07 41 42 33 |

  RTLPlayground leaves them at 0, so every slot is field 00 (DMAC bits 15:0) until something
  writes them.

**Rules from the emulated stock driver match on the chip** (rule 0, drop on match):

| Rule | Ping | HTTP | So |
|---|---|---|---|
| none | passes | passes | |
| everything (template 0, no cares) | dropped | dropped | |
| DMAC = the switch's | dropped | dropped | DMAC in template 0 slots 0-2, slot 0 the low bits |
| DMAC = another | passes | passes | |
| SMAC = the host's | dropped | dropped | SMAC in slots 3-5 |
| type 2 = 0x0800 | dropped | dropped | type 2 is the EtherType (slot 7 of template 0) |
| types 10, 14 = 1 | dropped | passes | IPv4 protocol / IPv6 next header: field 0x34 low byte |
| types 9, 13 = 1 | passes | passes | ToS / traffic class (0 here): field 0x34 high byte |
| types 19, 20 = 1 | passes | passes | field 0x35 high and low byte (TCP flags?): not identified |

**Action group 5, forwarding** (action word 1). Bits 20:17 are a code, and bits 30:21 a port mask,
1 << (21 + ASIC port). The emulator puts stock's port masks there too.

| Code | Effect (second host on front port 4, ASIC 7: its counters and IPv6 ping) |
|---|---|
| 0, 2 | forward as usual, and copy to the mask's ports (stock enables 13 and 15) |
| 1 | redirect: only to the mask's ports; an empty mask drops (stock enable 14) |
| 3, 4, 5 | trap to the CPU, mask ignored; RTLPlayground doesn't answer trapped frames |
| 6, 7 | forward as usual, mask ignored |
| 8-15 | as code 1 here; bit 20 is a flag of its own (stock enable 19) |

What tells 0 from 2, and 3 from 4 and 5, doesn't show in these tests (perhaps VLAN or egress
filtering, or the CPU queue).

- **Groups 0 and 4 with all-zero parameters drop as well.** That fits a VLAN or policer index of 0.
  So a rule should only set the groups it uses in 0x4848, as stock does.

**Rule word 4 bits 20:11 are the ingress port, one bit per ASIC port** (bit 11 + port, the CPU
at 9). A frame sets its own port's bit, so "any of these ports" is "every other port's bit is 0".
Setting the members' bits to 1, as the emulated stock driver does, matches one port only; two or more
match nothing.

**Template 1 on the chip:**
- slots 0-1: source IPv4 address, low half first;
- slots 2-3: destination IPv4 address;
- slot 4: ToS (high byte) and protocol (low byte);
- slot 5: the L4 source port, and the ICMP type in its high byte;
- slot 6: the L4 destination port.

**Priority.** Within an action group, the lowest numbered matching rule wins. A rule without the
group (0x4848 bit clear) doesn't stop a later rule's.

DrDoof's [logicog/RTLPlayground#520](https://github.com/logicog/RTLPlayground/pull/520) had worked
out the same engine independently, on a SWTGW218AS, and goes further: every action group, range
entries, meters, counters and field selectors. Its `doc/acl.md` agrees with everything above. The
mod chip now uses that module instead of a command set of its own; see `firmware/rp2350/README.md`.

**Forwarding codes, as #520 names them:** 0 copy, 1 redirect, 2 mirror, 3 trap to the internal CPU,
4 to the external one, 5 to both. That matches what was measured above. 6 and 7 aren't used there.
