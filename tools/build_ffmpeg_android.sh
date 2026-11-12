#!/bin/bash
# Builds FFmpeg from source for Android arm64 (shared libraries, no programs) into deps/ffmpeg-android, where the
# android-arm64 preset and the CI build look for it. FFmpeg's own decoders and encoders only (no x264 / x265: they are not
# built here), so video renders on Android use FFmpeg's own MPEG-4 / VP8 / ... encoders.
#   ANDROID_NDK_ROOT=/path/to/ndk tools/build_ffmpeg_android.sh        (FFMPEG_VERSION with FFMPEG_SHA256, ANDROID_API to change them)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${FFMPEG_VERSION:-8.1.3}
# the release's SHA-256 (as Homebrew's ffmpeg@8 records it); another version is checked against FFMPEG_SHA256 if given
SHA=${FFMPEG_SHA256:-}
if [ "$VER" = 8.1.3 ] && [ -z "$SHA" ]; then SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3; fi
API=${ANDROID_API:-28}
NDK=${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT to the Android NDK folder}
HOST=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
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
rm -rf "build-android" && mkdir "build-android" && cd "build-android"
"../ffmpeg-$VER/configure" --prefix="$ROOT/deps/ffmpeg-android" \
  --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
  --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
  --ar="$TC/llvm-ar" --nm="$TC/llvm-nm" --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
  --enable-shared --disable-static --enable-pic --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --enable-jni --enable-mediacodec
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
make install
echo "FFmpeg for Android is in $ROOT/deps/ffmpeg-android"
