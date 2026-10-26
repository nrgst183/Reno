#!/bin/bash
# Downloads a full FFmpeg 8.1 build (GPL, all codecs: x264, x265, VP9, AV1, Opus, ...) into deps/ffmpeg,
# where CMake looks for it first. Linux x64.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=ffmpeg-n8.1-latest-linux64-gpl-shared-8.1
mkdir -p "$ROOT/deps"
cd "$ROOT/deps"
echo "Downloading $NAME"
curl -fL -o "$NAME.tar.xz" "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$NAME.tar.xz"
rm -rf ffmpeg
tar xf "$NAME.tar.xz"
mv "$NAME" ffmpeg
rm "$NAME.tar.xz"
echo "FFmpeg is in $ROOT/deps/ffmpeg"
