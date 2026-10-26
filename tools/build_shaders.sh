#!/bin/bash
# Regenerates src/ofx/vkshaders (*.spv.h, push_layouts.h and the shader index) from tools/gpukernels.py. Needs only Python 3.
# The generated files are committed, so a normal build does not run this.
set -e
cd "$(dirname "$0")/.."
OUT=src/ofx/vkshaders
mkdir -p $OUT
rm -f $OUT/*.spv.h
python3 tools/gpukernels.py $OUT
python3 tools/shader_index.py $OUT
rm -rf tools/__pycache__
