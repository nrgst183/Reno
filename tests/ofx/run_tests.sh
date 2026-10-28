#!/bin/sh
# Builds the OpenFX host test (hosttest) and the test plug-in (testfx) three ways and runs the suite each time:
#   plain      -O2
#   asan+ubsan -fsanitize=address,undefined -fno-sanitize-recover=undefined
#   tsan       -fsanitize=thread
# Usage: run_tests.sh [build-dir]        (default a folder under the system temp dir)
# Environment: RENO_TEST_PLUGIN  the test effects plug-in (.ofx) to test; its tests are skipped, with a message, if it is not set.
#              CXX          the compiler (default g++)
#              CONFIGS      which of "plain asan tsan" to run (default all three)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-${TMPDIR:-/tmp}/reno_ofx_tests}
CXX=${CXX:-g++}
CONFIGS=${CONFIGS:-"plain asan tsan"}
RENO_TEST_PLUGIN=${RENO_TEST_PLUGIN:-}
export RENO_TEST_PLUGIN
mkdir -p "$OUT" || exit 2

if [ ! -f "$RENO_TEST_PLUGIN" ]; then
    echo "NOTE: the test effects plug-in was not found (RENO_TEST_PLUGIN='$RENO_TEST_PLUGIN') - its tests will be skipped (set RENO_TEST_PLUGIN to run them)"
fi

COMMON="-std=c++17 -g -Wall -Wextra -I$ROOT/src/ofx -I$ROOT/third_party/openfx"
HOST_SRC="$ROOT/src/core/*.cpp $ROOT/src/ofx/*.cpp $ROOT/src/engine/profile.cpp"
status=0

run_config() {
    name=$1
    flags=$2
    dir="$OUT/$name"
    mkdir -p "$dir"
    echo "=== $name: building"
    # the test plug-in and the test program are built with the same flags as the host
    $CXX $COMMON $flags -fPIC -shared "$HERE/testfx.cpp" -o "$dir/testfx.ofx" -lpthread || { echo "BUILD FAILED (testfx, $name)"; status=1; return; }
    $CXX $COMMON $flags "$HERE/hosttest.cpp" $HOST_SRC -o "$dir/hosttest" -ldl -lpthread || { echo "BUILD FAILED (hosttest, $name)"; status=1; return; }
    echo "=== $name: running"
    HOSTTEST_TMP="$dir/tmp" "$dir/hosttest" "$dir/testfx.ofx" > "$dir/results.txt" 2> "$dir/stderr.txt"
    rc=$?
    grep -E '^(FAIL|SKIP|NOTE)' "$dir/results.txt"
    grep -E '^SUMMARY' "$dir/results.txt" || echo "SUMMARY: (none - the test program did not finish)"
    if [ -s "$dir/stderr.txt" ]; then
        # the effects' own messages go to stderr; sanitizer reports are the lines to look for
        if grep -qE 'ERROR: (Address|Leak)Sanitizer|runtime error|WARNING: ThreadSanitizer|SUMMARY: (Address|Thread|Undefined)' "$dir/stderr.txt"; then
            echo "SANITIZER REPORT in $dir/stderr.txt:"
            grep -E -m1 -A25 'ERROR: (Address|Leak)Sanitizer|runtime error|WARNING: ThreadSanitizer' "$dir/stderr.txt"
            rc=1
        fi
    fi
    [ $rc -eq 0 ] || status=1
    echo "=== $name: exit status $rc"
    # multi-resolution (the preview-size rendering of third-party effects): the effect says 1, 0, or nothing
    $CXX $COMMON $flags -fPIC -shared "$HERE/scalefx.cpp" -o "$dir/scalefx.ofx" -lpthread || { echo "BUILD FAILED (scalefx, $name)"; status=1; return; }
    $CXX $COMMON $flags "$HERE/scaletest.cpp" $HOST_SRC -o "$dir/scaletest" -ldl -lpthread || { echo "BUILD FAILED (scaletest, $name)"; status=1; return; }
    for m in multi nomulti default; do
        "$dir/scaletest" "$dir/scalefx.ofx" $m > "$dir/scale_$m.txt" 2>&1
        src=$?
        grep -E '^FAIL' "$dir/scale_$m.txt"
        echo "scaletest $m: $(tail -1 "$dir/scale_$m.txt") (exit $src)"
        [ $src -eq 0 ] || status=1
    done
    # rendering with OpenGL (Mesa's software OpenGL here): the effect draws with OpenGL when asked and on the processor
    # otherwise, to the same picture; "nogpu" is a host where no context can be made
    $CXX $COMMON $flags -fPIC -shared "$HERE/glfx.cpp" -o "$dir/glfx.ofx" -ldl -lpthread || { echo "BUILD FAILED (glfx, $name)"; status=1; return; }
    $CXX $COMMON $flags "$HERE/gputest.cpp" $HOST_SRC -o "$dir/gputest" -ldl -lpthread || { echo "BUILD FAILED (gputest, $name)"; status=1; return; }
    for m in gl nogpu; do
        TSAN_OPTIONS="${TSAN_OPTIONS:-} suppressions=$HERE/tsan_mesa.supp" "$dir/gputest" "$dir/glfx.ofx" $m > "$dir/gpu_$m.txt" 2>&1
        grc=$?
        grep -E '^(FAIL|NOTE)' "$dir/gpu_$m.txt"
        echo "gputest $m: $(tail -1 "$dir/gpu_$m.txt") (exit $grc)"
        [ $grc -eq 0 ] || status=1
    done
}

for c in $CONFIGS; do
    case $c in
    plain) run_config plain "-O2" ;;
    asan) ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0} run_config asan "-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" ;;
    tsan) TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=0:second_deadlock_stack=1} run_config tsan "-O1 -fsanitize=thread -fno-omit-frame-pointer" ;;
    *) echo "unknown config $c"; status=2 ;;
    esac
done
exit $status
