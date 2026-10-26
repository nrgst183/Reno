#!/bin/bash
# Checks tools/vegas/Reno Bridge.cs without VEGAS:
#  1. compiles it as VEGAS would (C# 5) against VEGAS Pro 18's own ScriptPortal.Vegas.dll and Windows Forms, together
#     with the two scripts it uses (Export to Reno.cs, Import Reno Project.cs), so every VEGAS member it calls exists;
#  2. compiles its core with BRIDGE_TEST (no VEGAS, no window) together with tests/compare/bridge_test.cs and runs
#     the tests (BridgeTests.Run) in PowerShell 7.
# The C# compiler is Roslyn inside PowerShell 7 (tests/scripting/roslyn.ps1).
#
#   [SUITE=<bridge folder made by makesuite>] bridge_test.sh [out folder]
# Environment: PWSH (PowerShell 7; default: pwsh from the path), RENO_VEGAS_DIR (the VEGAS Pro 18.0 program folder
# with ScriptPortal.Vegas.dll), RENO_WINFORMS_DIR (a folder with System.Windows.Forms.dll and System.Drawing.Common.dll).
# Without them the VEGAS compile check is skipped; without PowerShell the whole test is.
# Exit code: the number of failures.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-/tmp/bridge_test}; mkdir -p "$OUT"
PWSH=${PWSH:-$(command -v pwsh || true)}
if [ -z "$PWSH" ] || [ ! -x "$PWSH" ]; then echo "SKIP bridge_test: PowerShell 7 not found (set PWSH)"; exit 0; fi
VEGAS_DIR=${RENO_VEGAS_DIR:-${VEGAS_DIR:-/nonexistent}}
WINFORMS_DIR=${RENO_WINFORMS_DIR:-${WINFORMS_DIR:-/nonexistent}}
ROSLYN="$ROOT/tests/scripting/roslyn.ps1"
B="$ROOT/tools/vegas/Reno Bridge.cs"
fail=0

if [ -f "$VEGAS_DIR/ScriptPortal.Vegas.dll" ] && [ -f "$WINFORMS_DIR/System.Windows.Forms.dll" ]; then
  REFS="$VEGAS_DIR/ScriptPortal.Vegas.dll;$WINFORMS_DIR/System.Windows.Forms.dll;$WINFORMS_DIR/System.Drawing.Common.dll"
  for s in "Reno Bridge" "Export to Reno" "Import Reno Project"; do
    if "$PWSH" -NoProfile -File "$ROSLYN" "$OUT/vegas_check.dll" Check 5 "" "$REFS" "$ROOT/tools/vegas/$s.cs" > "$OUT/compile.log" 2>&1; then
      echo "PASS compiles against VEGAS's ScriptPortal.Vegas.dll: $s.cs"
    else
      echo "FAIL compiles against VEGAS's ScriptPortal.Vegas.dll: $s.cs"; cat "$OUT/compile.log"; fail=$((fail+1))
    fi
    grep -i "warning" "$OUT/compile.log" | head -5
  done
else
  echo "SKIP the VEGAS compile check (set RENO_VEGAS_DIR and RENO_WINFORMS_DIR to run it)"
fi

if "$PWSH" -NoProfile -File "$ROSLYN" "$OUT/bridge_core.dll" BridgeCoreTest 5 "BRIDGE_TEST" "" "$B;$HERE/bridge_test.cs" > "$OUT/core.log" 2>&1; then
  echo "PASS the core compiles without VEGAS (BRIDGE_TEST)"
  "$PWSH" -NoProfile -Command "[System.Reflection.Assembly]::LoadFrom('$OUT/bridge_core.dll') | Out-Null; exit [BridgeTests]::Run()"
  n=$?; fail=$((fail+n))
else
  echo "FAIL the core compiles without VEGAS"; cat "$OUT/core.log"; fail=$((fail+1))
fi
# 3. (with SUITE=<bridge folder> of makesuite) every suite project is read by Import Reno Project.cs's own JSON reader
if [ -n "${SUITE:-}" ] && [ -f "$OUT/vegas_check.dll" ]; then
  "$PWSH" -NoProfile -File "$ROSLYN" "$OUT/import.dll" Import 5 "" "$REFS" "$ROOT/tools/vegas/Import Reno Project.cs" > /dev/null 2>&1
  "$PWSH" -NoProfile -Command "
\$a = [System.Reflection.Assembly]::LoadFrom('$OUT/import.dll'); \$t = \$a.GetType('Json'); \$n = 0; \$bad = 0
foreach (\$f in Get-ChildItem '$SUITE/suite' -Filter *.reno) {
  try { \$j = \$t.GetMethod('Parse').Invoke(\$null, @([IO.File]::ReadAllText(\$f.FullName))); if (-not \$j.ContainsKey('tracks')) { \$bad++ }; \$n++ } catch { \$bad++; 'bad: ' + \$f.Name }
}
if (\$bad -eq 0 -and \$n -gt 0) { 'PASS the importer reads all ' + \$n + ' suite projects' ; exit 0 } else { 'FAIL the importer reads the suite projects (' + \$bad + ' of ' + \$n + ' bad)'; exit 1 }"
  fail=$((fail+$?))
fi
echo "bridge_test: $fail failed"
exit $fail
