#!/bin/bash
# The effects window with a bridged plug-in (tests/ui/vst32_ui_test.cpp), on Linux. Needs tests/vst32/run.sh run first
# (it builds reno_vst32 and bridgevst.so into the folder given here as $2) and uibase's library in $BASE.
#   RENO=<tree> BASE=<uibase folder> [UIBUILD=<the uibuild script>] tests/vst32/run_ui.sh <output folder> [folder of run.sh's build]
set -e
R=${RENO:-$(cd "$(dirname "$0")/../.." && pwd)}; BASE=${BASE:?set BASE to the uibase folder}; O=${1:-$BASE/vst32_ui}; L=${2:-${TMPDIR:-/tmp}/reno_vst32}
mkdir -p $O/home
RENO=$R BASE=$BASE EXTRA_CFLAGS="-DRENO_FX_HOST_PLUGIN_WINDOWS" ${UIBUILD:-uibuild} $BASE/vst32_ui_test $R/tests/ui/vst32_ui_test.cpp fxwindow \
  $R/src/app/fxlibrary.cpp $R/src/app/pluginchooser.cpp
QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu LC_ALL=C.UTF-8 HOME=$O/home $BASE/vst32_ui_test $O $L/reno_vst32 $L/bridgevst.so
