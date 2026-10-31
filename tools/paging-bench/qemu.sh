#!/usr/bin/env bash
# tools/paging-bench/qemu.sh — the paging engine's instruction count on the Pi 2 build (armhf, -marm, NEON flags) under
# qemu-arm with the FT8 work's TCG counter (tools/ft8-bench/qemu), as tools/msk144-bench/qemu.sh. A count, not cycles:
# the Pi 2 estimate takes 0.6–0.8 instructions per cycle at 900 MHz. The engine has no SIMD path (see the header), so
# there is one build.   qemu.sh [seconds=60] [rate=12000]
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"
mkdir -p "$Q/q" "$Q/src-paging"
cp tools/ft8-bench/qemu/qrun.sh "$Q/"
rm -rf "$Q/src-paging"; mkdir -p "$Q/src-paging"
tar -c android/app/src/main/cpp/decoders/paging_decoder.h android/app/src/main/cpp/decoders/paging_decoder.cpp tools/paging-bench | tar -x -C "$Q/src-paging"
cat > "$Q/xpaging.sh" <<'EOS'
set -e
S=/w/src-paging; CPP=$S/android/app/src/main/cpp; O=/w/arm-paging; mkdir -p $O
arm-linux-gnueabihf-g++ -std=c++17 -w -O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static -I $CPP \
  -o $O/paging_bench $S/tools/paging-bench/paging_bench.cpp $CPP/decoders/paging_decoder.cpp
EOS
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 15 bash /w/xpaging.sh 2>&1 | grep -E "error" || true
[ $# -gt 0 ] || set -- 60 12000
docker run --rm -v "$Q:/w" ft8perf-x86 nice -n 15 bash /w/qrun.sh "arm-paging/paging_bench" "paging" "$@"
tail -2 "$Q/q/paging.out"
