#!/usr/bin/env bash
# Build tools/wspr-bench/wspr_bench — VibeSDR's WSPR decoder over recordings, synthetic sweeps and noise. -O2, like the
# server. SCALAR=1 builds the plain-C kernels (VIBE_FORCE_SCALAR) — what the SIMD ones are worth, and their reference.
set -euo pipefail
cd "$(dirname "$0")/../.."
CPP=android/app/src/main/cpp
OUT="${OUT:-${TMPDIR:-/tmp}/wspr-bench}"
mkdir -p "$OUT"
DEF=""; NAME=wspr_bench
if [ "${SCALAR:-0}" = 1 ]; then DEF="-DVIBE_FORCE_SCALAR"; NAME=wspr_bench_scalar; fi
cc -O2 $DEF -c -o "$OUT/pffft$DEF.o" $CPP/vibedsp/third_party/pffft/pffft.c
c++ -std=c++17 -O2 $DEF -I "$CPP" -o "$OUT/$NAME" tools/wspr-bench/wspr_bench.cpp "$CPP/decoders/wspr_decoder.cpp" "$OUT/pffft$DEF.o" -lpthread
echo "$OUT/$NAME"
