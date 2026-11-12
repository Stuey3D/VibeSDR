#!/bin/sh
# The handheld WEFAX port's test (src/dec_pic_align.h): the app's wefaxAlign.ts / wefaxCrisp.ts make a golden file over
# the WEFAX world corpus (gen_wefax_golden.ts) that the C++ must reproduce state change for state change. Needs node >= 22,
# a C++17 compiler and zlib. Silent: no audio, no GL.   handheld-ui/test/run_wefax_golden.sh [outDir] [--quick]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-${TMPDIR:-/tmp}/wefax-golden}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_wefax_golden.ts "$OUT/wefax" $2
${CXX:-c++} -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -o "$OUT/wefax_golden_test" wefax_golden_test.cpp -lz
"$OUT/wefax_golden_test" "$OUT/wefax"
