#!/usr/bin/env python3
"""Compares the settings Reno's built-in OpenFX effects describe with what VEGAS reported for the same effects in a coverage run.

  vegcov_params.py <vegas-output-folder> <vfxparams.tsv> [--report FILE]

  <vegas-output-folder>   the folder tools/vegas/RenoCoverage.cs wrote (projects/*.json: per effect, per setting its type,
                          default and, for choices, the choice names)
  <vfxparams.tsv>         tests/ui/vfxparams_dump's output (each built-in effect with every setting's type, defaults, choices)

For each effect (by its OpenFX id, in the context VEGAS used it in) it lists settings VEGAS has that Reno lacks (and the other
way round), settings whose type differs, defaults that differ, and choice lists that differ.  Exit status 0 when nothing differs.
"""
import json
import math
import os
import re
import sys


def norm_id(u):
    m = re.match(r"^\{Svfx:(.*)\}$", (u or "").strip())
    return (m.group(1) if m else (u or "")).strip()


def load_reno(path):
    effects = {}          # id -> {context -> {name -> dict}}
    cur = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if c[0] == "EFFECT":
                cur = effects.setdefault(c[1], {}).setdefault(c[2], {})
            elif c[0] == "P" and cur is not None and len(c) >= 18:
                cur[c[2]] = {"type": c[3].replace("OfxParamType", ""), "default": [float(x) for x in c[9].split(",") if x != ""],
                             "choices": c[17].split("|") if c[17] else []}
    return effects


def load_vegas(folder):
    pdir = os.path.join(folder, "projects")
    if not os.path.isdir(pdir):
        pdir = folder
    out = {}              # id -> {name -> dict}, first one seen
    contexts = {}

    def take(fx, context):
        if not fx.get("isOFX") or "params" not in fx:
            return
        eid = norm_id(fx.get("uniqueId"))
        contexts.setdefault(eid, set()).add(context)
        d = out.setdefault(eid, {})
        for name, p in fx["params"].items():
            d.setdefault(name, {"type": p.get("type"), "default": p.get("default"), "choices": p.get("choices")})

    for fn in sorted(os.listdir(pdir)):
        if not fn.endswith(".json") or fn.endswith(".vegas.json"):
            continue
        with open(os.path.join(pdir, fn), encoding="utf-8-sig") as f:
            m = json.load(f)
        for t in m.get("tracks", []):
            for fx in t.get("effects", []):
                take(fx, "Filter")
            for e in t.get("events", []):
                for fx in e.get("effects", []):
                    take(fx, "Filter")
                tr = (e.get("fadeIn") or {}).get("transition")
                if tr:
                    take(tr, "Transition")
        for fx in m.get("videoBus", {}).get("effects", []):
            take(fx, "Filter")
        for g in m.get("media", []):
            if g.get("generator"):
                take(g["generator"], "Generator")
            for fx in g.get("effects", []):
                take(fx, "Filter")
    return out, contexts


def close(a, b):
    return abs(a - b) <= max(1e-6, 1e-5 * max(abs(a), abs(b)))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    report = None
    if "--report" in argv:
        report = argv[argv.index("--report") + 1]
    vegas, contexts = load_vegas(argv[1])
    reno = load_reno(argv[2])
    lines, problems = [], 0
    for eid in sorted(vegas):
        if eid not in reno:
            lines.append("%s: no built-in effect in Reno" % eid)
            problems += 1
            continue
        ctx = sorted(contexts[eid])[0]
        r = reno[eid].get(ctx) or next(iter(reno[eid].values()))
        v = vegas[eid]
        for name in sorted(v):
            vp = v[name]
            if name not in r:
                lines.append("%s: VEGAS has setting '%s' (%s), Reno has none" % (eid, name, vp["type"]))
                problems += 1
                continue
            rp = r[name]
            if vp["type"] != rp["type"]:
                lines.append("%s.%s: type VEGAS %s, Reno %s" % (eid, name, vp["type"], rp["type"]))
                problems += 1
                continue
            if isinstance(vp["default"], list) and vp["type"] not in ("String", "Custom"):
                a, b = vp["default"], rp["default"]
                if len(a) != len(b) or not all(close(x, y) for x, y in zip(a, b)):
                    lines.append("%s.%s: default VEGAS %s, Reno %s" % (eid, name, a, b))
                    problems += 1
            if vp.get("choices") is not None and vp["choices"] != rp["choices"]:
                lines.append("%s.%s: choices VEGAS %s, Reno %s" % (eid, name, vp["choices"], rp["choices"]))
                problems += 1
        for name in sorted(r):
            if name not in v:
                lines.append("%s: Reno has setting '%s' (%s) that VEGAS did not report" % (eid, name, r[name]["type"]))
                problems += 1
    lines.append("%d effects compared, %d differences" % (len(vegas), problems))
    text = "\n".join(lines)
    print(text)
    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
