#!/usr/bin/env python3
"""Generate gui/keymap_names.h from vendor/common.c.

Extracts the pedal keymap's name strings so the GUI can validate captured
key tokens against exactly the set fpedal accepts. Run by the Makefile.
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "vendor/common.c"
src = open(path, encoding="utf-8", errors="replace").read()

m = re.search(r"keymap_entry keymap\[\]\s*=\s*\{(.*?)\n\s*\};", src, re.S)
if not m:
    sys.exit("gen_keymap: keymap array not found in " + path)

names = re.findall(r'\{\s*"((?:\\.|[^"\\])*)"', m.group(1))


def unescape(s: str) -> str:
    """Convert a C string-literal body to the actual characters it denotes."""
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r",
                        "0": "\0"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def cesc(s: str) -> str:
    """Re-encode characters as a C string literal body."""
    s = s.replace("\\", "\\\\")
    s = s.replace('"', '\\"')
    s = s.replace("\n", "\\n")
    s = s.replace("\t", "\\t")
    return s


print("/* auto-generated from vendor/common.c by scripts/gen_keymap.py")
print(" * -- do not edit. */")
print("static const char *const kKeymapNames[] = {")
for n in names:
    print('    "%s",' % cesc(unescape(n)))
print("    nullptr,")
print("};")
