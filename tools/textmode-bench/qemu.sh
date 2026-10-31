#!/usr/bin/env bash
# tools/textmode-bench/qemu.sh — the text-mode decoders (MFSK, OLIVIA/CONTESTIA, THOR) on the Pi 2 build (armhf -marm NEON,
# and VIBE_FORCE_SCALAR + PFFFT_SIMD_DISABLE) under qemu-arm with the FT8 work's TCG counter, as tools/alert-bench/qemu.sh:
# 60 s of noise through each family's AUTO (every common sub-mode running — the worst case). A count, not cycles: the
# Pi 2 estimate takes 0.6–0.8 instructions per cycle at 900 MHz. Also runs test-textmodes under qemu-arm (NEON and scalar,
# -ffp-contract=off and the Android CMake's fast) so the decoded output can be compared with the Mac's.
#   qemu.sh [seconds=60]
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"
mkdir -p "$Q/q"
cp tools/ft8-bench/qemu/qrun.sh "$Q/"
rm -rf "$Q/src-tm"; mkdir -p "$Q/src-tm"
tar -c android/app/src/main/cpp/decoders/textmodes.* android/app/src/main/cpp/decoders/mfsk_varicode.h \
    android/app/src/main/cpp/decoders/digimodes/q65_simd.h android/app/src/main/cpp/vibedsp/third_party/pffft \
    tools/textmode-bench vibeserver/test-textmodes.cpp | tar -x -C "$Q/src-tm"
cat > "$Q/xtm.sh" <<'EOS'
set -e
S=/w/src-tm; CPP=$S/android/app/src/main/cpp; O=/w/arm-tm; mkdir -p $O
A="-O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
for v in neon scalar; do for fc in off fast; do
  F="-ffp-contract=$fc"; P=""; [ $v = scalar ] && F="$F -DVIBE_FORCE_SCALAR" && P="-DPFFFT_SIMD_DISABLE"
  arm-linux-gnueabihf-gcc $A -ffp-contract=$fc $P -c -o $O/pffft_${v}_$fc.o $CPP/vibedsp/third_party/pffft/pffft.c
  arm-linux-gnueabihf-g++ -std=c++17 -w $A $F -static -I $CPP -o $O/tm_bench_${v}_$fc $S/tools/textmode-bench/tm_bench.cpp $CPP/decoders/textmodes.cpp $O/pffft_${v}_$fc.o
  arm-linux-gnueabihf-g++ -std=c++17 -w $A $F -static -I $CPP -o $O/test_tm_${v}_$fc $S/vibeserver/test-textmodes.cpp $CPP/decoders/textmodes.cpp $O/pffft_${v}_$fc.o
done; done
EOS
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 15 bash /w/xtm.sh 2>&1 | grep -E "error" || true
for fam in mfsk olivia thor; do for v in neon scalar; do
  docker run --rm -v "$Q:/w" ft8perf-x86 nice -n 15 bash /w/qrun.sh "arm-tm/tm_bench_${v}_fast" "tm_${fam}_$v" cost $fam "${1:-60}"
done; done
for v in neon scalar; do for fc in off fast; do
  docker run --rm -v "$Q:/w" -e VIBE_TM_NOISE_MIN=1 ft8perf-x86 nice -n 15 bash -c "cd /w && qemu-arm arm-tm/test_tm_${v}_$fc > q/test_tm_${v}_$fc.out; grep -E 'fingerprint|passed|FAILED' q/test_tm_${v}_$fc.out | tr '\n' ' '; echo ${v}_$fc"
done; done
