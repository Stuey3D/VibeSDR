#!/usr/bin/env bash
# tools/alert-bench/qemu.sh — EAS / SAME and SELCAL instruction counts on the Pi 2 build (armhf -marm NEON, and
# VIBE_FORCE_SCALAR) under qemu-arm with the FT8 work's TCG counter, as tools/paging-bench/qemu.sh. A count, not cycles:
# the Pi 2 estimate takes 0.6–0.8 instructions per cycle at 900 MHz.   qemu.sh [seconds=60]
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"
mkdir -p "$Q/q"
cp tools/ft8-bench/qemu/qrun.sh "$Q/"
rm -rf "$Q/src-alert"; mkdir -p "$Q/src-alert"
tar -c android/app/src/main/cpp/decoders/same_decoder.* android/app/src/main/cpp/decoders/selcal_decoder.* \
    android/app/src/main/cpp/decoders/digimodes/q65_simd.h tools/alert-bench | tar -x -C "$Q/src-alert"
cat > "$Q/xalert.sh" <<'EOS'
set -e
S=/w/src-alert; CPP=$S/android/app/src/main/cpp; O=/w/arm-alert; mkdir -p $O
for v in neon scalar; do F=""; [ $v = scalar ] && F="-DVIBE_FORCE_SCALAR"
arm-linux-gnueabihf-g++ -std=c++17 -w -O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static $F -I $CPP \
  -o $O/alert_bench_$v $S/tools/alert-bench/alert_bench.cpp $CPP/decoders/same_decoder.cpp $CPP/decoders/selcal_decoder.cpp; done
EOS
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 15 bash /w/xalert.sh 2>&1 | grep -E "error" || true
for d in same selcal; do for v in neon scalar; do
  docker run --rm -v "$Q:/w" ft8perf-x86 nice -n 15 bash /w/qrun.sh "arm-alert/alert_bench_$v" "${d}_$v" $d "${1:-60}"
done; done
