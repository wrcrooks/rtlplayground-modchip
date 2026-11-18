#!/usr/bin/env python3
"""For every field type of the stock ACL API (0-42): which bytes of the field
node are the mask and which the value, and which rule slots they land in.
STOCK_BIN must name the stock flash dump.

  acl_types.py [first type]      resume from a type (a full run takes about an hour)"""
import io, contextlib, os, sys
sys.path.insert(0, os.path.dirname(__file__)); sys.path.insert(0, os.path.dirname(__file__) + '/..')
from emu51 import Stop
with contextlib.redirect_stdout(io.StringIO()):
    from acl_add import run
    from acl_field_offsets import decode, slots


def slotdiff(a, b):
    return {i: ('%04x' % (a[0][i][0] ^ b[0][i][0]), '%04x' % (a[0][i][1] ^ b[0][i][1])) for i in range(8) if a[0][i] != b[0][i]}


for t in range(int(sys.argv[1]) if len(sys.argv) > 1 else 0, 43):
    try:
        r1, r2, node, st = run(t, fill=[0] * 0x6f)
    except Stop:
        print('type %2d: loops' % t); continue
    d = decode(st)
    if not d:
        print('type %2d: no rule (ret %s %s)' % (t, r1, r2)); continue
    base = slots(d)
    nslots = node[3]
    masks = []
    for k in range(4, 0x48):
        f = [0] * 0x6f; f[k] = 0xff
        try:
            r1, r2, node2, st = run(t, fill=f)
        except Stop:
            continue
        d = decode(st)
        if d and slots(d) != base:
            masks.append((k, slotdiff(slots(d), base)))
    mfill = [0] * 0x6f
    for k, _ in masks:
        mfill[k] = 0xff
    r1, r2, node2, st = run(t, fill=mfill)
    d = decode(st)
    mbase = slots(d) if d else base
    values = []
    for k in range(4, 0x48):
        if mfill[k]:
            continue
        f = list(mfill); f[k] = 0xff
        try:
            r1, r2, node2, st = run(t, fill=f)
        except Stop:
            continue
        d = decode(st)
        if d and slots(d) != mbase:
            values.append((k, slotdiff(slots(d), mbase)))
    print('type %2d: %d slot(s); mask bytes %s; value bytes %s' % (
        t, nslots, [k for k, _ in masks], [k for k, _ in values]))
    for k, s in values:
        print('    value[%d] -> %s' % (k, s))
    sys.stdout.flush()
