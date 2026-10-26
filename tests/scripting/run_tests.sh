#!/bin/bash
# Runs the VEGAS scripting tests (Tools > Scripting) on Linux:
#   1. compiles src/scripting/ScriptPortal.Vegas.cs (+ ScriptPortal.Vegas.Unsupported.cs) as C# 5 with Roslyn (from a
#      PowerShell 7 folder: PWSH), as the .NET Framework's csc.exe compiles it on Windows, against a small stand-in for
#      Windows Forms;
#   2. compiles the example scripts (tools/scripts), VEGAS's own sample script, your own project-building script (RENO_USER_SCRIPTS) and the test scripts;
#   3. checks that scriptapi.h and the C# side's numbers agree;
#   4. with VEGAS's own ScriptPortal.Vegas.dll and a real Windows Forms at hand: checks that the Unsupported file is
#      up to date, that Reno's examples compile against VEGAS's API and that all of VEGAS's own C# scripts compile
#      against Reno's;
#   5. builds script_test (which loads CoreCLR in-process, as Reno loads the .NET Framework) and runs it.
#   RENO=<tree> PWSH=<folder with pwsh and libcoreclr.so> LIB=<objects of the tree, lib.a> run_tests.sh <build folder>
# Environment (all optional but PWSH and LIB; what is missing is skipped):
#   RENO_VEGAS_DIR     a VEGAS Pro 18 program folder (ScriptPortal.Vegas.dll, "Script Menu")
#   RENO_WINFORMS_DIR  a folder with a real System.Windows.Forms.dll (e.g. the .NET one of the Windows Performance Toolkit)
#   RENO_USER_SCRIPTS  folder with your own VEGAS script "Build video project (run in VEGAS).cs"
#   RENO_SAMPLES       folder with sample VEGAS projects
#   FFMPEG_INCLUDE    folder with the FFmpeg headers, when they are not in the system's include path
set -e
R=${RENO:-$(cd "$(dirname "$0")/../.." && pwd)}; P=${PWSH:-}; B=${1:-${TMPDIR:-/tmp}/reno_scripting_test}
LIB=${LIB:-}
if [ -z "$P" ] || [ ! -x "$P/pwsh" ] || [ ! -f "$LIB" ]; then
  echo "SKIP scripting tests: set PWSH (folder with pwsh) and LIB (lib.a of the tree) - see the top of this file"; exit 0
fi
VEGAS_SCRIPTS=${VEGAS_SCRIPTS:-"${RENO_VEGAS_DIR:-/nonexistent}/Script Menu"}
USER_SCRIPTS=${USER_SCRIPTS:-"${RENO_USER_SCRIPTS:-/nonexistent}"}
VEGAS_DLL=${VEGAS_DLL:-"${RENO_VEGAS_DIR:-/nonexistent}/ScriptPortal.Vegas.dll"}
WFDIR=${WFDIR:-"${RENO_WINFORMS_DIR:-/nonexistent}"}
mkdir -p $B/scripts
T=$R/tests/scripting
cs() { $P/pwsh -NoProfile -File $T/roslyn.ps1 "$@"; }
many() { $P/pwsh -NoProfile -File $T/roslyn.ps1 -jobs "$1"; }

SHIM="$R/src/scripting/ScriptPortal.Vegas.cs;$R/src/scripting/ScriptPortal.Vegas.Unsupported.cs"
J=$B/jobs1.txt
{
  echo "$B/System.Windows.Forms.dll|System.Windows.Forms|latest|||$T/winforms_stub.cs"
  echo "$B/ScriptPortal.Vegas.dll|ScriptPortal.Vegas|5|RENO_NO_CODEDOM|$B/System.Windows.Forms.dll|$SHIM"
  # (the Windows build compiles the CodeDOM part too: check it compiles)
  echo "$B/ScriptPortal.Vegas.codedom.dll|ScriptPortal.Vegas|5||$B/System.Windows.Forms.dll|$SHIM"
} > $J
many $J
REFS="$B/ScriptPortal.Vegas.dll;$B/System.Windows.Forms.dll"
: > $J
for f in $T/scripts/*.cs "$R/tools/scripts/"*.cs "$R/tools/vegas/Export to Reno.cs" "$VEGAS_SCRIPTS/Group Video and Audio Events.cs"; do
  [ -f "$f" ] || continue
  n=$(basename "$f" .cs)
  echo "$B/scripts/$n.dll|$n|5||$REFS|$f" >> $J
done
if [ -f "$USER_SCRIPTS/Build video project (run in VEGAS).cs" ]; then
  mkdir -p $B/userbuild
  echo "$B/userbuild/Build video project.dll|Build|5||$REFS|$USER_SCRIPTS/Build video project (run in VEGAS).cs" >> $J
  # its pictures and songs, as empty files (the test's made-up media reader describes them by their extension)
  grep -o -E '"[^"]+\.(jpg|png|wav|mp3)"' "$USER_SCRIPTS/Build video project (run in VEGAS).cs" | tr -d '"' | while read -r n; do
    : > "$B/userbuild/$n"
    : > "$B/userbuild/pictures\\$n"     # (it asks for "pictures\\<name>": on Linux that is one file name)
  done
fi
many $J
python3 $T/check_abi.py $R/src/engine/scriptapi.h $R/src/scripting/ScriptPortal.Vegas.cs

# Against the real thing, when the files are here:
#  - VEGAS's own ScriptPortal.Vegas.dll: Reno's examples must compile against it too, and
#    ScriptPortal.Vegas.Unsupported.cs must be what gen_unsupported.py makes now (every VEGAS member Reno lacks);
#  - a real System.Windows.Forms (WFDIR, e.g. the .NET one of Windows Performance Toolkit): every C# script of VEGAS's
#    Script Menu folder must compile against Reno's ScriptPortal.Vegas.
if [ -f "$VEGAS_DLL" ] && [ -f "$WFDIR/System.Windows.Forms.dll" ]; then
  WF="$WFDIR/System.Windows.Forms.dll;$WFDIR/Accessibility.dll;$WFDIR/Microsoft.Win32.SystemEvents.dll"
  mkdir -p $B/real
  $P/pwsh -NoProfile -File $T/dump_api.ps1 "$VEGAS_DLL" $B/real/vegas_api.txt
  { echo "$B/real/main.dll|ScriptPortal.Vegas|5|RENO_NO_CODEDOM|$WF|$R/src/scripting/ScriptPortal.Vegas.cs"
    echo "$B/real/shim.dll|ScriptPortal.Vegas|5|RENO_NO_CODEDOM|$WF|$SHIM"; } > $J
  many $J
  $P/pwsh -NoProfile -File $T/dump_api.ps1 $B/real/main.dll $B/real/reno_api.txt
  python3 $T/gen_unsupported.py $B/real/vegas_api.txt $B/real/reno_api.txt $B/real/Unsupported.cs
  if cmp -s $B/real/Unsupported.cs $R/src/scripting/ScriptPortal.Vegas.Unsupported.cs; then echo "ScriptPortal.Vegas.Unsupported.cs is up to date"
  else echo "FAILED: ScriptPortal.Vegas.Unsupported.cs is out of date: run tests/scripting/gen_unsupported.py (see its first lines)"; exit 1; fi
  : > $J
  n=0
  for f in "$R/tools/scripts/"*.cs; do
    echo "$B/real/vegas_$(basename "$f" .cs).dll|x|5||$VEGAS_DLL;$WF|$f" >> $J
  done
  for f in "$VEGAS_SCRIPTS/"*.cs "$R/tools/scripts/"*.cs "$R/tools/vegas/"*.cs; do
    [ -f "$f" ] || continue
    echo "$B/real/reno_$(basename "$f" .cs).dll|x|5||$B/real/shim.dll;$WF|$f" >> $J
    n=$((n+1))
  done
  many $J
  echo "against VEGAS's API: $n scripts compile against Reno's ScriptPortal.Vegas, and Reno's examples against VEGAS's"
fi

# (EXTRA_CFLAGS: e.g. the sanitizers, when LIB was built with them)
F="-std=c++17 -O1 -g -D__cdecl= ${EXTRA_CFLAGS} -I$R/third_party/openfx -I$R/third_party/vst2 ${FFMPEG_INCLUDE:+-I$FFMPEG_INCLUDE}"
g++ $F $T/script_test.cpp $LIB -o $B/script_test -l:libavformat.so.60 -l:libavcodec.so.60 -l:libavutil.so.58 -l:libswscale.so.7 -l:libswresample.so.4 -ldl -lpthread
if [ -d "${RENO_SAMPLES:-}" ]; then
  cd $B && ./script_test $P $B "$RENO_SAMPLES" "$USER_SCRIPTS"
else
  echo "SKIP the sample-project part of script_test (RENO_SAMPLES not set)"
  cd $B && ./script_test $P $B
fi
