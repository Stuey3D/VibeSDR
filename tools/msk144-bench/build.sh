#!/usr/bin/env bash
# Build tools/msk144-bench/msk144_bench — VibeSDR's MSK144 receiver over the black-box corpus, synthetic pings, noise and
# recordings. -O2, like the server. SCALAR=1 builds the plain-C kernels (VIBE_FORCE_SCALAR).
set -euo pipefail
cd "$(dirname "$0")/../.."
CPP=android/app/src/main/cpp
OUT="${OUT:-${TMPDIR:-/tmp}/msk144-bench}"
DEF=""; NAME=msk144_bench
if [ "${SCALAR:-0}" = 1 ]; then DEF="-DVIBE_FORCE_SCALAR"; NAME=msk144_bench_scalar; fi
mkdir -p "$OUT/obj$DEF"
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
  o="$OUT/obj$DEF/$(basename "$f" .c).o"
  [ -f "$o" ] && [ "$o" -nt "$f" ] || nice -n 15 cc -O2 $DEF -c -I "$CPP/ft8_lib" -I "$CPP/vibedsp/third_party/kissfft" -o "$o" "$f"
done
nice -n 15 c++ -std=c++17 -O2 $DEF -I "$CPP" -I "$CPP/ft8_lib" -I "$CPP/vibedsp" -I tools/msk144-bench -o "$OUT/$NAME" tools/msk144-bench/msk144_bench.cpp \
  "$CPP/decoders/digimodes/msk144_code.cpp" "$CPP/decoders/digimodes/msk144_decoder.cpp" "$CPP/decoders/digimodes/ldpc_small.cpp" \
  "$CPP/vibedsp/ddc.cpp" "$CPP/decoders/ft8_decoder.cpp" "$CPP/decoders/ftx_fine.cpp" "$OUT"/obj$DEF/*.o -lpthread
echo "$OUT/$NAME"
