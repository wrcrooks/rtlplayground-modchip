#!/usr/bin/env python3
"""The restore check's command list (safety.c) against the commands the
firmware's parsers know: every one is either accepted or left out on purpose."""
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
known = set()
for f in [root / "rtlplayground/cmd_parser.c"] + sorted((root / "src").glob("rtl_*.c")):
    known |= set(re.findall(r'cmd_compare\(0, "([a-z0-9-]+)"\)', f.read_text()))
# These show, act or debug; none is ever saved
ACTIONS = {"acltbl", "bridge", "flash", "history", "phyget", "physet", "reboot", "regget", "regset", "reset", "rnd",
           "rollback", "sds", "sdsget", "sdsset", "stat", "temp", "version", "watchdog", "health", "update"}
out = subprocess.run([sys.argv[1]] + sorted(known), capture_output=True, text=True).stdout
bad = 0
for line in out.splitlines():
    ok, word = line.split(" ", 1)
    if (ok == "yes") == (word in ACTIONS):
        print("FAIL  %s is %s by the restore check" % (word, "accepted" if ok == "yes" else "refused"))
        bad += 1
print("%s  the restore check knows the %d commands" % ("FAIL" if bad else "PASS", len(known)))
sys.exit(bad != 0)
