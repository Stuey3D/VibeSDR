#!/usr/bin/env bash
# build-codec2.sh — Codec 2 (libcodec2.so, David Rowe et al., LGPL-2.1) for Android, from the pinned upstream release
# tarball (github.com/drowe67/codec2). V12: FreeDV 700D/700E/1600 and M17 voice on the Android app and VibeServer Lite.
# ★★★ A SHARED LIBRARY, LOADED AT RUN TIME — the same arrangement as on Linux and the Mac. vibe_freedv.h dlopens
#     "libcodec2.so" from the APK's own native-library directory and finds the FreeDV + Codec 2 API with dlsym; nothing
#     of codec2 is compiled or linked into libvibeserver. LGPL-2.1 is satisfied by shipping it as a separate,
#     replaceable .so with its source and licence named (COPYING.codec2 beside it, the app's licence list, README).
# ★★ THREE OUTPUTS, as build-faad2.sh (two sdr-kit roots):
#      sdr-kit/arm64-v8a, sdr-kit/armeabi-v7a   the app (Play + GitHub APK), android-24 like the rest of sdr-kit
#      lite/native/kit/armeabi-v7a               VibeServer Lite, android-21 — codec2 writes to stderr in ~600 places,
#                                                and from API 23 bionic exports `stderr` as a symbol (checked below)
# ★★ 16 KB PAGES (Google Play's rule for Android 15+ devices): -Wl,-z,max-page-size=16384, checked on the artefact.
# ★ SONAME is the plain "libcodec2.so" (CMake's Android platform drops the version from the soname) — checked below;
#   NEEDED may only name what every Android has (libc, libm, libdl, liblog).
# ★ Only the library target: no demos, no unit tests, no LPCNet (FreeDV 2020 is not offered). armeabi-v7a in ARM mode
#   with NEON, as FAAD2.
#   ANDROID_NDK=… android/app/src/main/cpp/sdr-kit/build-codec2.sh
set -euo pipefail
VER=1.2.0
SHA=cbccae52b2c2ecc5d2757e407da567eb681241ff8dadce39d779a7219dbcf449   # github.com/drowe67/codec2/archive/refs/tags/1.2.0.tar.gz
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../../../.." && pwd)"
LITE_KIT="$ROOT/lite/native/kit"
JOBS="${JOBS:-2}"
# ★ The SDK's cmake 3.22.1 (build-librtlsdr.sh says why: Homebrew's 4.x refuses old cmake_minimum_required).
CMAKE="${CMAKE:-$(ls -d "$HOME"/Library/Android/sdk/cmake/3.22.1/bin/cmake 2>/dev/null || command -v cmake)}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
[ -d "$NDK" ] || { echo "NDK not found at $NDK — set ANDROID_NDK"; exit 1; }
BIN="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)"
curl -fsSL "https://github.com/drowe67/codec2/archive/refs/tags/$VER.tar.gz" -o "$WORK/codec2.tar.gz"
got="$(shasum -a 256 "$WORK/codec2.tar.gz" | awk '{print $1}')"
[ "$got" = "$SHA" ] || { echo "!! codec2 $VER sha256 $got, want $SHA — refusing"; exit 1; }
mkdir -p "$WORK/src" && tar -xzf "$WORK/codec2.tar.gz" -C "$WORK/src" --strip-components 1
echo "==> codec2 $VER ($CMAKE)"

# ★ What vibe_freedv.h's loaders dlsym — every one must be exported, or the mode would silently not be offered.
NEED="freedv_open freedv_close freedv_rx freedv_nin freedv_tx freedv_get_n_max_speech_samples
      freedv_get_n_max_modem_samples freedv_get_n_speech_samples freedv_get_n_nom_modem_samples
      freedv_get_speech_sample_rate freedv_get_modem_sample_rate freedv_get_modem_stats freedv_get_rx_status
      freedv_set_squelch_en freedv_set_callback_txt freedv_get_version freedv_comptx
      codec2_create codec2_destroy codec2_encode codec2_decode codec2_samples_per_frame codec2_bits_per_frame"

# ★★ ANDROID 5 (Lite, android-21) HAS NO C99 COMPLEX MATHS IN libm: bionic's crealf/cimagf/conjf/conj/cabsf/cargf/cexp
#    arrive at API 23, and codec2's modems use them. For the API-21 build only, they are supplied here as static inline
#    functions forced into every file — the compiler builtins for the trivial ones, and the textbook definitions
#    (|z| = hypot, arg z = atan2, e^z = e^re (cos im + i sin im)) on functions libm has had since API 1.
cat > "$WORK/api21_complex.h" <<'H'
#pragma once
#include <complex.h>
#include <math.h>
static inline float crealf(float _Complex z) { return __builtin_crealf(z); }
static inline float cimagf(float _Complex z) { return __builtin_cimagf(z); }
static inline float _Complex conjf(float _Complex z) { return __builtin_conjf(z); }
static inline double _Complex conj(double _Complex z) { return __builtin_conj(z); }
static inline float cabsf(float _Complex z) { return hypotf(__builtin_crealf(z), __builtin_cimagf(z)); }
static inline float cargf(float _Complex z) { return atan2f(__builtin_cimagf(z), __builtin_crealf(z)); }
static inline double _Complex cexp(double _Complex z) {
  const double m = exp(__builtin_creal(z)), a = __builtin_cimag(z);
  return __builtin_complex(m * cos(a), m * sin(a));
}
H

# build <abi> <api> <out dir>
build() {
  local abi=$1 api=$2 out=$3 mode=() b="$WORK/b-$1-$2" cflags="-ffunction-sections -fdata-sections"
  [ "$abi" = armeabi-v7a ] && mode=(-DANDROID_ARM_MODE=arm -DANDROID_ARM_NEON=ON)
  [ "$api" -lt 23 ] && cflags="$cflags -include $WORK/api21_complex.h"
  "$CMAKE" -S "$WORK/src" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$api" ${mode[@]+"${mode[@]}"} -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON -DUNITTEST=OFF -DLPCNET=OFF \
        -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--build-id=sha1" \
        -DCMAKE_C_FLAGS="$cflags" >"$WORK/log-$abi-$api" 2>&1 \
    && "$CMAKE" --build "$b" --target codec2 -j"$JOBS" >>"$WORK/log-$abi-$api" 2>&1 \
    || { tail -30 "$WORK/log-$abi-$api"; echo "!! codec2 build failed ($abi, android-$api)"; exit 1; }
  local so; so="$(ls "$b"/src/libcodec2.so* | head -1)"
  cp -L "$so" "$WORK/libcodec2.so"
  "$BIN/llvm-strip" --strip-unneeded "$WORK/libcodec2.so"
  local f="$WORK/libcodec2.so"
  # ★ THE GATE — the artefact, not the exit code.
  local soname; soname="$("$BIN/llvm-readelf" -d "$f" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
  [ "$soname" = libcodec2.so ] || { echo "!! $abi: SONAME '$soname', want libcodec2.so"; exit 1; }
  local needed; needed="$("$BIN/llvm-readelf" -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
  for n in $needed; do
    case "$n" in libc.so|libm.so|libdl.so|liblog.so) ;; *) echo "!! $abi: NEEDED $n — not on every Android"; exit 1 ;; esac
  done
  local align; align="$("$BIN/llvm-readelf" -lW "$f" | awk '$1=="LOAD"{print $NF}' | sort -u | tr '\n' ' ')"
  for a in $align; do [ "$((a))" -ge 16384 ] || { echo "!! $abi: LOAD alignment $a < 16 KB"; exit 1; }; done
  for s in $NEED; do
    "$BIN/llvm-nm" -D --defined-only "$f" | awk '{print $NF}' | grep -qx "$s" || { echo "!! $abi: $s not exported"; exit 1; }
  done
  if [ "$api" -lt 23 ]; then
    # ★ nothing Android 5 lacks: not `stderr`, not a complex-maths function
    local bad; bad="$("$BIN/llvm-nm" -D --undefined-only "$f" | awk '{print $NF}' | grep -xE 'stderr|stdout|c(real|imag|onj|abs|arg|exp)f?' || true)"
    [ -z "$bad" ] || { echo "!! $abi android-$api wants: $bad"; exit 1; }
  fi
  mkdir -p "$out/lib"
  cp "$f" "$out/lib/libcodec2.so"
  echo "    $abi android-$api → ${out#$ROOT/}/lib/libcodec2.so ($(wc -c < "$f" | tr -d ' ') bytes; SONAME $soname; NEEDED $needed; LOAD align $align)"
}
build arm64-v8a 24 "$HERE/arm64-v8a"
build armeabi-v7a 24 "$HERE/armeabi-v7a"
build armeabi-v7a 21 "$LITE_KIT/armeabi-v7a"
cp "$WORK/src/COPYING" "$HERE/COPYING.codec2"
{ echo "codec2 $VER sha256 $SHA (github.com/drowe67/codec2, tag $VER), LGPL-2.1"
  echo "shared library libcodec2.so, Release, no LPCNet, stripped, 16 KB pages; loaded at run time by vibe_freedv.h"
  echo "NDK $(basename "$NDK")"; } > "$HERE/CODEC2-BUILT-FROM"
cp "$HERE/CODEC2-BUILT-FROM" "$LITE_KIT/CODEC2-BUILT-FROM"
cp "$HERE/COPYING.codec2" "$LITE_KIT/COPYING.codec2"
echo "==> codec2 $VER → sdr-kit/{arm64-v8a,armeabi-v7a} (android-24) + lite/native/kit/armeabi-v7a (android-21)"
