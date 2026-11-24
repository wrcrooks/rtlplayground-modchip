#!/usr/bin/env python3
"""OpenSSH's client against the SSH server: run_ssh.py <test_ssh>"""

import hashlib
import hmac
import os
import pty
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

PORT = 20022
failures = 0


def check(ok, what):
    global failures
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    failures += not ok
    return ok


def main():
    with tempfile.TemporaryDirectory() as d:
        ask = os.path.join(d, "ask")
        with open(ask, "w") as f:
            f.write('#!/bin/sh\necho "$SSHPW"\n')
        os.chmod(ask, 0o755)
        known = os.path.join(d, "known")
        env = dict(os.environ, SSH_ASKPASS=ask, SSH_ASKPASS_REQUIRE="force", SSHPW="pass & word")
        base = ["ssh", "-F", "none", "-p", str(PORT), "-o", "UserKnownHostsFile=" + known,
                "-o", "StrictHostKeyChecking=accept-new", "-o", "NumberOfPasswordPrompts=1", "-o", "PubkeyAuthentication=no",
                "-o", "ConnectTimeout=10"]

        def ssh(cmd, opts=(), pw=None, stdin=b""):
            e = dict(env, SSHPW=pw) if pw is not None else env
            return subprocess.run(base + list(opts) + ["admin@127.0.0.1"] + cmd, input=stdin, env=e, capture_output=True, timeout=60)

        proc = subprocess.Popen([sys.argv[1], str(PORT)], stdout=subprocess.PIPE)
        fp = proc.stdout.readline().decode()
        if "SSH on" not in fp:
            fp = proc.stdout.readline().decode()
        fp = re.search(r"SHA256:\S+", fp).group(0)
        time.sleep(1.5)
        try:
            run(ssh, base, env, fp, proc, d)
        finally:
            proc.kill()
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


def raw(data, wait=1.0):
    """Sends data, returns what comes back until the server closes or wait passes"""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=wait)
    out = b""
    try:
        s.sendall(data)
        while True:
            b = s.recv(4096)
            if not b:
                return out, True
            out += b
    except socket.timeout:
        return out, False
    except OSError:
        return out, True
    finally:
        s.close()


def packet(payload):
    pad = 8 - (5 + len(payload)) % 8
    pad += 8 if pad < 4 else 0
    return struct.pack(">IB", 1 + len(payload) + pad, pad) + payload + bytes(pad)


def kexinit(kex, strict=False):
    names = [kex + (b",kex-strict-c-v00@openssh.com" if strict else b""), b"ecdsa-sha2-nistp256", b"aes128-ctr", b"aes128-ctr",
             b"hmac-sha2-256", b"hmac-sha2-256", b"none", b"none", b"", b""]
    return b"\x14" + bytes(16) + b"".join(struct.pack(">I", len(n)) + n for n in names) + b"\0" * 5


def run(ssh, base, env, fp, proc, d):
    r = ssh(["echo", "hello"])
    check(r.returncode == 0 and r.stdout == b"hello\n", "a command: %r %r" % (r.stdout, r.stderr[-100:]))
    k = subprocess.run(["ssh-keygen", "-lf", os.path.join(d, "known")], capture_output=True).stdout.decode()
    check(fp in k, "the host key's fingerprint is the one the switch shows")
    r = ssh(["nonsense"])
    check(r.returncode == 1 and b"unknown command" in r.stdout, "a failing command's exit status")
    for kex in ("curve25519-sha256", "curve25519-sha256@libssh.org", "ecdh-sha2-nistp256"):
        check(ssh(["echo", "x"], ["-o", "KexAlgorithms=" + kex]).stdout == b"x\n", kex)
    for c in ("aes128-ctr", "aes256-ctr"):
        for m in ("hmac-sha2-256", "hmac-sha2-256-etm@openssh.com"):
            check(ssh(["echo", "x"], ["-c", c, "-m", m]).stdout == b"x\n", c + " " + m)
    for c in ("aes128-gcm@openssh.com", "aes256-gcm@openssh.com"):
        check(ssh(["echo", "x"], ["-c", c]).stdout == b"x\n", c)
    for c in ("chacha20-poly1305@openssh.com", "aes128-cbc"):
        check(ssh(["echo", "x"], ["-c", c]).returncode == 255, c + " refused")
    r = ssh(["big", "20000"])
    check(len(r.stdout) == 20000 and r.stdout[63:64] == b"\n", "20 KB of output")
    r = ssh(["backup"])
    check(r.stdout == b"# backup\nip 1.2.3.4\n", "backup")
    r = ssh(["restore", "5"], stdin=b"ip 5.6.7.8\n" * 300)
    check(r.returncode == 0 and b"OK: restored" in r.stdout, "restore < file")
    check(ssh(["restored"]).stdout == b"5 minutes, 3300 bytes\n", "the whole file, with a rollback")
    r = ssh(["restore"], stdin=b"bad\n")
    check(r.returncode == 1 and b"NO: line 1" in r.stdout, "a bad file")
    r = ssh(["restore"], stdin=b"x" * 9000)
    check(r.returncode == 1 and b"too long" in r.stdout, "a file too long")
    r = ssh([], stdin=b"echo one\necho two\r\nfail\n")
    check(r.stdout == b"one\ntwo\nError: unknown command\n", "commands on standard input: %r" % r.stdout)
    r = ssh(["echo", "y" * 200])
    check(r.returncode != 0, "a line too long is refused")

    # A terminal
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe("ssh", base + ["-o", "RekeyLimit=16K", "admin@127.0.0.1"], env)
    buf = b""

    def until(what, t=10):
        nonlocal buf
        end = time.time() + t
        while what not in buf and time.time() < end:
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    buf += os.read(fd, 4096)
                except OSError:
                    break
        ok = what in buf
        buf = buf.split(what, 1)[1] if ok else buf
        return ok

    check(until(b"testswitch> "), "a prompt")
    os.write(fd, b"echo abx\x7fc\r")
    check(until(b"\r\nabc\r\n") and until(b"testswitch> "), "typing, backspace, CR LF in the output")
    os.write(fd, b"echo second\r")
    until(b"\r\nsecond\r\n")
    os.write(fd, b"\x1b[A\x1b[A\r")
    check(until(b"\r\nabc\r\n"), "history with the arrow keys")
    os.write(fd, b"rubbish\x03echo ok\r")
    check(until(b"^C") and until(b"\r\nok\r\n"), "^C drops the line")
    for i in range(6):
        os.write(fd, b"big 15000\r")
        until(b"testswitch> ")
        until(b"testswitch> ", 5)
    os.write(fd, b"echo rekeyed\r")
    check(until(b"\r\nrekeyed\r\n"), "new keys within a session, every 16 KB")
    os.write(fd, b"\x04")
    end = time.time() + 5
    st = None
    while time.time() < end and st is None:
        try:
            select.select([fd], [], [], 0.1)[0] and os.read(fd, 4096)
        except OSError:
            pass
        p, s = os.waitpid(pid, os.WNOHANG)
        if p:
            st = os.waitstatus_to_exitcode(s)
    check(st == 0, "^D ends it")

    # Two at once, a third refused
    two = [subprocess.Popen(base + ["admin@127.0.0.1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
           for _ in range(2)]
    time.sleep(2)
    check(ssh(["echo", "x"]).returncode == 255, "no more than two connections")
    for p in two:
        out, _ = p.communicate(b"echo both\n", timeout=10)
        check(out == b"both\n", "both work")

    # Hostile clients
    out, closed = raw(b"GET / HTTP/1.1\r\n\r\n")
    check(closed, "not SSH: closed")
    out, closed = raw(b"SSH-2.0-" + b"A" * 400)
    check(closed, "an endless version line")
    out, closed = raw(b"SSH-2.0-x\r\n" + b"\xff" * 64)
    check(closed, "an absurd packet length")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(b"\x32" + bytes(40)))
    check(closed, "authentication before key exchange")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(kexinit(b"diffie-hellman-group1-sha1")))
    check(closed, "nothing in common")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(b"\x02\0\0\0\0") + packet(kexinit(b"curve25519-sha256", True)))
    check(closed, "strict KEX: an IGNORE before KEXINIT ends it (Terrapin)")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(kexinit(b"curve25519-sha256")) + packet(b"\x1e" + struct.pack(">I", 32) + bytes(32)))
    check(closed and b"\x1f" not in out[-60:], "curve25519: the zero point is refused")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(kexinit(b"ecdh-sha2-nistp256")) + packet(b"\x1e" + struct.pack(">I", 65) + b"\x04" + b"\x01" * 64))
    check(closed, "P-256: a point off the curve is refused")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(kexinit(b"curve25519-sha256")) + packet(b"\x15"))
    check(closed, "NEWKEYS before the exchange")
    out, closed = raw(b"SSH-2.0-x\r\n" + packet(kexinit(b"curve25519-sha256")[:40]))
    check(closed, "a cut off KEXINIT")
    rnd = __import__("random").Random(1)
    for _ in range(60):
        blob = bytearray(packet(kexinit(b"curve25519-sha256")) + packet(b"\x1e" + struct.pack(">I", 32) + bytes(range(32))))
        for _ in range(rnd.randrange(1, 12)):
            blob[rnd.randrange(len(blob))] = rnd.randrange(256)
        raw(b"SSH-2.0-x\r\n" + bytes(blob), 0.05)
    time.sleep(0.5)
    check(proc.poll() is None and ssh(["echo", "alive"]).stdout == b"alive\n", "still there after 60 mangled exchanges")

    # Passwords
    t = time.time()
    r = ssh(["echo", "x"], pw="wrong")
    check(r.returncode == 255 and time.time() - t > 1 and b"Permission denied" in r.stderr, "a wrong password: refused, slowly")
    check(ssh(["echo", "x"], pw="pass").returncode == 255, "part of the password")
    check(ssh(["echo", "x"], pw="pass & word1").returncode == 255, "more than the password")
    ev = ssh(["events"]).stdout.decode()
    check("SSH login failed from 127.0.0.1 (admin)" in ev and "SSH login from 127.0.0.1 (admin)" in ev, "logins are events")
    for _ in range(6):
        ssh(["echo", "x"], pw="nope")
    check(ssh(["echo", "x"]).returncode == 255, "guessing locks everybody out for a while")
    time.sleep(31)
    check(ssh(["echo", "x"]).stdout == b"x\n", "and then it's open again")
    ssh(["ssh off"])
    time.sleep(2.5)
    check(ssh(["echo", "x"]).returncode == 255, "switched off from within: closed")


if __name__ == "__main__":
    sys.exit(main())
