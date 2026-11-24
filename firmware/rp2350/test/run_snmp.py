#!/usr/bin/env python3
"""Runs test_snmp (the firmware's SNMP agent on the host) and queries it
with net-snmp's snmpget, snmpwalk and snmpset.

  python3 run_snmp.py build/test_snmp
"""
import hashlib
import hmac
import shutil
import socket
import subprocess
import sys

failures = 0


def check(cond, name):
    global failures
    print(("PASS  " if cond else "FAIL  ") + name)
    failures += not cond


def main():
    if not shutil.which("snmpget"):
        print("SKIP  test_snmp: net-snmp's snmpget isn't installed")
        return 0
    traps = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    traps.bind(("127.0.0.1", 0))
    traps.settimeout(2)
    agent = subprocess.Popen([sys.argv[1], str(traps.getsockname()[1])], stdout=subprocess.PIPE, text=True)
    try:
        line = agent.stdout.readline()
        port = int(line.split()[1])
        target = "127.0.0.1:%d" % port

        def snmp(tool, *args, community="sim"):
            r = subprocess.run([tool, "-v2c", "-c", community, "-On", "-Oe", "-t", "1", "-r", "0", target] + list(args),
                               capture_output=True, text=True, timeout=30)
            return r.returncode, r.stdout + r.stderr

        def values(out):
            v = {}
            for ln in out.splitlines():
                if " = " in ln:
                    oid, val = ln.split(" = ", 1)
                    v[oid.lstrip(".")] = val
            return v

        rc, out = snmp("snmpget", "1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.1.6.0", "1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.1.0")
        v = values(out)
        check(rc == 0 and v.get("1.3.6.1.2.1.1.5.0") == "STRING: sim-switch", "sysName is the hostname")
        check(v.get("1.3.6.1.2.1.1.6.0") == "STRING: test bench" and v.get("1.3.6.1.2.1.1.4.0") == "STRING: nobody",
              "sysLocation, sysContact")
        check("RTLPlayground mod chip" in v.get("1.3.6.1.2.1.1.1.0", ""), "sysDescr")

        rc, out = snmp("snmpwalk", "1.3.6.1.2.1.2")
        v = values(out)
        check(v.get("1.3.6.1.2.1.2.1.0") == "INTEGER: 6", "ifNumber: 6 ports")
        check(v.get("1.3.6.1.2.1.2.2.1.2.1") == "STRING: Port 1", "ifDescr")
        check(v.get("1.3.6.1.2.1.2.2.1.5.3") == "Gauge32: 1000000000", "ifSpeed of the 1G port")
        check(v.get("1.3.6.1.2.1.2.2.1.8.3") == "INTEGER: 1" and v.get("1.3.6.1.2.1.2.2.1.8.1") == "INTEGER: 2",
              "ifOperStatus up where linked, down elsewhere")
        check(v.get("1.3.6.1.2.1.2.2.1.7.5") == "INTEGER: 2" and v.get("1.3.6.1.2.1.2.2.1.7.1") == "INTEGER: 1",
              "ifAdminStatus down for the disabled port")
        check(v.get("1.3.6.1.2.1.2.2.1.10.3") == "Counter32: 291", "ifInOctets wraps at 32 bits")
        check(v.get("1.3.6.1.2.1.2.2.1.14.3") == "Counter32: 7", "ifInErrors")
        check(v.get("1.3.6.1.2.1.2.2.1.6.1", "").replace(" ", "").endswith("06:0F:00:DD:3B:3E")
              or "6:f:0:dd:3b:3e" in v.get("1.3.6.1.2.1.2.2.1.6.1", "").lower(), "ifPhysAddress")
        check(len([k for k in v if k.startswith("1.3.6.1.2.1.2.2.1.")]) == 6 * 20, "ifTable: 20 columns for each port")

        rc, out = snmp("snmpwalk", "1.3.6.1.2.1.31.1.1")
        v = values(out)
        check(v.get("1.3.6.1.2.1.31.1.1.1.6.3") == "Counter64: 4294967587", "ifHCInOctets keeps all 64 bits")
        check(v.get("1.3.6.1.2.1.31.1.1.1.15.3") == "Gauge32: 1000", "ifHighSpeed")
        check(v.get("1.3.6.1.2.1.31.1.1.1.18.3") == "STRING: uplink", "ifAlias is the port name")
        check(v.get("1.3.6.1.2.1.31.1.1.1.1.6") == "STRING: port6", "ifName")

        rc, out = snmp("snmpwalk", "1.3.6.1.2.1")
        check(rc == 0 and "1.3.6.1.2.1.31.1.1.1.18.6" in out and "Timeout" not in out,
              "a walk of all of MIB-2 runs to its end (%d objects)" % len(values(out)))

        rc, out = snmp("snmpget", "1.3.6.1.2.1.1.5.0", community="wrong")
        check(rc != 0 and "Timeout" in out, "a wrong community gets no answer")
        rc, out = snmp("snmpset", "1.3.6.1.2.1.1.5.0", "s", "renamed")
        rc2, out2 = snmp("snmpget", "1.3.6.1.2.1.1.5.0")
        check(rc != 0 and "sim-switch" in out2, "writes are refused")

        # SNMPv3
        status = agent.stdout.readline()
        check("SNMPv3 user v3sha256: SHA-256, AES-128|" in status and "Engine ID 0x8000670d03060f00dd3b3e" in status,
              "status: %s" % status.strip())
        engine = bytes.fromhex("8000670d03060f00dd3b3e")

        def v3(user, level, auth="SHA-256", ap="authpassword", xp="privpassword", tool="snmpget", args=("1.3.6.1.2.1.1.5.0",)):
            cmd = [tool, "-v3", "-u", user, "-l", level, "-On", "-Oe", "-t", "1", "-r", "0"]
            if level != "noAuthNoPriv":
                cmd += ["-a", auth, "-A", ap]
            if level == "authPriv":
                cmd += ["-x", "AES", "-X", xp]
            r = subprocess.run(cmd + [target] + list(args), capture_output=True, text=True, timeout=30)
            return r.returncode, r.stdout + r.stderr

        rc, out = v3("v3sha256", "authPriv")
        check(rc == 0 and "sim-switch" in out, "SNMPv3 SHA-256, AES-128: %s" % out.strip()[:60])
        rc, out = v3("v3sha", "authPriv", auth="SHA")
        check(rc == 0 and "sim-switch" in out, "SNMPv3 SHA-1, AES-128")
        rc, out = v3("v3auth", "authNoPriv")
        check(rc == 0 and "sim-switch" in out, "SNMPv3 SHA-256, not encrypted")
        rc, out = v3("v3sha256", "authPriv", tool="snmpbulkwalk", args=("1.3.6.1.2.1.2.2",))
        check(rc == 0 and len(values(out)) == 6 * 21 + 0 or len(values(out)) >= 120, "SNMPv3 bulk walk: %d objects" % len(values(out)))
        rc, out = v3("v3sha256", "authPriv", ap="wrongpassword")
        check(rc != 0 and "uthentication failure" in out, "a wrong password: %s" % out.strip()[:50])
        rc, out = v3("v3sha256", "authPriv", xp="wrongpassword")
        check(rc != 0 and "sim-switch" not in out, "a wrong privacy password")
        rc, out = v3("v3sha256", "authNoPriv")
        check(rc != 0 and "sim-switch" not in out, "unencrypted, by a user who encrypts")
        rc, out = v3("v3sha256", "noAuthNoPriv")
        check(rc != 0 and "sim-switch" not in out, "unauthenticated")
        rc, out = v3("nobody", "authPriv")
        check(rc != 0 and "nknown user" in out, "an unknown user")
        rc, out = v3("v3sha256", "authPriv", tool="snmpset", args=("1.3.6.1.2.1.1.5.0", "s", "renamed"))
        rc2, out2 = v3("v3sha256", "authPriv")
        check(rc != 0 and "sim-switch" in out2, "SNMPv3 writes are refused")

        # notifications
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(b"EVENT", ("127.0.0.1", port))
        got = []
        try:
            for _ in range(8):
                got.append(traps.recv(2000))
        except socket.timeout:
            pass
        v2 = [g for g in got if b"\x04\x03sim" in g]
        v3t = [g for g in got if b"v3sha256" in g]
        std = b"\x06\x09\x2b\x06\x01\x06\x03\x01\x01\x05"
        check(len(v2) == 4 and len(v3t) == 4, "four notifications to each of two addresses (%d, %d)" % (len(v2), len(v3t)))
        check(len(v2) == 4 and std + b"\x01" in v2[0] and std + b"\x03" in v2[1] and std + b"\x05" in v2[2]
              and b"Port 4 error-disabled: link-flap" in v2[3], "coldStart, linkDown, authenticationFailure, error-disable")
        check(len(v2) == 4 and b"\x06\x0a\x2b\x06\x01\x02\x01\x02\x02\x01\x08\x03\x02\x01\x02" in v2[1], "linkDown says ifOperStatus.3 = down")

        def localized(pw):
            d = hashlib.sha256((pw * (1048576 // len(pw) + 1)).encode()[:1048576]).digest()
            return hashlib.sha256(d + engine + d).digest()
        if v3t:
            m = bytearray(v3t[1])
            at = m.index(b"v3sha256") + 8
            mac = bytes(m[at + 2:at + 26])
            m[at + 2:at + 26] = bytes(24)
            check(m[at:at + 2] == b"\x04\x18" and hmac.new(localized("authpassword"), bytes(m), hashlib.sha256).digest()[:24] == mac,
                  "the v3 one is signed as RFC 7860 says")
            try:
                from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
                salt = bytes(m[at + 28:at + 36])
                i = m.index(engine) + len(engine)
                boots = int.from_bytes(m[i + 2:i + 2 + m[i + 1]], "big")
                i += 2 + m[i + 1]
                t = int.from_bytes(m[i + 2:i + 2 + m[i + 1]], "big")
                i = at + 36 + (2 if m[at + 37] < 128 else 4)
                iv = boots.to_bytes(4, "big") + t.to_bytes(4, "big") + salt
                d = Cipher(algorithms.AES(localized("privpassword")[:16]), modes.CFB(iv)).decryptor()
                clear = d.update(bytes(m[i:])) + d.finalize()
                check(std + b"\x03" in clear and boots == 7, "and decrypts to a linkDown, engineBoots counted up")
            except ImportError:
                print("SKIP  decrypting it: no cryptography module")
        s.sendto(b"V2COFF", ("127.0.0.1", port))
        rc, out = snmp("snmpget", "1.3.6.1.2.1.1.5.0")
        rc2, out2 = v3("v3sha256", "authPriv")
        check(rc != 0 and rc2 == 0, "snmp v2c off: only v3 is answered")
    finally:
        agent.kill()
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
