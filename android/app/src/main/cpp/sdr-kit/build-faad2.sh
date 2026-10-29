#!/usr/bin/env bash
# build-faad2.sh — FAAD2 2.11.3's DRM build (libfaad_drm.a) for the Android app's on-device server, from the pinned
# upstream tarball (the same version and SHA-256 as vibeserver/linux|mac/build-deps.sh) with VibeSDR's patch series
# (vibeserver/linux/faad2-patches/series, the same patches the Linux and Mac servers get). V12 DRM.
# ★★★ GPL-2.0-or-later. It goes into the Android app (Play + GitHub APK) and VibeServer Lite — never the iOS App Store
#     binary. COPYING goes beside it as sdr-kit/COPYING.faad2 for the app's licence screen.
# ★★ THREE OUTPUTS, because there are two sdr-kit roots (CMakeLists.txt links whichever SDR_KIT_ROOT holds the file):
#      sdr-kit/arm64-v8a, sdr-kit/armeabi-v7a   the app (Play + GitHub APK), android-24 like the rest of sdr-kit
#      lite/native/kit/armeabi-v7a               VibeServer Lite, android-21 (lite/native/build-prebuilts-api21.sh says
#                                                why: from API 23 bionic exports `stderr` as a symbol; checked below)
# ★ armeabi-v7a in ARM mode with NEON (the NDK's default is Thumb-2): the same choice as the Linux armhf build, where
#   ARM mode measured faster on a Cortex-A7. arm64-v8a: the NDK's defaults (NEON is baseline there).
# ★ No -funsafe-math-optimizations — the DRM builds on Linux and the Mac do without it (docs/v12/DRM-AUDIO-ROUTES.md).
#   ANDROID_NDK=… android/app/src/main/cpp/sdr-kit/build-faad2.sh
set -euo pipefail
VER=2.11.3
SHA=860ab62087e336c1844a70e33196c1790b525fb9a9e7b6ac4fab1a1a4e4d5ce8
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../../../.." && pwd)"
PATCHES="$ROOT/vibeserver/linux/faad2-patches"
LITE_KIT="$ROOT/lite/native/kit"
JOBS="${JOBS:-2}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
[ -d "$NDK" ] || { echo "NDK not found at $NDK — set ANDROID_NDK"; exit 1; }
[ -f "$PATCHES/series" ] || { echo "!! no patch series at $PATCHES"; exit 1; }
NM="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-nm | head -1)"
curl -fsSL "https://github.com/knik0/faad2/archive/refs/tags/$VER.tar.gz" -o "$WORK/faad2.tar.gz"
got="$(shasum -a 256 "$WORK/faad2.tar.gz" | awk '{print $1}')"
[ "$got" = "$SHA" ] || { echo "!! faad2 $VER sha256 $got, want $SHA — refusing"; exit 1; }
mkdir -p "$WORK/src" && tar -xzf "$WORK/faad2.tar.gz" -C "$WORK/src" --strip-components 1
SERIES=""
while IFS= read -r p; do
  case "$p" in ""|\#*) continue ;; esac
  patch -d "$WORK/src" -p1 -s < "$PATCHES/$p" || { echo "!! FAAD2 patch $p did not apply"; exit 1; }
  SERIES="$SERIES $p"
done < "$PATCHES/series"
echo "==> FAAD2 $VER, patches:$SERIES"

# build <abi> <api> <out dir>
build() {
  local abi=$1 api=$2 out=$3 mode=()
  [ "$abi" = armeabi-v7a ] && mode=(-DANDROID_ARM_MODE=arm -DANDROID_ARM_NEON=ON)
  cmake -S "$WORK/src" -B "$WORK/b-$abi-$api" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$api" ${mode[@]+"${mode[@]}"} -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DFAAD_APPLY_DRC=OFF >"$WORK/log-$abi-$api" 2>&1 \
    && cmake --build "$WORK/b-$abi-$api" --target faad_drm -j"$JOBS" >>"$WORK/log-$abi-$api" 2>&1 \
    || { tail -30 "$WORK/log-$abi-$api"; echo "!! FAAD2 build failed ($abi, android-$api)"; exit 1; }
  local a="$WORK/b-$abi-$api/libfaad_drm.a"
  # ★ THE GATE — the artefact, not the exit code: the DRM entry point, the patched HCR (only DRM reaches it), the ABI,
  #   and (Lite) no reference to the API-23 `stderr` symbol.
  [ "$("$NM" "$a" 2>/dev/null | grep -c 'T NeAACDecInitDRM' || true)" -gt 0 ] || { echo "!! $abi: no NeAACDecInitDRM"; exit 1; }
  [ "$(grep -c VIBE_FAAD_STOCK "$WORK/src/libfaad/hcr.c" || true)" -gt 0 ] || { echo "!! hcr.c is not the patched one"; exit 1; }
  if [ "$api" -lt 23 ]; then
    [ "$("$NM" --undefined-only "$a" 2>/dev/null | grep -c -w stderr || true)" = 0 ] || { echo "!! $abi android-$api wants 'stderr'"; exit 1; }
  fi
  mkdir -p "$out/lib" "$out/include"
  cp "$a" "$out/lib/"
  cp "$WORK/src/include/neaacdec.h" "$out/include/"
  echo "    $abi android-$api → ${out#$ROOT/}/lib/libfaad_drm.a ($(wc -c < "$a" | tr -d ' ') bytes)"
}
build arm64-v8a 24 "$HERE/arm64-v8a"
build armeabi-v7a 24 "$HERE/armeabi-v7a"
build armeabi-v7a 21 "$LITE_KIT/armeabi-v7a"
cp "$WORK/src/COPYING" "$HERE/COPYING.faad2"
{ echo "faad2 $VER sha256 $SHA, DRM build (DRM_SUPPORT), float, GPL-2.0-or-later"
  echo "VibeSDR patches:$SERIES"
  echo "NDK $(basename "$NDK")"; } > "$HERE/FAAD2-BUILT-FROM"
cp "$HERE/FAAD2-BUILT-FROM" "$LITE_KIT/FAAD2-BUILT-FROM"
echo "==> faad2 $VER → sdr-kit/{arm64-v8a,armeabi-v7a} (android-24) + lite/native/kit/armeabi-v7a (android-21)"
