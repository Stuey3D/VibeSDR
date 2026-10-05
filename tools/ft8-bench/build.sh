#!/usr/bin/env bash
# Build tools/ft8-bench/ft8_wav — VibeSDR's FT8/FT4 slot decoder over a recording. -O2, like the server's decoders.
set -euo pipefail
cd "$(dirname "$0")/../.."
CPP=android/app/src/main/cpp
OUT="${OUT:-${TMPDIR:-/tmp}/ft8-bench}"
mkdir -p "$OUT/obj"
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c; do
  o="$OUT/obj/$(basename "$f" .c).o"
  [ "$o" -nt "$f" ] || cc -O2 -c -I "$CPP/ft8_lib" -o "$o" "$f"
done
c++ -std=c++17 -O2 -I "$CPP" -I "$CPP/ft8_lib" -o "$OUT/ft8_wav" tools/ft8-bench/ft8_wav.cpp "$CPP/decoders/ft8_decoder.cpp" "$OUT"/obj/*.o -lpthread
echo "$OUT/ft8_wav"
c++ -std=c++17 -O2 -I "$CPP/ft8_lib" -o "$OUT/ft8_synth" tools/ft8-bench/ft8_synth.cpp "$OUT"/obj/*.o
echo "$OUT/ft8_synth"
