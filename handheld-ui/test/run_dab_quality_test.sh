#!/usr/bin/env bash
# The R36S's DAB reception meter (dab_quality.h) against the app's and the web's (src/utils/dabQuality.ts): the TS
# meter's outputs on scripted report sequences, replayed through the port natively. Fails on any difference.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
node --no-warnings "$H/test/gen_dab_quality_golden.ts" > "$T/golden.jsonl"
c++ -std=c++17 -O1 -Wall -Wextra -o "$T/t" "$H/test/dab_quality_test.cpp"
"$T/t" "$T/golden.jsonl"
