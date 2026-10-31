#!/usr/bin/env bash
# tools/hell-bench/qemu.sh — FELD HELL on the Pi 2 build (armhf -marm NEON, and VIBE_FORCE_SCALAR + PFFFT_SIMD_DISABLE) under
# qemu-arm with the FT8 work's TCG counter, as tools/textmode-bench/qemu.sh: 60 s of a keyed signal at 0 dB, drawing all the
# time (the worst case). A count, not cycles: the Pi 2 estimate takes 0.6–0.8 instructions per cycle at 900 MHz. Also runs
# test-feld-hell under qemu-arm (NEON and scalar × -ffp-contract off and the Android CMake's fast).
#   qemu.sh [seconds=60]
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"
mkdir -p "$Q/q"
cp tools/ft8-bench/qemu/qrun.sh "$Q/"
rm -rf "$Q/src-hell"; mkdir -p "$Q/src-hell"
tar -c android/app/src/main/cpp/decoders/feld_hell.* android/app/src/main/cpp/decoders/digimodes/q65_simd.h \
    android/app/src/main/cpp/vibedsp/third_party/pffft tools/hell-bench vibeserver/test-feld-hell.cpp | tar -x -C "$Q/src-hell"
cat > "$Q/xhell.sh" <<'EOS'
set -e
S=/w/src-hell; CPP=$S/android/app/src/main/cpp; O=/w/arm-hell; mkdir -p $O
for fc in off fast; do
A="-O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -ffp-contract=$fc"
for v in neon scalar; do
  F=""; P=""; [ $v = scalar ] && F="-DVIBE_FORCE_SCALAR" && P="-DPFFFT_SIMD_DISABLE"
  arm-linux-gnueabihf-gcc $A $P -c -o $O/pffft_${v}_$fc.o $CPP/vibedsp/third_party/pffft/pffft.c
  [ $fc = fast ] && cp $O/pffft_${v}_$fc.o $O/pffft_$v.o
  [ $fc = fast ] && arm-linux-gnueabihf-g++ -std=c++17 -w $A $F -static -I $CPP -o $O/hell_file_$v $S/tools/hell-bench/hell_file.cpp $CPP/decoders/feld_hell.cpp $O/pffft_$v.o
  arm-linux-gnueabihf-g++ -std=c++17 -w $A $F -static -I $CPP -o $O/test_hell_${v}_$fc $S/vibeserver/test-feld-hell.cpp $CPP/decoders/feld_hell.cpp $O/pffft_${v}_$fc.o
done; done
EOS
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 15 bash /w/xhell.sh 2>&1 | grep -E "error" || true
for v in neon scalar; do
  docker run --rm -v "$Q:/w" ft8perf-x86 nice -n 15 bash /w/qrun.sh "arm-hell/hell_file_$v" "hell_$v" --cost "${1:-60}"
done
for v in neon scalar; do for fc in off fast; do
  docker run --rm -v "$Q:/w" -e VIBE_HELL_NOISE_MIN=1 ft8perf-x86 nice -n 15 bash -c "cd /w && qemu-arm arm-hell/test_hell_${v}_$fc > q/test_hell_${v}_$fc.out; grep -E 'fingerprint|passed|FAILED' q/test_hell_${v}_$fc.out | tr '\n' ' '; echo ${v}_$fc"
done; done
