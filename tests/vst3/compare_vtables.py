#!/usr/bin/env python3
"""Compares the virtual function tables the compiler lays out for Reno's VST3 interfaces
(src/audiofx/vst3_abi.h) with those of the official ones.

Input: what clang prints when abicheck.cpp is compiled with  -Xclang -fdump-vtable-layouts
(for a Windows target: this is how the order is checked for Windows without running anything).

    compare_vtables.py dump.txt
"""
import re
import sys


def tables(text):
    """{class name: [function names in table order]} for every 'VFTable for' / 'Vtable for' block."""
    out = {}
    blocks = re.split(r"\n\s*\n", text)
    for block in blocks:
        lines = block.strip().splitlines()
        if not lines:
            continue
        # Windows targets print "VFTable for 'Base' in 'Class' (n entries)." for a class that got its
        # table from a base class, other targets "Vtable for 'Class' (n entries)."
        # (with one more "in" for every further level of inheritance; the last name is the class)
        m = re.match(r"(?:VFTable|Vtable) for ((?:'[^']+'(?: in )?)+) \((\d+) entr", lines[0])
        if not m:
            continue
        cls = re.findall(r"'([^']+)'", m.group(1))[-1]
        if "::" not in cls:                         # (abicheck's own helper classes)
            continue
        names = []
        for line in lines[1:]:
            e = re.match(r"\s*(\d+) \| (.*)$", line)
            if not e:
                continue
            f = re.search(r"([A-Za-z_][A-Za-z_0-9:]*)::([A-Za-z_][A-Za-z_0-9]*)\(", e.group(2))
            if f:                                   # (RTTI and offset entries have no function in them)
                names.append(f.group(2))
        out[cls] = names
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    t = tables(open(sys.argv[1], encoding="utf-8", errors="replace").read())
    ours = {k: v for k, v in t.items() if k.startswith("reno::vst3::")}
    official = {}
    for k, v in t.items():
        if k.startswith("Steinberg::"):
            official[k.split("::")[-1]] = v
    if not ours:
        print("no tables of reno::vst3 interfaces in the input")
        return 2
    bad = 0
    for name in sorted(ours):
        short = name.split("::")[-1]
        if short not in official:
            print("MISSING   no official table for %s in the input" % short)
            bad += 1
        elif ours[name] != official[short]:
            print("MISMATCH  %s\n   ours:     %s\n   official: %s" % (short, ", ".join(ours[name]), ", ".join(official[short])))
            bad += 1
        else:
            print("ok   %-20s %2d functions: %s" % (short, len(ours[name]), ", ".join(ours[name])))
    print("\n%d interfaces compared, %d different" % (len(ours), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
