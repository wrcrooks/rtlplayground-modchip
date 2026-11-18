import sys, io, contextlib
sys.path.insert(0, __import__('os').path.dirname(__file__) + '/..')
sys.path.insert(0, __import__('os').path.dirname(__file__))
with contextlib.redirect_stdout(io.StringIO()):
    from acl_add import run
    from acl_field_offsets import decode, slots
fill = [0] * 0x6f; fill[14:20] = [0xff] * 6
def snap(st):
    s = slots(decode(st))
    act = st.tables.get((2, 3))
    regs = {a: v for a, v in st.writes if not (0x5cac <= a <= 0x5cc8)}
    return s[1], s[2], tuple(act) if act else None, regs
r1, r2, node, st = run(0, fill=fill)
base = snap(st)
print('base A %s B %s act %s regs %s' % (base[0], base[1], [hex(x) for x in base[2]], {hex(a): hex(v) for a, v in base[3].items()}))
for k in range(0, 0x60):
    for v in (0x01, 0xff):
        act = [0] * 0x100; act[k] = v
        r1, r2, node, st = run(0, fill=fill, act=act)
        if decode(st) is None: print('act[%d]=%02x: no rule, ret %s %s' % (k, v, r1, r2)); continue
        s = snap(st)
        if s != base:
            print('act[%2d]=%02x: A %s B %s act %s regs %s' % (k, v, s[0], s[1],
                  [hex(x) for x in s[2]] if s[2] != base[2] else '-',
                  {hex(a): hex(x) for a, x in s[3].items() if base[3].get(a) != x}))
