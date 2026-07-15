#!/bin/bash
# Cross-compile libopus for watchOS.
#
# UberSDR sends Opus, and Apple ships no Opus decoder — so a watch app cannot play a single
# sample without this. The existing ios/VibeSDR/opus/libopus.a is iOS arm64 ONLY, which is
# why the spike could not simply reuse it.
#
# TWO SLICES, because watchOS is two architectures:
#   arm64_32 — Series 4–8 (32-bit POINTERS on 64-bit ARM; an ILP32 ABI, not a 32-bit CPU)
#   arm64    — Series 9 onwards
# JR TARGETS SERIES 9+, so this builds arm64 ONLY. That is a product decision with a real
# engineering dividend: it drops the ILP32 slice (which is the awkward one to cross-compile),
# and it means the smallest screen to design for is 42mm rather than 41mm.
set -e

SRC="$(cd "$(dirname "$0")" && pwd)/opus-1.5.2"
OUT="$(cd "$(dirname "$0")" && pwd)/opus-watchos"
SDK=$(xcrun --sdk watchos --show-sdk-path)
MINVER=26.0

rm -rf "$OUT"; mkdir -p "$OUT"

build_arch () {
  ARCH=$1
  HOST=$2
  BDIR="$OUT/build-$ARCH"
  rm -rf "$BDIR"; mkdir -p "$BDIR"
  cd "$BDIR"

  # -mwatchos-version-min is what actually stamps the binary as watchOS; without it the
  # linker later refuses the slice ("building for watchOS but linking iOS").
  export CC="$(xcrun --sdk watchos -f clang)"
  export CFLAGS="-arch $ARCH -isysroot $SDK -mwatchos-version-min=$MINVER -O2"
  export LDFLAGS="-arch $ARCH -isysroot $SDK -mwatchos-version-min=$MINVER"

  # --host forces cross-compile mode so configure stops trying to RUN its test binaries
  # (it cannot: they are watch binaries on a Mac).
  # Everything not needed for DECODING is switched off — this is a decoder, and every
  # kilobyte and every cycle on a watch is worth having back.
  "$SRC/configure" \
    --host="$HOST" \
    --prefix="$BDIR/install" \
    --disable-shared --enable-static \
    --disable-doc --disable-extra-programs \
    --disable-rtcd \
    --enable-fixed-point \
    >/dev/null

  make -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
  make install >/dev/null 2>&1
  echo "  built $ARCH"
}

echo "--- building libopus for watchOS (SDK: $(basename "$SDK")) ---"
build_arch arm64    aarch64-apple-darwin

echo "--- collecting ---"
cd "$OUT"
cp "build-arm64/install/lib/libopus.a" "libopus.a"

mkdir -p include
cp -R "build-arm64/install/include/opus" include/

echo
echo "RESULT:"
lipo -info libopus.a
ls -la libopus.a
echo "headers: $(ls include/opus | tr '\n' ' ')"
