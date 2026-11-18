import sys
sys.path.insert(0, __import__('os').path.dirname(__file__) + '/..')
from emu51 import Stock
F = __import__('os').environ['STOCK_BIN']
BUF = 0x7000
def run(buf, idx=5):
    st = Stock(F)
    st.cpu.xdata[BUF:BUF + len(buf)] = bytes(buf)
    ret = st.call(14, 0x9aa6, {4: 0, 5: 0, 6: 0, 7: idx, 3: 1, 2: BUF >> 8, 1: BUF & 0xff})
    return ret, st.tables.get((1, idx)), st.tables.get((1, idx | 0x80))
def bits(words): return sum(w << (32 * (4 - i)) for i, w in enumerate(words))
care = [0] * 22 + [0xff] * 24 + [0] * 18
base = run(care)
print('base', [hex(x) for x in base[1]], [hex(x) for x in base[2]])
for off in list(range(0, 22)) + [26, 27] + list(range(46, 64)):
    for b in range(8):
        buf = list(care); buf[off] ^= 1 << b
        r = run(buf)
        if r != base:
            dd = bits(r[1]) ^ bits(base[1]); dc = bits(r[2]) ^ bits(base[2])
            print('in[%2d].%d data %s care %s' % (off, b, [i for i in range(160) if dd >> i & 1], [i for i in range(160) if dc >> i & 1]))
