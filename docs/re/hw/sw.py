import serial, time, re
class Con:
    def __init__(s, dev='/dev/ttyACM1'):
        s.s = serial.Serial(dev, 115200, timeout=0.05); s.s.read(65536)
    def cmd(s, c, wait=3.0):
        s.s.read(65536)
        s.s.write((c + '\r\n').encode())
        out = b''; end = time.time() + wait
        while time.time() < end:
            out += s.s.read(4096)
            if out.rstrip().endswith(b'>') and out.count(b'\n') > 1:
                break
        t = out.decode(errors='replace').replace('\r', '')
        lines = [l for l in t.split('\n') if l.strip() and l.strip() != '>' and l.strip() != c]
        return '\n'.join(lines)
    def reg(s, a):
        m = re.search(r'VAL: 0x([0-9a-fA-F]+)', s.cmd('regget %04x' % a))
        return int(m.group(1), 16) if m else None
    def regset(s, a, v):
        return s.cmd('regset %04x %08x' % (a, v))
    def rule(s, i):
        t = s.cmd('acl rule %d' % i)
        d = re.search(r'data ([0-9a-f ]+)', t); c = re.search(r'care ([0-9a-f ]+)', t)
        return [int(x, 16) for x in d.group(1).split()], [int(x, 16) for x in c.group(1).split()]
    def action(s, i):
        t = s.cmd('acl action %d' % i)
        return [int(x, 16) for x in re.search(r'action ([0-9a-f ]+)', t).group(1).split()]
