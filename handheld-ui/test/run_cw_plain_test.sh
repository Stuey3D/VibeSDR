#!/usr/bin/env bash
# The R36S's EXPLAIN reading (cw_plain.h) against the app's plainEnglish: regenerates cw_vocab.h + the corpus from
# src/utils/cwVocab.ts, compiles the test natively, runs it. Fails if any line reads differently.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
node "$H/test/gen_cw_vocab.mjs" --golden "$T/golden.tsv" 2>/dev/null
git -C "$H" diff --quiet -- src/cw_vocab.h || { echo "cw_vocab.h was stale — regenerated; commit it"; }
c++ -std=c++17 -O1 -Wall -Wextra -o "$T/t" "$H/test/cw_plain_test.cpp"
"$T/t" "$T/golden.tsv"
