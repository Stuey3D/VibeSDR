#!/bin/sh
# The handheld waterfall port's golden tests: the app's TS modules make the reference (gen_golden.ts), the C++ port must
# reproduce it (wf_golden_test.cpp). Needs node ≥ 22 (runs the app's .ts directly) and a C++17 compiler. No GL.
#   handheld-ui/test/run_golden.sh [outDir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-${TMPDIR:-/tmp}/wf-golden}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs gen_palettes.ts
node --no-warnings --import ./ts_resolve.mjs gen_golden.ts "$OUT"
SRC="$HERE/../src/wf"
${CXX:-c++} -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -o "$OUT/wf_golden_test" wf_golden_test.cpp \
  "$SRC/wf_row_place.cpp" "$SRC/wf_clock.cpp" "$SRC/wf_pace.cpp" "$SRC/wf_signal.cpp" "$SRC/wf_combine.cpp" \
  "$SRC/wf_palette.cpp" "$SRC/wf_model.cpp" "$SRC/wf_wide.cpp"
"$OUT/wf_golden_test" "$OUT"
