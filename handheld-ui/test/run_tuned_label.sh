#!/bin/sh
# The handheld TUNED STATION LABEL (src/tuned_label.h) against the app's src/utils/tunedStation.ts: the golden lines from
# the TS, replayed by the C++ in Docker linux/arm64 (aarch64 g++, as the R36S) when the image is there, else with the host
# compiler (the header is pure C++17). Silent: no audio anywhere.
#   handheld-ui/test/run_tuned_label.sh [outDir]
# Needs node ≥ 22.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/R36S-debug/tuned-label}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs gen_tuned_label_golden.ts "$OUT/tuned_label_golden.txt"
cp tuned_label_inputs.json "$OUT/"
if command -v docker >/dev/null 2>&1 && docker image inspect vibe-hh-map:trixie >/dev/null 2>&1; then
  docker run --rm --platform linux/arm64 -v "$HERE/..:/w" -v "$OUT:/o" vibe-hh-map:trixie sh -c "
    set -e; cd /w
    g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -o /o/tuned_label_test test/tuned_label_test.cpp
    /o/tuned_label_test /o/tuned_label_inputs.json /o/tuned_label_golden.txt"
else
  ${CXX:-c++} -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -o "$OUT/tuned_label_test" tuned_label_test.cpp
  "$OUT/tuned_label_test" "$OUT/tuned_label_inputs.json" "$OUT/tuned_label_golden.txt"
fi
