#!/usr/bin/env python3
"""Compare Reno's built-in Rays, Glint, Soft Contrast, Starburst, Fill Light and Defocus with VEGAS's own pictures.

The folder comes from the VEGAS script tools/scripts/"Reno Effect Reference.cs" (manifest.txt, effects.txt, ref_NNNN.png).
Each manifest entry is rendered with Reno's built-in (the reno_cli vegasfxrun child that vegasfx uses) and compared:
mean / largest difference (0..255 over R, G, B, A) and the share of pixels more than 2 levels off.
--png DIR writes each entry as VEGAS | Reno | difference x4.

  effectref_compare.py [--ref DIR] --cli PATH/reno_cli     (--ref defaults to the environment variable RENO_EFFECT_REF) [--only TEXT] [--png DIR] [--keep DIR]
"""
import argparse, json, os, subprocess, sys, tempfile

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("effectref_compare.py needs numpy and Pillow")

EFFECTS = {   # name in the manifest -> Reno's built-in id
    "Rays": "com.vegascreativesoftware:raysvelvetmatter",
    "Glint": "com.vegascreativesoftware:glintvelvetmatter",
    "Soft Contrast": "com.vegascreativesoftware:softcontrastvelvetmatter",
    "Starburst": "com.vegascreativesoftware:starburstvelvetmatter",
    "Fill Light": "com.vegascreativesoftware:filllightvelvetmatter",
    "Defocus": "com.vegascreativesoftware:defocusvelvetmatter",
}
CONTEXT = "OfxImageEffectContextFilter"
W, H = 320, 180


def child(cli, args, **kw):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    return subprocess.run([cli, "vegasfxrun"] + args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600, env=env, **kw)


def load_manifest(ref):
    rows = []
    with open(os.path.join(ref, "manifest.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            p = line.rstrip("\n").split("\t")
            while len(p) < 6:
                p.append("")
            rows.append(dict(index=int(p[0]), effect=p[1], kind=p[2], what=p[3], png=p[4], result=p[5]))
    return rows


def preset_sets(cli, eff_id, tmp):
    """{preset name: its settings} of Reno's built-in preset package for this effect (from the scenarios the child makes)."""
    sc = os.path.join(tmp, "sc_" + eff_id.split(":")[-1] + ".json")
    child(cli, ["--source", "builtin", "--id", eff_id, "--context", CONTEXT, "--size", "%dx%d" % (W, H), "--frames", "1", "--dir", tmp, "--tag", "x",
                "--out", os.path.join(tmp, "x.json"), "--presets", "builtin", "--max-params", "0", "--scenarios-out", sc, "--first", "100000"])
    out = {}
    if os.path.exists(sc):
        for s in json.load(open(sc)):
            if s.get("name", "").startswith("preset: "):
                out[s["name"][8:]] = s.get("set", {})
    return out


def run_scenarios(cli, eff_id, scen, tmp, tag):
    sp = os.path.join(tmp, tag + "_in.json")
    json.dump(scen, open(sp, "w"))
    outj = os.path.join(tmp, tag + ".json")
    first = 0
    for _ in range(40):
        if os.path.exists(outj):
            os.remove(outj)
        child(cli, ["--source", "builtin", "--id", eff_id, "--context", CONTEXT, "--size", "%dx%d" % (W, H), "--frames", "1", "--dir", tmp, "--tag", tag,
                    "--out", outj, "--max-params", "0", "--first", str(first), "--scenarios-in", sp, "--presets", "builtin"])
        if not os.path.exists(outj):
            break
        r = json.load(open(outj))
        if r.get("done"):
            break
        idx = [s.get("index", -1) for s in r.get("scenarios", [])]
        first = (max(idx) + 1) if idx else first + 1


def read_bgra(path):
    """Premultiplied BGRA, bottom row first -> straight RGBA float array, top row first."""
    b = np.fromfile(path, dtype=np.uint8)
    if b.size != W * H * 4:
        return None
    a = b.reshape(H, W, 4)[::-1].astype(np.float64)
    rgba = np.stack([a[..., 2], a[..., 1], a[..., 0], a[..., 3]], -1)
    al = rgba[..., 3:4] / 255.0
    rgba[..., :3] = np.where(al > 0, np.minimum(255.0, rgba[..., :3] / np.maximum(al, 1e-6)), 0)
    return rgba


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default=os.environ.get("RENO_EFFECT_REF", ""), help="folder written by Reno Effect Reference.cs (default $RENO_EFFECT_REF)")
    ap.add_argument("--cli", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--png", default="")
    ap.add_argument("--keep", default="", help="folder to keep Reno's raw pictures in")
    o = ap.parse_args()
    if not o.ref:
        ap.error("--ref (or the environment variable RENO_EFFECT_REF) names the folder written by Reno Effect Reference.cs")
    rows = [r for r in load_manifest(o.ref) if r["effect"] in EFFECTS and (not o.only or o.only.lower() in r["effect"].lower() or o.only == str(r["index"]))]
    tmp = o.keep or tempfile.mkdtemp(prefix="effectref_")
    os.makedirs(tmp, exist_ok=True)
    if o.png:
        os.makedirs(o.png, exist_ok=True)
    print("%-4s %-14s %-48s %8s %7s %7s" % ("idx", "effect", "test", "mean", "max", "%>2"))
    for eff, eid in EFFECTS.items():
        mine = [r for r in rows if r["effect"] == eff]
        if not mine:
            continue
        presets = preset_sets(o.cli, eid, tmp)
        scen = []
        for r in mine:
            s = {"name": r["what"] or "default", "kind": r["kind"], "frames": 1, "set": {}}
            if r["kind"] == "preset" and r["what"] in presets:
                s["set"] = presets[r["what"]]
            elif r["kind"] == "combo":   # "Name=value Name=value ..." (the capture's several-settings cases)
                for kv in r["what"].split():
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        s["set"][k] = [float(v)]
            elif r["kind"] == "preset" and r["what"] != "(Default)":
                s["name"] += " (not in Reno's preset package: defaults)"
            scen.append(s)
        tag = eid.split(":")[-1]
        run_scenarios(o.cli, eid, scen, tmp, tag)
        for k, r in enumerate(mine):
            ref = np.array(Image.open(os.path.join(o.ref, r["png"])).convert("RGBA")).astype(np.float64)
            got = read_bgra(os.path.join(tmp, "%s_%d_0.bgra" % (tag, k)))
            label = r["what"] or "default"
            # a combo whose VEGAS picture is byte for byte the input picture (the settings did not take, e.g. Points=4 is the default and the case came out blank) is not a target
            if r["kind"] == "combo":
                src = os.path.join(o.ref, "reno_test_picture.png")
                if os.path.exists(src) and np.array_equal(ref, np.array(Image.open(src).convert("RGBA")).astype(np.float64)):
                    print("%-4d %-14s %-48s  (VEGAS picture = the input: skipped)" % (r["index"], eff, label.replace("Threshold=0 Brilliance=100 Strength=100 ", "")[:48]))
                    continue
            if r["kind"] == "combo":
                label = label.replace("Threshold=0 Brilliance=100 Strength=100 ", "")
            if got is None:
                print("%-4d %-14s %-48s  (no picture from Reno)" % (r["index"], eff, label))
                continue
            d = np.abs(ref - got)
            print("%-4d %-14s %-48s %8.3f %7.0f %6.1f%%" % (r["index"], eff, label[:48], d.mean(), d.max(), 100.0 * (d.max(axis=2) > 2).mean()))
            if o.png:
                diff = np.minimum(255, d[..., :3] * 4)
                panel = np.concatenate([ref[..., :3], got[..., :3], diff], axis=1).astype(np.uint8)
                Image.fromarray(panel).save(os.path.join(o.png, "%04d_%s.png" % (r["index"], eff.replace(" ", ""))))


if __name__ == "__main__":
    main()
