#!/usr/bin/env bash
# The R36S's DRM reception meter (drm_quality.h) against the app's (src/utils/drmQuality.ts): the TS meter's outputs on
# scripted report sequences, replayed through the port natively. Fails on any difference in level, words or readings.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
node --no-warnings "$H/test/gen_drm_quality_golden.ts" "$T/golden.jsonl"
c++ -std=c++17 -O1 -Wall -Wextra -o "$T/t" "$H/test/drm_quality_test.cpp"
"$T/t" "$T/golden.jsonl"
