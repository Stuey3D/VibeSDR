#!/usr/bin/env bash
# build-faad2.sh — FAAD2 2.11.3's DRM build (libfaad_drm.a) for the Android app's on-device server, both ABIs, from the
# pinned upstream tarball (the same version and SHA-256 as vibeserver/linux|mac/build-deps.sh). V12 DRM.
# ★★★ GPL-2.0-or-later. It goes into the Android app (Play + GitHub APK) and VibeServer Lite — never the iOS App Store
#     binary. COPYING goes beside it as sdr-kit/COPYING.faad2 for the app's licence screen.
#   ANDROID_NDK=… android/app/src/main/cpp/sdr-kit/build-faad2.sh
set -euo pipefail
VER=2.11.3
SHA=860ab62087e336c1844a70e33196c1790b525fb9a9e7b6ac4fab1a1a4e4d5ce8
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
[ -d "$NDK" ] || { echo "NDK not found at $NDK — set ANDROID_NDK"; exit 1; }
curl -fsSL "https://github.com/knik0/faad2/archive/refs/tags/$VER.tar.gz" -o "$WORK/faad2.tar.gz"
got="$(shasum -a 256 "$WORK/faad2.tar.gz" | awk '{print $1}')"
[ "$got" = "$SHA" ] || { echo "!! faad2 $VER sha256 $got, want $SHA — refusing"; exit 1; }
mkdir -p "$WORK/src" && tar -xzf "$WORK/faad2.tar.gz" -C "$WORK/src" --strip-components 1
for ABI in arm64-v8a armeabi-v7a; do
  cmake -S "$WORK/src" -B "$WORK/b-$ABI" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DFAAD_APPLY_DRC=OFF >/dev/null
  cmake --build "$WORK/b-$ABI" --target faad_drm -j3 >/dev/null
  mkdir -p "$HERE/$ABI/lib" "$HERE/$ABI/include"
  cp "$WORK/b-$ABI/libfaad_drm.a" "$HERE/$ABI/lib/"
  cp "$WORK/src/include/neaacdec.h" "$HERE/$ABI/include/"
done
cp "$WORK/src/COPYING" "$HERE/COPYING.faad2"
echo "faad2 $VER ($SHA) → sdr-kit/{arm64-v8a,armeabi-v7a}/lib/libfaad_drm.a"
