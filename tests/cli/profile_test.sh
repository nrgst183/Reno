#!/bin/bash
# reno_cli --profile: the preview profiler's report on a small project with the test plug-in.
#   profile_test.sh <reno_cli> <testfx.ofx> <profile_make> <video file> <work folder>
set -u
CLI=$1; FX=$2; MAKE=$3; VIDEO=$4; WORK=$5
mkdir -p "$WORK"; pass=0; fail=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
"$MAKE" "$VIDEO" "$WORK/p.reno" || { echo "FAIL profile_make"; exit 1; }
export QT_QPA_PLATFORM=offscreen LC_ALL=C.UTF-8
OUT=$("$CLI" --profile "$WORK/p.reno" --range 0 2 --scale 0.5 --no-scan --plugin-path "$(dirname "$FX")" 2>"$WORK/err.txt"); code=$?
echo "$OUT" > "$WORK/out.txt"
ok "exit code 0" "[ $code = 0 ]"
ok "frames counted (60 frames in 2 s)" "grep -q '^frames    60 in' $WORK/out.txt"
ok "average and worst ms are printed" "grep -q 'average .* ms a frame, median .*worst .* ms' $WORK/out.txt"
ok "speed line" "grep -q '^speed ' $WORK/out.txt"
ok "the plug-in is in the table once, with 120 calls (2 uses x 60 frames)" "[ \$(grep -c 'reno.test.invert' $WORK/out.txt) = 1 ] && grep 'reno.test.invert' $WORK/out.txt | grep -q ' 120 '"
ok "the plug-in's thread safety is shown" "grep 'reno.test.invert' $WORK/out.txt | grep -q 'safe'"
ok "media decoding is listed with its seeks" "grep -q 'seeks' $WORK/out.txt && grep -q 'clip\|$(basename "$VIDEO" | cut -c1-20)' $WORK/out.txt"
ok "nothing is written beside the project" "[ \$(ls $WORK | grep -vc '^\(p.reno\|out.txt\|err.txt\)$') = 0 ]"
ok "--help mentions --profile" "'$CLI' --help | grep -q 'Profiling the preview'"
ok "--profile with an H.264 profile name still means that" "'$CLI' --profile high /nonexistent.mp4 2>&1 | grep -q 'is not there'"
ok "--scale out of range is refused" "! '$CLI' --profile $WORK/p.reno --scale 3 --no-scan >/dev/null 2>&1"
echo "SUMMARY: $pass passed, $fail failed"
[ $fail = 0 ]
