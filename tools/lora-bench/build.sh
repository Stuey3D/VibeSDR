#!/usr/bin/env bash
# tools/lora-bench/build.sh OUT [CXX] [CC] [EXTRA_FLAGS] — build lora_rows with exactly test-bench-decoders' deps and flags
# (scripts/run-tests.sh is the one list; this sources its flags_for / deps_for / cobjs_for rather than keeping a copy).
#   Mac:   tools/lora-bench/build.sh /tmp/lora_rows
#   armhf: tools/lora-bench/build.sh /out/lora_rows arm-linux-gnueabihf-g++ arm-linux-gnueabihf-gcc "-marm -mfpu=neon-vfpv4 -mfloat-abi=hard"
set -euo pipefail
cd "$(dirname "$0")/../.."
OUTBIN="$1"; CXX="${2:-c++}"; CCX="${3:-cc}"; EXTRA="${4:-}"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
SRC=vibeserver; VDSP=android/app/src/main/cpp/vibedsp; KISS="$VDSP/third_party/kissfft"
DIGIMODES="$(ls android/app/src/main/cpp/decoders/digimodes/*.cpp 2>/dev/null | tr '\n' ' ')"
FT8C=android/app/src/main/cpp/ft8_lib
eval "$(sed -n '/^flags_for() {/,/^}/p; /^deps_for() {/,/^}/p; /^cobjs_for() {/,/^}/p' scripts/run-tests.sh)"
cc() { command "$CCX" $EXTRA "$@"; }   # ★ cobjs_for compiles its C objects with `cc`
FLAGS="$(flags_for test-bench-decoders)"; DEPS="$(deps_for test-bench-decoders)"; COBJ="$(cobjs_for test-bench-decoders)"
# shellcheck disable=SC2086
# ★★ -DVIBE_DAB_PFFFT, as the server and the app build it: LoRa's capture FFT (vibeism::DetFft) is PFFFT there and the
#    one-line test builds' radix-2 otherwise — 88 % against 57 % of a Pi 2 core on vibe-lora (measured 2026-10-31).
"$CXX" -std=c++17 -O3 -ffp-contract=fast -DVIBE_DAB_PFFFT $EXTRA -I "$SRC" -I android/app/src/main/cpp ${FLAGS/-O2/} -o "$OUTBIN" tools/lora-bench/lora_rows.cpp $DEPS $COBJ -lpthread
echo "built $OUTBIN"
