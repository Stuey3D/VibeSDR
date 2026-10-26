#!/usr/bin/env bash
# test-dsc.sh — the DSC receiver's bench (tools/dsc-bench), built twice: with the SIMD FIR (NEON on ARM, SSE2 on x86) and
# with VIBE_FORCE_SCALAR — so what the vector loop is worth is measured, not assumed.
#
#   scripts/test-dsc.sh                  build both; every format, decode rate vs SNR, 30 min of noise, CPU (both builds)
#   scripts/test-dsc.sh build            build only (prints the binaries' paths)
#   scripts/test-dsc.sh clip <file> [vhf]  the server tests' IQ clip (960 kHz; MF/HF on 2187.5 kHz or VHF ch 70)
#   scripts/test-dsc.sh wav <file.wav> [centreHz]   a recording (mono int16, 12/24/48 kHz) through the MF/HF receiver
#
# ★ Silent (nothing is played). ★ CPU: nice, one compile at a time. The pass/fail checks are vibeserver/test-dsc.cpp
#   (run by scripts/run-tests.sh); this is the measuring bench whose numbers go in the commit messages and docs.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vibe-dsc-bench"
mkdir -p "$OUT"
CPP=android/app/src/main/cpp
build() {
  local srcs="tools/dsc-bench/dsc_bench.cpp $CPP/decoders/dsc_decoder.cpp"
  nice -n 10 c++ -std=c++17 -O2 -I $CPP -I tools/dsc-bench $srcs -o "$OUT/dsc_bench"
  nice -n 10 c++ -std=c++17 -O2 -DVIBE_FORCE_SCALAR -I $CPP -I tools/dsc-bench $srcs -o "$OUT/dsc_bench_scalar"
}
case "${1:-all}" in
  build) build; echo "$OUT/dsc_bench"; echo "$OUT/dsc_bench_scalar" ;;
  clip)
    build
    if [ "${3:-}" = "vhf" ]; then "$OUT/dsc_bench" clip "${2:?clip file}" 960000 18 156525000 156525000 vhf
    else "$OUT/dsc_bench" clip "${2:?clip file}" 960000 18 2187500 2187500; fi ;;
  wav)   build; "$OUT/dsc_bench" wav "${2:?file.wav}" "${3:-1700}" ;;
  all)
    build
    "$OUT/dsc_bench" formats
    "$OUT/dsc_bench" snr 20
    "$OUT/dsc_bench" noise 30
    "$OUT/dsc_bench" cpu 600
    "$OUT/dsc_bench_scalar" cpu 600 ;;
  *) echo "usage: $0 [build|clip <file> [vhf]|wav <file.wav> [centreHz]]" >&2; exit 2 ;;
esac
