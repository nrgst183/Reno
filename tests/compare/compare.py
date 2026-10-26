#!/usr/bin/env python3
"""Compares two sets of renders of the comparison suite (tests/compare/makesuite.cpp), usually VEGAS's (from Reno
Bridge) against Reno's (run_reno.sh), frame by frame and sample by sample, and writes a report.

  compare.py <bridge folder> [--a vegas] [--b reno] [--out <folder>] [--only id,id] [--title text]

  renders/<a>/<id>.avi|.wav  against  renders/<b>/<id>.avi|.wav, for every test in manifest.json.

Pictures: PSNR (RGB) and SSIM (luma) of each frame, the worst frames, a timing offset (the frame shift that matches
best, -3..3), frame count and frame size differences (a different size is scaled to compare, and said). Sound:
the offset (cross-correlation, to the sample), the level difference (dB), the difference left after lining up
(as a signal-to-difference ratio, without and with the level made equal), per channel. The worst frames are saved
as A | B | difference (x4) pictures. Writes <out>/index.html, <out>/report.md and <out>/summary.json.

Verdicts: "match" (no difference worth naming: mean PSNR >= 45 dB, worst >= 35, sound within 60 dB), "close"
(mean >= 30, worst >= 22, sound within 30 dB, no offset), "differs", "missing" (a render is not there). A test
whose plug-ins Reno cannot run on Linux (manifest: reno_linux_missing, or the render log says an effect "cannot be
loaded") is marked "reno-cannot-run" and not counted as a failure; text tests are marked font-dependent.
Exit code: the number of counted tests that differ or are missing (0 with --no-fail).
"""
import argparse
import html
import json
import os
import re
import subprocess
import sys

import numpy as np

try:
    from scipy.ndimage import gaussian_filter
except Exception:  # pragma: no cover
    gaussian_filter = None


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "stream=codec_type,width,height,r_frame_rate,nb_frames,sample_rate,channels:format=duration",
                          "-of", "json", path], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return json.loads(out.stdout)


def video_info(path):
    j = probe(path)
    if not j:
        return None
    for s in j.get("streams", []):
        if s.get("codec_type") == "video":
            num, den = (s.get("r_frame_rate") or "0/1").split("/")
            return {"w": int(s["width"]), "h": int(s["height"]), "fps": float(num) / max(1.0, float(den))}
    return None


def has_audio(path):
    j = probe(path)
    return bool(j) and any(s.get("codec_type") == "audio" for s in j.get("streams", []))


def read_frames(path, w, h, scale_to=None):
    """All frames as uint8 [n, h, w, 3] RGB (scaled to scale_to=(w, h) when given)."""
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-map", "0:v:0"]
    if scale_to:
        cmd += ["-vf", "scale=%d:%d:flags=bicubic" % scale_to]
        w, h = scale_to
    cmd += ["-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = len(raw) // (w * h * 3)
    return np.frombuffer(raw[:n * w * h * 3], dtype=np.uint8).reshape(n, h, w, 3)


def read_audio(path, rate=48000):
    """Stereo float32 [n, 2] at 'rate' (resampled when the file has another rate)."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0", "-ac", "2", "-ar", str(rate),
                          "-f", "f32le", "-"], capture_output=True).stdout
    a = np.frombuffer(raw, dtype=np.float32)
    return a[:len(a) // 2 * 2].reshape(-1, 2).astype(np.float64)


def psnr(a, b):
    d = (a.astype(np.float64) - b.astype(np.float64))
    mse = float(np.mean(d * d))
    return 99.0 if mse <= 1e-10 else min(99.0, 10 * np.log10(255.0 * 255.0 / mse))


def luma(img):
    f = img.astype(np.float64)
    return 0.299 * f[..., 0] + 0.587 * f[..., 1] + 0.114 * f[..., 2]


def ssim(a, b):
    x, y = luma(a), luma(b)
    if gaussian_filter is None:
        mx, my = x.mean(), y.mean()
        vx, vy, cxy = x.var(), y.var(), ((x - mx) * (y - my)).mean()
        c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
        return float(((2 * mx * my + c1) * (2 * cxy + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2)))
    g = lambda z: gaussian_filter(z, 1.5, truncate=3.5)
    mx, my = g(x), g(y)
    sxx, syy, sxy = g(x * x) - mx * mx, g(y * y) - my * my, g(x * y) - mx * my
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    m = ((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))
    return float(m.mean())


def save_png(path, img):
    h, w = img.shape[:2]
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (w, h), "-i", "-",
                    "-frames:v", "1", path], input=np.ascontiguousarray(img).tobytes(), check=False)


def side_by_side(a, b):
    d = np.abs(a.astype(np.int32) - b.astype(np.int32))
    d = np.clip(d * 4, 0, 255).astype(np.uint8)
    gap = np.full((a.shape[0], 4, 3), 128, np.uint8)
    return np.concatenate([a, gap, b, gap, d], axis=1)


def compare_video(fa, fb, imgdir, tid, shift_range=3):
    res = {}
    ia, ib = video_info(fa), video_info(fb)
    if not ia or not ib:
        return {"error": "no picture in " + (fa if not ia else fb)}
    res["size_a"] = [ia["w"], ia["h"]]
    res["size_b"] = [ib["w"], ib["h"]]
    res["fps_a"], res["fps_b"] = round(ia["fps"], 4), round(ib["fps"], 4)
    scale = None
    if (ia["w"], ia["h"]) != (ib["w"], ib["h"]):
        scale = (ib["w"], ib["h"])
        res["note"] = "frame sizes differ: A was scaled to B's size to compare"
    A = read_frames(fa, ia["w"], ia["h"], scale)
    B = read_frames(fb, ib["w"], ib["h"])
    res["frames_a"], res["frames_b"] = int(len(A)), int(len(B))
    n = min(len(A), len(B))
    if n == 0:
        res["error"] = "no frames to compare"
        return res
    # timing: the shift k (B[i + k] against A[i]) that matches best, over a sample of frames
    idx = list(range(0, n, max(1, n // 24)))
    best_k, best = 0, None
    scores = {}
    for k in range(-shift_range, shift_range + 1):
        vals = [psnr(A[i], B[i + k]) for i in idx if 0 <= i + k < len(B)]
        if not vals:
            continue
        scores[k] = float(np.mean(vals))
        if best is None or scores[k] > best + 1e-9:
            best_k, best = k, scores[k]
    # a shift counts only when it is clearly better than none (still pictures look the same at any shift)
    if best_k != 0 and scores.get(best_k, 0) - scores.get(0, 0) < 1.0:
        best_k = 0
    res["offset_frames"] = best_k
    res["offset_scores"] = {str(k): round(v, 2) for k, v in scores.items()}
    per = []
    for i in range(n):
        p = psnr(A[i], B[i])
        per.append({"frame": i, "psnr": round(p, 2), "ssim": round(ssim(A[i], B[i]), 5),
                    "mad": round(float(np.mean(np.abs(A[i].astype(np.int32) - B[i].astype(np.int32)))), 3),
                    "max": int(np.max(np.abs(A[i].astype(np.int32) - B[i].astype(np.int32))))})
    ps = np.array([x["psnr"] for x in per])
    ss = np.array([x["ssim"] for x in per])
    res["psnr_mean"] = round(float(ps.mean()), 2)
    res["psnr_min"] = round(float(ps.min()), 2)
    res["ssim_mean"] = round(float(ss.mean()), 5)
    res["ssim_min"] = round(float(ss.min()), 5)
    res["max_diff"] = int(max(x["max"] for x in per))
    res["per_frame"] = per
    if best_k != 0:
        shifted = [psnr(A[i], B[i + best_k]) for i in range(n) if 0 <= i + best_k < len(B)]
        res["psnr_mean_shifted"] = round(float(np.mean(shifted)), 2)
    worst = sorted(per, key=lambda x: x["psnr"])[:3]
    res["worst"] = []
    if imgdir and worst[0]["psnr"] < 99:
        os.makedirs(imgdir, exist_ok=True)
        for w in worst:
            if w["psnr"] >= 99:
                continue
            name = "%s_f%04d.png" % (tid, w["frame"])
            save_png(os.path.join(imgdir, name), side_by_side(A[w["frame"]], B[w["frame"]]))
            res["worst"].append({"frame": w["frame"], "psnr": w["psnr"], "ssim": w["ssim"], "image": "img/" + name})
    return res


def db(x):
    return -999.0 if x <= 1e-12 else 20 * np.log10(x)


def compare_audio(fa, fb, rate=48000, max_shift=0.25):
    A, B = read_audio(fa, rate), read_audio(fb, rate)
    res = {"samples_a": int(len(A)), "samples_b": int(len(B))}
    n = min(len(A), len(B))
    if n == 0:
        res["error"] = "no sound to compare" if len(A) == len(B) else "one of them has no sound"
        return res
    a, b = A[:n], B[:n]
    # offset: cross-correlation of the mono mixes (FFT), within +-max_shift s
    ma, mb = a.mean(axis=1), b.mean(axis=1)
    m = 1 << int(np.ceil(np.log2(2 * n)))
    xc = np.fft.irfft(np.fft.rfft(mb, m) * np.conj(np.fft.rfft(ma, m)), m)
    lim = min(int(max_shift * rate), n - 1)
    lags = np.concatenate([np.arange(0, lim + 1), np.arange(-lim, 0)])
    vals = np.concatenate([xc[:lim + 1], xc[m - lim:]])
    lag = 0
    if np.max(np.abs(vals)) > 1e-9:
        lag = int(lags[int(np.argmax(vals))])
        # a periodic sound matches at many lags: keep 0 unless the best is clearly better
        if lag != 0 and vals[int(np.argmax(vals))] < 1.02 * xc[0]:
            lag = 0
    res["offset_samples"] = lag
    res["offset_ms"] = round(1000.0 * lag / rate, 3)
    if lag > 0:
        a2, b2 = a[:n - lag], b[lag:]
    elif lag < 0:
        a2, b2 = a[-lag:], b[:n + lag]
    else:
        a2, b2 = a, b
    rms_a = np.sqrt(np.mean(a2 ** 2, axis=0))
    rms_b = np.sqrt(np.mean(b2 ** 2, axis=0))
    res["rms_db_a"] = [round(float(db(x)), 2) for x in rms_a]
    res["rms_db_b"] = [round(float(db(x)), 2) for x in rms_b]
    tot_a, tot_b = float(np.sqrt(np.mean(a2 ** 2))), float(np.sqrt(np.mean(b2 ** 2)))
    res["level_diff_db"] = round(float(db(tot_b) - db(tot_a)), 3) if tot_a > 1e-9 and tot_b > 1e-9 else None
    res["peak_a"], res["peak_b"] = round(float(np.max(np.abs(a2))), 5), round(float(np.max(np.abs(b2))), 5)
    d = a2 - b2
    sig = float(np.mean(a2 ** 2))
    err = float(np.mean(d ** 2))
    res["max_sample_diff"] = round(float(np.max(np.abs(d))), 6)
    res["sdr_db"] = 120.0 if err <= 1e-20 else round(10 * np.log10(max(sig, 1e-20) / err), 2)
    if sig <= 1e-12 and float(np.mean(b2 ** 2)) <= 1e-12:
        res["sdr_db"] = 120.0          # both silent
    # with the level made equal (a gain difference alone is not counted twice)
    g = float(np.sum(a2 * b2) / max(np.sum(b2 * b2), 1e-20))
    err2 = float(np.mean((a2 - g * b2) ** 2))
    res["gain_matched_sdr_db"] = 120.0 if err2 <= 1e-20 else round(10 * np.log10(max(sig, 1e-20) / err2), 2)
    res["gain_to_match"] = round(g, 5)
    return res


def find_render(folder, tid):
    for ext in (".avi", ".wav", ".mov", ".mp4"):
        p = os.path.join(folder, tid + ext)
        if os.path.exists(p):
            return p
    return None


def missing_plugins(log):
    if not log or not os.path.exists(log):
        return []
    out = []
    with open(log, errors="replace") as f:
        for line in f:
            m = re.match(r"\s+(\S+) \(OpenFX\): ", line)
            if m:
                out.append(m.group(1))
    return out


def verdict(r, thresholds):
    v, a = r.get("video"), r.get("audio")
    if r.get("missing"):
        return "missing"
    worst = "match"
    order = ["match", "close", "differs"]

    def lower(x):
        nonlocal worst
        if order.index(x) > order.index(worst):
            worst = x
    if v:
        if v.get("error"):
            lower("differs")
        else:
            if v["psnr_mean"] >= thresholds["match_mean"] and v["psnr_min"] >= thresholds["match_min"] and v["offset_frames"] == 0 \
                    and v["frames_a"] == v["frames_b"] and v["size_a"] == v["size_b"]:
                pass
            elif v["psnr_mean"] >= thresholds["close_mean"] and v["psnr_min"] >= thresholds["close_min"] and v["offset_frames"] == 0:
                lower("close")
            else:
                lower("differs")
    if a:
        if a.get("error"):
            lower("differs")
        elif a["sdr_db"] >= thresholds["audio_match"] and a["offset_samples"] == 0:
            pass
        elif a["sdr_db"] >= thresholds["audio_close"] and a["offset_samples"] == 0:
            lower("close")
        else:
            lower("differs")
    return worst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--a", default="vegas")
    ap.add_argument("--b", default="reno")
    ap.add_argument("--out")
    ap.add_argument("--only")
    ap.add_argument("--title")
    ap.add_argument("--no-fail", action="store_true")
    ap.add_argument("--match-mean", type=float, default=45)
    ap.add_argument("--match-min", type=float, default=35)
    ap.add_argument("--close-mean", type=float, default=30)
    ap.add_argument("--close-min", type=float, default=22)
    ap.add_argument("--audio-match", type=float, default=60)
    ap.add_argument("--audio-close", type=float, default=30)
    o = ap.parse_args()
    th = {"match_mean": o.match_mean, "match_min": o.match_min, "close_mean": o.close_mean, "close_min": o.close_min,
          "audio_match": o.audio_match, "audio_close": o.audio_close}
    root = os.path.abspath(o.root)
    man = json.load(open(os.path.join(root, "manifest.json")))
    out = os.path.abspath(o.out or os.path.join(root, "report_%s_vs_%s" % (o.a, o.b)))
    os.makedirs(os.path.join(out, "img"), exist_ok=True)
    only = set(o.only.split(",")) if o.only else None
    da, dbb = os.path.join(root, "renders", o.a), os.path.join(root, "renders", o.b)
    results = []
    for t in man["tests"]:
        tid = t["id"]
        if only and tid not in only:
            continue
        r = {"id": tid, "title": t["title"], "font_dependent": t.get("font_dependent", False)}
        fa, fb = find_render(da, tid), find_render(dbb, tid)
        miss = list(t.get("reno_linux_missing", []))
        for side in (o.a, o.b):
            for p in missing_plugins(os.path.join(root, "renders", side, tid + ".log")):
                if p not in miss:
                    miss.append(p)
        r["reno_cannot_run"] = miss
        if not fa or not fb:
            r["missing"] = [x for x, f in ((o.a, fa), (o.b, fb)) if not f]
        else:
            r["file_a"], r["file_b"] = os.path.relpath(fa, root), os.path.relpath(fb, root)
            if not t.get("audio_only") and video_info(fa):
                r["video"] = compare_video(fa, fb, os.path.join(out, "img"), tid)
            if has_audio(fa) or has_audio(fb):
                r["audio"] = compare_audio(fa, fb, int(t.get("sample_rate", 48000)))
        r["verdict"] = verdict(r, th)
        r["counted"] = not miss
        if miss:
            r["mark"] = "reno-cannot-run"
        results.append(r)
        v, a = r.get("video") or {}, r.get("audio") or {}
        print("%-8s %-28s %s%s%s" % (r["verdict"], tid,
              ("video %.1f dB (min %.1f) ssim %.4f%s  " % (v["psnr_mean"], v["psnr_min"], v["ssim_mean"],
               (" offset %+d" % v["offset_frames"]) if v.get("offset_frames") else "")) if "psnr_mean" in v else "",
              ("audio sdr %.1f dB level %s offset %s  " % (a["sdr_db"], a.get("level_diff_db"), a["offset_samples"])) if "sdr_db" in a else "",
              ("[reno cannot run: %s]" % ", ".join(miss)) if miss else ""))
        sys.stdout.flush()

    counted = [r for r in results if r["counted"]]
    summary = {"a": o.a, "b": o.b, "tests": len(results),
               "counted": len(counted),
               "match": sum(r["verdict"] == "match" for r in counted),
               "close": sum(r["verdict"] == "close" for r in counted),
               "differs": sum(r["verdict"] == "differs" for r in counted),
               "missing": sum(r["verdict"] == "missing" for r in counted),
               "reno_cannot_run": len(results) - len(counted),
               "thresholds": th}
    light = []
    for r in results:
        x = dict(r)
        if "video" in x and "per_frame" in x["video"]:
            x["video"] = dict(x["video"])
            x["video"]["per_frame_psnr"] = [f["psnr"] for f in x["video"].pop("per_frame")]
        light.append(x)
    json.dump({"summary": summary, "results": light}, open(os.path.join(out, "summary.json"), "w"), indent=1)
    write_md(os.path.join(out, "report.md"), o, summary, results)
    write_html(os.path.join(out, "index.html"), o, summary, results)
    print("\n%(tests)d tests: %(match)d match, %(close)d close, %(differs)d differ, %(missing)d missing; "
          "%(reno_cannot_run)d not counted (plug-ins Reno cannot run here)" % summary)
    print("report:", os.path.join(out, "index.html"))
    bad = summary["differs"] + summary["missing"]
    return 0 if o.no_fail else min(bad, 125)


def fmt_v(v):
    if not v:
        return ""
    if v.get("error"):
        return v["error"]
    s = "%.1f dB (min %.1f), SSIM %.4f" % (v["psnr_mean"], v["psnr_min"], v["ssim_mean"])
    if v["offset_frames"]:
        s += ", offset %+d frame(s) (%.1f dB shifted)" % (v["offset_frames"], v.get("psnr_mean_shifted", 0))
    if v["frames_a"] != v["frames_b"]:
        s += ", frames %d vs %d" % (v["frames_a"], v["frames_b"])
    if v["size_a"] != v["size_b"]:
        s += ", size %dx%d vs %dx%d" % tuple(v["size_a"] + v["size_b"])
    return s


def fmt_a(a):
    if not a:
        return ""
    if a.get("error"):
        return a["error"]
    s = "SDR %.1f dB" % a["sdr_db"]
    if a.get("level_diff_db") is not None and abs(a["level_diff_db"]) >= 0.05:
        s += ", level %+.2f dB" % a["level_diff_db"]
    if a["offset_samples"]:
        s += ", offset %+d samples (%+.2f ms)" % (a["offset_samples"], a["offset_ms"])
    if a["samples_a"] != a["samples_b"]:
        s += ", length %d vs %d samples" % (a["samples_a"], a["samples_b"])
    return s


def write_md(path, o, s, results):
    L = ["# %s" % (o.title or "%s vs %s" % (o.a, o.b)), "",
         "%d tests: **%d match, %d close, %d differ, %d missing**; %d not counted (plug-ins Reno cannot run on Linux)." %
         (s["tests"], s["match"], s["close"], s["differs"], s["missing"], s["reno_cannot_run"]), "",
         "A = renders/%s, B = renders/%s. PSNR/SSIM per frame, sound as signal-to-difference ratio (SDR)." % (o.a, o.b), "",
         "| test | verdict | picture | sound | notes |", "|---|---|---|---|---|"]
    for r in results:
        notes = []
        if r.get("reno_cannot_run"):
            notes.append("Reno cannot run " + ", ".join(r["reno_cannot_run"]))
        if r.get("font_dependent"):
            notes.append("font-dependent")
        if r.get("missing"):
            notes.append("no render from " + ", ".join(r["missing"]))
        if (r.get("video") or {}).get("note"):
            notes.append(r["video"]["note"])
        L.append("| %s | %s | %s | %s | %s |" % (r["id"], r["verdict"], fmt_v(r.get("video")), fmt_a(r.get("audio")), "; ".join(notes)))
    open(path, "w").write("\n".join(L) + "\n")


def write_html(path, o, s, results):
    e = html.escape
    title = o.title or "%s vs %s" % (o.a, o.b)
    H = ["<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>",
         "<title>%s</title>" % e(title),
         "<style>body{font:14px system-ui,sans-serif;margin:16px;color:#222;background:#fff}table{border-collapse:collapse}"
         "td,th{border:1px solid #ccc;padding:3px 6px;vertical-align:top}th{background:#eee}.match{background:#dfd}"
         ".close{background:#ffd}.differs{background:#fdd}.missing{background:#ddd}.marked{opacity:.6}"
         "img{max-width:100%;image-rendering:pixelated;border:1px solid #888}code{font-size:12px}</style>",
         "<h1>%s</h1>" % e(title),
         "<p>%d tests: <b>%d match, %d close, %d differ, %d missing</b>; %d not counted (plug-ins Reno cannot run on Linux).</p>" %
         (s["tests"], s["match"], s["close"], s["differs"], s["missing"], s["reno_cannot_run"]),
         "<p>A = <code>renders/%s</code>, B = <code>renders/%s</code>. Pictures below: A | B | difference x4.</p>" % (e(o.a), e(o.b)),
         "<table><tr><th>test</th><th>verdict</th><th>picture</th><th>sound</th><th>notes</th></tr>"]
    for r in results:
        notes = []
        if r.get("reno_cannot_run"):
            notes.append("Reno cannot run " + ", ".join(r["reno_cannot_run"]))
        if r.get("font_dependent"):
            notes.append("font-dependent")
        if r.get("missing"):
            notes.append("no render from " + ", ".join(r["missing"]))
        cls = r["verdict"] + (" marked" if r.get("reno_cannot_run") else "")
        H.append("<tr class='%s'><td><a href='#%s'>%s</a><br><small>%s</small></td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" %
                 (cls, e(r["id"]), e(r["id"]), e(r["title"]), e(r["verdict"]), e(fmt_v(r.get("video"))), e(fmt_a(r.get("audio"))), e("; ".join(notes))))
    H.append("</table>")
    for r in results:
        v = r.get("video") or {}
        if not v.get("worst") and not r.get("audio"):
            continue
        H.append("<h3 id='%s'>%s - %s</h3><p>%s</p>" % (e(r["id"]), e(r["id"]), e(r["verdict"]), e(r["title"])))
        for w in v.get("worst", []):
            H.append("<p>frame %d: PSNR %.1f dB, SSIM %.4f<br><img src='%s'></p>" % (w["frame"], w["psnr"], w["ssim"], e(w["image"])))
        a = r.get("audio")
        if a and not a.get("error"):
            H.append("<p>sound: RMS per channel A %s dB, B %s dB; peak A %s, B %s; SDR %s dB, %s dB with the level matched (gain %s)</p>" %
                     (a["rms_db_a"], a["rms_db_b"], a["peak_a"], a["peak_b"], a["sdr_db"], a["gain_matched_sdr_db"], a["gain_to_match"]))
    open(path, "w").write("\n".join(H) + "\n")


if __name__ == "__main__":
    sys.exit(main())
