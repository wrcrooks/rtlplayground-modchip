#!/usr/bin/env python3
"""Helpers for reading Realtek's stock RTL8372/RTL8373 firmware (Keil C51,
code banking), as used for docs/acl-reverse-engineering.md.

  stock_re.py banks  STOCK.bin OUTDIR     one 64 KiB image per bank, for r2 -a 8051
  stock_re.py tramp  STOCK.bin            the bank-switch trampolines: code address -> bank, target
  stock_re.py tbl    STOCK.bin            every TBL_CTRL (0x5cac) access: bank, address, table type, command
  stock_re.py where  STOCK.bin OFFSET     file offset -> bank and code address

The flash image is laid out as RTLPlayground's tools/imagebuilder.c writes it:
a 2-byte header, the common area (code 0x0000-0x3fff) at file offset 2, and
bank n's window (code 0x4000-0xffff) at file offset (n - 1) * 0xc000 + 0x4000.
The stock firmware switches banks with SFR 0x96 (PSBANK) in a table of
stubs at code 0x1100, one per bank; callers reach a banked function through
a trampoline "mov dptr, #target; ljmp stub".
Nothing from the stock image is part of this repository: run it on your own
switch's flash dump.
"""
import re
import subprocess
import sys

HDR = 2
BANK = 0xC000


def where(off):
    if off < 0x4000 + HDR:
        return 0, off - HDR
    n = (off - 0x4000) // BANK + 1
    return n, off - (n - 1) * BANK


def file_of(bank, addr):
    return addr + HDR if bank == 0 or addr < 0x4000 else (bank - 1) * BANK + addr


def banks(d, out):
    n = 1
    while (n - 1) * BANK + 0x4000 + BANK <= len(d):
        o = (n - 1) * BANK + 0x4000
        with open("%s/bank%02d.bin" % (out, n), "wb") as f:
            f.write(d[HDR:0x4000 + HDR] + d[o:o + BANK])
        n += 1
    print("%d banks" % (n - 1))


def stubs(d):
    """Code address of each bank's switch stub -> bank number"""
    m = {}
    for a in range(0x1000, 0x1400):
        b = d[a + HDR:a + HDR + 12]
        # mov 0xbb, 0x96 ; mov 0x96, #n ; acall ... ; mov 0x96, 0xbb ; ret
        if len(b) == 12 and b[0:3] == b"\x85\x96\xbb" and b[3:5] == b"\x75\x96" and b[8:12] == b"\x85\xbb\x96\x22":
            m[a] = b[5]
    return m


def tramp(d):
    st = stubs(d)
    for m in re.finditer(rb"\x90(..)\x02(..)", d[HDR:0x4000 + HDR], re.S):
        tgt = m.group(1)[0] << 8 | m.group(1)[1]
        stub = m.group(2)[0] << 8 | m.group(2)[1]
        if stub in st:
            print("0x%04x -> bank %2d 0x%04x" % (m.start(), st[stub], tgt))


def tbl(d):
    import tempfile
    for m in re.finditer(rb"\x7f\xac\x7e\x5c", d):
        n, a = where(m.start())
        if n == 0:
            continue
        with tempfile.NamedTemporaryFile(suffix=".bin") as t:
            t.write(d[HDR:0x4000 + HDR] + d[(n - 1) * BANK + 0x4000:(n - 1) * BANK + 0x4000 + BANK])
            t.flush()
            r2 = subprocess.run(["r2", "-q", "-a", "8051", "-e", "scr.color=0", "-e", "asm.bytes=false",
                                 "-e", "asm.lines=false", "-e", "asm.comments=false",
                                 "-c", "s %d; pd 45" % a, t.name], capture_output=True).stdout.decode()
        ors = [ln.split("#")[1].strip() for ln in r2.splitlines() if "orl a, #" in ln]
        print("bank %2d 0x%04x  orl %s" % (n, a, " ".join(ors[:6])))


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    if cmd == "banks":
        banks(d, sys.argv[3])
    elif cmd == "tramp":
        tramp(d)
    elif cmd == "tbl":
        tbl(d)
    elif cmd == "where":
        n, a = where(int(sys.argv[3], 0))
        print("bank %d, code 0x%04x" % (n, a))


if __name__ == "__main__":
    main()
