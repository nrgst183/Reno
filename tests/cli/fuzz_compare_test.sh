#!/bin/bash
# Dry run of tools/fuzz_compare.py and of reno_cli --stills without VEGAS: fuzz_fakegen makes a project and manifest in the layout of "Reno Plugin
# Fuzz.cs"; Reno itself then renders the "VEGAS" snapshots and audio, a few are damaged on purpose, and fuzz_compare.py must
# find exactly those. Usage: fuzz_compare_test.sh <reno_cli> <fuzz_fakegen> <work folder>
set -u
CLI=$(readlink -f "$1"); GEN=$(readlink -f "$2"); W=$3
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$W"; mkdir -p "$W"; W=$(readlink -f "$W")
export QT_QPA_PLATFORM=offscreen
python3 -I - "$W" <<'PY'
import sys, wave, math
import numpy as np
from PIL import Image
w = sys.argv[1]
H, W_ = 180, 320
y, x = np.mgrid[0:H, 0:W_]
img = np.zeros((H, W_, 3), np.uint8)
img[..., 0] = x * 255 // (W_ - 1)
img[..., 1] = y * 255 // (H - 1)
img[..., 2] = ((x // 16 + y // 16) & 1) * 200 + 20
Image.fromarray(img).save(w + "/pic.png")
n = 96000
t = np.arange(n) / 48000.0
s = 0.5 * np.sin(2 * math.pi * 100 * 80 ** (np.minimum(t, 0.6) / 0.6) * t) * (t < 1.2)
pcm = (np.stack([s, s * 0.8], axis=1) * 32767).astype("<i2")
f = wave.open(w + "/snd.wav", "wb"); f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000); f.writeframes(pcm.tobytes()); f.close()
PY
"$GEN" "$W" "$W/pic.png" "$W/snd.wav"
# "VEGAS's" snapshots: Reno's own pictures of the project
python3 -I - "$W" > "$W/times.txt" <<'PY'
import json, sys, os
m = json.load(open(sys.argv[1] + "/manifest.json"))
for t in m["tests"]:
    for s in t["snapshots"]:
        print("%s=%r" % (os.path.splitext(s["file"])[0], s["frame"] / 30.0))
PY
"$CLI" --stills "$W/times.txt" --stills-dir "$W" --no-scan -q "$W/fuzz.veg"
# "VEGAS's" audio: Reno's own render
"$CLI" -t "wav/48,000 Hz, 32 Bit (float), Stereo, IEEE" --audio-only --find-media -y -q --no-scan -o "$W/fuzz_audio.wav" "$W/fuzz.veg"
# damage: test 1 (Black and White) brighter, test 2 (Gaussian Blur) shifted by 5 pixels, test 5 (the transition) one snapshot gone,
# the second audio test 5 samples late and 3 dB quieter, the third audio test noisy, the fourth one is a plug-in nobody has
python3 -I - "$W" "$HERE/../../tools/fuzz_compare.py" <<'PY'
import sys, json, wave, struct
import numpy as np
from PIL import Image
w = sys.argv[1]
m = json.load(open(w + "/manifest.json"))
def snap(i, suffix=""):
    return "%s/snap_%04d%s.png" % (w, i, suffix)
a = np.asarray(Image.open(snap(1)).convert("RGB"), dtype=np.int16)
Image.fromarray(np.clip(a + 40, 0, 255).astype(np.uint8)).save(snap(1))
a = np.asarray(Image.open(snap(2)).convert("RGB"), dtype=np.int16)
Image.fromarray(np.roll(a, 5, axis=1).astype(np.uint8)).save(snap(2))
import os
os.remove(snap(5, "_50"))
# audio
sys.path.insert(0, ".")
import importlib.util
spec = importlib.util.spec_from_file_location("fc", sys.argv[2])
fc = importlib.util.module_from_spec(spec); spec.loader.exec_module(fc)
x, rate = fc.read_wav(w + "/fuzz_audio.wav")
tests = [t for t in m["tests"] if t["kind"] == "audio"]
t1, t2 = tests[1], tests[2]   # (tests[0] stays as it is, tests[3] is the plug-in nobody has)
a0, a1 = int(t1["start_s"] * rate), int((t1["start_s"] + 3.5) * rate)
seg = x[a0:a1].copy()
x[a0:a1] = np.concatenate([np.zeros((5, 2), np.float32), seg[:-5]]) * 0.708
b0, b1 = int(t2["start_s"] * rate), min(len(x), int((t2["start_s"] + 3.5) * rate))
rng = np.random.RandomState(1)
x[b0:b1] += (rng.randn(b1 - b0, 2) * 0.01).astype(np.float32)
# write 32-bit float WAV
data = x.astype("<f4").tobytes()
with open(w + "/fuzz_audio.wav", "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, rate, rate * 8, 8, 32) + b"data" + struct.pack("<I", len(data)) + data)
PY
# reno_cli --stills itself
S=0; F=0
ok() { if eval "$2"; then echo "PASS $1"; S=$((S+1)); else echo "FAIL $1"; F=$((F+1)); fi; }
ok "--stills wrote 320x180 pictures and fx_status.tsv" "python3 -I -c \"from PIL import Image; import sys; sys.exit(0 if Image.open('$W/snap_0000.png').size == (320, 180) else 1)\" && [ -f $W/fx_status.tsv ]"
ok "fx_status.tsv says the missing plug-in is missing" "grep -q 'nobodyhasthis.*missing' $W/fx_status.tsv && grep -q 'blackandwhite.*ok' $W/fx_status.tsv"
ok "--stills names pictures by name=time and numbers the others" "'$CLI' --stills 0.5,1.5,x=2 --stills-dir $W/st --no-scan -q $W/fuzz.veg >/dev/null 2>&1 && [ -f $W/st/still_0000.png ] && [ -f $W/st/still_0001.png ] && [ -f $W/st/x.png ]"
ok "--stills refuses a time it cannot read" "! '$CLI' --stills abc --stills-dir $W/st2 --no-scan -q $W/fuzz.veg >/dev/null 2>&1"
ok "--help mentions --stills" "'$CLI' --help | grep -q -- '--stills TIMES'"
python3 -I "$HERE/../../tools/fuzz_compare.py" "$W" --cli "$CLI" --png 3 --top 40 --chunk 4 > "$W/compare.log" 2>&1 || true
cat "$W/compare.log"
python3 -I - "$W" <<'PY'
import sys, json
w = sys.argv[1]
r = {(x["kind"], x["plugin"], x["id"]): x for x in json.load(open(w + "/compare/report.json"))["results"]}
by = {x["id"]: x for x in r.values()}
ok = True
def check(c, msg):
    global ok
    print(("PASS " if c else "FAIL ") + msg)
    ok = ok and c
check(by[0]["status"] == "ok" and by[0]["mean"] < 0.01, "untouched Invert matches (mean %s)" % by[0].get("mean"))
check(by[1]["mean"] > 30, "brightened Black and White is found (mean %.1f)" % by[1]["mean"])
check(by[2]["mean"] > 1 and by[2]["pct_over_2"] > 5, "shifted Gaussian Blur is found (mean %.2f, %.1f%% over 2)" % (by[2]["mean"], by[2]["pct_over_2"]))
check(by[4]["status"] == "not available", "the plug-in nobody has is 'not available' (%s)" % by[4]["note"])
check(by[5]["status"] == "not compared", "transition with a snapshot missing is 'not compared' (%s)" % by[5]["note"])
check(by[6]["status"] == "ok" and by[6]["mean"] < 0.01, "untouched cross-fade matches (mean %s)" % by[6].get("mean"))
check(by[7]["status"] == "ok" and abs(by[7]["residual_db"]) > -999 and by[7]["residual_db"] < -60, "untouched first audio test matches (residual %.1f dB)" % by[7]["residual_db"])
check(by[8]["lag_samples"] == -5 or by[8]["lag_samples"] == 5, "5 samples late is found (lag %d)" % by[8]["lag_samples"])
check(abs(abs(by[8]["level_diff_db"]) - 3.0) < 0.3, "3 dB quieter is found (level %.2f dB)" % by[8]["level_diff_db"])
check(by[9]["residual_db"] > -60, "noisy audio test has a residual (%.1f dB)" % by[9]["residual_db"])
check(by[10]["status"] == "not available", "the audio plug-in nobody has is 'not available' (%s)" % by[10]["note"])
sys.exit(0 if ok else 1)
PY
RC=$?
echo "SUMMARY: stills $S passed, $F failed; compare script exit $RC"
[ $F = 0 ] && [ $RC = 0 ]
