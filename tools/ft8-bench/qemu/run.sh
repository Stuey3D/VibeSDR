#!/bin/bash
# run.sh [REV] — the FT8 decoder's instruction count on the Pi 2 build (armhf), under qemu-arm, by function.
#
# ★★ WHY (2026-10-25, the FT8 CPU work). There is no Pi 2 on the bench to profile, and Apple Silicon cannot run
#    AArch32 — but qemu-arm can, and a TCG plugin can count every guest instruction it executes. So the Pi 2 build
#    (static, the server's Release -O3 + -marm -mfpu=neon-vfpv4, cross-compiled in vibeserver-cross:bookworm-armhf)
#    runs ft8_wav over a real recording here, and each translation block's count is mapped to its function
#    (fnsum.py) or source line (lines.sh, with a -g build). An instruction count is NOT a cycle count — a Cortex-A7
#    takes two cycles for a 128-bit NEON op and ~15-18 for a VFP divide — but it finds the hot spots and it counts
#    the work, with no noise.
#  Work dir: $FT8Q (default ~/Library/Caches/ft8perf — under $HOME so Colima can mount it). qemu-plugin.h is
#  fetched from QEMU v7.2.0 (the version bookworm's qemu-user is) for the plugin build; insncount.c is ours.
#  Usage: run.sh [git rev, default HEAD] WAV START_UTC [ft8_wav options...]
set -euo pipefail
cd "$(dirname "$0")/../../.."
Q="${FT8Q:-$HOME/Library/Caches/ft8perf}"; REV="${1:-HEAD}"; shift || true
mkdir -p "$Q/q" "$Q/audio"
cp tools/ft8-bench/qemu/{insncount.c,xbuild.sh,xbuild86.sh,qrun.sh,lines.sh} "$Q/"; mv "$Q/insncount.c" "$Q/q/"
[ -f "$Q/q/qemu-plugin.h" ] || curl -sfL -o "$Q/q/qemu-plugin.h" https://raw.githubusercontent.com/qemu/qemu/v7.2.0/include/qemu/qemu-plugin.h
docker image inspect ft8perf-x86 >/dev/null 2>&1 || nice -n 10 docker build -q -t ft8perf-x86 tools/ft8-bench/qemu
docker run --rm -v "$Q:/w" -w /w/q ft8perf-x86 gcc -O2 -shared -fPIC -o insncount.so insncount.c
S="$Q/src-$REV"; rm -rf "$S"; mkdir -p "$S"
git archive "$REV" android/app/src/main/cpp/ft8_lib android/app/src/main/cpp/decoders android/app/src/main/cpp/vibe_clock.h \
  android/app/src/main/cpp/vibedsp/vibe_denormals.h android/app/src/main/cpp/vibedsp/third_party/pffft tools/ft8-bench | tar -x -C "$S"
docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 10 bash /w/xbuild.sh "src-$REV" "arm-$REV"
[ $# -ge 2 ] || { echo "built $Q/arm-$REV/ft8_wav; give WAV START_UTC [options] to count"; exit 0; }
WAV="$1"; shift; cp "$WAV" "$Q/audio/"
docker run --rm -v "$Q:/w" ft8perf-x86 bash /w/qrun.sh "arm-$REV/ft8_wav" "$REV" "audio/$(basename "$WAV")" "$@"
python3 tools/ft8-bench/qemu/fnsum.py "$REV" 20
