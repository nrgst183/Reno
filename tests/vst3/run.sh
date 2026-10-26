#!/bin/sh
# Builds the VST3 test plug-in and the host test on Linux and runs the test, once for each kind of
# plug-in factory. Nothing is written into the source tree: everything goes to the folder given.
#
#     tests/vst3/run.sh <folder for what is built> [plain|asan|tsan|comstyle]
#
#   plain      the normal build (the default)
#   asan       with -fsanitize=address,undefined
#   tsan       with -fsanitize=thread
#   comstyle   host and plug-in both built with the Windows flavour of ids and result codes
#              (RENO_VST3_COM_STYLE=1), to run that part of the host's code on Linux
# CXX picks the compiler (default g++).
set -e
if [ -z "$1" ]; then
    echo "usage: $0 <folder for what is built> [plain|asan|tsan|comstyle]"
    exit 2
fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FLAVOUR=${2:-plain}
OUT=$1/$FLAVOUR
CXX=${CXX:-g++}
case "$FLAVOUR" in
    plain)    EXTRA="-O1 -g" ;;
    asan)     EXTRA="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" ;;
    tsan)     EXTRA="-O1 -g -fsanitize=thread" ;;
    comstyle) EXTRA="-O1 -g -DRENO_VST3_COM_STYLE=1" ;;
    *) echo "unknown flavour: $FLAVOUR"; exit 2 ;;
esac
case "$(uname -m)" in
    aarch64) ARCH=aarch64-linux ;;
    x86_64)  ARCH=x86_64-linux ;;
    *)       ARCH=i386-linux ;;
esac
FLAGS="-std=c++17 -Wall -Wextra $EXTRA"
BIN=$OUT/plugins/RenoTest.vst3/Contents/$ARCH
mkdir -p "$BIN" "$OUT/other"

# the plug-in, in a bundle, and a library that is no plug-in
$CXX $FLAGS -fPIC -fvisibility=hidden -shared "$ROOT/tests/vst3/testplug.cpp" -o "$BIN/RenoTest.vst3"
printf 'extern "C" int renoNothingToSeeHere() { return 1; }\n' > "$OUT/notplugin.cpp"
$CXX $FLAGS -fPIC -shared "$OUT/notplugin.cpp" -o "$OUT/other/NotAPlugin.vst3"

# the two DirectX functions the rest of the audio effect code wants (the real ones are Windows only)
cat > "$OUT/dxstub.cpp" <<EOF
#include "$ROOT/src/audiofx/audioplugin.h"
namespace reno {
std::unique_ptr<AudioPlugin> dxCreate(const AudioPluginDesc&, std::string* error) { if (error) *error = "no DirectX here"; return nullptr; }
std::vector<AudioPluginDesc> listDirectXAudioPlugins() { return {}; }
}
EOF

# the test program: the host and what it needs from the rest of Reno
$CXX $FLAGS -D__cdecl= -I"$ROOT/third_party/vst2" \
    "$ROOT/tests/vst3/hosttest.cpp" "$ROOT"/src/core/*.cpp "$ROOT/src/audiofx/audioplugin.cpp" \
    "$ROOT/src/audiofx/vst2host.cpp" "$ROOT/src/audiofx/vst3host.cpp" "$OUT/dxstub.cpp" \
    -o "$OUT/hosttest" -ldl -lpthread

STATUS=0
for LEVEL in 3 2 1; do
    echo "---------------- $FLAVOUR, IPluginFactory$( [ $LEVEL = 1 ] || echo $LEVEL )"
    RENO_TESTPLUG_FACTORY=$LEVEL "$OUT/hosttest" "$BIN/RenoTest.vst3" "$OUT/other/NotAPlugin.vst3" || STATUS=1
done
exit $STATUS
