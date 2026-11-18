#!/usr/bin/env python3
"""ACL checks on a switch running the mod chip firmware, over its USB console
(docs/re/hw/sw.py). The host is on front port 1 (ASIC port 4) and pings the
switch. Each test installs rule 0, drops on match, and checks ping (ICMP) and
HTTP (TCP); everything is cleared afterwards. Rules come from the emulated
stock driver (docs/re/probes/acl_add.py).

  acl_hw.py [switch IP] [console]
"""
import re
import subprocess
import sys
import time

from sw import Con

IP = sys.argv[1] if len(sys.argv) > 1 else '192.168.10.247'
c = Con(sys.argv[2] if len(sys.argv) > 2 else '/dev/ttyACM1')
HOST_PORT = 4

# The stock init's templates (XDATA 0xa903): per template, slots 0-3 then 4-7
TEMPLATES = {0x4824: 0x03020100, 0x4820: 0x4f060504, 0x482c: 0x13121110, 0x4828: 0x40363534,
             0x4834: 0x44431312, 0x4830: 0x48474645, 0x483c: 0x4a491110, 0x4838: 0x4e4d4c4b,
             0x4844: 0x08323130, 0x4840: 0x33424107}
ANY = 'ffffffff ' * 4
RULES = {
    'match all': ANY + 'ffffffff ' + ANY + 'fffffff8',
    'dmac switch': 'ff22c4c1 fffff9f0 ffffffff ffffffff ffffffff 00dd3b3e ffff060f ffffffff ffffffff fffffff8',
    'smac host': 'ffffffff 2d24ffff 9391f8e0 ffffffff ffffffff ffffffff d2dbffff 6c6e071f ffffffff fffffff8',
    'ethertype 0800': 'ffffffff ffffffff ffffffff fffff7ff ffffffff ffffffff ffffffff ffffffff ffff0800 fffffff8',
    'ip protocol 1': 'ffffffff ffffffff fffffffe ffffffff fffffffe ffffffff ffffffff ffffff01 ffffffff fffffff9',
    'ip tos 1': 'ffffffff ffffffff fffffeff ffffffff fffffffe ffffffff ffffffff ffff01ff ffffffff fffffff9',
}


def ping(n=6):
    r = subprocess.run(['ping', '-c%d' % n, '-i0.2', '-W1', '-q', IP], capture_output=True, text=True).stdout
    m = re.search(r'(\d+) received', r)
    return int(m.group(1)) if m else -1


def http():
    r = subprocess.run(['curl', '-s', '-o', '/dev/null', '-w', '%{http_code}', '-m', '4', 'http://%s/' % IP],
                       capture_output=True, text=True)
    return r.stdout or 'fail'


def frames_rx():
    return int(re.search(r'frames rx (\d+)', c.cmd('bridge')).group(1))


def clear():
    c.cmd('acl rule 0 ' + '00000000 ' * 10)
    c.regset(0x4818, 0)
    c.regset(0x481c, 0)
    c.regset(0x4848, 0xff)


for a, v in TEMPLATES.items():
    c.regset(a, v)
c.regset(0x481c, 1 << HOST_PORT)          # unmatched frames pass
c.regset(0x4818, 1 << HOST_PORT)          # ACL on the host's port
c.regset(0x4848, 0x20)                    # rule 0 uses action group 5 only
c.cmd('acl action 0 00000000 00020000 00000000 00000000 00000000')   # code 1, no ports: drop
print('%-16s ping  http' % 'rule')
print('%-16s %4d  %s' % ('(none)', ping(), http()))
for name, r in RULES.items():
    c.cmd('acl rule 0 ' + r)
    time.sleep(0.3)
    print('%-16s %4d  %s' % (name, ping(), http()))

c.cmd('acl rule 0 ' + RULES['match all'])
print('\nforward code (action word 1 bits 20:17), mask in bits 30:21')
for code, pm, what in ((0, 0, ''), (1, 0, ''), (1, 1 << 9, 'CPU'), (2, 0, ''), (3, 0, ''), (4, 0, ''),
                       (5, 0, ''), (6, 0, ''), (7, 0, ''), (8, 0, ''), (8, 1 << 9, 'CPU')):
    c.cmd('acl action 0 00000000 %08x 00000000 00000000 00000000' % (code << 17 | pm << 21))
    time.sleep(0.3)
    f0 = frames_rx()
    p = ping()
    time.sleep(0.5)
    print('code %2d %-4s ping %d/6, frames to the CPU +%d' % (code, what, p, frames_rx() - f0))
clear()
print('\ncleared: ping %d/6' % ping())
