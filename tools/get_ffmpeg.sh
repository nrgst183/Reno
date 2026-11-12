#!/bin/bash
# Downloads a full FFmpeg 8.1 build (GPL, all codecs: x264, x265, VP9, AV1, Opus, ...) into deps/ffmpeg,
# where CMake looks for it first. Linux x64.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# a fixed build (BtbN keeps the end-of-month ones), checked against its SHA-256 before it is used
RELEASE=autobuild-2026-10-31-12-59
NAME=ffmpeg-n8.1.3-27-g8dca1586f4-linux64-gpl-shared-8.1
SHA256=202fe67679a0860b155817a0907ffcc5c1ea82e186acaa21cb87d307f54fd59a
mkdir -p "$ROOT/deps"
cd "$ROOT/deps"
echo "Downloading $NAME"
curl -fL -o "$NAME.tar.xz" "https://github.com/BtbN/FFmpeg-Builds/releases/download/$RELEASE/$NAME.tar.xz"
if ! echo "$SHA256  $NAME.tar.xz" | sha256sum -c --quiet -; then
  rm -f "$NAME.tar.xz"
  echo "$NAME.tar.xz is not the file expected" >&2
  exit 1
fi
rm -rf ffmpeg
tar xf "$NAME.tar.xz"
mv "$NAME" ffmpeg
rm "$NAME.tar.xz"
echo "FFmpeg is in $ROOT/deps/ffmpeg"
