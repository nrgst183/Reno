#!/bin/bash
# Runs the Windows half of vegcookie_test (VEGAS Pro 18's own OpenFX effects through Reno's OpenFX host) under Wine.
#
#   vegofx_wine.sh WORKDIR FILE.ofx JOBS.json OUTDIR [VEGAS_PROGRAM_DIR] [WINE_TARBALL]
#
# WORKDIR            a scratch folder (the Wine prefix and the program go there)
# FILE.ofx           a VEGAS effect file, e.g. ".../OFX Video Plug-Ins/Vfx1.ofx.bundle/Contents/Win64/Vfx1.ofx" (Cookie Cutter)
# JOBS.json          what vegcookie_test --jobs wrote
# OUTDIR             where the pictures and descriptions go (read by vegcookie_test --compare)
# VEGAS_PROGRAM_DIR  VEGAS Pro 18's program folder (read only): its OpenColorIO.dll and ErrorReport.dll, which
#                    Vfx1.ofx needs, are copied beside the program (default $RENO_VEGAS_DIR)
# Needs a Wine 10 build (default: Kron4ek's wine-10.0-amd64.tar.xz, downloaded once into WORKDIR), Xvfb, and the
# Windows cross compiler: RENO_WINENV names a script that sets WINFLAGS and WINLINK for clang++ (a cross compile against the Windows SDK).
set -e
W=$(realpath "$1"); OFXF=$(realpath "$2"); JOBS=$(realpath "$3"); mkdir -p "$4"; OUTD=$(realpath "$4")
VP=${5:-${RENO_VEGAS_DIR:?give VEGAS_PROGRAM_DIR or set RENO_VEGAS_DIR}}
R=$(cd "$(dirname "$0")/../.." && pwd)
TAR=${6:-https://github.com/Kron4ek/Wine-Builds/releases/download/10.0/wine-10.0-amd64.tar.xz}
mkdir -p "$W/obj"
[ -x "$W/wine-10.0-amd64/bin/wine64" ] || { curl -sSL "$TAR" -o "$W/wine.tar.xz"; tar -xJf "$W/wine.tar.xz" -C "$W"; }
export WINEPREFIX=$W/pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
wine() { "$W/wine-10.0-amd64/bin/wine64" "$@"; }
pgrep -x Xvfb >/dev/null || { (setsid Xvfb :77 -screen 0 1000x760x24 >/dev/null 2>&1 &); sleep 2; }
export DISPLAY=${DISPLAY:-:77}
[ -d "$WINEPREFIX/drive_c/windows/system32" ] || { wine wineboot -i >/dev/null 2>&1 || true; "$W/wine-10.0-amd64/bin/wineserver" -w; }
# the program: Reno's OpenFX host and the test's Windows half
. "${RENO_WINENV:?set RENO_WINENV to the script that sets up the Windows cross compiler}"
cd "$W/obj"
for f in $R/src/core/*.cpp $R/src/ofx/*.cpp $R/tests/vegas/vegofx_wine.cpp; do
  o=$(basename $(dirname $f))_$(basename $f .cpp).obj
  [ -f $o ] && [ $o -nt $f ] && [ -z "$(find $R/src -name '*.h' -newer $o | head -1)" ] && continue
  clang++ -std=c++17 -O1 -w $WINFLAGS -I$R/third_party/openfx -c $f -o $o
done
clang++ $WINFLAGS *.obj -o "$W/vegofx_wine.exe" $WINLINK -lshlwapi -lversion -lcomctl32 -lopengl32 -ldwmapi
# the effect's DLLs from VEGAS's program folder beside the program (Windows looks there)
for d in OpenColorIO.dll ErrorReport.dll; do [ -f "$W/$d" ] || cp "$VP/$d" "$W/"; done
cp "$JOBS" "$W/jobs.json"
cd "$W" && wine ./vegofx_wine.exe "$(wine winepath -w "$OFXF" 2>/dev/null)" jobs.json "$(wine winepath -w "$OUTD" 2>/dev/null)"
