#!/bin/bash
# Tests reno_cli (src/cli/main.cpp), the command-line renderer and plug-in checker, on Linux: made-up media files
# rendered to every kind of output with the template options, the time range options, the errors and exit codes,
# Ctrl+C, and the sample VEGAS projects (read only: the files are written to the output folder). The files made are
# checked with ffprobe (streams, codecs, frame size, frame rate, sample rate, channels, length). The plug-in checking
# commands (check, ofxprobe, audioprobe, dxprobe: src/cli/plugincheck.cpp) are run on the test OpenFX plug-in
# (tests/ofx/testfx.cpp, built here with g++). With a rendercli_test program (tests/ui/rendercli_test.cpp, built with
# uibuild ... mainwindow) it also runs that: the same projects rendered by File > Render As and by reno_cli, compared
# picture by picture and sample by sample.
#
#   reno_cli_test.sh <reno_cli> <output folder> [sample projects folder] [rendercli_test]
#
# Needs ffmpeg and ffprobe. HOME is pointed at the output folder (so Reno's settings and plug-in list there are a
# fresh, empty set). Exit code: the number of failed checks (0: all passed).
set -u
RR=$(readlink -f "$1"); OUT=$(readlink -f "${2:-/tmp/reno_cli_test}"); VEG=${3:-${RENO_SAMPLES:-}}; UITEST=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT/home" "$OUT/media" "$OUT/r" "$OUT/ofx"
export HOME="$OUT/home" QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu LC_ALL=C.UTF-8
cd "$OUT"
pass=0; fail=0
ok()  { echo "PASS $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1${2:+: $2}"; fail=$((fail+1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', want '$3'"; fi; }
near() { if awk -v a="$2" -v b="$3" -v t="$4" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}'; then ok "$1"; else bad "$1" "got $2, want $3 (+-$4)"; fi; }
# ffprobe one value: probe FILE STREAM(v:0|a:0) ENTRY
probe() { ffprobe -v error -select_streams "$2" -show_entries "stream=$3" -of default=nw=1:nk=1 "$1" 2>/dev/null | head -1; }
dur() { ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$1" 2>/dev/null | head -1; }
nstreams() { ffprobe -v error -select_streams "$2" -show_entries stream=index -of csv=p=0 "$1" 2>/dev/null | wc -l; }
run() { "$RR" "$@" > "$OUT/last.log" 2>&1; echo $?; }

# ---- media
M=$OUT/media
[ -f $M/clip.mp4 ] || ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25:duration=4 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=4 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest $M/clip.mp4
[ -f $M/clipb.mp4 ] || ffmpeg -v error -y -f lavfi -i "smptebars=size=640x360:rate=25:duration=4" -f lavfi -i sine=frequency=660:sample_rate=48000:duration=4 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest $M/clipb.mp4
[ -f $M/tone.wav ] || ffmpeg -v error -y -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=3" -ac 2 $M/tone.wav
[ -f $M/still.png ] || ffmpeg -v error -y -f lavfi -i "testsrc=size=800x600:duration=1" -frames:v 1 $M/still.png
[ -f $M/long.mp4 ] || ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=30:duration=60 -f lavfi -i sine=frequency=300:sample_rate=48000:duration=60 \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac $M/long.mp4
R=$OUT/r

# ---- the command line
"$RR" --help > help.txt; check "--help exits 0" $? 0
grep -q "Examples:" help.txt && ok "--help has examples" || bad "--help has examples"
check "no arguments: the help, exit 2" "$(run)" 2
grep -q "^Usage:" last.log && grep -q "Examples:" last.log && grep -q "reno_cli check" last.log && ok "  no arguments prints the whole help" || bad "  no arguments prints the help" "$(head -3 last.log)"
check "unknown option: exit 2" "$(run $M/clip.mp4 --frobnicate)" 2
check "bad --size: exit 2" "$(run $M/clip.mp4 --size 12)" 2
check "missing input: exit 3" "$(run $M/none.mp4 -o $R/x.mp4)" 3
check "unknown template: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 -t 'No Such Template')" 2
check "ambiguous template: exit 2" "$(run $M/clip.mp4 -o $R/x.wav -t 'Default Template')" 2
grep -q "Several formats" last.log && ok "ambiguous template names the formats" || bad "ambiguous template names the formats" "$(cat last.log)"
check "--list-templates exits 0" "$(run --list-templates)" 0
grep -q "MAGIX AVC/AAC MP4" last.log && grep -q "Internet HD 1080p 29.97 fps" last.log && grep -q "Wave (Microsoft)" last.log && ok "--list-templates lists Render As's" || bad "--list-templates lists Render As's"
n=$(grep -E "^[0-9]+ templates" last.log | cut -d' ' -f1); [ "${n:-0}" -ge 100 ] && ok "--list-templates: $n templates" || bad "--list-templates count" "$n"
check "--list-templates 720p -v" "$(run -l 720p -v)" 0
grep -q "Video: 25.000 fps, 1280x720" last.log && ok "-l -v shows what a template makes" || bad "-l -v details"
check "--list-templates no match: exit 2" "$(run -l nothingmatches)" 2
"$RR" --version | grep -q "^reno_cli " && ok "--version" || bad "--version"

# ---- the plug-in checking commands (what reno.exe and the plug-in scan run, each in a process of its own)
g++ -std=c++17 -O2 -fPIC -shared "$ROOT/tests/ofx/testfx.cpp" -I"$ROOT/src/ofx" -I"$ROOT/third_party/openfx" -o $OUT/ofx/testfx.ofx -lpthread 2>/dev/null
if [ -f $OUT/ofx/testfx.ofx ]; then
    rm -f probe.json
    check "ofxprobe --no-render" "$(run ofxprobe --out probe.json --no-render $OUT/ofx/testfx.ofx)" 0
    grep -q '"id": "reno.test.invert"' probe.json && grep -q '"label": "Test Invert"' probe.json && ok "  the effects with their labels" || bad "  ofxprobe output" "$(head -c 400 probe.json)"
    check "ofxprobe (rendering a few frames)" "$(run ofxprobe --out probe2.json $OUT/ofx/testfx.ofx)" 0
    grep -q '"rendered"' probe2.json && ok "  frames rendered" || bad "  frames rendered"
    check "ofxprobe of a file that is not a plug-in: exit 0, an error said" "$(run ofxprobe --out probe3.json $M/tone.wav)" 0
    grep -q '"ok": false' probe3.json && ok "  not ok" || bad "  not a plug-in" "$(cat probe3.json)"
    check "audioprobe of a file that is not a plug-in" "$(run audioprobe --out aprobe.json --describe-only $M/tone.wav)" 0
    grep -q '"ok": false' aprobe.json && ok "  not ok" || bad "  audioprobe" "$(cat aprobe.json)"
    check "dxprobe" "$(run dxprobe --out dx.json)" 0
    [ -f dx.json ] && ok "  answered" || bad "  dxprobe answered"
    # an effect that gives no label of its own: the label in its bundle's resource file (OpenFX's resource overrides)
    NB=$OUT/ofx/nolabel/nolabelfx.ofx.bundle/Contents
    mkdir -p $NB/Linux-x86-64 $NB/Resources
    g++ -std=c++17 -O2 -fPIC -shared "$ROOT/tests/ofx/nolabelfx.cpp" -I"$ROOT/third_party/openfx" -o $NB/Linux-x86-64/nolabelfx.ofx 2>/dev/null
    printf '<OfxImageEffectResource><OfxPlugin name="reno.nolabel:resourceNamed"><OfxResourceSet ofxHost="default"><OfxPropLabel>Reno Resource Label</OfxPropLabel></OfxResourceSet></OfxPlugin></OfxImageEffectResource>\n' > $NB/Resources/nolabelfx.xml
    check "ofxprobe of a plug-in with a resource file" "$(run ofxprobe --out probe4.json --no-render $NB/Linux-x86-64/nolabelfx.ofx)" 0
    grep -q '"label": "Reno Resource Label"' probe4.json && ok "  its label from the resource file" || bad "  resource label" "$(grep label probe4.json)"
    rm -rf chk; mkdir -p chk
    OFX_PLUGIN_PATH=$OUT/ofx "$RR" check --out-dir chk --no-audio --no-dx > chk.log 2>&1; rc=$?
    check "check (every plug-in, each in a reno_cli of its own)" $rc 0
    grep -q "ok .*Test Invert  \[reno.test.invert\]" chk/plugin_report.txt && ok "  plugin_report.txt lists the test effects by label" || bad "  plugin_report.txt" "$(head -20 chk/plugin_report.txt)"
    [ -s chk/plugin_report.json ] && ok "  plugin_report.json written" || bad "  plugin_report.json"
fi

# ---- a media file, default output
rm -f $R/clip_def.mp4
check "media file to mp4" "$(run $M/clip.mp4 -o $R/clip_def.mp4)" 0
check "  video codec" "$(probe $R/clip_def.mp4 v:0 codec_name)" h264
check "  frame size (the media's)" "$(probe $R/clip_def.mp4 v:0 width)x$(probe $R/clip_def.mp4 v:0 height)" 640x360
check "  frame rate (the media's)" "$(probe $R/clip_def.mp4 v:0 r_frame_rate)" 25/1
check "  audio" "$(probe $R/clip_def.mp4 a:0 codec_name) $(probe $R/clip_def.mp4 a:0 sample_rate) $(probe $R/clip_def.mp4 a:0 channels)" "aac 48000 2"
near "  length" "$(dur $R/clip_def.mp4)" 4.0 0.05
grep -q "Rendered" last.log && grep -q "100%" last.log && grep -q "fps" last.log && ok "  progress and result printed" || bad "  progress printed"
# a frame: the same as the source (the media's own picture, through the engine and x264 at 16 Mbps)
psnr=$(ffmpeg -v error -i $R/clip_def.mp4 -i $M/clip.mp4 -lavfi "[0:v]trim=start_frame=30:end_frame=31[a];[1:v]trim=start_frame=30:end_frame=31[b];[a][b]psnr" -f null - 2>&1 | grep -o "average:[0-9.inf]*" | cut -d: -f2)
[ -z "$psnr" ] && psnr=$(ffmpeg -i $R/clip_def.mp4 -i $M/clip.mp4 -lavfi "[0:v][1:v]psnr" -f null - 2>&1 | grep -o "average:[0-9.inf]*" | cut -d: -f2)
awk -v p="${psnr:-0}" 'BEGIN{exit !(p=="inf" || p>35)}' && ok "  picture matches the source (PSNR $psnr dB)" || bad "  picture matches the source" "PSNR $psnr"
# default output name beside the input
cp $M/clip.mp4 $R/beside.mp4; rm -f $R/beside_render.mp4
check "no -o: beside the input" "$(run $R/beside.mp4)" 0
[ -f $R/beside_render.mp4 ] && ok "  made beside_render.mp4" || bad "  made beside_render.mp4"
# the file is there: stops unless -y
before=$(stat -c %Y $R/clip_def.mp4)
check "output exists: exit 5" "$(run $M/clip.mp4 -o $R/clip_def.mp4)" 5
check "  the file was left alone" "$(stat -c %Y $R/clip_def.mp4)" "$before"
check "-y replaces it" "$(run $M/clip.mp4 -o $R/clip_def.mp4 -y --length 1)" 0
near "  the new one" "$(dur $R/clip_def.mp4)" 1.0 0.05

# ---- templates
check "Format/Template (AVC 720p 25)" "$(run $M/clip.mp4 -o $R/t720.mp4 -y -t 'MAGIX AVC/AAC MP4/Internet HD 720p 25 fps')" 0
check "  1280x720 25 fps" "$(probe $R/t720.mp4 v:0 width)x$(probe $R/t720.mp4 v:0 height) $(probe $R/t720.mp4 v:0 r_frame_rate)" "1280x720 25/1"
check "a template by its name alone (only AVC has 480p)" "$(run $M/clip.mp4 -o $R/t480.mp4 -y -t 'internet 480p 25 fps widescreen' --length 1)" 0
check "  854x480 25 fps" "$(probe $R/t480.mp4 v:0 width)x$(probe $R/t480.mp4 v:0 height) $(probe $R/t480.mp4 v:0 r_frame_rate)" "854x480 25/1"
check "a name both AVC and HEVC have: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 -t 'Internet HD 720p 25 fps')" 2
check "Format/Template with the short name (avc/...)" "$(run $M/clip.mp4 -o $R/tshort.mp4 -y -t 'avc/Internet 360p 29.97 fps widescreen' --length 1)" 0
check "  640x360 29.97" "$(probe $R/tshort.mp4 v:0 width)x$(probe $R/tshort.mp4 v:0 height) $(probe $R/tshort.mp4 v:0 r_frame_rate)" "640x360 30000/1001"
check "WAV 24 bit" "$(run $M/clip.mp4 -o $R/a.wav -y -t 'Wave (Microsoft)/48,000 Hz, 24 Bit, Stereo, PCM')" 0
check "  pcm_s24le 48000 2" "$(probe $R/a.wav a:0 codec_name) $(probe $R/a.wav a:0 sample_rate) $(probe $R/a.wav a:0 channels)" "pcm_s24le 48000 2"
near "  length" "$(dur $R/a.wav)" 4.0 0.02
check "MP3 320" "$(run $M/clip.mp4 -o $R/a.mp3 -y -t 'MP3 Audio/320 Kbps, CD Transparent Audio')" 0
check "  mp3 44100" "$(probe $R/a.mp3 a:0 codec_name) $(probe $R/a.mp3 a:0 sample_rate)" "mp3 44100"
br=$(probe $R/a.mp3 a:0 bit_rate); near "  320 Kbps" "${br:-0}" 320000 2000
check "FLAC by extension" "$(run $M/clip.mp4 -o $R/a.flac -y)" 0
check "  flac" "$(probe $R/a.flac a:0 codec_name)" flac
check "ProRes 422 HQ (.mov)" "$(run $M/clip.mp4 -o $R/p.mov -y -t 'QuickTime 7/Apple ProRes 422 HQ' --length 1)" 0
check "  prores pcm" "$(probe $R/p.mov v:0 codec_name) $(probe $R/p.mov a:0 codec_name)" "prores pcm_s16le"
check "HEVC template" "$(run $M/clip.mp4 -o $R/h.mp4 -y -t 'MAGIX HEVC/AAC MP4/Internet HD 720p 29.97 fps' --length 1)" 0
check "  hevc" "$(probe $R/h.mp4 v:0 codec_name)" hevc
check "WebM" "$(run $M/clip.mp4 -o $R/w.webm -y -t 'WebM/HD 720p, project frame rate' --length 1)" 0
check "  vp9 opus" "$(probe $R/w.webm v:0 codec_name) $(probe $R/w.webm a:0 codec_name)" "vp9 opus"
check "uncompressed AVI" "$(run $M/clip.mp4 -o $R/u.avi -y -t 'Video for Windows/Default Template (uncompressed)' --length 0.4)" 0
check "  rawvideo 10 frames" "$(probe $R/u.avi v:0 codec_name) $(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $R/u.avi)" "rawvideo 10"

# ---- overrides
check "--size --fps --crf --audio-bitrate" "$(run $M/clip.mp4 -o $R/o.mp4 -y --size 320x180 --fps 29.97 --crf 30 --audio-bitrate 96k)" 0
check "  320x180 30000/1001" "$(probe $R/o.mp4 v:0 width)x$(probe $R/o.mp4 v:0 height) $(probe $R/o.mp4 v:0 r_frame_rate)" "320x180 30000/1001"
abr=$(probe $R/o.mp4 a:0 bit_rate); near "  96 Kbps audio" "${abr:-0}" 96000 12000
check "--video-bitrate 1M" "$(run $M/clip.mp4 -o $R/vb.mp4 -y --video-bitrate 1M --size 640x360)" 0
vbr=$(probe $R/vb.mp4 v:0 bit_rate); near "  about 1 Mbps" "${vbr:-0}" 1000000 400000
check "--video-codec hevc into MKV" "$(run $M/clip.mp4 -o $R/v.mkv -y -t 'Matroska (MKV)/Project size and rate' --video-codec hevc --length 1)" 0
check "  hevc in mkv" "$(probe $R/v.mkv v:0 codec_name)" hevc
check "--video-codec as an encoder (libx264) and --audio-codec mp3" "$(run $M/clip.mp4 -o $R/enc.mkv -y -t 'Matroska (MKV)/Project size and rate' --video-codec libx264 --audio-codec mp3 --length 1)" 0
check "  h264 mp3" "$(probe $R/enc.mkv v:0 codec_name) $(probe $R/enc.mkv a:0 codec_name)" "h264 mp3"
check "--sample-rate 44100 --channels 1" "$(run $M/clip.mp4 -o $R/m.wav -y --sample-rate 44100 --channels 1)" 0
check "  44100 mono" "$(probe $R/m.wav a:0 sample_rate) $(probe $R/m.wav a:0 channels)" "44100 1"
check "--two-pass" "$(run $M/clip.mp4 -o $R/2p.mp4 -y --two-pass --video-bitrate 2M --length 1)" 0
grep -q "two passes" last.log && ok "  the encoder made two passes" || bad "  two passes" "$(grep encoder last.log)"
ls $R/2p.mp4.renopass* $R/2p.mp4.pass1* > /dev/null 2>&1 && bad "  pass files left" || ok "  no pass files left"
check "--audio-only (mp4)" "$(run $M/clip.mp4 -o $R/ao.mp4 -y --audio-only)" 0
check "  no video, one audio stream" "$(nstreams $R/ao.mp4 v) $(nstreams $R/ao.mp4 a)" "0 1"
check "--video-only" "$(run $M/clip.mp4 -o $R/vo.mp4 -y --video-only)" 0
check "  one video, no audio stream" "$(nstreams $R/vo.mp4 v) $(nstreams $R/vo.mp4 a)" "1 0"
check "--audio-only --video-only: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 --audio-only --video-only)" 2
check "--video-only to WAV: exit 2" "$(run $M/clip.mp4 -o $R/x.wav --video-only)" 2
check "--threads 1" "$(run $M/clip.mp4 -o $R/th.mp4 -y --threads 1 --length 1)" 0
near "  length" "$(dur $R/th.mp4)" 1.0 0.05
check "--render-quality draft --loudness-log" "$(run $M/clip.mp4 -o $R/q.mp4 -y --render-quality draft --loudness-log --length 2)" 0
[ -s $R/q_loudness.txt ] && grep -q "Integrated" $R/q_loudness.txt && ok "  loudness log written" || bad "  loudness log written"
check "--width 320 alone (the height keeps the shape)" "$(run $M/clip.mp4 -o $R/wd.mp4 -y --width 320 --length 1)" 0
check "  320x180" "$(probe $R/wd.mp4 v:0 width)x$(probe $R/wd.mp4 v:0 height)" 320x180
check "--height 240 --quality draft" "$(run $M/clip.mp4 -o $R/ht.mp4 -y --height 240 --quality draft --length 1)" 0
check "  426x240" "$(probe $R/ht.mp4 v:0 width)x$(probe $R/ht.mp4 v:0 height)" 426x240
check "--size with --width: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 --size 320x180 --width 100)" 2
check "--quality wrong: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 --quality superb)" 2
# stdout: only the result line; the progress and messages on stderr
"$RR" $M/clip.mp4 -o $R/so.mp4 -y --length 1 > so.out 2> so.err; rc=$?
check "stdout/stderr run" $rc 0
check "  stdout: one line" "$(wc -l < so.out | tr -d ' ')" 1
grep -q "^Rendered .*so.mp4" so.out && ok "  stdout: Rendered <file>" || bad "  stdout: Rendered <file>" "$(cat so.out)"
grep -q "100%" so.err && grep -q "fps" so.err && grep -q "Template:" so.err && ok "  stderr: progress and messages" || bad "  stderr: progress" "$(cat so.err)"
check "--plugin-path that is not there: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 -y --plugin-path $OUT/nothing_here)" 2
check "a still picture" "$(run $M/still.png -o $R/still.mp4 -y)" 0
check "  800x600, 5 s (the still length preference)" "$(probe $R/still.mp4 v:0 width)x$(probe $R/still.mp4 v:0 height)" 800x600
near "  still length" "$(dur $R/still.mp4)" 5.0 0.1
check "a sound file" "$(run $M/tone.wav -o $R/tone.mp3 -y)" 0
near "  3 s" "$(dur $R/tone.mp3)" 3.0 0.1

# ---- the range
check "--start 1 --end 3" "$(run $M/clip.mp4 -o $R/r1.mp4 -y --start 1 --end 3)" 0
near "  2 s" "$(dur $R/r1.mp4)" 2.0 0.05
check "--start 00:00:01.500 --length 1.5" "$(run $M/clip.mp4 -o $R/r2.mp4 -y --start 00:00:01.500 --length 1.5)" 0
near "  1.5 s" "$(dur $R/r2.mp4)" 1.5 0.05
check "--start 00:00:01;00 (timecode) --end 75f" "$(run $M/clip.mp4 -o $R/r3.mp4 -y --start '00:00:01;00' --end 75f)" 0
near "  2 s (1 s to frame 75 at 25 fps)" "$(dur $R/r3.mp4)" 2.0 0.05
check "--loop-region 0.5,1.3 to an image sequence" "$(run $M/clip.mp4 -o $R/seq/shot.png -y -t 'Image Sequence/PNG' --loop-region 0.5,1.3)" 0
check "  20 numbered PNGs" "$(ls $R/seq/shot_*.png 2>/dev/null | wc -l)" 20
[ -f $R/seq/shot_000000.png ] && [ -f $R/seq/shot_000019.png ] && ok "  shot_000000.png .. shot_000019.png" || bad "  sequence names"
check "  640x360 pictures" "$(probe $R/seq/shot_000005.png v:0 width)x$(probe $R/seq/shot_000005.png v:0 height)" 640x360
check "--range 1 2.5" "$(run $M/clip.mp4 -o $R/rg.mp4 -y --range 1 2.5)" 0
near "  1.5 s" "$(dur $R/rg.mp4)" 1.5 0.05
check "--range with one time: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 -y --range 1)" 2
check "--end before --start: exit 3" "$(run $M/clip.mp4 -o $R/x.mp4 -y --start 3 --end 2)" 3
check "--loop-region wrong: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 -y --loop-region 3)" 2
check "--region in a project without regions: exit 2" "$(run $M/clip.mp4 -o $R/x.mp4 -y --region Chorus)" 2

# ---- Ctrl+C
rm -f $R/cancel.mp4
"$RR" $M/long.mp4 -o $R/cancel.mp4 -y > cancel.log 2>&1 &
P=$!; sleep 6; kill -INT $P; wait $P; rc=$?
check "Ctrl+C: exit 130" "$rc" 130
[ -f $R/cancel.mp4 ] && bad "  the half-made file was deleted" || ok "  the half-made file was deleted"
grep -q "stopped" cancel.log && ok "  says it stopped" || bad "  says it stopped" "$(tail -2 cancel.log)"
rm -rf $R/seqc; "$RR" $M/long.mp4 -o $R/seqc/f.png -t 'Image Sequence/JPEG' -y > cancel2.log 2>&1 &
P=$!; sleep 6; kill -INT $P; wait $P; rc=$?
check "Ctrl+C an image sequence: exit 130" "$rc" 130
check "  its pictures deleted" "$(ls $R/seqc 2>/dev/null | wc -l)" 0

# ---- the sample VEGAS projects (read only; their media is not on this machine, so it is listed as missing)
if [ -n "$VEG" ] && [ -d "$VEG" ]; then
    k=0
    while IFS= read -r v; do
        k=$((k+1)); name=$(basename "$v" .veg)
        rc=$(run "$v" -o "$R/veg_$k.mp4" -y --no-scan --length 2 --size 320x180 --fps 30)
        check "VEGAS project $name renders" "$rc" 0
        grep -q "missing" last.log && ok "  $name: missing media listed" || echo "  (note: $name lists no missing media)"
        near "  $name: 2 s" "$(dur "$R/veg_$k.mp4")" 2.0 0.1
        check "  $name: 320x180 30 fps, audio" "$(probe "$R/veg_$k.mp4" v:0 width)x$(probe "$R/veg_$k.mp4" v:0 height) $(probe "$R/veg_$k.mp4" v:0 r_frame_rate) $(nstreams "$R/veg_$k.mp4" a)" "320x180 30/1 1"
    done < <(find "$VEG" -name '*.veg' | sort | head -4)
    # one project for the strict / audio-only checks: $RENO_SAMPLE_VEG, else the first .veg found
    v=${RENO_SAMPLE_VEG:-$(find "$VEG" -name '*.veg' | sort | head -1)}
    if [ -n "$v" ] && [ -f "$v" ]; then
        run "$v" -o $R/lenient.mp4 -y --no-scan --length 1 > /dev/null
        if grep -q "missing" last.log; then
            check "--strict with missing media: exit 4" "$(run "$v" -o $R/strict.mp4 -y --no-scan --strict --length 1)" 4
            [ -f $R/strict.mp4 ] && bad "  nothing rendered" || ok "  nothing rendered"
        fi
        check "WAV of a VEGAS project's region-less range, audio only" "$(run "$v" -o $R/vegaudio.wav -y --no-scan --audio-only --start 0.5 --length 1)" 0
        near "  1 s" "$(dur $R/vegaudio.wav)" 1.0 0.02
    fi
fi

# ---- File > Render As against reno_cli (tests/ui/rendercli_test.cpp)
if [ -n "$UITEST" ] && [ -x "$UITEST" ]; then
    rm -rf $OUT/ui; mkdir -p $OUT/ui
    "$UITEST" $OUT/ui "$RR" $M/clip.mp4 $M/clipb.mp4 $M/tone.wav $OUT/ofx/testfx.ofx > ui.log 2>&1; rc=$?
    grep -E "^(PASS|FAIL)" ui.log | sed 's/^/  ui: /'
    p=$(grep -c "^PASS" ui.log); f=$(grep -c "^FAIL" ui.log)
    pass=$((pass+p)); fail=$((fail+f))
    [ $rc -eq 0 ] || [ $f -gt 0 ] || bad "rendercli_test ran" "exit $rc: $(tail -5 ui.log)"
    # the project it made has the test OpenFX effect (reno.test.invert): without the plug-in's folder it is listed as
    # missing; with --plugin-path (no OFX_PLUGIN_PATH, a fresh plug-in list; reno_cli tries it in a process of its
    # own) it renders the same pictures as File > Render As did with it
    P=$OUT/ui/rendercli_project.reno
    if [ -f "$P" ] && [ -f $OUT/ui/reno_dialog.avi ]; then
        rm -rf "$HOME/.config/reno"
        AVI='Video for Windows/Default Template (uncompressed)'
        check "OpenFX effect missing: still renders" "$(run "$P" -o $R/pp_none.avi -t "$AVI" -y --no-scan)" 0
        grep -q "cannot be loaded" last.log && grep -q "Invert \[reno.test.invert\]" last.log && ok "  the missing effect is listed (its name and identifier)" || bad "  the missing effect is listed" "$(head -20 last.log)"
        check "  --strict: exit 4" "$(run "$P" -o $R/pp_strict.avi -t "$AVI" -y --no-scan --strict)" 4
        check "--plugin-path with the test OpenFX plug-in" "$(run "$P" -o $R/pp.avi -t "$AVI" -y --no-scan --plugin-path $OUT/ofx)" 0
        grep -q "cannot be loaded" last.log && bad "  nothing missing" "$(head -20 last.log)" || ok "  nothing missing"
        a=$(ffmpeg -v error -i $R/pp.avi -map 0:v -f framemd5 - | grep -v '^#' | cut -d, -f6 | md5sum)
        b=$(ffmpeg -v error -i $OUT/ui/reno_dialog.avi -map 0:v -f framemd5 - | grep -v '^#' | cut -d, -f6 | md5sum)
        c=$(ffmpeg -v error -i $R/pp_none.avi -map 0:v -f framemd5 - | grep -v '^#' | cut -d, -f6 | md5sum)
        check "  every picture the same as File > Render As's" "$a" "$b"
        [ "$a" != "$c" ] && ok "  (and not the same as without the effect)" || bad "  differs from the render without the effect"
        [ -f "$HOME/.config/reno/plugins.json" ] && grep -q testfx "$HOME/.config/reno/plugins.json" && bad "  Reno's plug-in list left alone" || ok "  Reno's plug-in list left alone"
    fi
fi

echo
echo "reno_cli_test: $pass passed, $fail failed"
exit $fail
