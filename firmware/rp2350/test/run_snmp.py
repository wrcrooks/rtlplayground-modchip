#!/usr/bin/env python3
"""Runs test_snmp (the firmware's SNMP agent on the host) and queries it
with net-snmp's snmpget, snmpwalk and snmpset.

  python3 run_snmp.py build/test_snmp
"""
import shutil
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
    agent = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE, text=True)
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
    finally:
        agent.kill()
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
