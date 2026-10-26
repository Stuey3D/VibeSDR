#!/usr/bin/env bash
# test-jtty.sh — the JTTY receiver's bench (tools/jtty-bench), built twice: with the SIMD kernels (NEON on ARM, SSE2 on
# x86) and with VIBE_FORCE_SCALAR — so what the vector loops are worth is measured, not assumed.
#
#   scripts/test-jtty.sh                    build both; CPU (both builds) on a busy passband and on noise
#   scripts/test-jtty.sh build              build only (prints the binaries' paths)
#   scripts/test-jtty.sh clip <file>        the server tests' IQ clip (960 kHz centred on 14.0915 MHz, JTTY on 14.090 USB)
#   scripts/test-jtty.sh wav <file.wav>...  recordings through the receiver: every frame and message
#
# ★ Silent (nothing is played). ★ CPU: nice, one compile at a time. The pass/fail checks are vibeserver/test-jtty.cpp
#   (run by scripts/run-tests.sh); this is the measuring bench whose numbers go in docs/v12/JTTY.md.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vibe-jtty-bench"
mkdir -p "$OUT"
CPP=android/app/src/main/cpp
build() {
  [ -f "$OUT/pffft.o" ] || nice -n 10 cc -O2 -c -o "$OUT/pffft.o" $CPP/vibedsp/third_party/pffft/pffft.c
  local srcs="tools/jtty-bench/jtty_bench.cpp $CPP/decoders/jtty_decoder.cpp $OUT/pffft.o"
  nice -n 10 c++ -std=c++17 -O2 -I $CPP $srcs -o "$OUT/jtty_bench"
  nice -n 10 c++ -std=c++17 -O2 -DVIBE_FORCE_SCALAR -I $CPP $srcs -o "$OUT/jtty_bench_scalar"
}
case "${1:-all}" in
  build) build; echo "$OUT/jtty_bench"; echo "$OUT/jtty_bench_scalar" ;;
  clip)  build; "$OUT/jtty_bench" clip "${2:?clip file}" 960000 24 14091500 14090000 ;;
  wav)   build; shift; "$OUT/jtty_bench" wav "$@" ;;
  all)   build; "$OUT/jtty_bench" cpu 300; "$OUT/jtty_bench_scalar" cpu 300 ;;
  *) echo "usage: $0 [build|clip <file>|wav <file.wav>...]" >&2; exit 2 ;;
esac
