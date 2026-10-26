#!/bin/sh
# The tests of Reno's VST3 host that need Steinberg's VST3 SDK (which is not part of Reno), on Linux:
#   1. abicheck: src/audiofx/vst3_abi.h compared with the official interface headers
#   2. three plug-in files built from the SDK's own sources, and Reno's host tried on each with 'probe':
#        adelay    the SDK's "ADelay" sample
#        mda       the SDK's "mda-vst3" sample (34 effects and instruments in one file)
#        sdkcheck  tests/vst3/sdkcheckplug.cpp: Steinberg's host checks (from the "HostChecker" sample)
# Nothing is written into the source tree or the SDK: everything goes to the folder given.
#
#     tests/vst3/run_sdk.sh <SDK folder> <folder for what is built>
#
# <SDK folder> is one that has pluginterfaces/, public.sdk/ and base/ in it (the layout of
# https://github.com/steinbergmedia/vst3sdk with its submodules; the three can also be separate
# clones of vst3_pluginterfaces, vst3_public_sdk and vst3_base linked or copied under those names).
# CXX picks the compiler (default g++); clang++, if there is one, is used for a second look at the
# virtual function tables.
set -e
if [ -z "$2" ] || [ ! -d "$1/pluginterfaces" ] || [ ! -d "$1/public.sdk" ] || [ ! -d "$1/base" ]; then
    echo "usage: $0 <SDK folder with pluginterfaces/, public.sdk/ and base/> <folder for what is built>"
    exit 2
fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
CXX=${CXX:-g++}
case "$(uname -m)" in
    aarch64) ARCH=aarch64-linux ;;
    x86_64)  ARCH=x86_64-linux ;;
    *)       ARCH=i386-linux ;;
esac
STATUS=0

echo "---------------- abicheck"
mkdir -p "$OUT/abi"
$CXX -std=c++17 -I"$SDK" -I"$ROOT/src/audiofx" "$ROOT/tests/vst3/abicheck.cpp" -o "$OUT/abi/abicheck"
"$OUT/abi/abicheck" > "$OUT/abi/abicheck.txt" || STATUS=1
tail -n 3 "$OUT/abi/abicheck.txt"
if command -v clang++ > /dev/null 2>&1; then
    clang++ -std=c++17 -Wno-pragma-pack -I"$SDK" -I"$ROOT/src/audiofx" -Xclang -fdump-vtable-layouts -c "$ROOT/tests/vst3/abicheck.cpp" \
        -o "$OUT/abi/abicheck_clang.o" > "$OUT/abi/vtables.txt"
    python3 "$ROOT/tests/vst3/compare_vtables.py" "$OUT/abi/vtables.txt" | tail -n 1 || STATUS=1
fi

echo "---------------- building the SDK's plug-in base (a while)"
# (stands in for a file the SDK's CMake build generates)
mkdir -p "$OUT/gen" "$OUT/obj/base" "$OUT/obj/adelay" "$OUT/obj/mda" "$OUT/obj/sdkcheck"
cat > "$OUT/gen/projectversion.h" <<EOF
#pragma once
#define MAJOR_VERSION_STR "1"
#define MAJOR_VERSION_INT 1
#define SUB_VERSION_STR "0"
#define SUB_VERSION_INT 0
#define RELEASE_NUMBER_STR "0"
#define RELEASE_NUMBER_INT 0
#define BUILD_NUMBER_STR "1"
#define BUILD_NUMBER_INT 1
#define FULL_VERSION_STR "1.0.0.1"
#define VERSION_STR "1.0.0"
EOF
FLAGS="-std=c++17 -O1 -fPIC -fvisibility=hidden -DRELEASE=1 -DNDEBUG -w -I$SDK -I$OUT/gen"
compile() {     # compile <folder for the objects> <source files...>
    DIR=$1
    shift
    for SRC in "$@"; do
        $CXX $FLAGS -c "$SRC" -o "$DIR/$(basename "$SRC" .cpp).o"
    done
}
V=$SDK/public.sdk/source/vst
compile "$OUT/obj/base" "$V/vstaudioeffect.cpp" "$V/vstcomponent.cpp" "$V/vstcomponentbase.cpp" "$V/vsteditcontroller.cpp" \
    "$V/vstparameters.cpp" "$V/vstbus.cpp" "$V/vstinitiids.cpp" "$V/utility/vst2persistence.cpp" \
    "$SDK/public.sdk/source/main/pluginfactory.cpp" "$SDK/public.sdk/source/main/linuxmain.cpp" "$SDK/public.sdk/source/main/moduleinit.cpp" \
    "$SDK/public.sdk/source/common/pluginview.cpp" "$SDK/public.sdk/source/common/commoniids.cpp" \
    "$SDK"/base/source/*.cpp "$SDK"/base/thread/source/*.cpp \
    "$SDK/pluginterfaces/base/funknown.cpp" "$SDK/pluginterfaces/base/ustring.cpp" "$SDK/pluginterfaces/base/coreiids.cpp" \
    "$SDK/pluginterfaces/base/conststringtable.cpp"

link() {        # link <name> <folder with its objects>
    BIN=$OUT/plugins/$1.vst3/Contents/$ARCH
    mkdir -p "$BIN"
    $CXX -shared -o "$BIN/$1.so" "$2"/*.o "$OUT"/obj/base/*.o -Wl,--no-undefined -ldl -lpthread
}
echo "---------------- building adelay, mda and sdkcheck"
A=$SDK/public.sdk/samples/vst/adelay/source
compile "$OUT/obj/adelay" "$A/adelaycontroller.cpp" "$A/adelayprocessor.cpp" "$A/factory.cpp" "$V/utility/testing.cpp"
link adelay "$OUT/obj/adelay"
compile "$OUT/obj/mda" "$SDK"/public.sdk/samples/vst/mda-vst3/source/*.cpp
link mda "$OUT/obj/mda"
H=$SDK/public.sdk/samples/vst/hostchecker/source
FLAGS="$FLAGS -I$H"
compile "$OUT/obj/sdkcheck" "$ROOT/tests/vst3/sdkcheckplug.cpp" "$H/hostcheck.cpp" "$H/eventlogger.cpp" "$H/processsetupcheck.cpp" \
    "$H/processcontextcheck.cpp" "$H/eventlistcheck.cpp" "$H/parameterchangescheck.cpp"
link sdkcheck "$OUT/obj/sdkcheck"

echo "---------------- building probe (Reno's host, with -fsanitize=address,undefined)"
cat > "$OUT/dxstub.cpp" <<EOF
#include "$ROOT/src/audiofx/audioplugin.h"
namespace reno {
std::unique_ptr<AudioPlugin> dxCreate(const AudioPluginDesc&, std::string* error) { if (error) *error = "no DirectX here"; return nullptr; }
std::vector<AudioPluginDesc> listDirectXAudioPlugins() { return {}; }
}
EOF
# (not the "vptr" check: to it every object of a plug-in is of the wrong class, because the host's
#  declarations of the interfaces are not the plug-in's, they only have the same layout)
$CXX -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -D__cdecl= \
    -I"$ROOT/third_party/vst2" "$ROOT/tests/vst3/probe.cpp" "$ROOT"/src/core/*.cpp "$ROOT/src/audiofx/audioplugin.cpp" \
    "$ROOT/src/audiofx/vst2host.cpp" "$ROOT/src/audiofx/vst3host.cpp" "$OUT/dxstub.cpp" -o "$OUT/probe" -ldl -lpthread

# (the bundle folder is given, not the file in it: describeAudioPluginFile only takes names ending in .vst3 for VST3)
echo "---------------- adelay: a click must come out 1 second later (48000 samples), then 0.01 second later (sample 479)"
"$OUT/probe" "$OUT/plugins/adelay.vst3" --seconds 1.5 || STATUS=1
"$OUT/probe" "$OUT/plugins/adelay.vst3" --set 1=0.01 --seconds 1 --block 64 | grep "set parameter\|click in\|problem" || STATUS=1
echo "---------------- sdkcheck: Steinberg's checks must end with 0 error(s), 0 warning(s); gain 0.25 gives a peak of 0.5"
"$OUT/probe" "$OUT/plugins/sdkcheck.vst3" --set 0=0.25 --seconds 1 --block 128 || STATUS=1
echo "---------------- mda: every effect in the file (the whole output is in $OUT/mda.txt)"
"$OUT/probe" "$OUT/plugins/mda.vst3" --seconds 0.5 > "$OUT/mda.txt" 2>&1 || STATUS=1
echo "$(grep -c '^== ' "$OUT/mda.txt") effects tried; $(tail -n 1 "$OUT/mda.txt")"
exit $STATUS
