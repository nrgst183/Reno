#!/bin/bash
# Builds and runs the 32-bit bridge test on Linux (the server built 64-bit, a Linux VST2 test plug-in behind it).
#   RENO=<tree> tests/vst32/run.sh [build folder] [Auto-Tune_Evo_VST.dll]
# (objects are kept in the build folder: only changed files are compiled again)
set -e
R=${RENO:-$(cd "$(dirname "$0")/../.." && pwd)}; B=${1:-${TMPDIR:-/tmp}/reno_vst32}; mkdir -p $B/obj
CF="-std=c++17 -O1 -g -D__cdecl= -I$R/third_party/vst2 -I$R/third_party/openfx"
g++ -shared -fPIC $CF $R/tests/vst32/bridgevst.cpp -o $B/bridgevst.so
obj() {      # obj <flags> <source> -> object path
  local o=$B/obj/$1/$(basename $2 .cpp).o; mkdir -p $B/obj/$1
  if [ ! -f $o ] || [ $2 -nt $o ] || [ -n "$(find $R/src -name '*.h' -newer $o | head -1)" ]; then
    if [ $1 = san ]; then g++ $CF -fsanitize=address,undefined -fno-sanitize=vptr -c $2 -o $o; else g++ $CF -c $2 -o $o; fi
  fi
  echo $o
}
srv=""; for f in $R/src/vst32/vst32server.cpp $R/src/audiofx/vst32ipc.cpp $R/src/audiofx/vst2host.cpp $R/src/core/base.cpp $R/src/core/dynlib.cpp; do srv="$srv $(obj plain $f)"; done
g++ $srv -o $B/reno_vst32 -ldl -lpthread -lrt
cli=""; for f in $R/tests/vst32/vst32_test.cpp $R/src/core/*.cpp $R/src/audiofx/*.cpp; do cli="$cli $(obj san $f)"; done
g++ -fsanitize=address,undefined -fno-sanitize=vptr $cli -o $B/vst32_test -ldl -lpthread -lrt
cd $B && ASAN_OPTIONS=detect_leaks=0 ./vst32_test $B/reno_vst32 $B/bridgevst.so $2
