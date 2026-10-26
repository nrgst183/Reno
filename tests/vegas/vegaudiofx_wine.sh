#!/bin/bash
# Runs the Windows half of vegaudiofx_test (Reno's DirectX and VST2 hosts with VEGAS's own plug-ins) under Wine.
#
#   vegaudiofx_wine.sh WORKDIR LIST [WINE_TARBALL]
#
# WORKDIR    a scratch folder (the Wine prefix and the program go there)
# LIST       what the Linux half wrote with --list
# Needs, copied out of a VEGAS Pro 18 installation (read only) into WORKDIR/vegas:
#   vegas/Shared Plug-Ins/Audio_x64/*.dll                       VEGAS's DirectX plug-ins
#   vegas/MAGIX Plugins/essentialFX/x64/*.dll                   VEGAS's eFX VST plug-ins (any of them)
# and a Wine 10 build (default: Kron4ek's wine-10.0-amd64.tar.xz, downloaded once), Xvfb, and the Windows cross
# compiler: RENO_WINENV names a script that sets WINFLAGS and WINLINK for clang++ (a cross compile against the Windows SDK).
set -e
W=$(realpath "$1"); LIST=$(realpath "$2"); R=$(cd "$(dirname "$0")/../.." && pwd)
TAR=${3:-https://github.com/Kron4ek/Wine-Builds/releases/download/10.0/wine-10.0-amd64.tar.xz}
mkdir -p "$W/obj"
[ -x "$W/wine-10.0-amd64/bin/wine64" ] || { curl -sSL "$TAR" -o "$W/wine.tar.xz"; tar -xJf "$W/wine.tar.xz" -C "$W"; }
export WINEPREFIX=$W/pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
wine() { "$W/wine-10.0-amd64/bin/wine64" "$@"; }
pgrep -x Xvfb >/dev/null || { (setsid Xvfb :77 -screen 0 1000x760x24 >/dev/null 2>&1 &); sleep 2; }
export DISPLAY=${DISPLAY:-:77}
[ -d "$WINEPREFIX/drive_c/windows/system32" ] || { wine wineboot -i >/dev/null 2>&1 || true; "$W/wine-10.0-amd64/bin/wineserver" -w; }
# VEGAS's plug-ins want Visual C++ 2008's runtime and OpenMP as side-by-side assemblies (Wine has the DLLs)
SXS=$WINEPREFIX/drive_c/windows/winsxs
for a in crt:msvcr90.dll openmp:vcomp90.dll; do
  n=${a%%:*}; f=${a#*:}; N=$(echo $n | tr a-z A-Z | sed 's/OPENMP/OpenMP/')
  for v in 9.0.21022.8 9.0.30729.6161; do
    d=amd64_microsoft.vc90.${n}_1fc8b3b9a1e18e3b_${v}_none_deadbeef
    [ -f "$SXS/$d/$f" ] && continue
    mkdir -p "$SXS/$d" "$SXS/manifests"; cp "$W/wine-10.0-amd64/lib/wine/x86_64-windows/$f" "$SXS/$d/"
    printf '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">\n  <assemblyIdentity type="win32" name="Microsoft.VC90.%s" version="%s" processorArchitecture="amd64" publicKeyToken="1fc8b3b9a1e18e3b"/>\n  <file name="%s"/>\n</assembly>\n' "$N" $v $f > "$SXS/manifests/$d.manifest"
  done
done
# installed where VEGAS installs them (Reno finds the eFX by looking for VEGAS under Program Files)
PF="$WINEPREFIX/drive_c/Program Files/VEGAS"
mkdir -p "$PF/Shared Plug-Ins/Audio_x64" "$PF/VEGAS Pro 18.0/MAGIX Plugins/essentialFX/x64"
cp "$W/vegas/Shared Plug-Ins/Audio_x64/"*.dll "$PF/Shared Plug-Ins/Audio_x64/"
cp "$W/vegas/MAGIX Plugins/essentialFX/x64/"*.dll "$PF/VEGAS Pro 18.0/MAGIX Plugins/essentialFX/x64/" 2>/dev/null || true
chmod -R u+w "$PF"
for d in "$PF/Shared Plug-Ins/Audio_x64/"sf*.dll; do wine regsvr32 /s "C:\\Program Files\\VEGAS\\Shared Plug-Ins\\Audio_x64\\$(basename "$d")" || true; done
# the program: Reno's audio plug-in hosts and the test's Windows half
. "${RENO_WINENV:?set RENO_WINENV to the script that sets up the Windows cross compiler}"
cd "$W/obj"
for f in $R/src/core/base.cpp $R/src/core/dynlib.cpp $R/src/core/json.cpp $R/src/core/process.cpp $R/src/audiofx/*.cpp $R/tests/vegas/vegaudiofx_test.cpp; do
  clang++ -std=c++17 -O1 -w $WINFLAGS -I$R/third_party/vst2 -c $f -o $(basename $f .cpp).obj
done
clang++ $WINFLAGS *.obj -o "$W/vegaudiofx_test.exe" $WINLINK -lmsdmo -ldmoguids
mkdir -p "$W/pictures"
cd "$W" && wine ./vegaudiofx_test.exe --plugins "$LIST" "$W/pictures"
