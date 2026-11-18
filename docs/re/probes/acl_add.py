import sys
sys.path.insert(0, __import__('os').path.dirname(__file__) + '/..')
from emu51 import Stock, Stop
F = __import__('os').environ['STOCK_BIN']
CFG, FLD, ACT, NUM = 0x7000, 0x7100, 0x7200, 0x7400
def gp(a): return bytes([1, a >> 8, a & 0xff])
def run(ftype, fill=None, cfg=None, idx=3, act=None):
    st = Stock(F); x = st.cpu.xdata
    st.init_data()
    x[0xb325] = 1
    st.call(14, 0xa1b4)                 # ACL init, like the stock boot
    st.writes.clear(); st.tables.clear()
    x[CFG:CFG + 0x100] = bytes(cfg or [0] * 0x100)
    f = bytearray(fill if fill else [k & 0xff for k in range(0x6f)])
    f[0:4] = ftype.to_bytes(4, 'big')
    f[0x48:0x6f] = bytes(0x6f - 0x48)       # count, entries, next: the add-field call fills them
    x[FLD:FLD + 0x6f] = bytes(f)
    x[ACT:ACT + 0x100] = bytes(act or [0] * 0x100)
    x[0x8fe1:0x8fe4] = gp(FLD)
    r1 = st.call(14, 0xa33c, {3: 1, 2: CFG >> 8, 1: CFG & 0xff})
    node = bytes(x[FLD + 0x48:FLD + 0x6f])
    x[0x8fec:0x8fef] = gp(ACT); x[0x8fef:0x8ff2] = gp(NUM)
    r2 = st.call(14, 0xa51a, {4: 0, 5: 0, 6: 0, 7: idx, 3: 1, 2: CFG >> 8, 1: CFG & 0xff}, max_steps=10_000_000)
    return r1, r2, node, st
if __name__ == '__main__':
    for t in [0]:
        r1, r2, node, st = run(t)
        print('type', t, 'field_add', r1, 'rule_add', r2, 'node', node[:24].hex(' '))
        print('cfg head', st.cpu.xdata[CFG:CFG+8].hex(' '), 'num', st.cpu.xdata[NUM:NUM+4].hex())
        for a, v in st.writes: print('   %04x %08x' % (a, v))
