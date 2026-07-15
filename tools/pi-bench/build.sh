#!/usr/bin/env bash
# Build the VibeServer capacity benchmark.
#
# No dongle, no root, no librtlsdr needed — it feeds the DSP synthetic IQ.
# Safe to run on a box that's already doing something else (e.g. an ADS-B
# feeder); it only burns CPU while it runs.
#
#   sudo apt install -y g++            # if you haven't got a compiler
#   ./build.sh && ./bench
set -euo pipefail

cd "$(dirname "$0")"
DSP="../../android/app/src/main/cpp/vibedsp"

if [ ! -f "$DSP/vibedsp.h" ]; then
  echo "error: can't find vibedsp at $DSP" >&2
  echo "       run this from inside a full VibeSDR checkout." >&2
  exit 1
fi

# Same flags the real app builds the DSP with (android/.../CMakeLists.txt:38),
# so the numbers reflect the shipping binary rather than this script.
FLAGS="-std=c++17 -O3 -ffp-contract=fast -Wall"

echo "arch     : $(uname -m)"
echo "compiler : $(g++ --version | head -1)"
case "$(uname -m)" in
  aarch64|arm64) echo "note     : 64-bit ARM — the NEON fast path WILL compile in." ;;
  armv7l|armv6l) echo "note     : 32-bit ARM — vibedsp gates NEON on __aarch64__, so this"
                 echo "           will build the SCALAR path. Expect a pessimistic result." ;;
esac
echo

# 32-bit ARM has no native 64-bit atomics, so std::atomic<double> in RxPipeline
# (pipeline.cpp, deempTau_) lands on libatomic calls: "undefined reference to
# __atomic_load_8". aarch64/x86 do them inline and don't need this. macOS has
# no -latomic at all, hence the guard.
ATOMIC=""
if [ "$(uname -s)" = "Linux" ]; then ATOMIC="-latomic"; fi

g++ $FLAGS \
  bench.cpp \
  "$DSP/fft.cpp" \
  "$DSP/ddc.cpp" \
  "$DSP/resampler.cpp" \
  "$DSP/stereo.cpp" \
  "$DSP/rds.cpp" \
  "$DSP/pipeline.cpp" \
  "$DSP/third_party/kissfft/kiss_fft.c" \
  "$DSP/third_party/kissfft/kiss_fftr.c" \
  -I"$DSP" \
  -I"$DSP/third_party/kissfft" \
  -lm -lpthread $ATOMIC \
  -o bench

echo "built ./bench — run it with:  ./bench"
