#!/usr/bin/env python3
# tests/scripting: checks that the numbers of the C interface (src/engine/scriptapi.h) and of the C# side
# (class Abi in src/scripting/ScriptPortal.Vegas.cs) agree, name by name.
import re, sys
h = open(sys.argv[1]).read()
cs = open(sys.argv[2], encoding='utf-8').read()
cnames = dict((n, int(v)) for n, v in re.findall(r'\b(R[KPLO]_[A-Z0-9_]+)\s*=\s*(-?\d+)', h))
body = cs[cs.index('internal static class Abi'):cs.index('internal static class Reno')]
csnames = dict((n, int(v)) for n, v in re.findall(r'\b(R[KPLO]_[A-Z0-9_]+)\s*=\s*(-?\d+)', body))
bad = 0
for n, v in sorted(cnames.items()):
    if n not in csnames: print('only in scriptapi.h:', n); bad += 1
    elif csnames[n] != v: print('differs:', n, v, csnames[n]); bad += 1
for n in sorted(set(csnames) - set(cnames)): print('only in the C# side:', n); bad += 1
print('ABI check: %d names, %d problems' % (len(cnames), bad))
sys.exit(1 if bad else 0)
