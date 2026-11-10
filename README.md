<p align="center"><img src="resources/reno_256.png" width="128" alt="Reno logo"></p>

# Reno

A video editor that works like VEGAS Pro 18. It opens and saves `.veg` projects, uses the same menus and shortcuts,
and runs the same plug-ins (OpenFX video effects, VST2/VST3/DirectX audio effects). C++17, Qt 6 and FFmpeg.
Windows first; most of it also runs on Linux.

Not affiliated with VEGAS, MAGIX or Sony. No VEGAS code or files are included. Reno has its own versions of VEGAS's
effects - all of VEGAS Pro 18's video effects, transitions and generators - made to match VEGAS's output, and uses
them even when VEGAS is installed (VEGAS's own effect plug-ins are not loaded unless Preferences > Video says to).

## Screenshots

![The main window: media, preview, master bus and the timeline](docs/screenshots/main.png)

| Audio Mixing layout | Video event effects |
|---|---|
| ![The Audio Mixing layout with the mixing console](docs/screenshots/mixing.png) | ![The video event effects window](docs/screenshots/effects.png) |

(Made with generated test media; `tests/ui/vegasshot_test.cpp` renders them.)

## Features

- Opens, edits and saves `.veg` projects, keeping settings it doesn't understand
- Video and audio tracks, buses, envelopes, transitions, compositing, Pan/Crop, Track Motion, nested projects
- OpenFX, VST2, VST3 and DirectX plug-in hosts (32-bit VST2 too)
- Built-in versions of VEGAS's video effects, transitions and generators, with Vulkan GPU rendering
- Time stretch and pitch shift, 5.1 audio, loudness meters, recording
- Runs VEGAS scripts (`.cs`, `.js`, `.vb`)
- Opens and renders every audio and video format FFmpeg supports (see below), from the editor or the command line (`reno_cli`)

## Formats and codecs

The Windows and Linux builds come with FFmpeg's full build (8.1), so Reno opens and renders everything FFmpeg
supports. The common ones:

- **Video:** H.264 / AVC, H.265 / HEVC, AV1, VP8, VP9, ProRes, DNxHD / DNxHR, CineForm, MPEG-2, MPEG-4 Part 2 (Xvid,
  DivX), Theora, MJPEG, FFV1, HuffYUV, UT Video, QuickTime Animation, uncompressed RGB / YUV, WMV, VC-1
- **Audio:** AAC, MP3, Opus, Vorbis, FLAC, ALAC, WAV / PCM (16, 24, 32-bit and float), AIFF, AC-3, E-AC-3, DTS, WMA,
  TrueHD, WavPack, Monkey's Audio
- **Containers:** MP4, MOV, MKV, WebM, AVI, MXF, MPEG-TS, MPEG-PS, M4A, OGG, FLV, WMV / ASF, GIF, image sequences
  (PNG, JPEG, TIFF, EXR, DPX, BMP)

Hardware encoding uses NVENC, Quick Sync or AMF where the graphics card has it. Render As has the usual templates, and
`reno_cli` takes any FFmpeg encoder by name: `reno_cli project.veg --video-codec libx265 --audio-codec flac -o out.mkv`
(`ffmpeg -encoders` lists them).

Keep backups: Reno saves the previous file as `<name>.veg.bak`, but its `.veg` output isn't tested against every
VEGAS version. Known gaps are listed in [docs/NOTES.md](docs/NOTES.md).

## Downloads

Builds are on the [Releases](../../releases) page. Every push to `main` updates the "Nightly build" prerelease;
versioned releases come from pushing a `v*` tag (for example `v0.1.0`) or from Actions > Build > Run workflow with a
version filled in.

macOS builds come for Apple Silicon (`reno-macos-arm64.zip`) and Intel Macs (`reno-macos-x64.zip`). They are new and
not yet tried on a Mac. Reno.app is not signed by a developer account, so macOS refuses it at first: right-click it and
choose Open, or run `xattr -dr com.apple.quarantine Reno.app`. Effects use Vulkan through MoltenVK when it is
installed (`brew install molten-vk`), and the CPU otherwise. 32-bit plug-ins and VEGAS's DirectX audio plug-ins are
Windows-only; Reno's own versions of VEGAS's effects work everywhere.

The Windows 7, FreeBSD, Android and iOS builds are new and not yet tried on those systems:

- `reno-windows7-x64.zip`: for Windows 7 SP1 and 8.1 (64-bit), with Qt's DLLs patched to run there
  ([qt6windows7](https://github.com/crystalidea/qt6windows7)). The FFmpeg DLLs need the Universal C Runtime (Windows
  Update KB2999226), and GPU effects need a graphics driver with Vulkan for Windows 7.
- `reno-freebsd-x64.tar.gz`: FreeBSD 14 (amd64). Install Qt and FFmpeg from packages first:
  `pkg install qt6-base qt6-multimedia ffmpeg vulkan-loader`.
- `reno-android-arm64.apk`: Android 9 and later on 64-bit ARM. One finger works as the mouse; two fingers pinch to zoom
  the timeline and drag to scroll it. Each build is signed with a key of its own, so uninstall the older one first.
- `reno-ios-unsigned.ipa`: iPhone and iPad (iOS 16 and later). It is not signed: sign it with your own Apple ID (for
  example with Sideloadly or AltStore) to install it.

On Android and iOS Reno has its own effects only (plug-ins are Windows, macOS or Linux programs and cannot load there),
no command-line `reno_cli`, and FFmpeg's own encoders instead of x264 / x265 for rendering. The interface is the desktop
one, made for a mouse and keyboard; a tablet with a mouse or keyboard attached works best.

## Building on Windows

You need Visual Studio 2022 with the C++ workload and its "C++ Clang tools for Windows" component (Reno is built
with clang-cl; `build_msvc.bat msvc` uses MSVC instead), and Qt 6.8.3 for `msvc2022_64` with Multimedia. Get a full FFmpeg build
(all codecs) into `deps\ffmpeg` with:

    powershell -ExecutionPolicy Bypass -File tools\get_ffmpeg.ps1

Then either run `build_msvc.bat` (output in `build\out\`) or open `Reno.sln` and build x64 Release.
Qt is expected in `C:\Qt\6.8.3\msvc2022_64`; set `RENO_QT_DIR` (and `RENO_FFMPEG_DIR` for another FFmpeg) to use
other folders.

## Building on Linux

    sudo apt install cmake ninja-build g++ libgl-dev
    tools/get_ffmpeg.sh
    pip install aqtinstall
    aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtmultimedia -O ~/Qt
    cmake --preset linux-release
    cmake --build --preset linux-release

Other presets are in `CMakePresets.json`.

## Building on macOS

    brew install cmake ninja ffmpeg
    pip3 install aqtinstall
    aqt install-qt mac desktop 6.8.3 clang_64 -m qtmultimedia -O ~/Qt
    cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/macos -DRENO_FFMPEG_DIR="$(brew --prefix)"
    cmake --build build
    open build/bin/Reno.app

The build is for the Mac's own processor (Apple Silicon or Intel). To make a Reno.app to give to someone else, run
`macdeployqt build/bin/Reno.app -executable=build/bin/Reno.app/Contents/MacOS/reno_cli` and then
`codesign --force --deep --sign - build/bin/Reno.app`.

## Building for Windows 7

Build as on Windows with `build_msvc.bat win7` (output in `build\out-win7\`), or with the `windows7-clangcl-release`
preset followed by `powershell -ExecutionPolicy Bypass -File tools\use_qt_win7.ps1 <the folder with reno.exe>`. Reno is
then compiled for Windows 7 (only functions Windows 7 has), and the script swaps Qt's DLLs for the patched ones from
[qt6windows7](https://github.com/crystalidea/qt6windows7) (it needs 7-Zip). Windows XP is not possible: Qt 6, FFmpeg,
Vulkan and Visual Studio's compilers no longer support it.

## Building on FreeBSD

    pkg install cmake ninja pkgconf qt6-base qt6-multimedia ffmpeg vulkan-loader
    cmake --preset freebsd-release
    cmake --build --preset freebsd-release

## Building for Android

You need Qt 6.8.3 for Android (`android_arm64_v8a`, with Multimedia) and for your computer, the Android SDK, NDK
26.1.10909125 and JDK 17. Build FFmpeg for Android, then Reno's APK:

    export ANDROID_SDK_ROOT=~/Android/Sdk ANDROID_NDK_ROOT=~/Android/Sdk/ndk/26.1.10909125
    tools/build_ffmpeg_android.sh
    ~/Qt/6.8.3/android_arm64_v8a/bin/qt-cmake -S . -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DQT_HOST_PATH=~/Qt/6.8.3/gcc_64 -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT -DANDROID_NDK_ROOT=$ANDROID_NDK_ROOT \
        -DRENO_FFMPEG_DIR=$PWD/deps/ffmpeg-android
    cmake --build build/android --target apk

The APK is under `build/android/android-build/build/outputs/apk/`. Sign it with `apksigner` (from the SDK's
build-tools) to install it; Qt Creator can also open the project and run it on a connected device.

## Building for iOS

On a Mac with Xcode, Qt 6.8.3 for iOS and for macOS (with Multimedia):

    tools/build_ffmpeg_ios.sh
    ~/Qt/6.8.3/ios/bin/qt-cmake -S . -B build/ios -G Xcode -DQT_HOST_PATH=~/Qt/6.8.3/macos -DRENO_FFMPEG_DIR=$PWD/deps/ffmpeg-ios
    open build/ios/Reno.xcodeproj

In Xcode choose your team under Signing & Capabilities for the `reno` target, then run it on your iPhone or iPad.

## Usage

Run `reno.exe`. The first start scans for plug-ins; each one is tested in its own process, so a crashing plug-in
won't take Reno down.

Rendering from the command line:

    reno_cli project.veg
    reno_cli project.veg -o out.mp4 -t "MAGIX AVC/AAC MP4/Internet HD 1080p 29.97 fps" -y
    reno_cli --help

How fast the built-in effects are on your processor and graphics card (milliseconds per frame, and a chain of effects):

    reno_cli fxbench --both --size 1920x1080
    reno_cli gpupick --again        (which of the two Reno will use for each effect; it measures this by itself once per card)

## Tests

Tests are in `tests/`, one program per `.cpp`; each file's header says how to build and run it. Tests that need
real VEGAS projects or VEGAS output read their location from environment variables (`RENO_SAMPLES` and others, see
[docs/NOTES.md](docs/NOTES.md)) and skip when they aren't set.

The tools used to record VEGAS's output and compare Reno against it (VEGAS capture scripts, comparison scripts) are
development tools kept out of the repository (see `.gitignore`).

## Third-party code

- `third_party/openfx` - OpenFX headers (BSD 3-clause)
- `third_party/vst2` - clean-room VST2 header (BSD 3-clause)
- `third_party/vulkan` - Vulkan headers (Apache-2.0 or MIT)

## License

No license yet; all rights reserved.
