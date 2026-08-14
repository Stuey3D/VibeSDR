#!/usr/bin/env bash
# Rebuild the vendored librtlsdr.so for both Android ABIs.
#
# ★★★ THIS SCRIPT EXISTS BECAUSE THE .so HAD NO PROVENANCE. Two binaries sat in the tree with
#     nothing recording which librtlsdr they came from or how to make another — so "add support for
#     a new dongle" was an unanswerable question, and any attempt would have been a guess with a
#     working radio at stake.
#
# ★★★ AND THE TRAP THAT NEARLY SHIPPED: upstream librtlsdr HAS NO rtlsdr_open_sys_dev(). An
#     unrooted Android process cannot enumerate USB — the Java layer opens the device and passes a
#     FILE DESCRIPTOR down — so that function is the only way a phone ever reaches a dongle, and
#     local_sdr_shim.cpp calls it. A clean upstream build compiles, links, and cannot open a radio
#     at all. librtlsdr-android.patch re-adds it, sharing the tail of rtlsdr_open() rather than
#     duplicating 150 lines that would drift.
#
# Usage:  sdr-kit/build-librtlsdr.sh [tag]        (default: the release below)
set -euo pipefail
TAG="${1:-797f814}"                     # osmocom/rtl-sdr — Release 2.0.3 (has RTL-SDR Blog V4L)
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"

[ -d "$NDK" ] || { echo "NDK not found at $NDK — set ANDROID_NDK"; exit 1; }
git clone --quiet https://github.com/osmocom/rtl-sdr.git "$WORK/src"
git -C "$WORK/src" checkout --quiet "$TAG"
git -C "$WORK/src" apply "$HERE/librtlsdr-android.patch"

for ABI in arm64-v8a armeabi-v7a; do
  cmake -S "$WORK/src" -B "$WORK/$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release \
    -DLIBUSB_INCLUDE_DIRS="$HERE/$ABI/include" \
    -DLIBUSB_LIBRARIES="$HERE/$ABI/lib/libusb1.0.so" \
    -DDETACH_KERNEL_DRIVER=OFF -DENABLE_ZEROCOPY=OFF >/dev/null
  cmake --build "$WORK/$ABI" --target rtlsdr -j8 >/dev/null

  SO="$WORK/$ABI/src/librtlsdr.so"
  # ★★ VERIFY BOTH PROPERTIES BEFORE INSTALLING. A library missing either one is worse than the
  #    old one: no sys_dev means no radio opens on Android at all, and no V4L means a V4 Lite is
  #    mistaken for a plain R820T — which "works" on VHF and fails confusingly on HF.
  nm -D --defined-only "$SO" | grep -q rtlsdr_open_sys_dev \
    || { echo "$ABI: rtlsdr_open_sys_dev MISSING — refusing to install"; exit 1; }
  strings "$SO" | grep -q "Blog V4L" \
    || { echo "$ABI: RTL-SDR Blog V4L support MISSING — refusing to install"; exit 1; }
  cp "$SO" "$HERE/$ABI/lib/librtlsdr.so"
  cp "$WORK/src/include/rtl-sdr.h" "$HERE/$ABI/include/rtl-sdr.h"
  echo "$ABI: installed ($(stat -f%z "$HERE/$ABI/lib/librtlsdr.so") bytes)"
done
rm -rf "$WORK"
