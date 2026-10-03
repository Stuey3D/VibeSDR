#!/usr/bin/env bash
# Cross-compile libopus (static) for the Android ABIs VibeSDR ships, so the ANDROID VibeServer can
# encode Opus audio at parity with the macOS build.
#
# ★ Why this exists: Android has NO libopus in-tree — UberSDR *decode* on Android is ExoPlayer, not
# libopus, so there was nothing to reuse for *encoding*. This vendors a decode+encode static lib per
# ABI, which the cpp CMake links behind VIBE_HAVE_OPUS (same flag the macOS core uses).
#
# Output: cpp/opus/include/opus/*.h  and  cpp/opus/lib/<ABI>/libopus.a
# Run once (and after an NDK/opus version bump). Idempotent.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CPP="$(cd "$HERE/.." && pwd)"                 # android/app/src/main/cpp
OUT="$CPP/opus"
VER="1.5.2"
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"   # opus-1.5.2.tar.gz
ABIS=("arm64-v8a" "armeabi-v7a")
API=24                                         # minSdk 24 (see memory: minSdk has ALWAYS been 24)

NDK="${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || { echo "!! NDK toolchain not found at $TOOLCHAIN — set ANDROID_NDK_HOME"; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
echo "==> Fetching opus $VER source"
# ★★ PINNED SHA-256 (audit 2026-10-03). The tarball was `curl -sL` with no -f and no check, so an
#    error page or a swapped file went straight into a library we ship. Value = xiph's published
#    SHA256SUMS for opus-1.5.2.tar.gz (also Homebrew's pin for 1.5.2) — written in offline, so if it
#    ever refuses, compare the "got" hash with https://downloads.xiph.org/releases/opus/SHA256SUMS.txt
#    before changing it. Same pattern as vibeserver/mac/build-deps.sh fetch().
curl -fsSL --retry 3 "https://downloads.xiph.org/releases/opus/opus-$VER.tar.gz" -o "$WORK/opus.tgz"
GOT="$(shasum -a 256 "$WORK/opus.tgz" | awk '{print $1}')"
[ "$GOT" = "$OPUS_SHA256" ] || { echo "!! SHA-256 MISMATCH for opus-$VER.tar.gz: got $GOT, pinned $OPUS_SHA256 — refusing"; exit 1; }
tar -xzf "$WORK/opus.tgz" -C "$WORK"
SRC="$WORK/opus-$VER"

for ABI in "${ABIS[@]}"; do
  echo "==> Building libopus for $ABI"
  B="$WORK/build-$ABI"; mkdir -p "$B"
  cmake -S "$SRC" -B "$B" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DOPUS_BUILD_SHARED_LIBRARY=OFF \
    -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF >/dev/null
  cmake --build "$B" --target opus -j >/dev/null
  mkdir -p "$OUT/lib/$ABI"
  cp "$B/libopus.a" "$OUT/lib/$ABI/libopus.a"
  echo "    -> $OUT/lib/$ABI/libopus.a ($(du -h "$OUT/lib/$ABI/libopus.a" | cut -f1))"
done

echo "==> Copying headers"
rm -rf "$OUT/include"; mkdir -p "$OUT/include/opus"
cp "$SRC/include/"*.h "$OUT/include/opus/"
echo "==> DONE. Vendored libopus for: ${ABIS[*]}"
