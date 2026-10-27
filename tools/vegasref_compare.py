#!/usr/bin/env python3
"""Compare Reno's built-in VEGAS effects with VEGAS's own output, test by test.

The reference folder is what `reno_cli vegasfx --keep-real` writes on a machine with VEGAS installed
(<ref>/reference/<effect>_<context>/scenarios.json, real.json and <test>_<frame>.bgra, premultiplied BGRA, bottom row first).
Each effect is rendered with Reno's built-in (reno_cli vegasfxrun) and compared per test: mean / largest difference
(0..255 over B, G, R, A) and the share of pixels more than 2 levels off. --png DIR writes each effect's worst test
as VEGAS | Reno | difference x4.

  vegasref_compare.py --ref DIR --cli PATH/reno_cli [--only TEXT] [--png OUTDIR] [--size 320x180] [--json OUT]
"""
import argparse, json, os, subprocess, sys, tempfile


def load_json(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


# Reference results that Reno is expected to differ from, with the reason. (effect name substring, test index) -> reason
KNOWN_ANOMALIES = {
    ("layerdimensionality", 3): "light at (3,3,6): the reference shows no inner shadow, but VEGAS's decoded render code (run on Linux) draws one "
                                 "for that light; the host's value for the parameter is unexplained, so Reno follows the decoded render",
    ("broadcastcolors", 24): "75IREsetup=0 with Studio on: Vfx1 run on Linux differs from this capture by 462 pixels, the capture equals both on; redo in VEGAS",
}


def known_anomaly(name, test):
    for (sub, t), why in KNOWN_ANOMALIES.items():
        if t == test and sub.lower() in name.lower():
            return why
    return None


def diff_stats(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return None
    tot = 0
    mx = 0
    big = 0
    for i in range(0, n, 4):
        px = 0
        for k in range(4):
            d = abs(a[i + k] - b[i + k])
            tot += d
            if d > mx:
                mx = d
            if d > px:
                px = d
        if px > 2:
            big += 1
    return tot / n, mx, 100.0 * big / (n // 4)


def run_builtin(cli, eff_id, ctx, scen, w, h, frames, tmp, max_params):
    out = os.path.join(tmp, "b.json")
    first = 0
    done_any = False
    for _ in range(50):
        args = [cli, "vegasfxrun", "--source", "builtin", "--id", eff_id, "--context", ctx, "--size", "%dx%d" % (w, h),
                "--frames", str(frames), "--dir", tmp, "--tag", "b", "--out", out, "--max-params", str(max_params),
                "--first", str(first), "--scenarios-in", scen, "--presets", "builtin"]
        if os.path.exists(out):
            os.remove(out)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600, env=env)
        if not os.path.exists(out):
            break
        r = load_json(out)
        done_any = True
        if r.get("done"):
            break
        # it died in a test: carry on after the last one it reached
        idx = [s.get("index", -1) for s in r.get("scenarios", [])]
        first = (max(idx) + 1) if idx else first + 1
    return done_any


def write_png(path, w, h, panels):
    try:
        from PIL import Image
    except ImportError:
        return
    img = Image.new("RGB", (w * len(panels), h))
    for pi, (data, amp) in enumerate(panels):
        px = img.load()
        for y in range(h):
            row = (h - 1 - y) * w * 4
            for x in range(w):
                o = row + x * 4
                if o + 3 >= len(data):
                    continue
                b, g, r, a = data[o], data[o + 1], data[o + 2], data[o + 3]
                if amp:
                    r, g, b = min(255, r * amp), min(255, g * amp), min(255, b * amp)
                else:
                    bg = 150 if ((x >> 3) + (y >> 3)) & 1 else 100
                    inv = 255 - a
                    r, g, b = min(255, r + bg * inv // 255), min(255, g + bg * inv // 255), min(255, b + bg * inv // 255)
                px[pi * w + x, y] = (r, g, b)
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--png", default="")
    ap.add_argument("--json", default="")
    ap.add_argument("--size", default="320x180")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--max-params", type=int, default=12)
    ap.add_argument("--quiet", action="store_true", help="one line per effect")
    ap.add_argument("--keep", default="", help="folder to copy Reno's raw pictures to (<effect>/b_<test>_<frame>.bgra)")
    o = ap.parse_args()
    w, h = (int(v) for v in o.size.split("x"))
    root = os.path.join(o.ref, "reference")
    results = {}
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        if o.only and o.only.lower() not in name.lower():
            continue
        if not os.path.exists(os.path.join(d, "real.json")):
            continue
        real = load_json(os.path.join(d, "real.json"))
        scen = os.path.join(d, "scenarios.json")
        scen_list = load_json(scen)
        eff_id, ctx = real.get("id", ""), real.get("context", "")
        with tempfile.TemporaryDirectory() as tmp:
            ok = run_builtin(o.cli, eff_id, ctx, scen, w, h, o.frames, tmp, o.max_params)
            if o.keep:
                import shutil
                kd = os.path.join(o.keep, name)
                os.makedirs(kd, exist_ok=True)
                for fn in os.listdir(tmp):
                    if fn.endswith(".bgra"):
                        shutil.copy(os.path.join(tmp, fn), kd)
            rows = []
            worst = None
            for i, s in enumerate(scen_list):
                for f in range(int(s.get("frames", 1))):
                    rp = os.path.join(d, "%d_%d.bgra" % (i, f))
                    bp = os.path.join(tmp, "b_%d_%d.bgra" % (i, f))
                    if not os.path.exists(rp):
                        continue
                    ra = open(rp, "rb").read()
                    if not os.path.exists(bp):
                        rows.append((i, f, s.get("name", ""), None))
                        continue
                    rb = open(bp, "rb").read()
                    st = diff_stats(ra, rb)
                    rows.append((i, f, s.get("name", ""), st))
                    if st and (worst is None or st[0] > worst[0]):
                        worst = (st[0], i, f, ra, rb)
            mean_all = [r[3][0] for r in rows if r[3]]
            missing = sum(1 for r in rows if r[3] is None)
            avg = sum(mean_all) / len(mean_all) if mean_all else -1
            mx = max(mean_all) if mean_all else -1
            exact = sum(1 for r in rows if r[3] and r[3][1] == 0)
            results[name] = {"id": eff_id, "context": ctx, "tests": len(rows), "exact": exact, "missing": missing,
                             "meanOfMeans": avg, "worstMean": mx,
                             "rows": [{"test": r[0], "frame": r[1], "name": r[2], "mean": r[3][0] if r[3] else None,
                                       "max": r[3][1] if r[3] else None, "over2pct": r[3][2] if r[3] else None} for r in rows]}
            print("%-62s tests %3d  exact %3d  missing %2d  mean %7.3f  worst %7.3f%s" % (name, len(rows), exact, missing, avg, mx,
                  "" if ok else "  (Reno's built-in did not run)"))
            if not o.quiet:
                for i, f, nm, st in rows:
                    if st is None:
                        print("     %3d.%d %-48s Reno made no picture" % (i, f, nm[:48]))
                    elif st[1] > 0:
                        why = known_anomaly(name, i)
                        print("     %3d.%d %-48s mean %7.3f  max %3d  >2: %5.1f%%%s" % (i, f, nm[:48], st[0], st[1], st[2],
                              ("  [known anomaly: " + why + "]") if why else ""))
            if o.png and worst:
                os.makedirs(o.png, exist_ok=True)
                diff = bytes(min(255, abs(a - b)) if (k % 4) != 3 else 255 for k, (a, b) in enumerate(zip(worst[3], worst[4])))
                write_png(os.path.join(o.png, "%s_t%d_f%d.png" % (name, worst[1], worst[2])), w, h,
                          [(worst[3], 0), (worst[4], 0), (diff, 4)])
            sys.stdout.flush()
    if o.json:
        with open(o.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
