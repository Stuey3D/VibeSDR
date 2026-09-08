#!/usr/bin/env bash
# Rebuild the vendored libusb1.0.so for both Android ABIs.
#
# ★★★ THIS SCRIPT EXISTS BECAUSE THE .so HAD NO PROVENANCE AND COULD NOT BE REBUILT. librtlsdr had
#     build-librtlsdr.sh beside it; libusb had nothing — the binary was simply in the tree, and
#     build-librtlsdr.sh only ever LINKED against it. So when Google Play's 16 KB page size rule
#     made the old 4 KB-aligned binary unshippable (in force since 2025-11-01), there was no way to
#     make another. Same reasoning as the librtlsdr script: a dependency you cannot rebuild is a
#     dependency you cannot fix.
#
# ★★★ SAME VERSION, NEW TOOLCHAIN — DELIBERATELY. The vendored header declares
#     LIBUSB_API_VERSION 0x01000109, which is v1.0.26, and that is what this builds. Moving the
#     version at the same time as the toolchain would mean two changes and one radio to blame it
#     on. Verified like-for-like against the old binary: same soname, same 91 exported symbols.
#     (It no longer pulls in libstdc++.so, which the older NDK added and pure-C libusb never
#     needed.)
#
# Usage:  sdr-kit/build-libusb.sh [tag]          (default: the version below)
set -euo pipefail
TAG="${1:-v1.0.26}"                     # matches LIBUSB_API_VERSION 0x01000109 in our libusb.h
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
READELF="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf"
[ -x "$READELF" ] || READELF="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"

[ -d "$NDK" ] || { echo "NDK not found at $NDK — set ANDROID_NDK"; exit 1; }
git clone --quiet --depth 1 --branch "$TAG" https://github.com/libusb/libusb.git "$WORK/src"

# ★ libusb ships its own ndk-build project; use it rather than inventing a CMake port that would
#   then have to be kept in step with upstream's source list.
# ★★ APP_LDFLAGS must keep -llog: Application.mk sets it, and passing APP_LDFLAGS on the command
#    line REPLACES that value rather than adding to it. Dropping it links a library that fails to
#    resolve __android_log_print at load time — on the device, not here.
"$NDK/ndk-build" -C "$WORK/src/android/jni" \
  APP_ABI="arm64-v8a armeabi-v7a" APP_PLATFORM=android-24 \
  APP_LDFLAGS="-llog -Wl,-z,max-page-size=16384" \
  NDK_PROJECT_PATH="$WORK/src/android" NDK_LIBS_OUT="$WORK/out" >/dev/null

for ABI in arm64-v8a armeabi-v7a; do
  SO="$WORK/out/$ABI/libusb1.0.so"
  [ -f "$SO" ] || { echo "$ABI: nothing built"; exit 1; }
  # ★★★ COUNT, NEVER `grep -q`, UNDER pipefail — see the note in build-librtlsdr.sh. grep -q closes
  #     the pipe on its first match, the producer dies of SIGPIPE, and a SUCCESSFUL match reports
  #     as a failure. That bug silently froze librtlsdr in this tree for weeks.
  SYMS="$(nm -D --defined-only "$SO" | grep -c 'libusb_' || true)"
  [ "$SYMS" -gt 50 ] || { echo "$ABI: only $SYMS libusb symbols — refusing to install"; exit 1; }
  LOG="$($READELF -dW "$SO" | grep -c 'NEEDED.*liblog' || true)"
  [ "$LOG" -gt 0 ] || { echo "$ABI: liblog not linked — refusing to install (see APP_LDFLAGS)"; exit 1; }
  ALIGN="$($READELF -lW "$SO" | awk '/LOAD/{print $NF}' | sort -u | tail -1)"
  echo "$ABI: max LOAD alignment $ALIGN, $SYMS libusb symbols"
  if [ "$ABI" = "arm64-v8a" ] && [ "$ALIGN" != "0x4000" ] && [ "$ALIGN" != "0x10000" ]; then
      echo "$ABI: NOT 16 KB aligned ($ALIGN) — refusing to install, Play requires it since 2025-11-01"
      exit 1
  fi
  cp "$SO" "$HERE/$ABI/lib/libusb1.0.so"
  echo "$ABI: installed ($(stat -f%z "$HERE/$ABI/lib/libusb1.0.so") bytes)"
done
rm -rf "$WORK"
