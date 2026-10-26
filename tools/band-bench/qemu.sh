#!/usr/bin/env bash
# qemu.sh WAV START_UTC CENTRE [band_bench options…] — Band spots' INSTRUCTION COUNT on the Pi 2's build (armhf, Cortex-A7:
# -O3 -marm -mfpu=neon-vfpv4, the server's flags), under qemu-arm with tools/ft8-bench/qemu/insncount.c (a TCG plugin
# that counts every guest instruction). Pass `drain` so every slot is decoded however slow the emulation is.
# ★ An instruction count is not a cycle count (an A7 dual-issues simple ops, takes two cycles per 128-bit NEON op and
#   ~15 for a VFP divide) — docs/v12/DIGITAL-SPOTS.md says how it is turned into "% of a Pi 2 core", and that it is an
#   estimate. Work dir $BANDQ (default ~/Library/Caches/bandperf, under $HOME so the Docker VM can mount it).
#   The images: vibeserver-cross:bookworm-armhf (the server's own cross compiler) and bandperf-arm64 (qemu-user + gcc,
#   native arm64 — no x86 emulation underneath). Remove with `docker rmi bandperf-arm64` and `rm -rf $BANDQ` after.
set -euo pipefail
cd "$(dirname "$0")/../.."
Q="${BANDQ:-$HOME/Library/Caches/bandperf}"
mkdir -p "$Q/q" "$Q/audio"
cp tools/ft8-bench/qemu/insncount.c "$Q/q/"
[ -f "$Q/q/qemu-plugin.h" ] || curl -sfL -o "$Q/q/qemu-plugin.h" https://raw.githubusercontent.com/qemu/qemu/v7.2.0/include/qemu/qemu-plugin.h
if ! docker image inspect bandperf-arm64 >/dev/null 2>&1; then
  printf 'FROM debian:bookworm-slim\nRUN apt-get update -qq && apt-get install -y -qq --no-install-recommends qemu-user gcc libc6-dev libglib2.0-dev binutils-arm-linux-gnueabihf && rm -rf /var/lib/apt/lists/*\n' \
    | nice -n 10 docker build -q -t bandperf-arm64 -
fi
docker run --rm -v "$Q:/w" -w /w/q bandperf-arm64 gcc -O2 -shared -fPIC -I/usr/include/glib-2.0 -I/usr/lib/aarch64-linux-gnu/glib-2.0/include -o insncount.so insncount.c
S="$Q/src"; rm -rf "$S"; mkdir -p "$S"
tar -cf - android/app/src/main/cpp/ft8_lib android/app/src/main/cpp/decoders android/app/src/main/cpp/vibe_clock.h \
  android/app/src/main/cpp/vibedsp tools/band-bench | tar -x -C "$S"
cat > "$Q/xb.sh" <<'XB'
set -e
C=/w/src/android/app/src/main/cpp; O=/w/arm; mkdir -p $O/obj
F="-O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
for f in $C/ft8_lib/ft8/*.c $C/ft8_lib/fft/kiss_fft.c $C/ft8_lib/fft/kiss_fftr.c $C/ft8_lib/common/monitor.c $C/vibedsp/third_party/pffft/pffft.c; do
  arm-linux-gnueabihf-gcc $F -c -I $C/ft8_lib -o $O/obj/$(basename $f .c).o $f
done
arm-linux-gnueabihf-g++ -std=c++17 $F -static -I $C -I $C/ft8_lib -I $C/vibedsp -o $O/band_bench /w/src/tools/band-bench/band_bench.cpp \
  $C/decoders/digi_modes.cpp $C/decoders/digi_tuner.cpp $C/decoders/digi_spotter.cpp $C/decoders/ft8_decoder.cpp \
  $C/decoders/jtty_decoder.cpp $C/decoders/psk_skimmer.cpp $C/decoders/cw_skimmer.cpp $C/decoders/digimodes/*.cpp \
  $C/vibedsp/ddc.cpp $O/obj/*.o -lpthread
ls -la $O/band_bench
XB
[ -x "$Q/arm/band_bench" ] && [ -z "${REBUILD:-}" ] || docker run --rm -v "$Q:/w" vibeserver-cross:bookworm-armhf nice -n 10 bash /w/xb.sh
[ $# -ge 3 ] || { echo "built $Q/arm/band_bench; give WAV START_UTC CENTRE [options] to count"; exit 0; }
WAV="$1"; START="$2"; CENTRE="$3"; shift 3
[ -f "$Q/audio/$(basename "$WAV")" ] || cp "$WAV" "$Q/audio/"
TAG="${TAG:-run}"
docker run --rm -v "$Q:/w" -w /w bandperf-arm64 bash -c "INSN_OUT=/w/q/$TAG.insn nice -n 10 qemu-arm -plugin /w/q/insncount.so /w/arm/band_bench /w/audio/$(basename "$WAV") $START $CENTRE $* > /w/q/$TAG.out 2>&1; cat /w/q/$TAG.out; grep TOTAL /w/q/$TAG.insn"
