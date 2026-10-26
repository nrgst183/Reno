#!/bin/bash
# Renders the comparison suite (tests/compare/makesuite.cpp) with Reno's command-line renderer on Linux, to the same
# kind of files Reno Bridge asks VEGAS for: uncompressed AVI ("Video for Windows / Default Template (uncompressed)":
# 24-bit RGB pictures, 16-bit PCM sound) or, for sound-only tests, a 16-bit WAV at the project's rate.
#
#   run_reno.sh <bridge folder> [--source vegas|reno] [--out NAME] [--only id,id...] [--renderer PATH]
#     --source vegas   render suite/<id>.veg, the project VEGAS built and saved (default; the real comparison)
#     --source reno    render suite/<id>.reno.veg, Reno's own .veg of the project (works before VEGAS has run)
#     --out NAME       renders/<NAME>/<id>.avi|.wav (default: reno for --source vegas, reno_renoveg for reno)
#     --renderer PATH  the renderer (default $RENO_RENDER, else reno_cli from the path)
# Writes renders/<NAME>/<id>.log and renders/<NAME>/status.tsv (id, exit code, seconds). Exit code: failed renders.
set -u
ROOT=$(readlink -f "$1"); shift
SRC=vegas; NAME=""; ONLY=""; RR=${RENO_RENDER:-reno_cli}
while [ $# -gt 0 ]; do
  case "$1" in
    --source) SRC=$2; shift 2;;
    --out) NAME=$2; shift 2;;
    --only) ONLY=",$2,"; shift 2;;
    --renderer) RR=$2; shift 2;;
    *) echo "unknown option $1"; exit 2;;
  esac
done
[ -n "$NAME" ] || { [ "$SRC" = vegas ] && NAME=reno || NAME=reno_renoveg; }
OUT="$ROOT/renders/$NAME"; mkdir -p "$OUT" "$ROOT/home"
# Reno's settings and plug-in list: a fresh set of its own, kept between runs
export HOME="$ROOT/home" QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=${QT_QPA_FONTDIR:-/usr/share/fonts/truetype/dejavu} LC_ALL=C.UTF-8
: > "$OUT/status.tsv"
failed=0; done_=0
python3 -c "
import json,sys
for t in json.load(open('$ROOT/manifest.json'))['tests']:
    print(t['id'], 1 if t['audio_only'] else 0)
" | while read -r id audio; do
  [ -z "$ONLY" ] || [[ "$ONLY" == *",$id,"* ]] || continue
  if [ "$SRC" = vegas ]; then in="$ROOT/suite/$id.veg"; else in="$ROOT/suite/$id.reno.veg"; fi
  if [ ! -f "$in" ]; then echo -e "$id\tmissing\t0" >> "$OUT/status.tsv"; echo "SKIP $id (no $in)"; continue; fi
  t0=$(date +%s.%N)
  if [ "$audio" = 1 ]; then
    $RR "$in" -o "$OUT/$id.wav" --audio-only --sample-rate project --audio-bits 16 --channels 2 -y > "$OUT/$id.log" 2>&1
  else
    $RR "$in" -o "$OUT/$id.avi" -t "Video for Windows/Default Template (uncompressed)" -y > "$OUT/$id.log" 2>&1
  fi
  rc=$?
  t1=$(date +%s.%N)
  secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
  echo -e "$id\t$rc\t$secs" >> "$OUT/status.tsv"
  if [ $rc = 0 ]; then echo "ok   $id ($secs s)"; else echo "FAIL $id (exit $rc; see $OUT/$id.log)"; fi
done
failed=$(awk -F'\t' '$2 != "0" && $2 != "missing"' "$OUT/status.tsv" | wc -l)
echo "run_reno: $(wc -l < "$OUT/status.tsv") projects, $failed failed -> $OUT"
exit $failed
