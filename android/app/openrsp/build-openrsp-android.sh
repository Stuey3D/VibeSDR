#!/usr/bin/env bash
# Build OpenRSP's libsdrplay_api.so for Android — the RSP driver with its service EMBEDDED — and refuse to hand
# Gradle anything that would not ship. Run by android/app/openrsp.gradle for both apps (main app: arm64-v8a +
# armeabi-v7a; VibeServer Lite: armeabi-v7a). docs/OPENRSP-BUNDLE-PLAN.md §5.3.
#
#   build-openrsp-android.sh <out-jniLibs-dir> <work-dir> <abi>...
#   env: ANDROID_NDK (required), ANDROID_SDK (for cmake 3.22.1 + ninja), VIBE_BUILD_JOBS (default 2)
#
# ★★★ API 26, WHATEVER THE APP'S minSdk. The embedded service needs ASharedMemory_create (Android 8.0), and
#     OpenRSP's own cmake refuses a lower platform. The library is never LINKED by anything (sdrplay_source.cpp
#     dlopen()s it, and only on API ≥ 26), so a phone below 26 simply carries 1 MB it never loads.
# ★★★ 16 KB PAGES (play_16kb_page_size). NDK 27.1 linked this library 4 KB-aligned by default (measured
#     2026-10-21); only ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON gave 0x4000. An arm64 build that is not
#     16 KB-aligned is REFUSED here, so a toolchain change cannot quietly ship one.
# ★★ NO DOWNLOADS. OpenRSP's Android cmake fetches libusb at configure time unless SDRPLAY_ANDROID_LIBUSB_SRC
#    names a tree; it names vendor/libusb-1.0.27, checked file by file against its SHA256SUMS first.
# ★ FROM A STAGED COPY with the pinned firmware as firmware/own, for the same reason as the Mac build (no sdcc
#   4.2.0 here; vendor/openrsp-firmware/README). The vendor tree is never written to.
# ★★★ COUNT, NEVER `grep -q`, UNDER pipefail: grep -q closes the pipe on its first match, the producer dies of
#     SIGPIPE, and a SUCCESSFUL match reports as a failure (that froze librtlsdr in this tree for weeks).
set -euo pipefail
OUT="${1:?out dir}"; WORK="${2:?work dir}"; shift 2
[ "$#" -gt 0 ] || { echo "build-openrsp-android: no ABIs given" >&2; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
NDK="${ANDROID_NDK:?ANDROID_NDK must name the NDK}"
SDK="${ANDROID_SDK:-$HOME/Library/Android/sdk}"
JOBS="${VIBE_BUILD_JOBS:-2}"
CMAKE="$SDK/cmake/3.22.1/bin/cmake"; NINJA="$SDK/cmake/3.22.1/bin/ninja"
HOSTTAG=darwin-x86_64; [ -d "$NDK/toolchains/llvm/prebuilt/$HOSTTAG" ] || HOSTTAG=linux-x86_64
READELF="$NDK/toolchains/llvm/prebuilt/$HOSTTAG/bin/llvm-readelf"
for t in "$CMAKE" "$NINJA" "$READELF"; do [ -x "$t" ] || { echo "build-openrsp-android: missing $t" >&2; exit 1; }; done

LUSB="$ROOT/vendor/libusb-1.0.27"; FW="$ROOT/vendor/openrsp-firmware/own"
( cd "$LUSB" && shasum -a 256 -c SHA256SUMS >/dev/null ) \
  || { echo "build-openrsp-android: vendor/libusb-1.0.27 does not match its SHA256SUMS — refusing" >&2; exit 1; }
( cd "$FW" && shasum -a 256 -c manifest.sha256 >/dev/null ) \
  || { echo "build-openrsp-android: vendor/openrsp-firmware/own does not match its manifest — refusing" >&2; exit 1; }

SRC="$WORK/src"
mkdir -p "$SRC"
rsync -a --delete --exclude firmware/own "$ROOT/vendor/openrsp/" "$SRC/"
mkdir -p "$SRC/firmware/own" && cp "$FW/"* "$SRC/firmware/own/"

WANT_API="sdrplay_api_Open sdrplay_api_Close sdrplay_api_GetDevices sdrplay_api_SelectDevice sdrplay_api_ReleaseDevice
          sdrplay_api_GetDeviceParams sdrplay_api_Init sdrplay_api_Uninit sdrplay_api_Update sdrplay_api_LockDeviceApi
          sdrplay_api_UnlockDeviceApi sdrplay_api_GetErrorString sdrplay_api_ApiVersion
          sdrplay_android_service_start sdrplay_android_usb_attach sdrplay_android_usb_detach sdrplay_android_usb_count"

for ABI in "$@"; do
  B="$WORK/b-$ABI"
  "$CMAKE" -S "$SRC" -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE=Release -DSDRPLAY_BUILD_TESTS=OFF -DSDRPLAY_ANDROID_LIBUSB_SRC="$LUSB" \
    > "$WORK/configure-$ABI.log" 2>&1 \
    || { tail -30 "$WORK/configure-$ABI.log" >&2; echo "build-openrsp-android: $ABI configure failed" >&2; exit 1; }
  "$CMAKE" --build "$B" -j "$JOBS" --target sdrplay_api > "$WORK/build-$ABI.log" 2>&1 \
    || { grep -E "error" "$WORK/build-$ABI.log" | head -20 >&2; echo "build-openrsp-android: $ABI build failed" >&2; exit 1; }
  L="$B/libsdrplay_api.so"
  [ -s "$L" ] || { echo "build-openrsp-android: $ABI produced no library" >&2; exit 1; }

  ALIGN="$("$READELF" -lW "$L" | awk '/LOAD/{print $NF}' | sort -u | tail -1)"
  if [ "$ABI" = "arm64-v8a" ] && [ "$ALIGN" != "0x4000" ] && [ "$ALIGN" != "0x10000" ]; then
    echo "build-openrsp-android: $ABI is NOT 16 KB aligned (LOAD $ALIGN) — refusing; Play requires it" >&2; exit 1
  fi
  EXPORTS="$("$READELF" -W --dyn-syms "$L" | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | sed 's/@.*//')"
  for f in $WANT_API; do
    [ "$(printf '%s\n' "$EXPORTS" | grep -c "^$f\$" || true)" -ge 1 ] \
      || { echo "build-openrsp-android: $ABI lacks the entry point $f — refusing" >&2; exit 1; }
  done
  # ★ Its libusb is hidden; leaking one would put a SECOND libusb_* beside sdr-kit's libusb1.0.so in one process.
  LEAK="$(printf '%s\n' "$EXPORTS" | grep -c '^libusb_' || true)"
  [ "$LEAK" -eq 0 ] || { echo "build-openrsp-android: $ABI exports $LEAK libusb_* symbols — refusing" >&2; exit 1; }
  # ★ Only the platform's own libraries: anything else would be missing on the phone.
  BAD="$("$READELF" -dW "$L" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | grep -vcE '^lib(android|log|m|dl|c)\.so$' || true)"
  [ "$BAD" -eq 0 ] || { echo "build-openrsp-android: $ABI needs a library the phone does not have:" >&2;
                        "$READELF" -dW "$L" | grep NEEDED >&2; exit 1; }

  mkdir -p "$OUT/$ABI"
  cp "$L" "$OUT/$ABI/libsdrplay_api.so"
  echo "build-openrsp-android: $ABI ok — LOAD $ALIGN, $(stat -f%z "$L" 2>/dev/null || stat -c%s "$L") bytes unstripped"
done
