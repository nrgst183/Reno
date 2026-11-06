#!/usr/bin/env python3
"""Compare Reno's pictures with the snapshots VEGAS took while running tools/vegas/RenoCoverage.cs.

  vegcov_render.py COVDIR --cli PATH/reno_cli [--out DIR] [--only TEXT] [--png N] [--reuse] [--workers 1]

COVDIR is the folder the script wrote: projects/NAME.veg + NAME.json (VEGAS's model manifest), media/ and
snapshots/NAME_0.png .. NAME_2.png. The script takes each snapshot at 10%, 50% and 90% of the project length
(clamped to length - 0.05 s; the length is "lengthNanos" in the manifest, in units of 1/nanosPerSecond), at the
project's full size and best render quality. This tool renders the same times with reno_cli --stills and reports,
per picture, the exact-match flag, the largest channel difference, the share of pixels more than 2 levels off
and the PSNR (over R, G, B), then a table per category (the first word of the scenario name; props_* is split
by its second word).

Nothing from COVDIR is copied anywhere: the work folder (default: a new folder under the system temp dir, or --out)
only holds symbolic links to the media and links to the .veg files so reno_cli's --find-media can find the media
next to them. Output: report.txt (worst first), report.json, and with --png N side-by-side pictures
(VEGAS | Reno | difference x4) of the worst N pictures.

Alpha is dropped (VEGAS's snapshots hold premultiplied colours, see load_rgb).
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("vegcov_render.py needs numpy and Pillow")

FRACTIONS = (0.1, 0.5, 0.9)


def snapshot_times(length):
    """The times RenoCoverage.cs takes its snapshots at (see TakeSnapshots)."""
    return [max(0.0, min(length - 0.05, length * f)) for f in FRACTIONS]


def category(name):
    parts = name.split("_")
    if parts[0] == "props" and len(parts) > 1:
        return "props_" + parts[1]
    return parts[0]


# Differences that are known and not Reno's rendering, found from the manifest: (why, test on the manifest). VEGAS shows these AI effects'
# "install the VEGAS AI bundle" message when the bundle is not installed (Reno draws its own approximation); Add Noise is random;
# VEGAS's snapshot of a project with output rotation 90 / 270 is composed unrotated at the swapped size; Titles & Text uses Verdana,
# which a Linux machine does not have.
AI_EFFECTS = ("VEGAS Colorization", "VEGAS Style Transfer", "VEGAS Smart Upscale")


def effect_names(m):
    names = set()

    def walk(chain):
        for e in chain or []:
            names.add(e.get("name", ""))
    for t in m.get("tracks", []):
        walk(t.get("effects"))
        for ev in t.get("events", []):
            walk(ev.get("effects"))
    for x in m.get("media", []):
        walk(x.get("effects"))
        if x.get("generator"):
            names.add(x["generator"].get("name", ""))
    walk((m.get("videoBus") or {}).get("effects"))
    return names


def explained(m):
    why = []
    names = effect_names(m)
    if names & set(AI_EFFECTS):
        why.append("AI effect (VEGAS without the AI bundle)")
    if "VEGAS Add Noise" in names:
        why.append("random noise")
    if (m.get("project", {}).get("video", {}).get("outputRotation") or "None") in ("QuarterTurnClockwise", "QuarterTurnCounterclockwise"):
        why.append("snapshot of a rotated project")
    if "VEGAS Titles & Text" in names:
        why.append("font")
    return why


def load_rgb(path):
    """R, G, B as saved. VEGAS saves a snapshot's colours premultiplied by its alpha (a faded picture has alpha < 255
    with the faded colour in RGB), which is what the picture looks like over black, so alpha is simply dropped.
    Reno's picture is opaque."""
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.int16)[:, :, :3]


def stats(a, b):
    """a = VEGAS, b = Reno. Returns a dict (size mismatch is reported as such)."""
    resized = False
    if a.shape != b.shape:
        # Same shape turned (output rotation): not comparable. Otherwise VEGAS's snapshot is at another size than the
        # project's frame (pixel aspect ratio applied to the width, or a reduced preview size): scale Reno's picture
        # to it and compare approximately; such a picture is never counted as exact.
        if (a.shape[0], a.shape[1]) == (b.shape[1], b.shape[0]):
            return {"sizeMismatch": True, "vegas": list(a.shape[:2]), "reno": list(b.shape[:2])}
        im = Image.fromarray(b.astype(np.uint8)).resize((a.shape[1], a.shape[0]), Image.BILINEAR)
        b = np.asarray(im, dtype=np.int16)
        resized = True
    d = np.abs(a - b)
    mse = float(np.mean((a - b).astype(np.float64) ** 2))
    return {
        "exact": bool(d.max() == 0) and not resized,
        "resized": resized,
        "maxDiff": int(d.max()),
        "meanDiff": float(d.mean()),
        "over2": float(np.mean(d.max(axis=2) > 2)),
        "psnr": (99.0 if mse == 0 else 10 * math.log10(255.0 * 255.0 / mse)),
    }


def side_by_side(a, b, path):
    if a.shape != b.shape:
        b = np.asarray(Image.fromarray(b.astype(np.uint8)).resize((a.shape[1], a.shape[0]), Image.BILINEAR), dtype=np.int16)
    d = np.clip(np.abs(a - b) * 4, 0, 255)
    img = np.concatenate([a, b, d], axis=1).astype(np.uint8)
    Image.fromarray(img).save(path)


def run_cli(cli, args, timeout=900):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    # Qt ships no fonts: point it at a font folder if the machine has one (text generators draw boxes without)
    for d in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu"):
        if os.path.isdir(d):
            env.setdefault("QT_QPA_FONTDIR", d)
            env.setdefault("LC_ALL", "C.UTF-8")
            break
    try:
        p = subprocess.run([cli] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, env=env)
        return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "timed out"
    except OSError as e:
        return -2, str(e)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("covdir")
    ap.add_argument("--cli", required=True)
    ap.add_argument("--out")
    ap.add_argument("--only", default="")
    ap.add_argument("--png", type=int, default=0)
    ap.add_argument("--reuse", action="store_true", help="keep Reno's pictures from an earlier run")
    ap.add_argument("--plugin-path", default="", help="extra OFX plug-in folder (OFX_PLUGIN_PATH)")
    a = ap.parse_args()

    proj_dir = os.path.join(a.covdir, "projects")
    snap_dir = os.path.join(a.covdir, "snapshots")
    media_dir = os.path.join(a.covdir, "media")
    out = a.out or tempfile.mkdtemp(prefix="vegcov_render_")
    work = os.path.join(out, "work")
    pics = os.path.join(out, "reno")
    os.makedirs(work, exist_ok=True)
    os.makedirs(pics, exist_ok=True)
    if os.path.isdir(media_dir):
        for f in os.listdir(media_dir):
            dst = os.path.join(work, f)
            if not os.path.lexists(dst):
                os.symlink(os.path.abspath(os.path.join(media_dir, f)), dst)

    # every project (one may use another as nested media) is linked next to the media
    for f in os.listdir(proj_dir):
        if f.endswith(".veg"):
            dst = os.path.join(work, f)
            if os.path.lexists(dst):
                os.remove(dst)
            os.symlink(os.path.abspath(os.path.join(proj_dir, f)), dst)

    names = sorted({f[:-6] for f in os.listdir(snap_dir) if f.endswith(".png") and f[-6] == "_"})
    names = [n for n in names if a.only in n]
    rows = []
    for n in names:
        mpath = os.path.join(proj_dir, n + ".json")
        vpath = os.path.join(proj_dir, n + ".veg")
        if not (os.path.exists(mpath) and os.path.exists(vpath)):
            continue
        with open(mpath, encoding="utf-8") as f:
            m = json.load(f)
        length = m["project"]["lengthNanos"] / float(m.get("nanosPerSecond", 10000000))
        times = snapshot_times(length)
        wveg = os.path.join(work, n + ".veg")
        have = all(os.path.exists(os.path.join(pics, "%s_%d.png" % (n, i))) for i in range(3))
        if not (a.reuse and have):
            tf = os.path.join(pics, "_times.txt")
            with open(tf, "w") as f:
                for i, t in enumerate(times):
                    f.write("%s_%d=%r\n" % (n, i, t))
                    p = os.path.join(pics, "%s_%d.png" % (n, i))
                    if os.path.exists(p):
                        os.remove(p)
            env_extra = ["--plugin-path", a.plugin_path] if a.plugin_path else []
            code, text = run_cli(a.cli, ["--stills", tf, "--stills-dir", pics, "-q", "--no-scan", "--find-media"] + env_extra + [wveg])
        else:
            code, text = 0, ""
        for i, t in enumerate(times):
            sp = os.path.join(snap_dir, "%s_%d.png" % (n, i))
            rp = os.path.join(pics, "%s_%d.png" % (n, i))
            row = {"name": "%s_%d" % (n, i), "scenario": n, "category": category(n), "time": t, "explained": explained(m)}
            if not os.path.exists(sp):
                continue
            if not os.path.exists(rp):
                row["missing"] = True
                row["cliExit"] = code
                row["cliText"] = text[-300:]
            else:
                row.update(stats(load_rgb(sp), load_rgb(rp)))
            rows.append(row)
        print("%s: %s" % (n, ", ".join(("-" if "missing" in r else "size" if "sizeMismatch" in r else "=" if r["exact"] else "%d" % r["maxDiff"]) for r in rows if r["scenario"] == n)), flush=True)

    # table per category
    cats = {}
    for r in rows:
        cats.setdefault(r["category"], []).append(r)
    lines = ["%-22s %5s %6s %6s %7s %9s %9s %7s" % ("category", "pics", "exact", "<=2", "nocmp", "worstMax", "minPSNR", "resized")]
    tot = [0, 0, 0, 0]
    for c in sorted(cats):
        rs = cats[c]
        ok = [r for r in rs if "exact" in r]
        ex = sum(1 for r in ok if r["exact"])
        near = sum(1 for r in ok if r["maxDiff"] <= 2 and not r["resized"])
        miss = sum(1 for r in rs if "exact" not in r)
        lines.append("%-22s %5d %6d %6d %7d %9s %9s %7d" % (c, len(rs), ex, near, miss,
                     max([r["maxDiff"] for r in ok], default="-"), ("%.1f" % min(r["psnr"] for r in ok)) if ok else "-", sum(1 for r in rs if r.get("resized"))))
        tot = [tot[0] + len(rs), tot[1] + ex, tot[2] + near, tot[3] + miss]
    lines.append("%-22s %5d %6d %6d %7d" % ("TOTAL", *tot))
    lines.append("")
    lines.append("worst first (maxDiff, share of pixels >2 off, PSNR):")
    ranked = sorted(rows, key=lambda r: (-1 if "exact" not in r else -r["maxDiff"] * 0 - (100 - r["psnr"])))
    for r in ranked:
        if "exact" in r:
            if r["exact"]:
                continue
            lines.append("  %-45s%s max %3d  mean %.3f  >2: %5.1f%%  PSNR %.1f%s" % (r["name"], " (resized)" if r["resized"] else "", r["maxDiff"], r["meanDiff"], r["over2"] * 100, r["psnr"],
                         ("   [" + "; ".join(r["explained"]) + "]") if r.get("explained") else ""))
        elif r.get("sizeMismatch"):
            lines.append("  %-45s size VEGAS %s Reno %s%s" % (r["name"], r["vegas"], r["reno"], ("   [" + "; ".join(r["explained"]) + "]") if r.get("explained") else ""))
        else:
            lines.append("  %-45s no Reno picture (exit %s) %s" % (r["name"], r.get("cliExit"), r.get("cliText", "").replace("\n", " ")))
    text = "\n".join(lines)
    with open(os.path.join(out, "report.txt"), "w") as f:
        f.write(text + "\n")
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(rows, f, indent=1)
    if a.png:
        sd = os.path.join(out, "side")
        os.makedirs(sd, exist_ok=True)
        worst = [r for r in ranked if "exact" in r and not r["exact"] and r["maxDiff"] > 2][:a.png]
        for r in worst:
            side_by_side(load_rgb(os.path.join(snap_dir, r["name"] + ".png")), load_rgb(os.path.join(pics, r["name"] + ".png")),
                         os.path.join(sd, r["name"] + ".png"))
    print()
    print(text)
    print("\nwork folder: " + out)


if __name__ == "__main__":
    main()
