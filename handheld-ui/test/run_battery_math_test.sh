#!/usr/bin/env bash
# battery_math.h (the R36S Battery block's words and sums) — compiled natively, no SDL, no device.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
c++ -std=c++17 -O1 -Wall -Wextra -o "$T/t" "$H/test/battery_math_test.cpp"
"$T/t"
