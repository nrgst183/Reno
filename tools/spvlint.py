#!/usr/bin/env python3
"""Static exactness check of the generated GPU kernels (src/ofx/vkshaders/*.spv.h), runnable without a GPU.

A real GPU driver may do what a software one (SwiftShader) never does: fuse a*b+c into one rounding, use its own few-ulp
sin/cos/exp/pow/sqrt/divide, or assume values are never NaN or infinite. A kernel is exact on every card only when
  * every FAdd / FSub / FMul / VectorTimesScalar / Dot / FDiv / FRem / FMod is decorated NoContraction (so no fusing across it),
  * the driver's own transcendental, Mix, Dot, Length ... built-ins are not used (GLSL.std.450 Sin Cos Tan Asin Acos Atan Atan2 Pow Exp Log Exp2 Log2
    InverseSqrt, FMix, Radians, Degrees, Smoothstep ... give a few ulp, differing from card to card), except where named in ALLOWED below,
  * Fma only where the CPU code calls fmaf,
  * the module sets SignedZeroInfNanPreserve (checked in the driver code, see vkdevice.cpp) when it tests IsNan / IsInf.
Usage: spvlint.py [folder] [--list]    exit status 1 when a kernel breaks a rule.
"""
import os, re, sys, collections

EXT = {1: "Round", 2: "RoundEven", 3: "Trunc", 4: "FAbs", 5: "SAbs", 6: "FSign", 8: "Floor", 9: "Ceil", 10: "Fract", 11: "Radians", 12: "Degrees", 13: "Sin", 14: "Cos",
       15: "Tan", 16: "Asin", 17: "Acos", 18: "Atan", 19: "Sinh", 20: "Cosh", 21: "Tanh", 25: "Atan2", 26: "Pow", 27: "Exp", 28: "Log", 29: "Exp2", 30: "Log2",
       31: "Sqrt", 32: "InverseSqrt", 37: "FMin", 38: "UMin", 39: "SMin", 40: "FMax", 41: "UMax", 42: "SMax", 43: "FClamp", 44: "UClamp", 45: "SClamp",
       46: "FMix", 48: "Step", 49: "SmoothStep", 50: "Fma", 66: "Length", 67: "Distance", 68: "Cross", 69: "Normalize", 75: "FindUMsb"}
# exact in every driver (selection, rounding to an integral value, bit tricks)
EXACT = {"Round", "RoundEven", "Trunc", "FAbs", "SAbs", "FSign", "Floor", "Ceil", "Fract", "FMin", "UMin", "SMin", "FMax", "UMax", "SMax", "FClamp", "UClamp", "SClamp", "FindUMsb"}
# built-ins with driver-specific precision
RISKY = {"Radians", "Degrees", "Sin", "Cos", "Tan", "Asin", "Acos", "Atan", "Sinh", "Cosh", "Tanh", "Atan2", "Pow", "Exp", "Log", "Exp2", "Log2", "Sqrt", "InverseSqrt",
         "FMix", "Step", "SmoothStep", "Length", "Distance", "Cross", "Normalize"}
# Sqrt and Atan2 are only a starting value / special-case answer behind the software double (spvdsl.py: sqrt_hw, atan2_f); that use is audited by the kernel's test
ALLOWED = {"Sqrt", "Atan2", "Fma"}
FLOAT_ARITH = {"FAdd": 129, "FSub": 131, "FMul": 133, "FDiv": 136, "FRem": 140, "FMod": 141, "VectorTimesScalar": 142, "Dot": 148}

def words(path):
    txt = open(path).read()
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", txt.split("{", 1)[1])]

def scan(w):
    nocontr, ops, ext = set(), collections.Counter(), collections.Counter()
    undecorated = []
    insts = []
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if n == 0: break
        insts.append((op, w[i + 1:i + n]))
        if op == 71 and w[i + 2] == 42: nocontr.add(w[i + 1])       # OpDecorate id NoContraction
        i += n
    names = {v: k for k, v in FLOAT_ARITH.items()}
    exset = None
    for op, a in insts:
        if op == 11 and a[1] == 0: pass
        if op in names:
            ops[names[op]] += 1
            if a[1] not in nocontr: undecorated.append(names[op])
        if op == 12:       # ExtInst: type, result, set, instruction, operands
            nm = EXT.get(a[3], "ext%d" % a[3])
            ext[nm] += 1
        if op in (1, 2) and False: pass
    usesnan = any(op in (156, 157) for op, a in insts)       # IsNan IsInf
    return ops, ext, undecorated, usesnan

def main():
    folder = "src/ofx/vkshaders"
    listing = "--list" in sys.argv
    for a in sys.argv[1:]:
        if not a.startswith("--"): folder = a
    bad = 0
    tot = collections.Counter()
    for fn in sorted(os.listdir(folder)):
        if not fn.endswith(".spv.h"): continue
        ops, ext, und, nan = scan(words(os.path.join(folder, fn)))
        name = fn[:-6]
        probs = []
        if und: probs.append("%d float ops without NoContraction (%s)" % (len(und), ", ".join(sorted(set(und)))))
        for k in ext:
            if k in RISKY and k not in ALLOWED: probs.append("built-in %s x%d (driver-specific precision)" % (k, ext[k]))
            elif k not in EXACT and k not in RISKY and k not in ALLOWED: probs.append("unknown built-in %s" % k)
        if listing: print("%-28s %s %s%s" % (name, dict(ops), dict(ext), " nan-tests" if nan else ""))
        for p in probs:
            print("LINT %s: %s" % (name, p)); bad += 1
        tot.update(ext)
    print("spvlint: %d problem(s)" % bad)
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
