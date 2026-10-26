#!/usr/bin/env bash
# test-cw.sh — the CW skimmer's bench (tools/cw-bench), built twice: with the SIMD kernels (NEON on ARM, SSE2 on x86) and
# with VIBE_FORCE_SCALAR (the reference loops, and PFFFT's scalar FFT) — so the speed-up is measured, not assumed.
#
#   scripts/test-cw.sh                 build both; run accuracy, conditions, human fists, multi, 30 min of noise, CPU
#   scripts/test-cw.sh build           build only (prints the two binaries' paths)
#   scripts/test-cw.sh clip <file>     build, then write the synthetic HF clip test-server-cw.mjs / test-web-cw.mjs replay
#   scripts/test-cw.sh wav <iq.wav> <centreHz> [width]   a real recording (stereo int16 I/Q) → spots and signals
#
# ★ Silent (nothing is played). ★ CPU: nice, one compile at a time. The pass/fail checks are vibeserver/test-cw.cpp
#   (run by scripts/run-tests.sh); this is the measuring bench whose numbers go in the commit messages and docs.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vibe-cw-bench"
mkdir -p "$OUT"
CPP=android/app/src/main/cpp
PFFFT=$CPP/vibedsp/third_party/pffft/pffft.c

build() {
  # ★ pffft.c is C (compiled as C, separately); the scalar build gets PFFFT's scalar FFT too.
  [ "$OUT/pffft.o" -nt "$PFFFT" ] || nice -n 10 cc -O2 -c "$PFFFT" -o "$OUT/pffft.o"
  [ "$OUT/pffft_scalar.o" -nt "$PFFFT" ] || nice -n 10 cc -O2 -DPFFFT_SIMD_DISABLE -c "$PFFFT" -o "$OUT/pffft_scalar.o"
  local srcs="tools/cw-bench/cw_bench.cpp $CPP/decoders/cw_skimmer.cpp"
  nice -n 10 c++ -std=c++17 -O2 -I $CPP -I tools/cw-bench $srcs "$OUT/pffft.o" -o "$OUT/cw_bench"
  nice -n 10 c++ -std=c++17 -O2 -DVIBE_FORCE_SCALAR -I $CPP -I tools/cw-bench $srcs "$OUT/pffft_scalar.o" -o "$OUT/cw_bench_scalar"
}

case "${1:-all}" in
  build) build; echo "$OUT/cw_bench"; echo "$OUT/cw_bench_scalar" ;;
  clip)  build; "$OUT/cw_bench" clip "${2:?clip file}" 960000 40 ;;
  wav)   build; "$OUT/cw_bench" wav "${2:?iq.wav}" "${3:?centre Hz}" "${4:-192000}" ;;
  all)
    build
    for m in accuracy conditions fists multi; do "$OUT/cw_bench" $m; done
    "$OUT/cw_bench" noise 30
    "$OUT/cw_bench" cpu 60
    "$OUT/cw_bench_scalar" cpu 60 ;;
  *) echo "usage: $0 [build|clip <file>|wav <iq.wav> <centreHz> [width]]" >&2; exit 2 ;;
esac
