#!/usr/bin/env bash
# test-psk.sh — the PSK skimmer's bench (tools/psk-bench), built twice: with the SIMD kernels (NEON on ARM, SSE2 on x86)
# and with VIBE_FORCE_SCALAR (the reference loops and PFFFT's scalar FFT) — so the speed-up is measured, not assumed.
#
#   scripts/test-psk.sh                 build both; run accuracy, offsets, drift, multi, 30 min of noise, CPU
#   scripts/test-psk.sh build           build only (prints the two binaries' paths)
#   scripts/test-psk.sh clip <file>     build, then write the synthetic HF clip test-server-psk.mjs / test-web-psk.mjs replay
#   scripts/test-psk.sh wav <file.wav> <centreHz> [width]   a real recording (stereo int16 I/Q, or mono audio)
#
# ★ Silent (nothing is played). ★ CPU: nice, one compile at a time. The pass/fail checks are vibeserver/test-psk.cpp
#   (run by scripts/run-tests.sh); this is the measuring bench whose numbers go in the commit messages and docs.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vibe-psk-bench"
mkdir -p "$OUT"
CPP=android/app/src/main/cpp
PFFFT=$CPP/vibedsp/third_party/pffft/pffft.c

build() {
  [ "$OUT/pffft.o" -nt "$PFFFT" ] || nice -n 10 cc -O2 -c "$PFFFT" -o "$OUT/pffft.o"
  [ "$OUT/pffft_scalar.o" -nt "$PFFFT" ] || nice -n 10 cc -O2 -DPFFFT_SIMD_DISABLE -c "$PFFFT" -o "$OUT/pffft_scalar.o"
  local srcs="tools/psk-bench/psk_bench.cpp $CPP/decoders/psk_skimmer.cpp $CPP/decoders/cw_skimmer.cpp"
  nice -n 10 c++ -std=c++17 -O2 -I $CPP -I tools/psk-bench $srcs "$OUT/pffft.o" -o "$OUT/psk_bench"
  nice -n 10 c++ -std=c++17 -O2 -DVIBE_FORCE_SCALAR -I $CPP -I tools/psk-bench $srcs "$OUT/pffft_scalar.o" -o "$OUT/psk_bench_scalar"
}

case "${1:-all}" in
  build) build; echo "$OUT/psk_bench"; echo "$OUT/psk_bench_scalar" ;;
  clip)  build; "$OUT/psk_bench" clip "${2:?clip file}" 48000 40 ;;
  wav)   build; "$OUT/psk_bench" wav "${2:?file.wav}" "${3:-0}" "${4:-0}" ;;
  all)
    build
    for m in accuracy offsets drift multi; do "$OUT/psk_bench" $m; done
    "$OUT/psk_bench" noise 30
    "$OUT/psk_bench" cpu 60
    "$OUT/psk_bench_scalar" cpu 60 ;;
  *) echo "usage: $0 [build|clip <file>|wav <file.wav> <centreHz> [width]]" >&2; exit 2 ;;
esac
