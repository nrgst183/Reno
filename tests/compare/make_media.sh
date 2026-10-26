#!/bin/bash
# Makes the small media files the comparison suite's projects use (tests/compare/makesuite.cpp), always the same:
# pictures that show their own frame number (so a frame that is shown at the wrong time is plain to see) and sounds
# with distinct left and right tones and clicks at known times (so an offset can be measured). Uncompressed AVI and
# PCM WAV where possible, so VEGAS and Reno read exactly the same pixels and samples; one H.264 file checks decoding.
#
#   make_media.sh <folder>        (files already there are kept)
set -eu
D=$1; mkdir -p "$D"; cd "$D"
FONT=${FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf}
num="drawtext=fontfile=$FONT:text='%{frame_num}':start_number=0:x=(w-tw)/2:y=(h-th)/2:fontsize=h/3:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=6"
lbl() { echo "drawtext=fontfile=$FONT:text='$1':x=6:y=h-th-6:fontsize=h/10:fontcolor=yellow"; }
mk() { [ -f "$1" ] && return 0; shift_out=$1; shift; ffmpeg -v error -y "$@" "$shift_out"; }

# (each file is kept under 20 MB: the largest file the file bridge to the VEGAS machine takes in one piece)
# 320x180 25 fps, 4.2 s: a moving test picture with the frame number; sound: 440 Hz left, 660 Hz right, clicks each 0.5 s
mk clip_a.avi -f lavfi -i "testsrc2=size=320x180:rate=25:duration=4.2,$num,$(lbl A)" \
   -f lavfi -i "aevalsrc=0.25*sin(2*PI*440*t)+0.5*lt(mod(t\,0.5)\,0.002)|0.25*sin(2*PI*660*t)+0.5*lt(mod(t\,0.5)\,0.002):s=48000:d=4.2" \
   -c:v rawvideo -pix_fmt bgr24 -c:a pcm_s16le -shortest
# a second clip: colour bars, a different tone (550 / 330 Hz)
mk clip_b.avi -f lavfi -i "smptehdbars=size=320x180:rate=25:duration=4.2,$num,$(lbl B)" \
   -f lavfi -i "aevalsrc=0.25*sin(2*PI*550*t)|0.25*sin(2*PI*330*t):s=48000:d=4.2" \
   -c:v rawvideo -pix_fmt bgr24 -c:a pcm_s16le -shortest
# 59.94 fps (1.8 s) for the matching-rate project
mk clip_5994.avi -f lavfi -i "testsrc2=size=320x180:rate=60000/1001:duration=1.8,$num,$(lbl 59.94)" \
   -c:v rawvideo -pix_fmt bgr24
# H.264 + AAC at 29.97, 640x360 (decoding and colour conversion)
mk clip_h264.mp4 -f lavfi -i "testsrc2=size=640x360:rate=30000/1001:duration=4,$num,$(lbl H264)" \
   -f lavfi -i "sine=frequency=500:sample_rate=48000:duration=4" \
   -c:v libx264 -preset medium -crf 12 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest
# stills: 400x300 (another shape than the projects'), and one with transparency
mk still.png -f lavfi -i "testsrc=size=400x300:duration=1" -frames:v 1
mk still_alpha.png -f lavfi -i "color=c=red@0.0:size=320x180,format=rgba,geq=r='255*lt(hypot(X-160\,Y-90)\,60)':g='128':b='255*gt(X\,160)':a='255*lt(hypot(X-160\,Y-90)\,70)'" -frames:v 1
# sound files: a stereo tone at 48 kHz (-12 dBFS, 440 / 880 Hz), a quiet one (-30 dBFS, for Normalize), clicks only,
# and a 44.1 kHz file (resampling)
mk tone.wav -f lavfi -i "aevalsrc=0.25*sin(2*PI*440*t)|0.25*sin(2*PI*880*t):s=48000:d=4" -c:a pcm_s16le
mk quiet.wav -f lavfi -i "aevalsrc=0.0316*sin(2*PI*300*t)|0.0316*sin(2*PI*300*t):s=48000:d=3" -c:a pcm_s16le
mk clicks.wav -f lavfi -i "aevalsrc=0.8*lt(mod(t\,0.25)\,0.001)|0.8*lt(mod(t+0.125\,0.25)\,0.001):s=48000:d=4" -c:a pcm_s16le
mk tone44.wav -f lavfi -i "aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1500*t):s=44100:d=3" -c:a pcm_s16le
ls -la
