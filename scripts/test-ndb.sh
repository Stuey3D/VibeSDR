#!/usr/bin/env bash
# test-ndb.sh — the NDB skimmer's bench (tools/ndb-bench), built twice: with the SIMD kernels (NEON on ARM, SSE2 on x86) and
# with VIBE_FORCE_SCALAR (the reference loops, and PFFFT's scalar FFT) — so the speed-up is measured, not assumed.
#
#   scripts/test-ndb.sh                 build both; run bias, accuracy, multi, 30 min of noise, CPU (SIMD and scalar)
#   scripts/test-ndb.sh build           build only (prints the two binaries' paths)
#   scripts/test-ndb.sh wav <iq.wav> <centreHz> [loKHz hiKHz]   a real recording (stereo int16 I/Q) → idents
#
# ★ Silent (nothing is played). ★ CPU: nice, one compile at a time. The pass/fail checks are vibeserver/test-ndb.cpp
#   (run by scripts/run-tests.sh); this is the measuring bench whose numbers go in docs/v12/NDB-SKIMMER.md.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vibe-ndb-bench"
mkdir -p "$OUT"
CPP=android/app/src/main/cpp
PFFFT=$CPP/vibedsp/third_party/pffft/pffft.c

build() {
  [ "$OUT/pffft.o" -nt "$PFFFT" ] || nice -n 10 cc -O2 -c "$PFFFT" -o "$OUT/pffft.o"
  [ "$OUT/pffft_scalar.o" -nt "$PFFFT" ] || nice -n 10 cc -O2 -DPFFFT_SIMD_DISABLE -c "$PFFFT" -o "$OUT/pffft_scalar.o"
  local srcs="tools/ndb-bench/ndb_bench.cpp $CPP/decoders/ndb_skimmer.cpp $CPP/decoders/cw_skimmer.cpp"
  nice -n 10 c++ -std=c++17 -O2 -I $CPP -I tools/ndb-bench $srcs "$OUT/pffft.o" -o "$OUT/ndb_bench"
  nice -n 10 c++ -std=c++17 -O2 -DVIBE_FORCE_SCALAR -I $CPP -I tools/ndb-bench $srcs "$OUT/pffft_scalar.o" -o "$OUT/ndb_bench_scalar"
}

case "${1:-all}" in
  build) build; echo "$OUT/ndb_bench"; echo "$OUT/ndb_bench_scalar" ;;
  wav)   build; "$OUT/ndb_bench" wav "${2:?iq.wav}" "${3:?centre Hz}" "${4:-0}" "${5:-0}" ;;
  all)
    build
    "$OUT/ndb_bench" bias
    "$OUT/ndb_bench" accuracy
    "$OUT/ndb_bench" multi
    "$OUT/ndb_bench" noise 30
    "$OUT/ndb_bench" cpu 30
    "$OUT/ndb_bench_scalar" cpu 30 ;;
  *) echo "usage: $0 [build|wav <iq.wav> <centreHz> [loKHz hiKHz]]" >&2; exit 2 ;;
esac
