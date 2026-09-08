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

# ★★★ USE THE SDK'S OWN CMAKE, NOT WHATEVER IS ON $PATH. Homebrew's cmake reached 4.x, which
#     REFUSES a project whose cmake_minimum_required predates 3.5 — and osmocom/rtl-sdr's does. The
#     build then fails inside a try_compile, long before anything of ours is reached, and the only
#     visible symptom is this script's own V4L guard reporting a missing feature: a toolchain fault
#     wearing the costume of a source-code fault (2026-09-08).
#  ★ The SDK's 3.22.1 is the version the NDK's toolchain file is written against, so this also
#    stops the build drifting every time the host's cmake is upgraded.
READELF="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf"
[ -x "$READELF" ] || READELF="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
CMAKE="$(ls -1d "$HOME"/Library/Android/sdk/cmake/*/bin/cmake 2>/dev/null | sort -V | tail -1)"
[ -x "$CMAKE" ] || CMAKE="$(command -v cmake)"
[ -x "$CMAKE" ] || { echo "no cmake found"; exit 1; }
echo "using cmake: $CMAKE ($("$CMAKE" --version | head -1))"
git clone --quiet https://github.com/osmocom/rtl-sdr.git "$WORK/src"
git -C "$WORK/src" checkout --quiet "$TAG"
git -C "$WORK/src" apply "$HERE/librtlsdr-android.patch"

for ABI in arm64-v8a armeabi-v7a; do
  # ★★★ 16 KB PAGES ARE A PLAY REQUIREMENT, NOT A NICETY. Since 2025-11-01 every update targeting
  #     Android 15+ must support them, and a 4 KB-aligned .so will not load on a 16 KB-page device
  #     — so this library failing it takes the dongle down on exactly the newest hardware. NDK r27
  #     makes it opt-in and r28 makes it the default; we are on r27, so ask for it explicitly
  #     rather than inherit whatever the toolchain's default happens to be.
  "$CMAKE" -S "$WORK/src" -B "$WORK/$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    -DLIBUSB_INCLUDE_DIRS="$HERE/$ABI/include" \
    -DLIBUSB_LIBRARIES="$HERE/$ABI/lib/libusb1.0.so" \
    -DDETACH_KERNEL_DRIVER=OFF -DENABLE_ZEROCOPY=OFF >/dev/null
  "$CMAKE" --build "$WORK/$ABI" --target rtlsdr -j8 >/dev/null

  SO="$WORK/$ABI/src/librtlsdr.so"
  # ★★ VERIFY BOTH PROPERTIES BEFORE INSTALLING. A library missing either one is worse than the
  #    old one: no sys_dev means no radio opens on Android at all, and no V4L means a V4 Lite is
  #    mistaken for a plain R820T — which "works" on VHF and fails confusingly on HF.
  # ★★★ COUNT, NEVER `grep -q`, UNDER `set -o pipefail`. grep -q exits the instant it matches and
  #     closes the pipe; the producer then dies of SIGPIPE, pipefail propagates that, and A
  #     SUCCESSFUL MATCH IS REPORTED AS A FAILURE. This script refused to install a PERFECTLY GOOD
  #     library for exactly that reason — "RTL-SDR Blog V4L support MISSING" on a binary that
  #     contained the string — so librtlsdr could not be rebuilt at all, and the 4 KB-aligned
  #     binary stayed frozen in the tree until 2026-09-08.
  #  ★ The nm check above it passed only because its output is short enough to finish before grep
  #    exits, which is the worst possible property: the same bug, hidden, waiting for the symbol
  #    table to grow. Both are counted now.
  #  ★ Same family as the amd64 release harness, where `cmake --build | grep error` under pipefail
  #    failed when there were NO errors. It is worth knowing this shape by sight.
  HAVE_SYSDEV="$(nm -D --defined-only "$SO" | grep -c rtlsdr_open_sys_dev || true)"
  [ "$HAVE_SYSDEV" -gt 0 ] \
    || { echo "$ABI: rtlsdr_open_sys_dev MISSING — refusing to install"; exit 1; }
  HAVE_V4L="$(strings "$SO" | grep -c "Blog V4L" || true)"
  [ "$HAVE_V4L" -gt 0 ] \
    || { echo "$ABI: RTL-SDR Blog V4L support MISSING — refusing to install"; exit 1; }
  # ★★★ AND THE THIRD PROPERTY, for the same reason as the other two: a library that is wrong in a
  #     way nothing checks is a library that ships. arm64 is what 16 KB-page devices run; the
  #     32-bit ABI has no such devices, so it is reported but not enforced.
  ALIGN="$("$READELF" -lW "$SO" 2>/dev/null | awk '/LOAD/{print $NF}' | sort -u | tail -1)"
  echo "$ABI: max LOAD alignment $ALIGN"
  if [ "$ABI" = "arm64-v8a" ] && [ "$ALIGN" != "0x4000" ] && [ "$ALIGN" != "0x10000" ]; then
      echo "$ABI: NOT 16 KB aligned ($ALIGN) — refusing to install, Play requires it since 2025-11-01"
      exit 1
  fi
  cp "$SO" "$HERE/$ABI/lib/librtlsdr.so"
  cp "$WORK/src/include/rtl-sdr.h" "$HERE/$ABI/include/rtl-sdr.h"
  echo "$ABI: installed ($(stat -f%z "$HERE/$ABI/lib/librtlsdr.so") bytes)"
done
rm -rf "$WORK"
