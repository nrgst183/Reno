#!/bin/bash
# Tests "reno_cli vegasfx" (src/ofx/fxcompare.cpp) on Linux: the real-against-built-in comparison with the stand-in plug-ins
# (tests/ofx/standinfx.cpp: VEGAS's Invert identifier, a generator and five plug-ins that fail in different ways) standing in for VEGAS's
# own, the built-ins-only run, the options, and --vegas-fx.
#
#   vegasfx_test.sh <reno_cli> <output folder>
#
# Needs g++ (the stand-ins are built here) and python3 (the JSON is read). Exit code: the number of failed checks.
set -u
RR=$(readlink -f "$1"); OUT=$(readlink -f "${2:-/tmp/vegasfx_test}")
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT/home" "$OUT/plugins"
export HOME="$OUT/home" QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu LC_ALL=C.UTF-8
cd "$OUT"
pass=0; fail=0
ok()  { echo "PASS $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1${2:+: $2}"; fail=$((fail+1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', want '$3'"; fi; }
has() { if grep -qF -- "$3" "$2"; then ok "$1"; else bad "$1" "no '$3' in $2"; fi; }
hasnt() { if grep -qF -- "$3" "$2"; then bad "$1" "found '$3' in $2"; else ok "$1"; fi; }

g++ -std=c++17 -g -fPIC -shared -I"$ROOT/third_party/openfx" "$ROOT/tests/ofx/standinfx.cpp" -o "$OUT/plugins/standinfx.ofx" || { echo "FAIL the stand-in plug-ins did not build"; exit 1; }

# ---- the comparison, with the stand-ins as "VEGAS's" plug-ins
rm -rf r1; mkdir r1
"$RR" vegasfx --plugin-path "$OUT/plugins" --out-dir r1 --size 160x90 --frames 2 --timeout 10 > r1.log 2>&1
check "vegasfx exits 0" $? 0
[ -s r1/vegasfx_report.txt ] && ok "the report is written" || bad "the report is written"
[ -s r1/vegasfx_report.json ] && ok "the JSON is written" || bad "the JSON is written"
R=r1/vegasfx_report.txt
has "the stand-in plug-ins were found" $R "effects in them to try: 7"
has "a crash in a test is reported and the run goes on" $R "crashes in some tests"
has "  the crashed tests are named" $R "every setting at its largest: the process crashed"
has "an effect that hangs is stopped and the run goes on" $R "took too long and was stopped"
has "an effect that draws nothing" $R "draws nothing"
has "an effect whose render fails" $R "render fails"
has "an effect that makes no instance" $R "makes no instance"
has "a real generator with no built-in works" $R "verdict: works (no built-in)"
has "the effects with no built-in and no way to use are listed" $R "VEGAS effects that do not work in Reno, with no built-in"
has "Invert is compared with the built-in" $R "VEGAS Invert (stand-in) (Filter)"
has "  the difference is listed" $R "Built-ins that look different"
hasnt "no 'none found' note when plug-ins were found" $R "none of VEGAS's own OpenFX plug-ins were found"
ls r1/*.png > /dev/null 2>&1 && ok "a picture of the worst case is written" || bad "a picture of the worst case is written"
for p in r1/*.png; do
    [ "$(head -c 8 "$p" | od -An -tx1 | tr -d ' \n')" = "89504e470d0a1a0a" ] && ok "  $(basename "$p") is a PNG" || bad "  $(basename "$p") is a PNG"
    if command -v ffprobe > /dev/null; then check "  its size is four pictures wide" "$(ffprobe -v error -show_entries stream=width,height -of csv=p=0 "$p")" "646,90"; fi
done
python3 - <<'PY' > r1/json_check.txt 2>&1
import json, sys
d = json.load(open("r1/vegasfx_report.json"))
rows = {(e["id"], e["context"]): e for e in d["effects"]}
def say(name, ok): print(("PASS " if ok else "FAIL ") + name)
inv = rows.get(("com.vegascreativesoftware:invert", "Filter"))
say("json: Invert row", inv is not None)
if inv:
    say("json: real works", inv["real"]["works"] and inv["real"]["status"] == "works")
    say("json: built-in works", inv["builtin"]["works"])
    c = inv.get("compare", {})
    say("json: difference measured (worst %.2f)" % c.get("worstScore", -1), 1.0 < c.get("worstScore", 0) < 10.0)
    say("json: at the defaults too", 1.0 < c.get("defaultScore", 0) < 10.0)
    say("json: the largest single difference is the stand-in's offset of 6", c.get("maxDifference") == 6)
    say("json: tests listed with their channels", len(c.get("tests", [])) >= 8 and len(c["tests"][0]["meanRGBA"]) == 4)
    say("json: more than the defaults were tried (min, middle, max, one at a time)", inv["real"]["tests"] >= 6)
    say("json: times", "msPerFrame" in inv["real"] and "msPerFrame" in inv["builtin"])
    say("json: verdict", inv["verdict"] in ("differs visibly", "close"))
    say("json: the picture is named", c.get("picture", "").endswith(".png"))
cr = rows.get(("de.magix:standincrash", "Filter"))
say("json: crash row", cr is not None and cr["real"]["crashes"] >= 1 and not cr["real"]["works"] and cr["real"]["testsOk"] >= 1)
hg = rows.get(("de.magix:standinhang", "Filter"))
say("json: hang row", hg is not None and hg["real"]["crashes"] == 1 and hg["real"]["testsOk"] >= 5)
bl = rows.get(("de.magix:standinblank", "Filter"))
say("json: blank row", bl is not None and bl["real"]["status"] == "draws nothing")
fl = rows.get(("de.magix:standinfail", "Filter"))
say("json: failing row", fl is not None and fl["real"]["status"] == "render fails")
ni = rows.get(("de.magix:standinnoinstance", "Filter"))
say("json: no-instance row", ni is not None and ni["real"]["status"] == "makes no instance")
ge = rows.get(("de.magix:standingenerator", "Generator"))
say("json: generator row", ge is not None and ge["real"]["works"] and not ge["builtin"]["present"])
bi = [e for e in d["effects"] if e["builtin"]["present"] and not e["real"]["present"]]
say("json: the built-ins with no real counterpart are tried too (%d)" % len(bi), len(bi) > 80)
say("json: every one of them works or is only suspect", all(e["builtin"]["works"] for e in bi))
ctx = set(e["context"] for e in d["effects"])
say("json: filters, generators and transitions", {"Filter", "Generator", "Transition"} <= ctx)
PY
cat r1/json_check.txt
pass=$((pass + $(grep -c '^PASS' r1/json_check.txt))); fail=$((fail + $(grep -c '^FAIL' r1/json_check.txt)))

# ---- only the built-ins (no plug-in folder given: this machine has no VEGAS)
rm -rf r2; mkdir r2
"$RR" vegasfx --out-dir r2 --size 160x90 --frames 1 --quick > r2.log 2>&1
check "built-ins only: exit 0" $? 0
has "  says so" r2/vegasfx_report.txt "none of VEGAS's own OpenFX plug-ins were found"
has "  every built-in is tried" r2/vegasfx_report.txt "Reno's built-in:"
has "  none of them fails" r2/vegasfx_report.txt " 0 do not"
python3 -c "
import json
d=json.load(open('r2/vegasfx_report.json'))
print('PASS built-ins only: %d effects, none with a real plug-in' % len(d['effects'])) if len(d['effects'])>90 and all(not e['real']['present'] for e in d['effects']) else print('FAIL built-ins only json')
" | tee r2/j.txt; grep -q '^PASS' r2/j.txt && pass=$((pass+1)) || fail=$((fail+1))
ls r2/*.png > /dev/null 2>&1 && bad "built-ins only: no comparison pictures" || ok "built-ins only: no comparison pictures"

# ---- options
rm -rf r3; mkdir r3
"$RR" vegasfx --only "Gaussian" --out-dir r3 --size 96x54 --frames 1 > r3.log 2>&1
check "--only: exit 0" $? 0
n=$(grep -c "^\[" r3.log)
check "--only Gaussian tries one effect" "$n" 1
has "  Gaussian Blur" r3/vegasfx_report.txt "Gaussian Blur (Filter)"
rm -rf "$OUT/r4"
"$RR" vegasfx --plugin-path "$OUT/plugins" --only invert --out-dir "$OUT/r4/deeper/still" --size 96x54 --frames 1 --max-params 0 > r4.log 2>&1
check "an output folder that does not exist is made" $? 0
[ -s "$OUT/r4/deeper/still/vegasfx_report.txt" ] && ok "  and the report is in it" || bad "  and the report is in it"
python3 -c "
import json
d=json.load(open('r4/deeper/still/vegasfx_report.json'))
r=[e for e in d['effects'] if e['id']=='com.vegascreativesoftware:invert'][0]
print('PASS --max-params 0: no one-at-a-time tests (%d tests)' % r['real']['tests']) if r['real']['tests'] <= 4 + 0 + 30 and not any(t['name'].startswith('min: ') for t in r['real']['testResults']) else print('FAIL --max-params 0')
" | tee r4/j.txt; grep -q '^PASS' r4/j.txt && pass=$((pass+1)) || fail=$((fail+1))
for bad_args in "--size 12" "--size 99999x10" "--frames 0" "--frames 99" "--frobnicate" "--out-dir"; do
    "$RR" vegasfx $bad_args > /dev/null 2>&1; check "vegasfx $bad_args: exit 2" $? 2
done

# ---- the built-ins' settings against resource files: a made-up bundle that disagrees with the built-in Invert in every way
B="$OUT/res/Fake.ofx.bundle/Contents/Resources"; mkdir -p "$B"
cat > "$B/Fake.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<!-- a comment <with> tags -->
<OfxImageEffectResource>
  <OfxPlugin name="com.vegascreativesoftware:invert">
    <OfxResourceSet ofxHost="default">
      <OfxPropLabel>VEGAS Invert</OfxPropLabel>
      <OfxImageEffectContext name="OfxImageEffectContextFilter">
        <OfxParamTypeDouble name="BlendAmount"><OfxPropLabel>Blend amount</OfxPropLabel></OfxParamTypeDouble>
        <OfxParamTypeBoolean name="BlendAmount2"><OfxPropLabel>Second</OfxPropLabel></OfxParamTypeBoolean>
      </OfxImageEffectContext>
    </OfxResourceSet>
  </OfxPlugin>
  <OfxPlugin name="com.vegascreativesoftware:sepia">
    <OfxResourceSet ofxHost="default">
      <OfxImageEffectContext name="default">
        <OfxParamTypeChoice name="Nothing"><OfxParamPropChoiceOption>A</OfxParamPropChoiceOption><OfxParamPropChoiceOption>B &amp; C</OfxParamPropChoiceOption></OfxParamTypeChoice>
      </OfxImageEffectContext>
    </OfxResourceSet>
  </OfxPlugin>
  <OfxPlugin name="com.example:only.in.vegas">
    <OfxResourceSet ofxHost="default">
      <OfxImageEffectContext name="OfxImageEffectContextFilter"><OfxParamTypeDouble name="X"/></OfxImageEffectContext>
    </OfxResourceSet>
  </OfxPlugin>
</OfxImageEffectResource>
XML
cp "$B/Fake.xml" "$B/Fake.de-DE.xml"; echo '<broken' > "$B/Fake_pt-BR.xml"; echo '<OfxPresetPackage/>' > "$B/PresetPackage.xml"
rm -rf r5; mkdir r5
"$RR" vegasfx --params-only --resources "$OUT/res" --out-dir r5 > r5.log 2>&1
check "--params-only: exit 0" $? 0
P=r5/vegasfx_params.txt
has "resource files: one read (locale, preset and broken files are not)" $P "resource files read: 1 (in 1 bundles); effects described in them: 3"
has "a setting missing in the built-in" $P "missing in the built-in: BlendAmount2 (OfxParamTypeBoolean)"
has "a setting that is only in the built-in is told apart" $P "the built-in also has"
has "choices that the built-in lacks are reported as a missing setting" $P "missing in the built-in: Nothing (OfxParamTypeChoice)"
has "an effect with no built-in" $P "com.example:only.in.vegas"
hasnt "a matching setting is not reported" $P "BlendAmount: "
python3 - <<'PY' | tee r5/j.txt
import json
d = json.load(open("r5/vegasfx_params.json"))["parameterCheck"]
inv = [e for e in d["effects"] if e["id"] == "com.vegascreativesoftware:invert"][0]
ok = d["parametersDiffer"] == 2 and d["noBuiltin"] == ["com.example:only.in.vegas"] and inv["differs"] and inv["contexts"][0]["missingInBuiltin"] == ["BlendAmount2 (OfxParamTypeBoolean)"]
print("PASS json: the parameter check" if ok else "FAIL json: the parameter check %r" % (d,))
PY
pass=$((pass + $(grep -c '^PASS' r5/j.txt))); fail=$((fail + $(grep -c '^FAIL' r5/j.txt)))
# VEGAS's real bundles' resource files, if they are on this machine: the list of built-ins that differ is the result
REAL=${VEGAS_OFX_DIR:-${RENO_VEGAS_DIR:-/nonexistent}/OFX Video Plug-Ins}
if [ -d "$REAL" ]; then
    rm -rf r6; mkdir r6
    "$RR" vegasfx --params-only --resources "$REAL" --out-dir r6 > r6.log 2>&1
    check "VEGAS's resource files: exit 0" $? 0
    has "  119 effects described" r6/vegasfx_params.txt "effects described in them: 119"
    has "  the built-ins are compared" r6/vegasfx_params.txt "built-ins with a resource entry: 119"
    hasnt "  Dual Fish Eye Stitching has VEGAS's settings" r6/vegasfx_params.txt "com.magix.ofx.vr.dual.stich (Dual"
    has "  effects with no built-in are listed" r6/vegasfx_params.txt "com.vegascreativesoftware:bzmasking"
else echo "NOTE VEGAS's resource files not found ($REAL): skipped"; fi

# ---- --vegas-fx
"$RR" --vegas-fx bogus x.mp4 > /dev/null 2>&1; check "--vegas-fx bogus: exit 2" $? 2
"$RR" --vegas-fx builtin --version > /dev/null 2>&1; check "--vegas-fx builtin is understood" $? 0
"$RR" --vegas-fx real --version > /dev/null 2>&1; check "--vegas-fx real is understood" $? 0
"$RR" --help > help.txt
has "--help lists --vegas-fx" help.txt "--vegas-fx real|builtin"
has "--help lists the vegasfx command" help.txt "reno_cli vegasfx"

echo "$pass passed, $fail failed"
exit $fail
