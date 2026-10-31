#!/usr/bin/env bash
# tools/msk144-bench/qemu.sh — MSK144's instruction count on the Pi 2 build (armhf, -marm NEON), and the scalar build, under
# qemu-arm with the FT8 work's TCG counter (tools/ft8-bench/qemu). Uses the WORKING TREE. A count, not a cycle count:
# the Pi 2 estimate takes 0.6–0.8 instructions per cycle at 900 MHz (docs/v12/DIGITAL-SPOTS.md's convention).
#   qemu.sh [msk144_bench args…]      default: cpu 60 (a minute of noise through the whole receiver)
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"
mkdir -p "$Q/q" "$Q/src-msk144"
cp tools/ft8-bench/qemu/{insncount.c,qrun.sh} "$Q/q/" 2>/dev/null || true; cp tools/ft8-bench/qemu/qrun.sh "$Q/"
[ -f "$Q/q/qemu-plugin.h" ] || curl -sfL -o "$Q/q/qemu-plugin.h" https://raw.githubusercontent.com/qemu/qemu/v7.2.0/include/qemu/qemu-plugin.h
[ -f "$Q/q/insncount.so" ] || docker run --rm -v "$Q:/w" -w /w/q ft8perf-x86 gcc -O2 -shared -fPIC -o insncount.so insncount.c
rm -rf "$Q/src-msk144"; mkdir -p "$Q/src-msk144"
tar -c android/app/src/main/cpp/ft8_lib android/app/src/main/cpp/decoders android/app/src/main/cpp/vibedsp \
  android/app/src/main/cpp/vibe_clock.h android/app/src/main/cpp/vibedsp/vibe_denormals.h tools/msk144-bench 2>/dev/null | tar -x -C "$Q/src-msk144"
cat > "$Q/xmsk144.sh" <<'EOS'
set -e
S=/w/src-msk144; CPP=$S/android/app/src/main/cpp
for v in simd scalar; do
  O=/w/arm-msk144-$v; mkdir -p $O/obj; X=""; [ $v = scalar ] && X=-DVIBE_FORCE_SCALAR
  F="-w -Wno-psabi -O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard $X"
  for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
    arm-linux-gnueabihf-gcc $F -c -I $CPP/ft8_lib -o $O/obj/$(basename $f .c).o $f
  done
  arm-linux-gnueabihf-g++ -std=c++17 $F -static -I $CPP -I $CPP/ft8_lib -I $CPP/vibedsp -I $S/tools/msk144-bench -o $O/msk144_bench $S/tools/msk144-bench/msk144_bench.cpp \
    $CPP/decoders/digimodes/msk144_code.cpp $CPP/decoders/digimodes/msk144_decoder.cpp $CPP/decoders/digimodes/ldpc_small.cpp $CPP/vibedsp/ddc.cpp $CPP/decoders/ft8_decoder.cpp $CPP/decoders/ftx_fine.cpp $O/obj/*.o -lpthread
done
EOS
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 15 bash /w/xmsk144.sh 2>&1 | grep -E "error" || true
[ $# -gt 0 ] || set -- cpu 60
for v in simd scalar; do
  docker run --rm -v "$Q:/w" ft8perf-x86 nice -n 15 bash /w/qrun.sh "arm-msk144-$v/msk144_bench" "msk144-$v" "$@"
  tail -2 "$Q/q/msk144-$v.out"
  python3 tools/ft8-bench/qemu/fnsum.py "msk144-$v" 12
done
