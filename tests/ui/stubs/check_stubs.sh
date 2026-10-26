#!/bin/bash
# Checks that every interface test links with any of the components replaced by its stand-in here: each test is
# linked with each component stubbed in turn, and with all of them stubbed at once; the symbols a stand-in lacks
# are printed. Nothing is printed but "stubs: ok" when all is well.
#   RENO=<tree> QT_DIR=<Qt install> BASE=<uibase folder> [FFMPEG_INCLUDE=<FFmpeg headers>] check_stubs.sh <work folder>
# (BASE as for uibuild: $BASE/libbase.a made by uibase. Objects are kept in the work folder between runs.)
R=${RENO:-$(cd "$(dirname "$0")/../../.." && pwd)}; Q=${QT_DIR:?set QT_DIR to the Qt install}; B=${BASE:?set BASE to the uibase folder}; S=${1:-${TMPDIR:-/tmp}/check_stubs}
mkdir -p $S
QTV=${QT_VERSION:-6.8.3}
KF="-I$Q/include/QtGui/$QTV -I$Q/include/QtGui/$QTV/QtGui -I$Q/include/QtCore/$QTV -I$Q/include/QtCore/$QTV/QtCore"
CF="-std=c++17 -O0 -fPIC -w -D__cdecl= $KF -I$R/third_party/openfx -I$R/third_party/vst2 ${FFMPEG_INCLUDE:+-I$FFMPEG_INCLUDE} -I$Q/include -I$Q/include/QtCore -I$Q/include/QtGui -I$Q/include/QtWidgets"
comp="timeline fxwindow panels mixer editdialogs vegas"
build() { # source object
  if [ ! -f $2 ] || [ $1 -nt $2 ] || [ -n "$(find $R/src $R/tests/ui -name '*.h' -newer $2 | head -1)" ]; then
    rm -f $2; g++ $CF -c $1 -o $2 2>&1 | grep -E 'error' | head -5
  fi
}
for c in $comp; do build $R/tests/ui/stubs/${c}_stub.cpp $S/stub_$c.o; done
for c in timeline fxwindow panels mixer editdialogs mainwindow; do build $R/src/app/$c.cpp $S/real_$c.o; done
vegas=""; for f in $R/src/vegas/*.cpp; do o=$S/real_vegas_$(basename $f .cpp).o; build $f $o; vegas="$vegas $o"; done
# (the other windows the main window makes: every src/app file that is not one of the parts above or in the base library)
extra=""; for f in $R/src/app/*.cpp; do
  case $(basename $f .cpp) in main|mainwindow|appstate|playback|audioout_qt|timeline|fxwindow|panels|mixer|editdialogs) ;;
  *) o=$S/real_app_$(basename $f .cpp).o; build $f $o; extra="$extra $o" ;; esac
done
tests=""; for f in $R/tests/ui/*_test.cpp $R/tests/ui/look_shot.cpp $R/tests/ui/uishot.cpp; do t=$(basename $f .cpp); tests="$tests $t"; build $f $S/t_$t.o; done
real() { if [ $1 = vegas ]; then echo $vegas; else echo $S/real_$1.o; fi; }
link() { g++ $@ -o $S/a.out $B/libbase.a -L$Q/lib -lQt6Widgets -lQt6Gui -lQt6Core -l:libavformat.so.60 -l:libavcodec.so.60 -l:libavutil.so.58 -l:libswscale.so.7 -l:libswresample.so.4 -ldl -lpthread 2>&1 | grep -o "undefined reference to \`[^']*'" | sort -u; }
bad=0
for t in $tests; do
  mw=""; grep -q 'mainwindow.h' $R/tests/ui/$t.cpp && mw="$S/real_mainwindow.o $extra"
  for c in $comp all; do
    objs="$S/t_$t.o $mw"
    for x in $comp; do if [ $c = all ] || [ $c = $x ]; then objs="$objs $S/stub_$x.o"; else objs="$objs $(real $x)"; fi; done
    r=$(link $objs)
    if [ -n "$r" ]; then echo "== $t with $c stubbed:"; echo "$r" | head -12; bad=1; fi
  done
done
[ $bad = 0 ] && echo "stubs: ok"
exit $bad
