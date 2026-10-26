#!/usr/bin/env python3
"""Compare Reno with VEGAS on the output of the VEGAS script tools/scripts/"Reno Plugin Fuzz.cs".

FOLDER holds fuzz.veg, manifest.json, snap_NNNN.png (VEGAS's picture of each video test; transitions _25 _50 _75),
fuzz_audio.wav (VEGAS's audio render) and the media files. This script opens fuzz.veg with reno_cli and
  * renders the same pictures (reno_cli --stills) and reports mean / largest difference over R, G, B and the share of
    pixels more than 2 levels off;
  * renders the audio and compares each audio test's range (the 2 s event and its 1.5 s tail): correlation, lag in
    samples (+ Reno late, - early), level difference in dB and the residual relative to VEGAS in dB;
  * marks tests as "not available" when Reno lacks the plug-in (from fx_status.tsv).

Writes report.txt and report.json (worst first; score is the relative error in percent) and, with --png N, side-by-side
pictures (VEGAS | Reno | difference x4) of the worst N video / transition tests.

  fuzz_compare.py FOLDER --cli PATH/reno_cli [--out DIR] [--png N] [--only TEXT] [--kind video|transition|audio]
                  [--top N] [--chunk N] [--scan] [--reuse]

--only: plug-in name or variant contains TEXT. --top: limit report lines. --chunk: pictures per reno_cli process (a crash
loses only its chunk, which is retried one by one). --scan: look for new plug-ins first (default --no-scan). --reuse: keep
Reno's pictures and sound from an earlier run.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys
import time

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("fuzz_compare.py needs numpy and Pillow")

RANGE_TOLERANCE = 1e-6


# ---------------------------------------------------------------------------------------------------------- reno_cli
def run_cli(cli, args, timeout=900):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    try:
        p = subprocess.run([cli] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, env=env)
        return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "timed out"
    except OSError as e:
        return -2, str(e)


def render_stills(cli, veg, wants, outdir, chunk, scan, log):
    """wants: [(name, seconds)]. Returns {name: path or None}. Chunked; a chunk that leaves pictures out is retried one by one."""
    os.makedirs(outdir, exist_ok=True)
    result = {}

    def attempt(items):
        tf = os.path.join(outdir, "_times.txt")
        with open(tf, "w") as f:
            for name, t in items:
                f.write("%s=%r\n" % (name, t))
        args = ["--stills", tf, "--stills-dir", outdir, "-q"]
        if not scan:
            args.append("--no-scan")
        args.append(veg)
        for name, _ in items:
            p = os.path.join(outdir, name + ".png")
            if os.path.exists(p):
                os.remove(p)
        code, text = run_cli(cli, args)
        for name, _ in items:
            p = os.path.join(outdir, name + ".png")
            result[name] = p if os.path.exists(p) else None
        return code, text

    for i in range(0, len(wants), chunk):
        part = wants[i:i + chunk]
        code, text = attempt(part)
        left = [w for w in part if result.get(w[0]) is None]
        if left and len(part) > 1:
            log("  chunk %d..%d: %d of %d pictures missing (exit %s); retrying one by one" % (i, i + len(part) - 1, len(left), len(part), code))
            for w in left:
                attempt([w])
        elif left:
            log("  picture %s missing (exit %s): %s" % (left[0][0], code, text.strip().splitlines()[-1] if text.strip() else ""))
    return result


def read_fx_status(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            p = line.rstrip("\n").split("\t")
            while len(p) < 8:
                p.append("")
            try:
                rows.append(dict(start=float(p[0]), length=float(p[1]), scope=p[2], kind=p[3], name=p[4], id=p[5], status=p[6], why=p[7]))
            except ValueError:
                pass
    return rows


# ---------------------------------------------------------------------------------------------------------- pictures
def load_rgb(path):
    im = Image.open(path)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
        bg.alpha_composite(im)
        im = bg
    return np.asarray(im.convert("RGB"), dtype=np.int16)


def picture_stats(a, b):
    resized = False
    if a.shape != b.shape:
        b = np.asarray(Image.fromarray(b.astype(np.uint8)).resize((a.shape[1], a.shape[0]), Image.LANCZOS), dtype=np.int16)
        resized = True
    d = np.abs(a - b)
    px = d.max(axis=2)
    return dict(mean=float(d.mean()), max=int(d.max()), pct_over_2=float(100.0 * (px > 2).mean()), resized=resized), b


def side_by_side(a, b, path, title):
    d = np.clip(np.abs(a - b) * 4, 0, 255)
    w = a.shape[1]
    h = a.shape[0]
    canvas = np.zeros((h, w * 3 + 8, 3), dtype=np.uint8)
    canvas[:, :w] = a
    canvas[:, w + 4:2 * w + 4] = b
    canvas[:, 2 * w + 8:] = d
    im = Image.fromarray(canvas)
    scale = 2 if w <= 480 else 1
    if scale > 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    try:
        from PIL import ImageDraw
        dr = ImageDraw.Draw(im)
        dr.rectangle([0, 0, im.width, 12], fill=(0, 0, 0))
        dr.text((3, 0), title[:150], fill=(255, 255, 255))
    except Exception:
        pass
    im.save(path)


# ---------------------------------------------------------------------------------------------------------- sound
def read_wav(path):
    """(samples float32 [n, ch], rate). PCM 8/16/24/32, float 32/64, also WAVE_FORMAT_EXTENSIBLE."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise ValueError("not a WAVE file")
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", data[body:body + 16])
            if tag == 0xFFFE and size >= 40:
                tag = struct.unpack("<H", data[body + 24:body + 26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            end = len(data) if size == 0xFFFFFFFF or body + size > len(data) else body + size
            pcm = data[body:end]
            break
        pos = body + size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError("no fmt/data chunk")
    tag, ch, rate, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(pcm[:len(pcm) // 4 * 4], dtype="<f4").astype(np.float32)
    elif tag == 3 and bits == 64:
        x = np.frombuffer(pcm[:len(pcm) // 8 * 8], dtype="<f8").astype(np.float32)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(pcm[:len(pcm) // 2 * 2], dtype="<i2").astype(np.float32) / 32768.0
    elif tag == 1 and bits == 24:
        n = len(pcm) // 3
        b = np.frombuffer(pcm[:n * 3], dtype=np.uint8).reshape(n, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float32) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(pcm[:len(pcm) // 4 * 4], dtype="<i4").astype(np.float32) / 2147483648.0
    elif tag == 1 and bits == 8:
        x = (np.frombuffer(pcm, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    else:
        raise ValueError("unsupported WAVE format %d/%d bits" % (tag, bits))
    n = len(x) // ch
    return x[:n * ch].reshape(n, ch), rate


def seg(x, a, b):
    """Samples a..b (zero padded past the end) as [n, 2]."""
    n = b - a
    out = np.zeros((n, 2), dtype=np.float64)
    lo = max(a, 0)
    hi = min(b, len(x))
    if hi > lo:
        s = x[lo:hi]
        if s.shape[1] == 1:
            s = np.repeat(s, 2, axis=1)
        out[lo - a:hi - a] = s[:, :2]
    return out


def rms(x):
    return float(math.sqrt(float(np.mean(x * x)))) if x.size else 0.0


def db(r):
    return 20.0 * math.log10(r) if r > 1e-12 else -240.0


def best_lag(v, r, maxlag):
    """The k (samples) for which r[n + k] matches v[n] best: Reno is late by k (negative: early)."""
    a = v.sum(axis=1)
    b = r.sum(axis=1)
    n = len(a)
    if n == 0 or not np.any(a) or not np.any(b):
        return 0
    size = 1
    while size < 2 * n:
        size *= 2
    fa = np.fft.rfft(a, size)
    fb = np.fft.rfft(b, size)
    c = np.fft.irfft(np.conj(fa) * fb, size)        # c[k] = sum_n a[n] * b[n + k]
    ks = list(range(0, maxlag + 1)) + list(range(-maxlag, 0))
    vals = np.array([c[k % size] for k in ks])
    return int(ks[int(np.argmax(vals))])


def shifted(r, k):
    """Reno moved earlier by k samples (r'[n] = r[n + k]), zero padded."""
    if k == 0:
        return r
    out = np.zeros_like(r)
    if k > 0:
        out[:len(r) - k] = r[k:]
    else:
        out[-k:] = r[:len(r) + k]
    return out


def audio_stats(v, r, rate, tail_from):
    res = {}
    rv, rr = rms(v), rms(r)
    res["vegas_rms_db"] = db(rv)
    res["reno_rms_db"] = db(rr)
    res["level_diff_db"] = db(rr) - db(rv) if rv > 1e-9 and rr > 1e-9 else (0.0 if rv <= 1e-9 and rr <= 1e-9 else None)
    lag = best_lag(v, r, int(0.1 * rate))
    res["lag_samples"] = lag
    r2 = shifted(r, lag)
    a, b = v.reshape(-1), r2.reshape(-1)
    if rv > 1e-9 and rr > 1e-9:
        res["correlation"] = float(np.corrcoef(a, b)[0, 1])
    else:
        res["correlation"] = 1.0 if (rv <= 1e-9 and rr <= 1e-9) else None
    d = r2 - v
    res["residual_db"] = db(rms(d) / rv) if rv > 1e-9 else (db(rms(d)) if rms(d) > 1e-9 else -240.0)
    res["residual_pct"] = 100.0 * rms(d) / rv if rv > 1e-9 else (100.0 if rms(d) > 1e-6 else 0.0)
    res["peak_diff"] = float(np.max(np.abs(d))) if d.size else 0.0
    tv, tr, td = v[tail_from:], r2[tail_from:], d[tail_from:]
    res["tail_vegas_db"] = db(rms(tv))
    res["tail_reno_db"] = db(rms(tr))
    res["tail_residual_db"] = db(rms(td) / rms(tv)) if rms(tv) > 1e-9 else (db(rms(td)) if rms(td) > 1e-9 else -240.0)
    return res


# ---------------------------------------------------------------------------------------------------------- the tests
def test_label(t):
    return "%s / %s / %s" % (t["kind"], t["plugin"], t["variant"])


def availability(t, rows, kind_word):
    """None if Reno has what the test needs, else the reason it is not available."""
    s, e = t["start_s"], t["start_s"] + t["length_s"]
    inside = [r for r in rows if r["kind"] == kind_word and s - RANGE_TOLERANCE <= r["start"] < e - RANGE_TOLERANCE and r["scope"] in ("event", "transition")]
    if t["kind"] == "transition":
        inside = [r for r in inside if r["scope"] == "transition"]
    else:
        inside = [r for r in inside if r["scope"] == "event"]
    for r in inside:
        if r["status"] != "ok":
            return "%s: %s" % (r["name"], r["why"] or "plug-in not found")
    if not inside and t["kind"] != "transition":
        return "no such effect in the project Reno read (not installed in Reno, or the .veg import dropped it)"
    return None


def main():
    ap = argparse.ArgumentParser(description="Compare Reno with VEGAS on the output of 'Reno Plugin Fuzz.cs'")
    ap.add_argument("folder")
    ap.add_argument("--cli", required=True, help="path of reno_cli")
    ap.add_argument("--out", help="output folder (default FOLDER/compare)")
    ap.add_argument("--png", type=int, default=0, help="side by side pictures of the worst N video/transition tests")
    ap.add_argument("--only", help="only tests whose plug-in or variant contains this text")
    ap.add_argument("--kind", choices=["video", "transition", "audio"], help="only this kind of test")
    ap.add_argument("--top", type=int, default=0, help="lines in the report (default all)")
    ap.add_argument("--chunk", type=int, default=40, help="pictures per reno_cli process")
    ap.add_argument("--scan", action="store_true", help="let reno_cli look for new plug-in files first")
    ap.add_argument("--reuse", action="store_true", help="keep Reno's pictures / sound from an earlier run")
    args = ap.parse_args()

    folder = os.path.abspath(args.folder)
    out = os.path.abspath(args.out or os.path.join(folder, "compare"))
    os.makedirs(out, exist_ok=True)

    def log(s):
        print(s, flush=True)

    mpath = os.path.join(folder, "manifest.json")
    veg = os.path.join(folder, "fuzz.veg")
    if not os.path.exists(mpath) or not os.path.exists(veg):
        sys.exit("%s needs manifest.json and fuzz.veg" % folder)
    with open(mpath, encoding="utf-8") as f:
        man = json.load(f)
    fps = float(man.get("project", {}).get("fps", 30))
    rate = int(man.get("project", {}).get("sample_rate", 48000))
    tests = [t for t in man.get("tests", [])]
    if args.only:
        low = args.only.lower()
        tests = [t for t in tests if low in t["plugin"].lower() or low in t["variant"].lower()]
    if args.kind:
        tests = [t for t in tests if t["kind"] == args.kind]
    log("%d tests (%s)" % (len(tests), ", ".join("%d %s" % (sum(1 for t in tests if t["kind"] == k), k) for k in ("video", "transition", "audio"))))

    results = []

    def add(t, **kw):
        r = dict(id=t["id"], kind=t["kind"], plugin=t["plugin"], variant=t["variant"], uid=t.get("uid", ""), ofx=t.get("ofx", False),
                 start_s=t["start_s"], length_s=t["length_s"], status="ok", note="", score=None)
        r.update(kw)
        results.append(r)
        return r

    # ---- pictures
    vtests = [t for t in tests if t["kind"] in ("video", "transition")]
    reno_dir = os.path.join(out, "reno")
    fx_rows = []
    if vtests or any(t["kind"] == "audio" for t in tests):
        wants = []
        for t in vtests:
            if t["status"] != "ok":
                continue
            for s in t["snapshots"]:
                if s["file"] and s["result"] == "ok":
                    wants.append((os.path.splitext(s["file"])[0], s["frame"] / fps))
        if not wants:
            wants = [("none", 0.0)]
        have = {}
        if args.reuse:
            for n, _ in wants:
                p = os.path.join(reno_dir, n + ".png")
                if os.path.exists(p):
                    have[n] = p
        todo = [w for w in wants if w[0] not in have]
        t0 = time.time()
        if todo:
            log("Reno: rendering %d pictures from fuzz.veg ..." % len(todo))
            have.update({k: v for k, v in render_stills(args.cli, veg, todo, reno_dir, args.chunk, args.scan, log).items() if v})
            log("  done in %.0f s" % (time.time() - t0))
        fx_rows = read_fx_status(os.path.join(reno_dir, "fx_status.tsv"))
        if not fx_rows:
            log("  (no fx_status.tsv: is reno_cli new enough to know --stills?)")
        worst_pics = []
        for t in vtests:
            if t["status"] != "ok":
                add(t, status="not compared", note="the script could not build this test: " + t.get("error", ""))
                continue
            why = availability(t, fx_rows, "video") if fx_rows else None
            if why:
                add(t, status="not available", note=why)
                continue
            snaps = []
            problem = None
            for s in t["snapshots"]:
                name = os.path.splitext(s["file"])[0] if s["file"] else ""
                vp = os.path.join(folder, s["file"]) if s["file"] else ""
                if s["result"] != "ok" or not vp or not os.path.exists(vp):
                    problem = "no VEGAS picture (%s)" % (s["result"] or "missing")
                    break
                rp = have.get(name)
                if not rp:
                    problem = "Reno could not render the picture (see the log; reno_cli failed or crashed)"
                    break
                try:
                    a, b = load_rgb(vp), load_rgb(rp)
                    st, b2 = picture_stats(a, b)
                except Exception as e:
                    problem = "cannot read the pictures: %s" % e
                    break
                st["percent"] = s["percent"]
                st["file"] = s["file"]
                snaps.append((st, a, b2))
            if problem:
                add(t, status="not compared", note=problem)
                continue
            worst = max(snaps, key=lambda x: x[0]["mean"])
            r = add(t, pictures=[x[0] for x in snaps], mean=max(x[0]["mean"] for x in snaps), max=max(x[0]["max"] for x in snaps),
                    pct_over_2=max(x[0]["pct_over_2"] for x in snaps), score=max(x[0]["mean"] for x in snaps) / 255.0 * 100.0)
            if any(x[0]["resized"] for x in snaps):
                r["note"] = "Reno's picture was resized to VEGAS's size"
            worst_pics.append((r["score"], t, worst))
        if args.png > 0 and worst_pics:
            pdir = os.path.join(out, "png")
            os.makedirs(pdir, exist_ok=True)
            worst_pics.sort(key=lambda x: -x[0])
            for i, (score, t, w) in enumerate(worst_pics[:args.png]):
                fn = "%03d_%s_%s_%s.png" % (i + 1, t["kind"], "".join(c if c.isalnum() else "_" for c in t["plugin"])[:30], "".join(c if c.isalnum() else "_" for c in t["variant"])[:20])
                side_by_side(w[1], w[2], os.path.join(pdir, fn), "%s  mean %.2f max %d  (VEGAS | Reno | diff x4)" % (test_label(t), w[0]["mean"], w[0]["max"]))
            log("  %d side by side pictures in %s" % (min(args.png, len(worst_pics)), pdir))

    # ---- sound
    atests = [t for t in tests if t["kind"] == "audio"]
    if atests:
        vwav = os.path.join(folder, "fuzz_audio.wav")
        rwav = os.path.join(out, "reno_audio.wav")
        vx = vr = None
        verr = None
        if not os.path.exists(vwav):
            verr = "VEGAS's fuzz_audio.wav is not in the folder (%s)" % man.get("audio_render", {}).get("status", "")
        else:
            try:
                vx, vr = read_wav(vwav)
            except Exception as e:
                verr = "cannot read fuzz_audio.wav: %s" % e
        rerr = None
        rx = None
        if verr is None:
            if not (args.reuse and os.path.exists(rwav)):
                if os.path.exists(rwav):
                    os.remove(rwav)
                log("Reno: rendering the project's audio ...")
                t0 = time.time()
                code, text = run_cli(args.cli, ["-t", "wav/48,000 Hz, 32 Bit (float), Stereo, IEEE", "--audio-only", "--find-media", "-y", "-q"] + ([] if args.scan else ["--no-scan"]) + ["-o", rwav, veg], timeout=3600)
                log("  done in %.0f s (exit %s)" % (time.time() - t0, code))
                if not os.path.exists(rwav):
                    rerr = "Reno's audio render failed (exit %s): %s" % (code, text.strip().splitlines()[-1] if text.strip() else "")
            if rerr is None:
                try:
                    rx, rr_rate = read_wav(rwav)
                    if rr_rate != vr:
                        rerr = "sample rates differ: VEGAS %d, Reno %d" % (vr, rr_rate)
                except Exception as e:
                    rerr = "cannot read Reno's audio: %s" % e
            if rerr is None and vr != rate:
                rerr = "VEGAS rendered at %d Hz, not %d" % (vr, rate)
        for t in atests:
            if t["status"] != "ok":
                add(t, status="not compared", note="the script could not build this test: " + t.get("error", ""))
                continue
            if verr or rerr:
                add(t, status="not compared", note=verr or rerr)
                continue
            if t.get("mode") == "track":
                # (one track per test: the effect is on the track; fx_status.tsv names it, not the track)
                miss = [r for r in fx_rows if r["kind"] == "audio" and r["scope"] == "track" and r["status"] != "ok" and t["plugin"].lower() in r["name"].lower()]
                why = "%s: %s" % (miss[0]["name"], miss[0]["why"] or "plug-in not found") if miss else None
            else:
                why = availability(t, fx_rows, "audio") if fx_rows else None
            if why:
                add(t, status="not available", note=why)
                continue
            a = int(round(t["start_s"] * rate))
            b = int(round((t["start_s"] + t["length_s"]) * rate))
            v, r_ = seg(vx, a, b), seg(rx, a, b)
            tail_from = int(round(t.get("event_length_s", 2.0) * rate))
            st = audio_stats(v, r_, rate, tail_from)
            add(t, score=min(st["residual_pct"], 1000.0), **st)

    # ---- the report
    done = [r for r in results if r["score"] is not None]
    done.sort(key=lambda r: -r["score"])
    other = [r for r in results if r["score"] is None]
    lines = []
    lines.append("Reno Plugin Fuzz comparison of %s" % folder)
    lines.append("VEGAS: %s    tests: %d compared, %d not available in Reno, %d not compared" % (
        man.get("vegas", ""), len(done), sum(1 for r in other if r["status"] == "not available"), sum(1 for r in other if r["status"] == "not compared")))
    if atests:
        lines.append("audio render: %s (%s); audio effects were on the %s" % (man.get("audio_render", {}).get("template", ""), man.get("audio_render", {}).get("status", ""), man.get("audio_effect_mode", "?")))
    # a summary by plug-in: the worst score of each
    lines.append("")
    lines.append("== worst first (score = relative error %: pictures mean/255, sound residual/VEGAS level)")
    lines.append("%7s  %-10s %-34s %-22s  %s" % ("score", "kind", "plug-in", "variant", "details"))
    shown = done if args.top <= 0 else done[:args.top]
    for r in shown:
        if r["kind"] == "audio":
            det = "corr %s  lag %+d smp  level %s dB  residual %.1f dB  tail residual %.1f dB" % (
                "n/a" if r["correlation"] is None else "%.4f" % r["correlation"], r["lag_samples"],
                "n/a" if r["level_diff_db"] is None else "%+.2f" % r["level_diff_db"], r["residual_db"], r["tail_residual_db"])
        else:
            det = "mean %.2f  max %d  >2: %.1f%%%s" % (r["mean"], r["max"], r["pct_over_2"], "  (" + r["note"] + ")" if r["note"] else "")
        lines.append("%6.2f%%  %-10s %-34s %-22s  %s" % (r["score"], r["kind"], r["plugin"][:34], r["variant"][:22], det))
    if len(shown) < len(done):
        lines.append("... %d more (see report.json)" % (len(done) - len(shown)))
    na = [r for r in other if r["status"] == "not available"]
    if na:
        lines.append("")
        lines.append("== not available in Reno (%d)" % len(na))
        seen = {}
        for r in na:
            key = (r["kind"], r["plugin"], r["note"])
            seen[key] = seen.get(key, 0) + 1
        for (k, p, n), c in sorted(seen.items()):
            lines.append("  %-10s %-34s x%d  %s" % (k, p[:34], c, n))
    nc = [r for r in other if r["status"] == "not compared"]
    if nc:
        lines.append("")
        lines.append("== not compared (%d)" % len(nc))
        for r in nc[:200]:
            lines.append("  %-10s %-34s %-22s  %s" % (r["kind"], r["plugin"][:34], r["variant"][:22], r["note"]))
        if len(nc) > 200:
            lines.append("  ... %d more" % (len(nc) - 200))
    # by plug-in
    best = {}
    for r in done:
        key = (r["kind"], r["plugin"])
        best.setdefault(key, []).append(r["score"])
    lines.append("")
    lines.append("== by plug-in (worst variant's score, mean of its variants)")
    for (k, p), sc in sorted(best.items(), key=lambda kv: -max(kv[1])):
        lines.append("  %6.2f%%  %6.2f%%  %-10s %s  (%d)" % (max(sc), sum(sc) / len(sc), k, p, len(sc)))
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(dict(folder=folder, vegas=man.get("vegas", ""), results=done + other), f, indent=1)
    print(text)
    log("report: %s and report.json" % os.path.join(out, "report.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
