#!/bin/sh
# The handheld controller-keyboard port's golden test: the app's TS (kbLayouts.ts, controllerKeyboard.ts, and
# padKeyboard.ts's session logic) makes the reference (gen_kb_golden.ts), the C++ port (src/kb) must reproduce every
# line (kb_golden_test.cpp). Needs node ≥ 22 and a C++17 compiler. Silent: no audio, no GL.
#   handheld-ui/test/run_kb_golden.sh [outDir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-${TMPDIR:-/tmp}/kb-golden}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_kb_golden.ts "$OUT/kb_golden.txt"
SRC="$HERE/../src/kb"
${CXX:-c++} -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -o "$OUT/kb_golden_test" kb_golden_test.cpp \
  "$SRC/kb_layouts.cpp" "$SRC/controller_keyboard.cpp"
"$OUT/kb_golden_test" "$OUT/kb_golden.txt"
