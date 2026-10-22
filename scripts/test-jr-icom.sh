#!/bin/bash
# test-jr-icom.sh — run Jr's Icom rules (spike/WristSDR/WristSDR/IcomRadio.swift) on this Mac, no simulator.
# Compiles the one file with two stand-ins (scripts/jr-icom-check/stubs.swift) and the checks in main.swift.
set -o pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
nice -n 19 xcrun swiftc -o "$OUT/jr-icom-check" scripts/jr-icom-check/stubs.swift \
  spike/WristSDR/WristSDR/IcomRadio.swift scripts/jr-icom-check/main.swift || exit 1
"$OUT/jr-icom-check"
