#!/usr/bin/env bash
# tools/fst4-bench/qemu.sh — FST4's instruction count on the Pi 2 build (armhf, -marm NEON), and the scalar build, under
# qemu-arm with the FT8 work's TCG counter (tools/ft8-bench/qemu). Uses the WORKING TREE. A count, not a cycle count:
# the Pi 2 estimate takes 0.6–0.8 instructions per cycle at 900 MHz (docs/v12/DIGITAL-SPOTS.md's convention).
#   qemu.sh [fst4_bench args…]      default: noise fst4 60 2 (two FST4-60 noise windows: every candidate tried, the worst case)
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"
mkdir -p "$Q/q" "$Q/src-fst4"
cp tools/ft8-bench/qemu/{insncount.c,qrun.sh} "$Q/q/" 2>/dev/null || true; cp tools/ft8-bench/qemu/qrun.sh "$Q/"
[ -f "$Q/q/qemu-plugin.h" ] || curl -sfL -o "$Q/q/qemu-plugin.h" https://raw.githubusercontent.com/qemu/qemu/v7.2.0/include/qemu/qemu-plugin.h
[ -f "$Q/q/insncount.so" ] || docker run --rm -v "$Q:/w" -w /w/q ft8perf-x86 gcc -O2 -shared -fPIC -o insncount.so insncount.c
rm -rf "$Q/src-fst4"; mkdir -p "$Q/src-fst4"
tar -c android/app/src/main/cpp/ft8_lib android/app/src/main/cpp/decoders android/app/src/main/cpp/vibedsp \
  android/app/src/main/cpp/vibe_clock.h android/app/src/main/cpp/vibedsp/vibe_denormals.h tools/fst4-bench 2>/dev/null | tar -x -C "$Q/src-fst4"
cat > "$Q/xfst4.sh" <<'EOS'
set -e
S=/w/src-fst4; CPP=$S/android/app/src/main/cpp
for v in simd scalar; do
  O=/w/arm-fst4-$v; mkdir -p $O/obj; X=""; [ $v = scalar ] && X=-DVIBE_FORCE_SCALAR
  F="-w -Wno-psabi -O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard $X"
  for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
    arm-linux-gnueabihf-gcc $F -c -I $CPP/ft8_lib -o $O/obj/$(basename $f .c).o $f
  done
  arm-linux-gnueabihf-g++ -std=c++17 $F -static -I $CPP -I $CPP/ft8_lib -I $CPP/vibedsp -I $CPP/vibedsp/third_party/kissfft -I $S/tools/fst4-bench -o $O/fst4_bench $S/tools/fst4-bench/fst4_bench.cpp \
    $CPP/decoders/digimodes/fst4_code.cpp $CPP/decoders/digimodes/fst4_decoder.cpp $CPP/decoders/digimodes/ldpc_small.cpp $CPP/vibedsp/ddc.cpp $CPP/vibedsp/resampler.cpp $CPP/decoders/ft8_decoder.cpp $CPP/decoders/ftx_fine.cpp $O/obj/*.o -lpthread
done
EOS
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 15 bash /w/xfst4.sh 2>&1 | grep -E "error" || true
[ $# -gt 0 ] || set -- noise fst4 60 2
for v in simd scalar; do
  docker run --rm -v "$Q:/w" ft8perf-x86 nice -n 15 bash /w/qrun.sh "arm-fst4-$v/fst4_bench" "fst4-$v" "$@"
  tail -2 "$Q/q/fst4-$v.out"
  python3 tools/ft8-bench/qemu/fnsum.py "fst4-$v" 12
done
