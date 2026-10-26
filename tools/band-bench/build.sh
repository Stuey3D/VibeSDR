#!/usr/bin/env bash
# Build tools/band-bench/band_bench (this machine). -O2, like the server's decoders. OUT= where (default $TMPDIR/band-bench).
set -euo pipefail
cd "$(dirname "$0")/../.."
CPP=android/app/src/main/cpp
OUT="${OUT:-${TMPDIR:-/tmp}/band-bench}"
mkdir -p "$OUT/obj"
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
  o="$OUT/obj/$(basename "$f" .c).o"; [ "$o" -nt "$f" ] || nice -n 10 cc -O2 -c -I "$CPP/ft8_lib" -o "$o" "$f"
done
nice -n 10 c++ -std=c++17 -O2 -I "$CPP" -I "$CPP/ft8_lib" -I "$CPP/vibedsp" -o "$OUT/band_bench" tools/band-bench/band_bench.cpp \
  $CPP/decoders/digi_modes.cpp $CPP/decoders/digi_tuner.cpp $CPP/decoders/digi_spotter.cpp $CPP/decoders/ft8_decoder.cpp \
  $CPP/decoders/jtty_decoder.cpp $CPP/decoders/psk_skimmer.cpp $CPP/decoders/cw_skimmer.cpp $CPP/decoders/digimodes/*.cpp \
  $CPP/vibedsp/ddc.cpp "$OUT"/obj/*.o -lpthread
echo "$OUT/band_bench"
