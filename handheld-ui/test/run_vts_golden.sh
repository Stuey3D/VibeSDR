#!/bin/sh
# The handheld VTS port's golden test: the app's TS (vtsLine.ts, psStabiliser.ts, cleanLines.ts, dabTypes.ts,
# rdsCountry.ts, bandPlan.ts + stations.ts) makes the reference (gen_vts_golden.ts); the C++ port (src/vts_core.h) must
# reproduce every line, then pass the bar's scenario checks (vts_golden_test.cpp). Needs node ≥ 22 and a C++17
# compiler. Silent: no audio, no GL.
#   handheld-ui/test/run_vts_golden.sh [outDir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-${TMPDIR:-/tmp}/vts-golden}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_vts_golden.ts "$OUT/vts_golden.txt"
${CXX:-c++} -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -o "$OUT/vts_golden_test" vts_golden_test.cpp
"$OUT/vts_golden_test" "$OUT/vts_golden.txt"
