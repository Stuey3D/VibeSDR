#!/usr/bin/env bash
# Build tools/js8-bench/js8_wav — VibeSDR's JS8 slot decoder over a recording. -O2, like the server's decoders.
set -euo pipefail
cd "$(dirname "$0")/../.."
CPP=android/app/src/main/cpp
OUT="${OUT:-${TMPDIR:-/tmp}/js8-bench}"
mkdir -p "$OUT/obj"
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
  o="$OUT/obj/$(basename "$f" .c).o"
  cc -O2 -c -I "$CPP/ft8_lib" -o "$o" "$f"
done
c++ -std=c++17 -O2 -I "$CPP" -I "$CPP/ft8_lib" -o "$OUT/js8_wav" tools/js8-bench/js8_wav.cpp \
    "$CPP/decoders/js8_decoder.cpp" "$CPP/decoders/ftx_fine.cpp" "$CPP/decoders/js8_ldpc.cpp" "$CPP/decoders/js8_message.cpp" "$OUT"/obj/*.o -lpthread
echo "$OUT/js8_wav"
