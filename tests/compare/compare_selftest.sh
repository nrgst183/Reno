#!/bin/bash
# Checks the comparison tools without VEGAS: makes the suite's media and projects, renders Reno's own .veg of every
# test twice with Reno's renderer (run_reno.sh --source reno), and
#  1. compares the two sets: Reno against Reno must match everywhere;
#  2. compares Reno against a deliberately altered copy (a frame late, brighter, blurred, one black frame, scaled,
#     the sound 1 dB quieter, 10 ms late, left and right swapped) and checks that each change is found and measured
#     (offset in frames and samples, level in dB, the worst frame, the size note).
# Exit code: the number of failed checks.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(readlink -f "$1"); MS=${2:-${RENO_MAKESUITE:-makesuite}}; export RENO_RENDER=${3:-${RENO_RENDER:-reno_cli}}
fail=0
"$HERE/make_media.sh" "$ROOT/suite" > /dev/null || exit 1
"$MS" "$ROOT" || exit 1
"$HERE/run_reno.sh" "$ROOT" --source reno --out self_a > /dev/null || { echo "FAIL render self_a"; fail=$((fail+1)); }
"$HERE/run_reno.sh" "$ROOT" --source reno --out self_b > /dev/null || { echo "FAIL render self_b"; fail=$((fail+1)); }

# ---- 0. uncompressed AVI is RGB, as VEGAS writes it (not YUV 4:2:0, which would lose colour)
pf=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,pix_fmt -of csv=p=0 "$ROOT/renders/self_a/cut_basic.avi")
if [ "$pf" = "rawvideo,bgr24" ]; then echo "PASS uncompressed AVI is 24-bit RGB"; else echo "FAIL uncompressed AVI is 24-bit RGB (got $pf)"; fail=$((fail+1)); fi

# ---- 1. Reno against Reno
python3 "$HERE/compare.py" "$ROOT" --a self_a --b self_b --out "$ROOT/report_selftest_same" --title "Reno vs Reno" > "$ROOT/selftest_same.log"
python3 - "$ROOT/report_selftest_same/summary.json" <<'EOF' || fail=$((fail+1))
import json, sys
s = json.load(open(sys.argv[1]))
res = s["results"]; bad = [r["id"] for r in res if r["verdict"] != "match"]
n = len(res)
print(("PASS" if not bad and n >= 40 else "FAIL") + " Reno vs Reno: %d tests, all match" % n + ("" if not bad else ": not matching " + ", ".join(bad)))
off = [r["id"] for r in res if (r.get("video") or {}).get("offset_frames") or (r.get("audio") or {}).get("offset_samples")]
print(("PASS" if not off else "FAIL") + " Reno vs Reno: no offsets" + ("" if not off else " (" + ", ".join(off) + ")"))
sys.exit(1 if bad or off or n < 40 else 0)
EOF

# ---- 2. altered copies
A="$ROOT/renders/self_a"; X="$ROOT/renders/altered"; rm -rf "$X"; mkdir -p "$X"
ff() { ffmpeg -v error -y "$@"; }
V="-c:v rawvideo -pix_fmt bgr24 -c:a pcm_s16le"
# one frame late (the first frame shown twice), same length
n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$A/cut_basic.avi")
ff -i "$A/cut_basic.avi" -vf "tpad=start=1:start_mode=clone" -frames:v "$n" $V "$X/cut_basic.avi"
ff -i "$A/fade_linear.avi" -vf "eq=brightness=0.04" $V "$X/fade_linear.avi"
ff -i "$A/fx_gaussianblur.avi" -vf "gblur=sigma=1.5" $V "$X/fx_gaussianblur.avi"
ff -i "$A/composite_add.avi" -vf "drawbox=x=0:y=0:w=iw:h=ih:color=black:t=fill:enable='eq(n,10)'" $V "$X/composite_add.avi"
ff -i "$A/still_letterbox.avi" -vf "scale=640:360" $V "$X/still_letterbox.avi"
ff -i "$A/audio_plain.wav" -af "volume=-1dB" -c:a pcm_s16le "$X/audio_plain.wav"
ff -i "$A/audio_clicks_mix.wav" -af "adelay=10|10,atrim=end_sample=$(ffprobe -v error -select_streams a:0 -count_packets -show_entries stream=duration_ts -of csv=p=0 "$A/audio_clicks_mix.wav")" -c:a pcm_s16le "$X/audio_clicks_mix.wav"
ff -i "$A/audio_pan_left.wav" -af "pan=stereo|c0=c1|c1=c0" -c:a pcm_s16le "$X/audio_pan_left.wav"
cp "$A/cut_offset.avi" "$X/cut_offset.avi"          # (one left the same)
python3 "$HERE/compare.py" "$ROOT" --a self_a --b altered --only cut_basic,fade_linear,fx_gaussianblur,composite_add,still_letterbox,audio_plain,audio_clicks_mix,audio_pan_left,cut_offset,gen_noise \
    --out "$ROOT/report_selftest_altered" --title "Reno vs an altered copy" --no-fail > "$ROOT/selftest_altered.log"
python3 - "$ROOT/report_selftest_altered/summary.json" <<'EOF' || fail=$((fail+1))
import json, sys
r = {x["id"]: x for x in json.load(open(sys.argv[1]))["results"]}
bad = 0
def check(name, ok, detail=""):
    global bad
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    bad += 0 if ok else 1
v = lambda i: r[i].get("video") or {}
a = lambda i: r[i].get("audio") or {}
check("a frame late: offset +1 frame found", v("cut_basic").get("offset_frames") == 1, v("cut_basic").get("offset_scores"))
check("a frame late: differs", r["cut_basic"]["verdict"] == "differs")
check("a frame late: matches once shifted", v("cut_basic").get("psnr_mean_shifted", 0) > 40, v("cut_basic").get("psnr_mean_shifted"))
check("brighter: not a match", r["fade_linear"]["verdict"] != "match" and v("fade_linear")["psnr_mean"] < 35, v("fade_linear").get("psnr_mean"))
check("blurred: not a match", r["fx_gaussianblur"]["verdict"] != "match", v("fx_gaussianblur").get("psnr_mean"))
w = v("composite_add").get("worst", [{}])[0]
check("one black frame: worst frame is 10", w.get("frame") == 10, w)
check("one black frame: the others match", sorted(v("composite_add").get("per_frame_psnr", [0]))[1] >= 99, sorted(v("composite_add").get("per_frame_psnr", [0]))[:3])
check("one black frame: its picture saved", w.get("image", "").endswith(".png"))
check("scaled: the size difference is said", v("still_letterbox").get("size_b") == [640, 360] and "sizes differ" in v("still_letterbox").get("note", ""))
check("1 dB quieter: level -1 dB", abs((a("audio_plain").get("level_diff_db") or 0) + 1.0) < 0.05, a("audio_plain").get("level_diff_db"))
check("1 dB quieter: alike once the level is matched", a("audio_plain").get("gain_matched_sdr_db", 0) > 50, a("audio_plain").get("gain_matched_sdr_db"))
check("1 dB quieter: not a match", r["audio_plain"]["verdict"] != "match")
check("10 ms late: offset 480 samples", a("audio_clicks_mix").get("offset_samples") == 480, a("audio_clicks_mix").get("offset_samples"))
check("10 ms late: differs", r["audio_clicks_mix"]["verdict"] == "differs")
pl = a("audio_pan_left")
check("left and right swapped: per-channel levels swapped", pl and abs(pl["rms_db_a"][0] - pl["rms_db_b"][1]) < 0.1 and abs(pl["rms_db_a"][0] - pl["rms_db_b"][0]) > 3, pl and (pl["rms_db_a"], pl["rms_db_b"]))
check("left and right swapped: differs", r["audio_pan_left"]["verdict"] == "differs")
check("unchanged copy: match", r["cut_offset"]["verdict"] == "match")
check("no render: missing", r["gen_noise"]["verdict"] == "missing")
sys.exit(1 if bad else 0)
EOF
# ---- 3. a plug-in Reno cannot run here is marked, not counted
python3 - "$ROOT/report_selftest_same/summary.json" <<'EOF' || fail=$((fail+1))
import json, sys
s = json.load(open(sys.argv[1]))
r = {x["id"]: x for x in s["results"]}
ok = r["fx_cookiecutter"].get("mark") == "reno-cannot-run" and not r["fx_cookiecutter"]["counted"] and r["transition_dissolve"].get("mark") == "reno-cannot-run"
print(("PASS" if ok else "FAIL") + " effects Reno cannot run on Linux are marked and not counted (%d marked)" % s["summary"]["reno_cannot_run"])
sys.exit(0 if ok else 1)
EOF
echo "compare_selftest: $fail failed"
exit $fail
