#!/usr/bin/env python3
"""Runs test/sim and checks its output: the CHECK lines, and that every
.json endpoint's body parses as JSON, as the web UI's app.js expects.

  python3 run_sim.py build/sim [-v]
"""
import json
import subprocess
import sys


def main():
    verbose = "-v" in sys.argv
    out = subprocess.run([sys.argv[1]], capture_output=True, timeout=60).stdout
    failures = 0
    pos = 0
    while pos < len(out):
        end = out.find(b"\n", pos)
        if end < 0:
            end = len(out)
        line = out[pos:end].decode(errors="replace")
        pos = end + 1
        if line.startswith("CHECK "):
            print(line[6:])
            failures += line.startswith("CHECK FAIL")
        elif line.startswith("BODY ") or line.startswith("CONSOLE "):
            parts = line.split()
            n = int(parts[-1])
            data = out[pos:pos + n]
            pos += n + 1
            if line.startswith("CONSOLE"):
                if verbose:
                    print(data.decode(errors="replace"))
                continue
            path = parts[1]
            head, _, body = data.partition(b"\r\n\r\n")
            if verbose:
                print(path, body[:200])
            if ".json" in path:
                try:
                    json.loads(body)
                    print("PASS  %s is valid JSON" % path)
                except ValueError as e:
                    print("FAIL  %s is not JSON: %s\n      %r" % (path, e, body[:300]))
                    failures += 1
        elif line.startswith("DONE "):
            if int(line.split()[1]) and not failures:
                failures += 1
        elif line:
            print(line)
    if b"DONE" not in out:
        print("FAIL  the simulation did not finish")
        failures += 1
    print("FAILED" if failures else "all passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
