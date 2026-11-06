#!/usr/bin/env python3
"""Compares what VEGAS says a .veg holds with what Reno reads from it.

Both sides write the same JSON schema ("reno-model/1"):
  VEGAS side: tools/vegas/RenoCoverage.cs  (projects/<name>.json next to projects/<name>.veg)
  Reno side:  reno_cli model <file.veg>    (src/cli/modeldump.cpp)

Subcommands
  compare   <vegas-output-folder> [--reno-cli PATH] [--only NAME] [--report FILE] [--media-root FOLDER]
            For every projects/*.json the VEGAS script made: dump the .veg next to it with Reno and compare field by field.
  roundtrip <folder-with-.veg> --out FOLDER [--reno-cli PATH]
            Reno-side round trip: dump each .veg, save it again with Reno's writer, dump the copy and compare
            the two (nothing may change that Reno reads).  The copies in FOLDER/resaved are what the
            script's "Describe" mode opens in VEGAS (it writes <name>.vegas.json); then
            compare-describe checks VEGAS's reading of Reno's file against Reno's own.
  compare-describe <FOLDER> [--reno-cli PATH]
            FOLDER/resaved/*.veg + FOLDER/resaved/projects/*.vegas.json (or *.json) from VEGAS's Describe mode.
  diff      A.json B.json      compare two manifests directly (either side first).
  selftest  checks the normalisation and the comparison on built-in data.

Rules
  * Numbers are equal when they match within a tolerance: times (names ending ...Nanos) 1000 units (0.1 ms), other
    doubles 1e-5 relative / 1e-6 absolute.  Integers, booleans and strings must match exactly.
  * Geometry differs by side: VEGAS gives raw corners/positions ("motion"), Reno gives its native fractions
    ("panCropNative", "motionNative"); the VEGAS side is converted first (convert_* below).
  * Plug-in ids are compared without the {Svfx:...} wrapper; event groups as partitions (numbers relabelled by first use).
  * A field only one side has is listed apart ("only VEGAS" / "only Reno"), not as a difference.
Exit status 0 when nothing differs, 1 otherwise.
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

TIME_TOL = 1000          # Timecode.Nanos units (100 ns): 0.1 ms
REL_TOL = 1e-5
ABS_TOL = 1e-6
NULL_MEANS_NONE = {"transition"}
IGNORE_KEYS = {"errors", "source", "app", "schema", "notes", "mediaWidth", "mediaHeight",
               "projectFile",   # the script's own bookkeeping (the file the manifest belongs to)
               "selected"}      # which tracks/events are selected in the window: view state, not part of the project


# ---------------------------------------------------------------- normalisation
def norm_uid(u):
    """{Svfx:com.vendor:name} / {Svfx:{GUID}} / {GUID} / GUID -> the plug-in's own identifier: an upper-case GUID, or
    for an OpenFX effect its "com.vendor:name" (VEGAS writes the OpenFX id inside an {Svfx:...} wrapper; Reno keeps the
    bare id, so the two describe the same thing)."""
    if not isinstance(u, str):
        return u
    m = re.search(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}", u)
    if m:
        return m.group(0).upper()
    m = re.match(r"^\{Svfx:(.*)\}$", u.strip())
    return (m.group(1) if m else u).strip()


def convert_track_motion(keys, W, H):
    """VEGAS track motion keys (pixels from the centre, y up, angles in degrees) -> Reno's fractions and degrees."""
    out = []
    for k in keys:
        q = {"positionNanos": k.get("positionNanos")}
        if "x" in k and W:
            q["x"] = 0.5 + k["x"] / W
        if "y" in k and H:
            q["y"] = 0.5 - k["y"] / H
        if "w" in k and W:
            q["w"] = k["w"] / W
        if "h" in k and H:
            q["h"] = k["h"] / H
        # (the file keeps radians but the script's RotationZ is in degrees: 0.35 was set and read back as 0.35)
        if "rotationZ" in k:
            q["rotation"] = k["rotationZ"]
        # 3D Track Motion: depth is a twentieth of the frame height at Reno's 1; offsets and z are fractions of the width
        if W and H:
            for f, name, fn in (("z", "z", lambda v: v / W), ("depth", "d", lambda v: v * 20.0 / H), ("rotationOffsetX", "offX", lambda v: v / W),
                                ("rotationOffsetY", "offY", lambda v: -v / W), ("rotationOffsetZ", "offZ", lambda v: v / W)):
                if f in k and is_num(k[f]):
                    q[name] = fn(k[f])
        for f, name in (("rotationX", "rotX"), ("rotationY", "rotY"), ("orientationX", "oriX"), ("orientationY", "oriY"), ("orientationZ", "oriZ")):
            if f in k:
                q[name] = k[f]
        for f in ("smoothness", "type"):
            if f in k:
                q[f] = k[f]
        out.append(q)
    return out


def convert_pancrop(keys, mw, mh):
    """VEGAS pan/crop keys (four corners in media pixels) -> centre, size (fractions of the picture), angle in degrees."""
    out = []
    for k in keys:
        q = {"positionNanos": k.get("positionNanos")}
        try:
            tl, tr, bl, br = k["topLeft"], k["topRight"], k["bottomLeft"], k["bottomRight"]
            if mw and mh:
                q["cx"] = (tl[0] + tr[0] + bl[0] + br[0]) / 4.0 / mw
                q["cy"] = (tl[1] + tr[1] + bl[1] + br[1]) / 4.0 / mh
                q["w"] = math.hypot(tr[0] - tl[0], tr[1] - tl[1]) / mw
                q["h"] = math.hypot(bl[0] - tl[0], bl[1] - tl[1]) / mh
            q["rotation"] = math.degrees(math.atan2(tr[1] - tl[1], tr[0] - tl[0]))
        except (KeyError, TypeError, IndexError):
            pass
        # (the script's pan/crop Smoothness is 0..1; Reno keeps 0..100 like Track Motion)
        if "smoothness" in k and is_num(k["smoothness"]):
            q["smoothness"] = k["smoothness"] * 100.0
        if "type" in k:
            q["type"] = k["type"]
        out.append(q)
    return out


def relabel_groups(m):
    """Group numbers are labels: replace them by 0,1,2... in order of first use (-1 stays: no group)."""
    seen = {}
    for t in m.get("tracks", []):
        for e in t.get("events", []):
            g = e.get("group", -1)
            if g is None or g < 0:
                e["group"] = -1
                continue
            if g not in seen:
                seen[g] = len(seen)
            e["group"] = seen[g]


def norm_uids(node):
    """Every "uniqueId" in the manifest (effects of any chain, transitions, generators, media) in the common form."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "uniqueId":
                node[k] = norm_uid(v)
            elif k in ("key", "mediaKey") and isinstance(v, str) and v.startswith("gen:"):
                node[k] = "gen:" + norm_uid(v[4:])        # a generator's key is "gen:" and its plug-in id
            else:
                norm_uids(v)
    elif isinstance(node, list):
        for v in node:
            norm_uids(v)


def normalise(m):
    """Brings a manifest of either side into the common form (a copy)."""
    m = json.loads(json.dumps(m))
    pv = m.get("project", {}).get("video", {})
    W, H = pv.get("width"), pv.get("height")
    norm_uids(m)
    for t in m.get("tracks", []):
        v = t.get("video")
        if v is not None and "motion" in v:
            v["motionNative"] = convert_track_motion(v.pop("motion"), W, H)
        for e in t.get("events", []):
            ev = e.get("video")
            if ev is not None and "motion" in ev:
                ev["panCropNative"] = convert_pancrop(ev.pop("motion"), ev.get("mediaWidth"), ev.get("mediaHeight"))
    relabel_groups(m)
    norm_project(m)
    return m


def is_untouched_pancrop(keys):
    """VEGAS always reports one pan/crop key; an untouched one (the whole picture, centred, widened or heightened to the
    project's shape) is what Reno shows as no pan/crop at all (the reader drops it by the same test)."""
    if len(keys) != 1:
        return False
    k = keys[0]
    try:
        centred = abs(k["cx"] - 0.5) < 1e-4 and abs(k["cy"] - 0.5) < 1e-4
        fitted = (abs(k["w"] - 1) < 1e-4 and k["h"] >= 1 - 1e-4) or (abs(k["h"] - 1) < 1e-4 and k["w"] >= 1 - 1e-4)
        return centred and fitted and abs(k.get("rotation", 0)) < 1e-6
    except KeyError:
        return False


def is_untouched_motion(keys):
    """Likewise VEGAS's one default track motion key (the whole frame, centred, not turned) is Reno's empty list."""
    if len(keys) != 1:
        return False
    k = keys[0]
    try:
        return (abs(k["x"] - 0.5) < 1e-6 and abs(k["y"] - 0.5) < 1e-6 and abs(k["w"] - 1) < 1e-6 and abs(k["h"] - 1) < 1e-6
                and abs(k.get("rotation", 0)) < 1e-6)
    except KeyError:
        return False


def norm_project(m):
    """Project/track/event parts where the two sides describe the same thing differently (VEGAS manifest only)."""
    if m.get("source") != "vegas":
        return
    for t in m.get("tracks", []):
        v = t.get("video")
        if v is not None and is_untouched_motion(v.get("motionNative", [])):
            v["motionNative"] = []
        if v is not None:
            # VEGAS has default shadow and glow keys even while they are off; Reno has none then
            for on, keys in (("shadowEnabled", "shadow"), ("glowEnabled", "glow")):
                if not v.get(on) and keys in v:
                    del v[keys]
        for e in t.get("events", []):
            ev = e.get("video")
            if ev is not None and is_untouched_pancrop(ev.get("panCropNative", [])):
                ev["panCropNative"] = []
            a = e.get("audio")
            if a is not None:
                # null: VEGAS threw when the script read it (formant settings exist for the Pro mode only), not a value
                for f in ("formantLock", "formantShift"):
                    if a.get(f, 0) is None:
                        del a[f]


# ---------------------------------------------------------------- comparison
def is_num(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def num_equal(a, b, name):
    if a == b:
        return True
    if isinstance(a, int) and isinstance(b, int) and not name.endswith("Nanos"):
        return False
    if name.endswith("Nanos"):
        return abs(a - b) <= TIME_TOL
    return abs(a - b) <= max(ABS_TOL, REL_TOL * max(abs(a), abs(b)))


class Result:
    def __init__(self):
        self.diffs = []        # (path, vegas value, reno value)
        self.only_vegas = []   # (path, value)
        self.only_reno = []
        self.no_data = []      # (path, reno value): VEGAS's manifest has null there (its API refused), nothing to compare
        self.expected = []     # (path, vegas value, reno value): differences that follow from the format, not losses
        self.compared = 0

    def merge(self, o):
        self.diffs += o.diffs
        self.only_vegas += o.only_vegas
        self.only_reno += o.only_reno
        self.expected += o.expected
        self.no_data += o.no_data
        self.compared += o.compared


def short(v, n=90):
    s = json.dumps(v, ensure_ascii=False)
    return s if len(s) <= n else s[:n] + "..."


def walk(a, b, path, name, res):
    """a: VEGAS value, b: Reno value."""
    if isinstance(a, dict) and isinstance(b, dict):
        # Parameters the manifest lists only by name in "paramsOther" (groups, pages, buttons and the like: VEGAS does not persist
        # them) are in a .veg's record as a plain 0 and Reno's state keeps that: not a value of the effect.
        if "paramsOther" in a and isinstance(b.get("params"), dict):
            for item in a["paramsOther"]:
                nm = item.split(":")[0] if isinstance(item, str) else None
                if nm in b["params"] and nm not in a.get("params", {}):
                    del b["params"][nm]
        for k in a:
            if k in IGNORE_KEYS:
                continue
            if k not in b:
                res.only_vegas.append((path + "." + k, a[k]))
            else:
                walk(a[k], b[k], path + "." + k, k, res)
        for k in b:
            if k in IGNORE_KEYS:
                continue
            if k not in a:
                res.only_reno.append((path + "." + k, b[k]))
        return
    if isinstance(a, list) and isinstance(b, list):
        # A parameter value that is all zeros has no shape in a .veg: eight zero bytes are one double 0.0 or two whole numbers 0, 0
        # (what the value is depends on the plug-in's parameter type, which the file does not repeat).  Reno, reading the file
        # without the plug-in, takes the double.
        if len(a) != len(b) and name == "value" and a and b and all(x == 0 for x in a) and all(x == 0 for x in b):
            res.compared += 1
            return
        if len(a) != len(b):
            res.diffs.append((path + ".length", len(a), len(b)))
        for i in range(min(len(a), len(b))):
            walk(a[i], b[i], "%s[%d]" % (path, i), name, res)
        return
    # A value the manifest has as null where Reno has a plain value: the script's try/catch writes null when VEGAS's API refused the
    # read (for instance Effect.IsOFX and PlugInNode.Group raise E_FAIL for the older built-in effects), so VEGAS gave no data.
    # (A null that means "none", such as a missing transition, is compared against a null on both sides, never a plain value.)
    if a is None and b is not None and not isinstance(b, (dict, list)) and name not in NULL_MEANS_NONE:
        res.no_data.append((path, b))
        return
    # An empty text parameter and a whole number 0 are the same bytes in a .veg (a length of 0 or the value 0 and no keys): Reno,
    # which does not have the plug-in's parameter types, reads such a value as the number.
    if a == "" and b == [0]:
        res.compared += 1
        return
    res.compared += 1
    if is_num(a) and is_num(b):
        if not num_equal(a, b, name):
            res.diffs.append((path, a, b))
        return
    if a != b:
        res.diffs.append((path, a, b))


# Fields whose VEGAS value cannot be compared with what the file holds
SMPTE_RULERS = {"SmpteFilmSyncIVTC", "SmpteFilmSync", "SmpteEBU", "SmpteNonDrop", "SmpteDrop"}


def set_aside_known(res, vegas, reno):
    """Moves differences that follow from how the manifest was made (not from a loss in Reno) to res.expected."""
    keep = []
    all_diffs = list(res.diffs)
    smpte = vegas.get("project", {}).get("ruler", {}).get("format") in SMPTE_RULERS
    for p, a, b in res.diffs:
        # The resample quality of the audio is not in the .veg (changing it changed no byte of the file); Reno's own
        # default applies after a load.
        if p == ".project.audio.resampleQuality":
            res.expected.append((p, a, b))
        # The ruler formats SMPTE film/EBU/NTSC set the project's frame rate when VEGAS saves, but the script's
        # Video.FrameRate keeps the rate it was given; the file holds the format's rate (header 0x10), which Reno reads.
        elif p == ".project.video.frameRate" and smpte:
            res.expected.append((p, a, b))
        # VEGAS reports Time and Frames for an Audio CD Time ruler although the file holds AudioCDTime's own word (17)
        elif p == ".project.ruler.format" and a == "TimeAndFrames" and b == "AudioCDTime":
            res.expected.append((p, a, b))
        # An audio event whose stretch method is None reads PlaybackRate 1 in VEGAS's script API although the file keeps the
        # rate that was set (1.3 in the Coverage run: set to 1.3, then Method None). Reno keeps and plays the file's rate
        # (tape-like); whether VEGAS plays it is for a render comparison to say.
        elif re.fullmatch(r"\.tracks\[(\d+)\]\.events\[(\d+)\]\.playbackRate", p) and a == 1 and b != 1 and none_method(vegas, p):
            res.expected.append((p, a, b))
        # VEGAS gives keyframes of an Integer parameter the interpolation "Unknown" (there is nothing to interpolate between
        # whole numbers); Reno stores a curve for every key.
        elif re.fullmatch(r".*\.keyframes\[\d+\]\.interpolation", p) and a == "Unknown":
            res.expected.append((p, a, b))
        # A media generator's Preset: the script applied presets by name and VEGAS's Preset property answers the last name of the
        # plug-in's list ("Standard Turbulence", "Black") although the parameters VEGAS reports are those of the first preset
        # (the defaults) and the file stores the name it was created with.  Reno reads the file; when every parameter agrees, the
        # name is the only thing in question.
        elif re.fullmatch(r"\.media\[\d+\]\.generator\.preset", p) and not any(
                q.startswith(p[:-len("preset")] + "params") for q, _, _ in all_diffs):
            res.expected.append((p, a, b))
        # The script's Ruler.StartTime answered the start of the project's first region (2 s) for a project that never set it;
        # the file's ruler start (header word at 0x20) is 0 and VEGAS wrote it so.
        elif p == ".project.ruler.startTimeNanos" and b == 0 and a and regions_start(vegas) == a:
            res.expected.append((p, a, b))
        else:
            keep.append((p, a, b))
    res.diffs = keep


def regions_start(vegas):
    try:
        return vegas["regions"][0]["positionNanos"]
    except (KeyError, IndexError, TypeError):
        return None


def none_method(vegas, path):
    m = re.fullmatch(r"\.tracks\[(\d+)\]\.events\[(\d+)\]\.playbackRate", path)
    try:
        return vegas["tracks"][int(m.group(1))]["events"][int(m.group(2))]["audio"]["method"] == "None"
    except (KeyError, IndexError, TypeError):
        return False


def sort_media_like(nv, nr):
    """The media pool has no order that means anything: VEGAS's script lists it in the order the items were made, a .veg
    keeps its records in the order they were written (generators ahead of subclips, say).  When both sides list the same
    keys, Reno's list is put in VEGAS's order so the items are compared pairwise."""
    a, b = nv.get("media"), nr.get("media")
    if not isinstance(a, list) or not isinstance(b, list) or len(a) != len(b):
        return
    ka, kb = [m.get("key") for m in a], [m.get("key") for m in b]
    if sorted(map(str, ka)) == sorted(map(str, kb)) and len(set(ka)) == len(ka):
        by = dict((m.get("key"), m) for m in b)
        nr["media"] = [by[k] for k in ka]


def compare_manifests(vegas, reno):
    res = Result()
    nv, nr = normalise(vegas), normalise(reno)
    sort_media_like(nv, nr)
    # the script's media list was empty in the Coverage run (MediaPool.Values threw): no VEGAS data, nothing to compare
    if not nv.get("media"):
        nv.pop("media", None)
        nr.pop("media", None)
    walk(nv, nr, "", "", res)
    set_aside_known(res, vegas, reno)
    return res


MEDIA_STREAM_FIELD = re.compile(r"^\.media\[\d+\]\.(width|height|sampleRate|channels)$")


def set_aside_unreadable_media(res):
    """Round trip only: a .veg holds no picture size, sample rate or channel count of its media (VEGAS reads the file),
    so media whose file cannot be found when the resaved copy is read has them 0.  Moves those differences to
    res.expected."""
    keep = []
    for p, a, b in res.diffs:
        if MEDIA_STREAM_FIELD.match(p) and b == 0 and a:
            res.expected.append((p, a, b))
        else:
            keep.append((p, a, b))
    res.diffs = keep


def shape(path):
    return re.sub(r"\[\d+\]", "[]", path)


def summarise(name, res, out, limit=40):
    out.append("== %s: %d values compared, %d differ, %d only VEGAS, %d only Reno" %
               (name, res.compared, len(res.diffs), len(res.only_vegas), len(res.only_reno)))
    for p, a, b in res.diffs[:limit]:
        out.append("  DIFF %s: vegas=%s reno=%s" % (p, short(a), short(b)))
    if len(res.diffs) > limit:
        out.append("  ... %d more differences" % (len(res.diffs) - limit))
    if res.no_data:
        out.append("  (%d values the manifest has as null: VEGAS's API refused the read, nothing to compare)" % len(res.no_data))
    if res.expected:
        out.append("  (%d expected differences, not losses: %s)" % (len(res.expected), ", ".join(sorted(set(shape(p) for p, _, _ in res.expected)))))
    for p, a in res.only_vegas[:10]:
        out.append("  only-vegas %s = %s" % (p, short(a, 60)))
    for p, b in res.only_reno[:10]:
        out.append("  only-reno  %s = %s" % (p, short(b, 60)))


def tally(results, out):
    """Counts per field shape over all projects: what to fix first."""
    d, v, r = {}, {}, {}
    for res in results.values():
        for p, _, _ in res.diffs:
            d[shape(p)] = d.get(shape(p), 0) + 1
        for p, _ in res.only_vegas:
            v[shape(p)] = v.get(shape(p), 0) + 1
        for p, _ in res.only_reno:
            r[shape(p)] = r.get(shape(p), 0) + 1
    for title, t in (("Fields that differ", d), ("Fields only VEGAS reports", v), ("Fields only Reno reports", r)):
        out.append("")
        out.append("-- %s (by field, all projects)" % title)
        for k, n in sorted(t.items(), key=lambda kv: -kv[1])[:60]:
            out.append("  %6d  %s" % (n, k))
        if not t:
            out.append("  (none)")


# ---------------------------------------------------------------- running Reno
def find_cli(arg):
    if arg:
        return arg
    here = os.path.dirname(os.path.abspath(__file__))
    for c in (os.environ.get("RENO_CLI"), os.path.join(here, "..", "build", "reno_cli"), "reno_cli"):
        if c and (os.path.isfile(c) or c == "reno_cli"):
            return c
    return "reno_cli"


def reno_dump(cli, veg, out_json, resave=None):
    # reno_cli takes "model" as its first word; the engine-only build (tests/vegas/vegmodel.cpp) is the command itself
    cmd = [cli] + ([] if os.path.basename(cli).startswith("vegmodel") else ["model"]) + [veg, "-o", out_json]
    if resave:
        cmd += ["--resave", resave]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if r.returncode != 0 or not os.path.isfile(out_json):
        raise RuntimeError("reno_cli model failed for %s: %s" % (veg, r.stderr.strip()[:300]))
    with open(out_json, encoding="utf-8") as f:
        return json.load(f)


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


def finish(results, out, report):
    tally(results, out)
    bad = sum(1 for r in results.values() if r.diffs)
    out.append("")
    out.append("%d projects, %d with differences, %d values compared" % (len(results), bad, sum(r.compared for r in results.values())))
    text = "\n".join(out)
    print(text)
    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 1 if bad else 0


# ---------------------------------------------------------------- subcommands
def stage_media(media_root, tmp):
    """The .veg files hold the Windows paths of the machine VEGAS ran on.  Reno looks for a missing file next to the
    project (same name), so the comparison opens each project from a folder of its own that holds links to every
    file of --media-root; Reno then reads the media's size and sample rate exactly as it does when a project is moved
    together with its media.  Returns the folder (or None without a media root)."""
    if not media_root or not os.path.isdir(media_root):
        return None
    stage = os.path.join(tmp, "stage")
    os.makedirs(stage, exist_ok=True)
    for fn in os.listdir(media_root):
        src = os.path.abspath(os.path.join(media_root, fn))
        dst = os.path.join(stage, fn)
        if os.path.isfile(src) and not os.path.exists(dst):
            try:
                os.symlink(src, dst)
            except OSError:
                shutil.copy2(src, dst)
    return stage


def cmd_compare(args):
    folder, cli, only, report, media_root = None, None, None, None, None
    it = iter(args)
    for a in it:
        if a == "--media-root":
            media_root = next(it)
        elif a == "--reno-cli":
            cli = next(it)
        elif a == "--only":
            only = next(it)
        elif a == "--report":
            report = next(it)
        else:
            folder = a
    if not folder:
        print(__doc__)
        return 2
    cli = find_cli(cli)
    pdir = os.path.join(folder, "projects")
    if not os.path.isdir(pdir):
        pdir = folder
    tmp = tempfile.mkdtemp(prefix="vegcov_")
    if media_root is None and os.path.isdir(os.path.join(folder, "media")):
        media_root = os.path.join(folder, "media")     # the script writes its media next to the projects
    stage = stage_media(media_root, tmp)
    if stage:
        # (a project that uses another project as media finds it next to itself too)
        for fn in os.listdir(pdir):
            if fn.endswith(".veg") and not os.path.exists(os.path.join(stage, fn)):
                shutil.copy2(os.path.join(pdir, fn), os.path.join(stage, fn))
    results, out = {}, []
    for fn in sorted(os.listdir(pdir)):
        if not fn.endswith(".json") or fn.endswith(".vegas.json"):
            continue
        name = fn[:-5]
        if only and only not in name:
            continue
        veg = os.path.join(pdir, name + ".veg")
        if not os.path.isfile(veg):
            out.append("== %s: no .veg next to the manifest" % name)
            continue
        if stage:
            staged = os.path.join(stage, name + ".veg")
            if not os.path.exists(staged):
                shutil.copy2(veg, staged)
            veg = staged
        try:
            reno = reno_dump(cli, veg, os.path.join(tmp, name + ".reno.json"))
            results[name] = compare_manifests(load(os.path.join(pdir, fn)), reno)
            summarise(name, results[name], out)
        except Exception as e:
            out.append("== %s: ERROR %s" % (name, e))
    return finish(results, out, report)


def cmd_roundtrip(args):
    folder, outdir, cli, report = None, None, None, None
    it = iter(args)
    for a in it:
        if a == "--out":
            outdir = next(it)
        elif a == "--reno-cli":
            cli = next(it)
        elif a == "--report":
            report = next(it)
        else:
            folder = a
    if not folder or not outdir:
        print(__doc__)
        return 2
    cli = find_cli(cli)
    rs = os.path.join(outdir, "resaved")
    os.makedirs(rs, exist_ok=True)
    results, out = {}, []
    for root, _, files in os.walk(folder):
        for fn in sorted(files):
            if not fn.lower().endswith((".veg", ".reno")):
                continue
            src = os.path.join(root, fn)
            base = re.sub(r"[^A-Za-z0-9._-]+", "_", os.path.splitext(fn)[0])
            n = 1
            while os.path.exists(os.path.join(rs, base + ".veg")):   # same file name in two folders
                n += 1
                base = "%s_%d" % (re.sub(r"_\d+$", "", base), n)
            # the copy is made in a folder that holds links to everything beside the original, so that media and
            # nested projects are found again as they were
            work = os.path.join(outdir, "work", base)
            os.makedirs(work, exist_ok=True)
            for entry in os.listdir(root):
                link = os.path.join(work, entry)
                if not os.path.lexists(link):
                    try:
                        os.symlink(os.path.abspath(os.path.join(root, entry)), link)
                    except OSError:
                        pass
            copy = os.path.join(work, base + ".veg")
            if os.path.lexists(copy):
                os.remove(copy)
            try:
                a = reno_dump(cli, src, os.path.join(outdir, base + ".orig.json"), resave=copy)
                b = reno_dump(cli, copy, os.path.join(outdir, base + ".resaved.json"))
                results[base] = compare_manifests(a, b)
                set_aside_unreadable_media(results[base])
                summarise(base, results[base], out)
                for extra in (copy, copy + ".reno-vst3"):
                    if os.path.isfile(extra):
                        shutil.copyfile(extra, os.path.join(rs, os.path.basename(extra)))
            except Exception as e:
                out.append("== %s: ERROR %s" % (base, e))
    out.append("")
    out.append("Resaved copies for VEGAS's Describe mode are in: " + rs)
    return finish(results, out, report)


def cmd_compare_describe(args):
    folder, cli, report = None, None, None
    it = iter(args)
    for a in it:
        if a == "--reno-cli":
            cli = next(it)
        elif a == "--report":
            report = next(it)
        else:
            folder = a
    if not folder:
        print(__doc__)
        return 2
    cli = find_cli(cli)
    rs = os.path.join(folder, "resaved")
    results, out = {}, []
    for root, _, files in os.walk(folder):
        for fn in sorted(files):
            if not fn.endswith(".vegas.json"):
                continue
            base = fn[:-len(".vegas.json")]
            veg = os.path.join(rs, base + ".veg")
            if not os.path.isfile(veg):
                out.append("== %s: no resaved .veg" % base)
                continue
            try:
                reno = load(os.path.join(folder, base + ".resaved.json")) if os.path.isfile(os.path.join(folder, base + ".resaved.json")) \
                    else reno_dump(cli, veg, os.path.join(folder, base + ".resaved.json"))
                results[base] = compare_manifests(load(os.path.join(root, fn)), reno)
                summarise(base, results[base], out)
            except Exception as e:
                out.append("== %s: ERROR %s" % (base, e))
    return finish(results, out, report)


def cmd_diff(args):
    if len(args) < 2:
        print(__doc__)
        return 2
    res = compare_manifests(load(args[0]), load(args[1]))
    out = []
    summarise("diff", res, out, limit=200)
    print("\n".join(out))
    return 1 if res.diffs else 0


# ---------------------------------------------------------------- self test
def cmd_selftest(_):
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    check(norm_uid("{Svfx:{ABCDEF01-2345-6789-abcd-ef0123456789}}") == "ABCDEF01-2345-6789-ABCD-EF0123456789", "uid wrapper")
    check(norm_uid("plain") == "plain", "uid without guid")
    # numbers: times within 0.1 ms equal, doubles by relative tolerance, ints exact
    check(num_equal(10000000, 10000500, "lengthNanos"), "time tolerance")
    check(not num_equal(10000000, 10002000, "lengthNanos"), "time tolerance edge")
    check(num_equal(0.3333333, 1 / 3.0, "x"), "double tolerance")
    check(not num_equal(3, 4, "n"), "ints exact")
    # group relabel
    m = {"tracks": [{"events": [{"group": 7}, {"group": -1}, {"group": 3}, {"group": 7}]}]}
    relabel_groups(m)
    check([e["group"] for e in m["tracks"][0]["events"]] == [0, -1, 1, 0], "group partitions")
    # pan/crop: full-frame, centred, rotated by 90 degrees
    k = convert_pancrop([{"positionNanos": 0, "topLeft": [0, 0], "topRight": [200, 0], "bottomLeft": [0, 100], "bottomRight": [200, 100]}], 200, 100)[0]
    check(abs(k["cx"] - 0.5) < 1e-9 and abs(k["cy"] - 0.5) < 1e-9 and abs(k["w"] - 1) < 1e-9 and abs(k["h"] - 1) < 1e-9 and abs(k["rotation"]) < 1e-9, "pancrop full frame")
    # track motion: centre of the frame, half size, 90 degrees clockwise (rz positive) -> Reno rotation 90
    t = convert_track_motion([{"positionNanos": 5, "x": 0, "y": 0, "w": 320, "h": 180, "rotationZ": 90}], 640, 360)[0]
    check(abs(t["x"] - 0.5) < 1e-9 and abs(t["y"] - 0.5) < 1e-9 and abs(t["w"] - 0.5) < 1e-9 and abs(t["rotation"] - 90) < 1e-9, "track motion")
    # whole-manifest: a VEGAS-shaped and a Reno-shaped description of the same project must match
    vegas = {"project": {"video": {"width": 640, "height": 360}}, "media": [],
             "tracks": [{"type": "video", "video": {"motion": [{"positionNanos": 0, "x": 0, "y": 0, "w": 640, "h": 360, "rotationZ": 0, "type": "Linear"}]},
                         "effects": [{"uniqueId": "{Svfx:{ABCDEF01-2345-6789-ABCD-EF0123456789}}"}],
                         "events": [{"group": 12, "startNanos": 1000, "lengthNanos": 20000000, "effects": []},
                                    {"group": 12, "startNanos": 5000, "lengthNanos": 30000000, "effects": []}]}],
             "errors": ["x"]}
    reno = {"project": {"video": {"width": 640, "height": 360}}, "media": [],
            "tracks": [{"type": "video", "video": {"motionNative": [{"positionNanos": 0, "x": 0.5, "y": 0.5, "w": 1, "h": 1, "rotation": -0.0, "type": "Linear"}], "extra": 1},
                        "effects": [{"uniqueId": "ABCDEF01-2345-6789-ABCD-EF0123456789"}],
                        "events": [{"group": 0, "startNanos": 1200, "lengthNanos": 20000000, "effects": []},
                                   {"group": 0, "startNanos": 5000, "lengthNanos": 30000000, "effects": []}]}]}
    r = compare_manifests(vegas, reno)
    check(not r.diffs, "equal manifests: %r" % (r.diffs,))
    check(any(p.endswith(".extra") for p, _ in r.only_reno), "only-reno listed")
    reno["tracks"][0]["events"][1]["lengthNanos"] = 40000000
    r = compare_manifests(vegas, reno)
    check(len(r.diffs) == 1, "a real difference is found")
    for f in fails:
        print("selftest FAIL:", f)
    print("selftest: %s" % ("FAILED" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    cmds = {"compare": cmd_compare, "roundtrip": cmd_roundtrip, "compare-describe": cmd_compare_describe,
            "diff": cmd_diff, "selftest": cmd_selftest}
    if argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[argv[1]](argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
