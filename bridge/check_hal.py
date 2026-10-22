#!/usr/bin/env python3
"""Check that the helpers hal.c copies from rtlplayground.c are still the same.

Run from anywhere; exits non-zero and shows a diff for each helper that
differs. write_char() is not copied: the bridge has its own.
"""
import difflib, pathlib, re, sys

root = pathlib.Path(__file__).resolve().parents[2]


def functions(src):
    """name -> full text of each function defined at file scope"""
    found = {}
    for m in re.finditer(r'^[A-Za-z_][^\n;{}()]*\b(\w+)\s*\([^;{)]*\)[^;{]*\{', src, re.M):
        i, depth = m.end(), 1
        while depth:
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        found[m.group(1)] = src[m.start():i]
    return found


main = functions((root / "rtlplayground.c").read_text())
hal = functions((root / "modchip/bridge/hal.c").read_text())
bad = 0
for name, text in sorted(hal.items()):
    if name not in main:
        print("hal.c: %s is not in rtlplayground.c any more" % name)
        bad += 1
    elif main[name] != text:
        print("hal.c: %s differs from rtlplayground.c:" % name)
        sys.stdout.writelines(difflib.unified_diff(main[name].splitlines(True), text.splitlines(True),
                                                   "rtlplayground.c", "hal.c"))
        bad += 1
print("%d helpers checked, %d differ" % (len(hal), bad))
sys.exit(1 if bad else 0)
