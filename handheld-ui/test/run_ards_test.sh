#!/bin/sh
# The handheld Advanced RDS port's test (src/adv_rds_core.h): the app's lrMeter.ts / mpxPower.ts make a golden file
# (gen_ards_golden.ts) the C++ must reproduce, then the panel's readings are checked scenario by scenario against
# AdvRdsPanel.tsx's rules (adv_rds_test.cpp). Needs node >= 22 and a C++17 compiler. Silent: no audio, no GL.
#   handheld-ui/test/run_ards_test.sh [outDir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-${TMPDIR:-/tmp}/ards-test}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_ards_golden.ts "$OUT/ards_golden.txt"
${CXX:-c++} -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -o "$OUT/adv_rds_test" adv_rds_test.cpp
"$OUT/adv_rds_test" "$OUT/ards_golden.txt"
