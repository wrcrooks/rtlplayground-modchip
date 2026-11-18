import sys
sys.path.insert(0, __import__('os').path.dirname(__file__) + '/..')
sys.path.insert(0, __import__('os').path.dirname(__file__))
from acl_add import run
def decode(st, idx=3):
    X = st.tables.get((1, idx)); Y = st.tables.get((1, idx | 0x80))
    if not X or not Y: return None
    out = []
    for w in range(5):
        x, y = X[w], Y[w]
        c = (~x | ~y) & 0xffffffff     # c & d = ~x ; c & ~d = ~y
        d = (~x) & 0xffffffff
        out.append((d, c))
    return out
def slots(dec):
    # slot n: word n//2 ... order from the low-level probe: word0 = f1:f0, word1 = f3:f2, word2 = f5:f4, word3 = f7:f6
    s = []
    for w in range(4):
        d, c = dec[w]
        s += [(d & 0xffff, c & 0xffff), (d >> 16, c >> 16)]
    d, c = dec[4]
    return s, (d & 0x7ff, c & 0x7ff), ((d >> 11) & 0x3ff, (c >> 11) & 0x3ff)
t = int(sys.argv[1]) if len(sys.argv) > 1 else 0
BASEFILL = [0] * 0x6f
if len(sys.argv) > 2:
    for a in sys.argv[2].split(','):
        lo, hi = a.split('-'); BASEFILL[int(lo):int(hi) + 1] = [0xff] * (int(hi) - int(lo) + 1)
r1, r2, node, st = run(t, fill=BASEFILL)
base = slots(decode(st))
print('type', t, 'entries', node.hex(' ')[:60], 'base A', base[1], 'B', base[2])
for k in range(4, 0x48):
    fill = list(BASEFILL); fill[k] ^= 0xff
    r1, r2, node, st = run(t, fill=fill)
    dec = decode(st)
    if not dec: print(k, 'no rule', r1, r2); continue
    s = slots(dec)
    if s != base:
        diff = [(i, '%04x/%04x' % s[0][i]) for i in range(8) if s[0][i] != base[0][i]]
        print('offset %2d: slots %s A %s B %s' % (k, diff, s[1] if s[1] != base[1] else '-', s[2] if s[2] != base[2] else '-'))
