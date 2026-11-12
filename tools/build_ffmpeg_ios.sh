#!/bin/bash
# Builds FFmpeg from source for iOS arm64 devices (static libraries, no programs) into deps/ffmpeg-ios, where the ios
# preset and the CI build look for it. On a Mac with Xcode. FFmpeg's own codecs plus Apple's VideoToolbox / AudioToolbox.
#   tools/build_ffmpeg_ios.sh        (FFMPEG_VERSION with FFMPEG_SHA256, IOS_MIN to change them)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${FFMPEG_VERSION:-8.1.3}
# the release's SHA-256 (as Homebrew's ffmpeg@8 records it); another version is checked against FFMPEG_SHA256 if given
SHA=${FFMPEG_SHA256:-}
if [ "$VER" = 8.1.3 ] && [ -z "$SHA" ]; then SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3; fi
MIN=${IOS_MIN:-16.0}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
mkdir -p "$ROOT/deps/src"
cd "$ROOT/deps/src"
if [ ! -d "ffmpeg-$VER" ]; then
  curl -fL -o "ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  if [ -n "$SHA" ]; then
    if ! echo "$SHA  ffmpeg-$VER.tar.xz" | shasum -a 256 -c --quiet -; then
      rm -f "ffmpeg-$VER.tar.xz"
      echo "ffmpeg-$VER.tar.xz is not the file expected" >&2
      exit 1
    fi
  else
    echo "warning: no SHA-256 to check ffmpeg-$VER.tar.xz against (set FFMPEG_SHA256)" >&2
  fi
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
