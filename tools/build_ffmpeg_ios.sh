#!/bin/bash
# Builds FFmpeg from source for iOS arm64 devices (static libraries, no programs) into deps/ffmpeg-ios, where the ios
# preset and the CI build look for it. On a Mac with Xcode. FFmpeg's own codecs plus Apple's VideoToolbox / AudioToolbox.
#   tools/build_ffmpeg_ios.sh        (FFMPEG_VERSION, IOS_MIN to change them)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${FFMPEG_VERSION:-8.1}
MIN=${IOS_MIN:-16.0}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
mkdir -p "$ROOT/deps/src"
cd "$ROOT/deps/src"
if [ ! -d "ffmpeg-$VER" ]; then
  curl -fL -o "ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  tar xf "ffmpeg-$VER.tar.xz"
fi
rm -rf "build-ios" && mkdir "build-ios" && cd "build-ios"
"../ffmpeg-$VER/configure" --prefix="$ROOT/deps/ffmpeg-ios" \
  --enable-cross-compile --target-os=darwin --arch=arm64 --cc="$CC" --sysroot="$SDK" \
  --extra-cflags="-arch arm64 -miphoneos-version-min=$MIN" --extra-ldflags="-arch arm64 -miphoneos-version-min=$MIN" \
  --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --enable-videotoolbox --enable-audiotoolbox
make -j"$(sysctl -n hw.ncpu)"
make install
echo "FFmpeg for iOS is in $ROOT/deps/ffmpeg-ios"
