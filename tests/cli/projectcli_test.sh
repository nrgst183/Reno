#!/bin/bash
# Tests reno_cli's project commands (src/cli/projectcli.cpp): a project made with new and edit (tracks, events from
# media files, generated media, effects with settings and keyframes, a transition, audio effects, markers, a script
# file), checked with info, saved as .reno and .veg, and rendered; the errors and exit codes. The rendered pictures are
# checked with ffmpeg (a generated colour, an effect's keyframes changing the picture).
#
#   projectcli_test.sh <reno_cli> [output folder]
#
# Needs ffmpeg and ffprobe. HOME is pointed at the output folder (Reno's settings and plug-in list there are a fresh,
# empty set). Exit code: the number of failed checks (0: all passed).
set -u
RR=$(readlink -f "$1"); OUT=$(readlink -f "${2:-/tmp/projectcli_test}")
mkdir -p "$OUT/home" "$OUT/media"
export HOME="$OUT/home" QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu LC_ALL=C.UTF-8
cd "$OUT"
rm -f *.reno *.veg *.mp4 *.png *.txt *.log
pass=0; fail=0
ok()  { echo "PASS $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1${2:+: $2}"; fail=$((fail+1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', want '$3'"; fi; }
has() { if grep -qF -- "$2" "$3"; then ok "$1"; else bad "$1" "no '$2' in $3: $(head -c 600 "$3")"; fi; }
hasnt() { if grep -qF -- "$2" "$3"; then bad "$1" "'$2' is in $3"; else ok "$1"; fi; }
run() { "$RR" "$@" > "$OUT/last.out" 2> "$OUT/last.err"; echo $?; }
# the colour (r g b, 0..255) of pixel X Y of the frame at T seconds of FILE
pixel() { ffmpeg -v error -ss "$2" -i "$1" -frames:v 1 -vf "crop=2:2:$3:$4,scale=1:1" -f rawvideo -pix_fmt rgb24 - 2>/dev/null | od -An -tu1 | xargs; }
near3() {   # near3 NAME "r g b" "r g b" tolerance
    if awk -v a="$2" -v b="$3" -v t="$4" 'BEGIN{split(a,x," ");split(b,y," ");for(i=1;i<=3;i++){d=x[i]-y[i];if(d<0)d=-d;if(d>t)exit 1}}'; then ok "$1"; else bad "$1" "got '$2', want '$3' (+-$4)"; fi
}

M=$OUT/media
[ -f $M/clip.mp4 ] || ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25:duration=4 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=4 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest $M/clip.mp4
[ -f $M/clipb.mp4 ] || ffmpeg -v error -y -f lavfi -i "smptebars=size=640x360:rate=25:duration=4" -c:v libx264 -pix_fmt yuv420p $M/clipb.mp4
[ -f $M/tone.wav ] || ffmpeg -v error -y -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=3" -ac 2 $M/tone.wav
[ -f $M/grey.png ] || ffmpeg -v error -y -f lavfi -i "color=c=0x808080:size=640x360:duration=1" -frames:v 1 $M/grey.png

# ---- the help
"$RR" --help > help.txt
has "--help has the project commands" "reno_cli new FILE" help.txt
has "  and the edit commands" "transition E PLUGIN" help.txt

# ---- new: settings and the commands after it
check "new with commands: exit 0" "$(run new p.reno --size 640x360 --fps 25 + add $M/clip.mp4 + add $M/tone.wav --at 1 + marker 1.5 Hit + region 0 2 Intro)" 0
has "  prints the events made" "event 1.1 (id" last.out
has "  says it saved" "Saved p.reno" last.err
[ -f p.reno ] && ok "  p.reno was written" || bad "  p.reno was written"
check "new over an existing file: exit 5" "$(run new p.reno)" 5
check "info: exit 0" "$(run info p.reno)" 0
cp last.out info1.txt
has "  the settings" "640x360, 25 fps (25/1)" info1.txt
has "  a video track with the clip" "Track 1 (video)" info1.txt
has "  the clip's sound on an audio track, grouped" "Track 2 (audio)" info1.txt
has "  the tone on a track of its own, at 1 s" "event 3.1" info1.txt
grep -q "event 3.1 (id [0-9]*): 1 to 4 s, tone.wav" info1.txt && ok "  the tone from 1 to 4 s" || bad "  the tone from 1 to 4 s" "$(grep tone.wav info1.txt)"
has "  the marker" 'Marker 1.5 s "Hit"' info1.txt
has "  the region" 'Region 0 to 2 s "Intro"' info1.txt

# ---- edit: tracks, generated media, effects
check "add-track + generate with a setting: exit 0" "$(run edit p.reno add-track video --name Top --at 1 + generate 'Solid Color' --track 1 --at 0 --length 1 'Color=#ff8000')" 0
check "fx with settings and keyframes: exit 0" "$(run edit p.reno fx 2.1 'Gaussian Blur' 'Horizontal range=0.2' 'Vertical range@0=0' 'Vertical range@2=0.5' Alpha=off)" 0
has "  says where it went" "event 2.1 fx 1: Gaussian Blur" last.out
check "audio effects: exit 0" "$(run edit p.reno fx master 'eFX Compressor' Threshold=-20 Ratio=8 'Mode=Ext. Sidechain' + fx track:4 'eFX Reverb' + bypass track:4 1 on)" 0
check "a video clip over the first, on its track, and a transition: exit 0" "$(run edit p.reno add $M/clipb.mp4 --track 2 --at 3 --video-only + transition 2.2 '3D Blinds' Divisions=4)" 0
check "video-out effect, track and event changes: exit 0" "$(run edit p.reno fx video-out 'Brightness and Contrast' Brightness=0.1 + track 3 --volume -6 --pan -0.5 + event 2.1 --fade-in 0.5 --name Opening + track 1 --name Colour)" 0
run info p.reno --params > /dev/null; cp last.out info2.txt
has "  the track's new name" 'Track 1 (video) "Colour"' info2.txt
has "  the colour" "Color = 1,0.501961,0,1" info2.txt
has "  the blur's range" "Horizontal range = 0.2" info2.txt
has "  the blur's keyframes" "Vertical range = keyframes 0@0s 0.5@2s" info2.txt
has "  Alpha off" "Alpha = off" info2.txt
has "  the clip on track 2 next to the first (as asked)" "event 2.2" info2.txt
has "  its transition" "transition: 3D Blinds" info2.txt
has "  the transition's setting" "Divisions = 4" info2.txt
has "  the compressor's threshold" "Threshold = -20.0dB" info2.txt
has "  its ratio" "Ratio = 8.0 : 1" info2.txt
has "  its mode (a choice by its text)" "Mode = Ext. Sidechain" info2.txt
has "  the reverb, bypassed" "eFX Reverb  (VST2||1297625651)  [bypassed]" info2.txt
has "  the video output's effect" "Brightness = 0.1" info2.txt
has "  the track's volume and pan" "Track 3 (audio) volume -6 dB pan -0.5" info2.txt
has "  the event's name" '"Opening"' info2.txt
hasnt "  no placeholder audio settings" "      - = " info2.txt

# ---- set and remove-fx
check "set a keyframe on an existing effect: exit 0" "$(run edit p.reno set 2.1 1 'Horizontal range@1=0.05')" 0
run info p.reno --params > /dev/null
has "  keyed" "Horizontal range = keyframes 0.05@1s" last.out
check "remove-fx: exit 0" "$(run edit p.reno remove-fx video-out 1)" 0
run info p.reno > /dev/null
hasnt "  gone" "Video Output:" last.out

# ---- errors: nothing is saved
cp p.reno before.reno
check "an unknown event: exit 1" "$(run edit p.reno fx 9.9 'Gaussian Blur')" 1
has "  says why" "there is no track 9" last.err
has "  and that nothing was saved" "p.reno was not saved" last.err
check "a name that could be several effects: exit 1" "$(run edit p.reno fx 2.1 Blur)" 1
has "  lists them" "Gaussian Blur  (com.vegascreativesoftware:gaussianblur)" last.err
check "an unknown setting: exit 1" "$(run edit p.reno fx 2.1 'Gaussian Blur' Nope=1)" 1
check "a bad on/off: exit 1" "$(run edit p.reno fx 2.1 'Gaussian Blur' Red=maybe)" 1
check "a choice that is not there: exit 1" "$(run edit p.reno fx master 'eFX Compressor' Mode=Sideways)" 1
check "an audio value out of range: exit 1" "$(run edit p.reno fx master 'eFX Compressor' Threshold=-200)" 1
has "  says the range" "Threshold goes from -60 to 0" last.err
check "an audio effect on a video event: exit 1" "$(run edit p.reno fx 2.1 'eFX Compressor')" 1
has "  says so" "is an audio effect" last.err
check "an unknown command: exit 1" "$(run edit p.reno frobnicate)" 1
check "a file that cannot be read: exit 1" "$(run edit p.reno add $M/nope.mp4)" 1
check "a failing command after good ones: exit 1" "$(run edit p.reno add-track audio + fx 9.9 X)" 1
cmp -s p.reno before.reno && ok "  the project was left as it was" || bad "  the project was left as it was"
check "edit with no commands: exit 2" "$(run edit p.reno)" 2
check "a project that is not there: exit 3" "$(run edit nope.reno info)" 3
check "a project saved as .txt: exit 2" "$(run new p.txt)" 2

# ---- a script file, saved as .veg, rendered
cat > cmds.txt <<EOF
# a small project
settings --size 320x180 --fps 25
add "$M/grey.png" --length 2
fx 1.1 "Brightness and Contrast" Brightness@0=0 Brightness@1.6=0.3
generate "Solid Color" --at 2 --length 1 --track 1 "Color=0,0,1,1"
EOF
check "new --script, saved as .veg: exit 0" "$(run new s.veg --script cmds.txt)" 0
run info s.veg --params > /dev/null; cp last.out info3.txt
has "  the effect's name (from the plug-in: a .veg holds the identifier)" "fx 1: Brightness and Contrast" info3.txt
has "  its keyframes came back from the .veg" "Brightness = keyframes 0@0s 0.3@1.6s" info3.txt
check "render it: exit 0" "$(run s.veg -o s.mp4 -y -q)" 0
near3 "  the grey at the start" "$(pixel s.mp4 0.0 160 90)" "128 128 128" 6
b=$(pixel s.mp4 1.5 160 90); g=${b%% *}
[ "${g:-0}" -gt 150 ] && ok "  brighter where the keyframes say (1.5 s: $b)" || bad "  brighter where the keyframes say" "$b"
near3 "  the generated blue after it" "$(pixel s.mp4 2.5 160 90)" "0 0 255" 12
echo "add \"unclosed" > badcmds.txt
check "a script with a quote not closed: exit 2" "$(run edit s.veg --script badcmds.txt)" 2

# ---- fx-list and fx-params
check "fx-list: exit 0" "$(run fx-list blur --video)" 0
has "  lists the blurs" "Gaussian Blur" last.out
hasnt "  only video ones" "audio	" last.out
check "fx-params of a video effect: exit 0" "$(run fx-params 'Gaussian Blur')" 0
has "  its settings with ranges" "Horizontal range  [HorizontalRange]  number 0 .. 1, default" last.out
check "fx-params of an audio effect: exit 0" "$(run fx-params 'eFX Limiter')" 0
has "  its settings as the plug-in shows them" "Threshold" last.out
check "fx-params of a choice: exit 0" "$(run fx-params '3D Blinds')" 0
has "  the choices" '0 "Left to Right"' last.out

echo "SUMMARY: $pass passed, $fail failed"
exit $fail
