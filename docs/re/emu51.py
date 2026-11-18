#!/usr/bin/env python3
"""A plain 8051 emulator for running functions of the stock RTL8372/8373
firmware on the host, with the chip's register engine and table engine
modelled, so that what a function writes to the switch can be observed.

    from emu51 import Stock
    st = Stock("stock.bin")
    st.call(bank=14, addr=0x9aa6, r=..., xdata=...)   # see acl_probe.py

Banks: the stock firmware's per-bank stubs (code 0x1100) save PSBANK with
"mov 0xbb, 0x96" and restore it with "mov 0x96, 0xbb", around nested calls
too, so SFR 0xbb has to be a stack: a write pushes, a read pops.

The register engine is the one RTLPlayground uses (rtl837x_sfr.h): address in
SFR 0xa2/0xa3, data in 0xa4-0xa7 (most significant first), command in 0xa0
(1 read, 3 write), busy in 0xa1. Writes to TBL_CTRL (0x5cac) with the execute
bit run the table operation on a model of the tables: DATA_IN 0x5cb8.., DATA_OUT
0x5ccc.., five words.
"""

HDR = 2
BANK = 0xC000

TBL_CTRL, DATA_IN, DATA_OUT = 0x5CAC, 0x5CB8, 0x5CCC


class Stop(Exception):
    pass


class CPU:
    def __init__(self, fetch):
        self.fetch = fetch
        self.iram = bytearray(256)
        self.sfr = bytearray(256)
        self.xdata = bytearray(65536)
        self.pc = 0
        self.sfr[0x81] = 0x07
        self.on_sfr_write = None
        self.on_sfr_read = None
        self.steps = 0

    # ---- registers and memory ----
    @property
    def a(self): return self.sfr[0xE0]
    @a.setter
    def a(self, v): self.sfr[0xE0] = v & 0xFF
    @property
    def psw(self): return self.sfr[0xD0]
    @property
    def c(self): return self.sfr[0xD0] >> 7
    @c.setter
    def c(self, v): self.sfr[0xD0] = (self.sfr[0xD0] & 0x7F) | (0x80 if v else 0)
    @property
    def dptr(self): return self.sfr[0x83] << 8 | self.sfr[0x82]
    @dptr.setter
    def dptr(self, v):
        self.sfr[0x83] = (v >> 8) & 0xFF
        self.sfr[0x82] = v & 0xFF

    def rn_addr(self, n): return ((self.psw >> 3) & 3) * 8 + n
    def r(self, n): return self.iram[self.rn_addr(n)]
    def set_r(self, n, v): self.iram[self.rn_addr(n)] = v & 0xFF

    def rd(self, d):
        if d < 0x80:
            return self.iram[d]
        if self.on_sfr_read:
            v = self.on_sfr_read(d)
            if v is not None:
                return v
        return self.sfr[d]

    def wd(self, d, v):
        v &= 0xFF
        if d < 0x80:
            self.iram[d] = v
        else:
            self.sfr[d] = v
            if self.on_sfr_write:
                self.on_sfr_write(d, v)

    def rbit(self, b):
        if b < 0x80:
            return (self.iram[0x20 + (b >> 3)] >> (b & 7)) & 1
        return (self.rd(b & 0xF8) >> (b & 7)) & 1

    def wbit(self, b, v):
        if b < 0x80:
            a = 0x20 + (b >> 3)
            self.iram[a] = (self.iram[a] & ~(1 << (b & 7))) | ((1 if v else 0) << (b & 7))
        else:
            a = b & 0xF8
            self.wd(a, (self.rd(a) & ~(1 << (b & 7))) | ((1 if v else 0) << (b & 7)))

    def push(self, v):
        sp = (self.sfr[0x81] + 1) & 0xFF
        self.sfr[0x81] = sp
        self.iram[sp] = v & 0xFF

    def pop(self):
        sp = self.sfr[0x81]
        v = self.iram[sp]
        self.sfr[0x81] = (sp - 1) & 0xFF
        return v

    def code(self, a): return self.fetch(a & 0xFFFF)

    def next(self):
        v = self.code(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def rel(self):
        v = self.next()
        return v - 256 if v > 127 else v

    def flags_add(self, a, b, cin):
        r = a + b + cin
        cy = r > 0xFF
        ac = ((a & 0xF) + (b & 0xF) + cin) > 0xF
        ov = ((a ^ r) & (b ^ r) & 0x80) != 0
        p = self.sfr[0xD0] & ~0xC4
        self.sfr[0xD0] = p | (0x80 if cy else 0) | (0x40 if ac else 0) | (0x04 if ov else 0)
        return r & 0xFF

    def flags_sub(self, a, b, cin):
        r = a - b - cin
        cy = r < 0
        ac = ((a & 0xF) - (b & 0xF) - cin) < 0
        ov = ((a ^ b) & (a ^ r) & 0x80) != 0
        p = self.sfr[0xD0] & ~0xC4
        self.sfr[0xD0] = p | (0x80 if cy else 0) | (0x40 if ac else 0) | (0x04 if ov else 0)
        return r & 0xFF

    def ind(self, i): return self.r(i)          # @R0 / @R1 address

    # operand for opcodes with the low nibble 4..f: #imm, dir, @R0, @R1, R0-R7
    def src(self, op):
        lo = op & 0xF
        if lo == 4: return self.next()
        if lo == 5: return self.rd(self.next())
        if lo in (6, 7): return self.iram[self.ind(lo - 6)]
        return self.r(lo - 8)

    def step(self):
        self.steps += 1
        op = self.next()
        hi, lo = op >> 4, op & 0xF
        if lo == 1:                                  # AJMP / ACALL
            a = ((self.pc & 0xF800) | ((op >> 5) << 8) | self.next())
            if op & 0x10:
                self.push(self.pc & 0xFF)
                self.push(self.pc >> 8)
            self.pc = a
            return
        if op == 0x00: return
        if op == 0x02: self.pc = self.next() << 8 | self.next(); return
        if op == 0x12:
            a = self.next() << 8 | self.next()
            self.push(self.pc & 0xFF); self.push(self.pc >> 8); self.pc = a; return
        if op in (0x22, 0x32):
            h = self.pop(); l = self.pop(); self.pc = h << 8 | l; return
        if op == 0x03: self.a = (self.a >> 1) | ((self.a & 1) << 7); return
        if op == 0x13: c = self.c; self.c = self.a & 1; self.a = (self.a >> 1) | (c << 7); return
        if op == 0x23: self.a = (self.a << 1) | (self.a >> 7); return
        if op == 0x33: c = self.c; self.c = self.a >> 7; self.a = (self.a << 1) | c; return
        if hi in (0, 1) and lo >= 4:                 # INC / DEC
            d = 1 if hi == 0 else -1
            if lo == 4: self.a = self.a + d
            elif lo == 5: x = self.next(); self.wd(x, self.rd(x) + d)
            elif lo in (6, 7): x = self.ind(lo - 6); self.iram[x] = (self.iram[x] + d) & 0xFF
            else: self.set_r(lo - 8, self.r(lo - 8) + d)
            return
        if op in (0x10, 0x20, 0x30):                 # JBC, JB, JNB
            b = self.next(); o = self.rel(); v = self.rbit(b)
            if op == 0x10:
                if v: self.wbit(b, 0); self.pc = (self.pc + o) & 0xFFFF
            elif (op == 0x20) == bool(v):
                self.pc = (self.pc + o) & 0xFFFF
            return
        if hi in (2, 3) and lo >= 4:                 # ADD / ADDC
            self.a = self.flags_add(self.a, self.src(op), self.c if hi == 3 else 0); return
        if op in (0x40, 0x50, 0x60, 0x70, 0x80):
            o = self.rel()
            t = {0x40: self.c, 0x50: not self.c, 0x60: self.a == 0, 0x70: self.a != 0, 0x80: True}[op]
            if t: self.pc = (self.pc + o) & 0xFFFF
            return
        if hi in (4, 5, 6) and lo >= 2:              # ORL / ANL / XRL
            f = {4: lambda x, y: x | y, 5: lambda x, y: x & y, 6: lambda x, y: x ^ y}[hi]
            if lo == 2: d = self.next(); self.wd(d, f(self.rd(d), self.a)); return
            if lo == 3: d = self.next(); i = self.next(); self.wd(d, f(self.rd(d), i)); return
            self.a = f(self.a, self.src(op)); return
        if op == 0x72: self.c = self.c | self.rbit(self.next()); return
        if op == 0x82: self.c = self.c & self.rbit(self.next()); return
        if op == 0xA0: self.c = self.c | (1 - self.rbit(self.next())); return
        if op == 0xB0: self.c = self.c & (1 - self.rbit(self.next())); return
        if op == 0x73: self.pc = (self.a + self.dptr) & 0xFFFF; return
        if op == 0x74: self.a = self.next(); return
        if op == 0x75: d = self.next(); self.wd(d, self.next()); return
        if op in (0x76, 0x77): self.iram[self.ind(op - 0x76)] = self.next(); return
        if 0x78 <= op <= 0x7F: self.set_r(op - 0x78, self.next()); return
        if op == 0x83: self.a = self.code((self.a + self.pc) & 0xFFFF); return
        if op == 0x93: self.a = self.code((self.a + self.dptr) & 0xFFFF); return
        if op == 0x84:
            b = self.sfr[0xF0]
            if b == 0:
                self.sfr[0xD0] |= 0x04; self.c = 0
            else:
                q, m = divmod(self.a, b); self.a = q; self.sfr[0xF0] = m
                self.sfr[0xD0] &= ~0x04; self.c = 0
            return
        if op == 0xA4:
            r = self.a * self.sfr[0xF0]; self.a = r & 0xFF; self.sfr[0xF0] = r >> 8
            self.sfr[0xD0] = (self.sfr[0xD0] & ~0x04) | (0x04 if r > 0xFF else 0); self.c = 0
            return
        if op == 0x85: s = self.next(); d = self.next(); self.wd(d, self.rd(s)); return
        if op in (0x86, 0x87): d = self.next(); self.wd(d, self.iram[self.ind(op - 0x86)]); return
        if 0x88 <= op <= 0x8F: d = self.next(); self.wd(d, self.r(op - 0x88)); return
        if op == 0x90: self.dptr = self.next() << 8 | self.next(); return
        if op == 0x92: self.wbit(self.next(), self.c); return
        if op == 0xA2: self.c = self.rbit(self.next()); return
        if hi == 9 and lo >= 4: self.a = self.flags_sub(self.a, self.src(op), self.c); return
        if op == 0xA3: self.dptr = (self.dptr + 1) & 0xFFFF; return
        if op in (0xA6, 0xA7): self.iram[self.ind(op - 0xA6)] = self.rd(self.next()); return
        if 0xA8 <= op <= 0xAF: self.set_r(op - 0xA8, self.rd(self.next())); return
        if op == 0xB2: b = self.next(); self.wbit(b, 1 - self.rbit(b)); return
        if op == 0xB3: self.c = 1 - self.c; return
        if hi == 0xB and lo >= 4:                    # CJNE
            if lo == 4: x = self.a; y = self.next()
            elif lo == 5: x = self.a; y = self.rd(self.next())
            elif lo in (6, 7): x = self.iram[self.ind(lo - 6)]; y = self.next()
            else: x = self.r(lo - 8); y = self.next()
            o = self.rel(); self.c = x < y
            if x != y: self.pc = (self.pc + o) & 0xFFFF
            return
        if op == 0xC0: self.push(self.rd(self.next())); return
        if op == 0xD0: d = self.next(); self.wd(d, self.pop()); return
        if op == 0xC2: self.wbit(self.next(), 0); return
        if op == 0xC3: self.c = 0; return
        if op == 0xD2: self.wbit(self.next(), 1); return
        if op == 0xD3: self.c = 1; return
        if op == 0xC4: self.a = (self.a >> 4) | (self.a << 4); return
        if op == 0xC5: d = self.next(); t = self.rd(d); self.wd(d, self.a); self.a = t; return
        if op in (0xC6, 0xC7): x = self.ind(op - 0xC6); t = self.iram[x]; self.iram[x] = self.a; self.a = t; return
        if 0xC8 <= op <= 0xCF: t = self.r(op - 0xC8); self.set_r(op - 0xC8, self.a); self.a = t; return
        if op == 0xD4:
            a = self.a
            if (a & 0xF) > 9 or (self.psw & 0x40): a += 6
            if (a >> 4) > 9 or self.c or a > 0xFF: a += 0x60; self.c = 1
            self.a = a; return
        if op == 0xD5:
            d = self.next(); o = self.rel(); v = (self.rd(d) - 1) & 0xFF; self.wd(d, v)
            if v: self.pc = (self.pc + o) & 0xFFFF
            return
        if op in (0xD6, 0xD7):
            x = self.ind(op - 0xD6); t = self.iram[x]
            self.iram[x] = (t & 0xF0) | (self.a & 0xF); self.a = (self.a & 0xF0) | (t & 0xF); return
        if 0xD8 <= op <= 0xDF:
            o = self.rel(); v = (self.r(op - 0xD8) - 1) & 0xFF; self.set_r(op - 0xD8, v)
            if v: self.pc = (self.pc + o) & 0xFFFF
            return
        if op == 0xE0: self.a = self.xdata[self.dptr]; return
        if op in (0xE2, 0xE3): self.a = self.xdata[self.sfr[0xA0] << 8 | self.r(op - 0xE2)]; return
        if op == 0xF0: self.xdata[self.dptr] = self.a; return
        if op in (0xF2, 0xF3): self.xdata[self.sfr[0xA0] << 8 | self.r(op - 0xF2)] = self.a; return
        if op == 0xE4: self.a = 0; return
        if op == 0xE5: self.a = self.rd(self.next()); return
        if op in (0xE6, 0xE7): self.a = self.iram[self.ind(op - 0xE6)]; return
        if 0xE8 <= op <= 0xEF: self.a = self.r(op - 0xE8); return
        if op == 0xF4: self.a = ~self.a; return
        if op == 0xF5: self.wd(self.next(), self.a); return
        if op in (0xF6, 0xF7): self.iram[self.ind(op - 0xF6)] = self.a; return
        if 0xF8 <= op <= 0xFF: self.set_r(op - 0xF8, self.a); return
        raise Stop("unknown opcode 0x%02x at 0x%04x" % (op, (self.pc - 1) & 0xFFFF))


class Stock:
    """The stock image, its banks, and the switch's register file"""

    def __init__(self, path):
        self.d = open(path, "rb").read()
        self.regs = {}
        self.writes = []          # (reg, value) in order
        self.tables = {}          # (type, entry) -> [5 words]
        self.cpu = CPU(self.fetch)
        self.cpu.on_sfr_write = self.sfr_write
        self.cpu.on_sfr_read = self.sfr_read
        self.bank_stack = []
        self.uart = bytearray()

    def fetch(self, a):
        if a < 0x4000:
            return self.d[a + HDR]
        n = self.cpu.sfr[0x96]
        return self.d[(n - 1) * BANK + a] if n else 0

    def sfr_read(self, d):
        if d == 0xA1:
            return 0              # the register engine is never busy
        if d == 0xBB:
            return self.bank_stack.pop() if self.bank_stack else 0
        if d == 0x98:
            return self.cpu.sfr[0x98] | 0x02     # UART: transmit always done
        return None

    def sfr_write(self, d, v):
        if d == 0xBB:
            self.bank_stack.append(v)
            return
        if d == 0x99:
            self.uart.append(v)                  # what the firmware prints
            return
        if d != 0xA0 or v not in (1, 3):
            return
        s = self.cpu.sfr
        reg = s[0xA2] << 8 | s[0xA3]
        if v == 3:
            val = s[0xA4] << 24 | s[0xA5] << 16 | s[0xA6] << 8 | s[0xA7]
            self.reg_write(reg, val)
        else:
            val = self.regs.get(reg, 0)
            s[0xA4], s[0xA5], s[0xA6], s[0xA7] = val >> 24, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF
        s[0xA0] = 0

    def reg_write(self, reg, val):
        self.writes.append((reg, val))
        self.regs[reg] = val
        if reg == TBL_CTRL and val & 1:
            entry, typ, write = val >> 16, (val >> 8) & 0xFF, val & 2
            if write:
                self.tables[(typ, entry)] = [self.regs.get(DATA_IN + 4 * i, 0) for i in range(5)]
            else:
                w = self.tables.get((typ, entry), [0] * 5)
                for i in range(5):
                    self.regs[DATA_OUT + 4 * i] = w[i]
            self.regs[reg] = val & ~1

    def init_data(self, start=0x1016, done=0x1031, max_steps=5_000_000):
        """Run Keil's startup data initialisation (the C_INITSEG records), so
        XDATA holds the firmware's initialised variables and tables.

        The image holds two programs: a boot stage with its reset code at 0x0000
        (PSBANK 1, records at bank 1 0x4137, done at 0x0ddf) and the application
        with its own at 0x1000 (PSBANK 3, records at bank 3 0x63ed, done at
        0x1031). The defaults run the application's."""
        c = self.cpu
        c.sfr[0x81] = 0x7F
        c.pc = start
        n = 0
        while c.pc != done:
            c.step()
            n += 1
            if n > max_steps:
                raise Stop("data initialisation didn't finish, pc 0x%04x" % c.pc)
        return n

    def call(self, bank, addr, regs=None, max_steps=2_000_000):
        """Run the function at bank:addr until it returns; regs: {n: value} for R0-R7"""
        c = self.cpu
        c.sfr[0x96] = bank
        c.sfr[0xD0] = 0
        c.sfr[0x81] = 0x80
        for n, v in (regs or {}).items():
            c.set_r(n, v)
        c.push(0xFE); c.push(0xFF)              # return to 0xfffe: the end
        c.pc = addr
        start = c.steps
        while c.pc != 0xFFFE:
            c.step()
            if c.steps - start > max_steps:
                raise Stop("no return after %d steps, pc 0x%04x bank %d" % (max_steps, c.pc, c.sfr[0x96]))
        return c.r(6) << 8 | c.r(7)
