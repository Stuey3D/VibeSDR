#!/usr/bin/env bash
# VibeServer Lite — the engine's prebuilt dependencies at ANDROID API 21, armeabi-v7a only.
#
# ★ WHY A SECOND SET. The main app's prebuilts (cpp/sdr-kit, cpp/opus) are built at android-24,
#   and from API 23 bionic exports `stderr` as a SYMBOL where older levels have a macro over __sF.
#   librtlsdr.so and libopus.a both reference that symbol, so on Android 5.1 (a 2017 Fire 7, API 22)
#   the engine would not even load. Found 2026-09-18 linking the engine at API 21.
# ★★ NOTHING HERE TOUCHES THE MAIN APP'S PREBUILTS (the Lite rule): output goes to lite/native/kit
#    and lite/native/opus, and the engine's CMake is pointed at them with -DSDR_KIT_ROOT / -DOPUS_A.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP="$(cd "$HERE/../../android/app/src/main/cpp" && pwd)"
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
ABI=armeabi-v7a; API=21
TC="$NDK/build/cmake/android.toolchain.cmake"
NM="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"
KIT="$HERE/kit/$ABI"; mkdir -p "$KIT/lib" "$KIT/include" "$HERE/opus/lib/$ABI"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

echo "==> libusb v1.0.26 @ android-$API"
git clone --quiet --depth 1 --branch v1.0.26 https://github.com/libusb/libusb.git "$WORK/usb"
"$NDK/ndk-build" -C "$WORK/usb/android/jni" APP_ABI="$ABI" APP_PLATFORM=android-$API \
  APP_LDFLAGS="-llog" NDK_PROJECT_PATH="$WORK/usb/android" NDK_LIBS_OUT="$WORK/usbout" >/dev/null
cp "$WORK/usbout/$ABI/libusb1.0.so" "$KIT/lib/"
cp -R "$CPP/sdr-kit/$ABI/include/." "$KIT/include/"      # same headers as the main kit

echo "==> librtlsdr 797f814 (2.0.3, Blog V4L) + the fd patch @ android-$API"
git clone --quiet https://github.com/osmocom/rtl-sdr.git "$WORK/rtl"
RTL_SHA=797f8143266d983c56d8f35d2d442527529dd8a5        # 797f814 in full — same pin as vibeserver/mac/build-deps.sh
git -C "$WORK/rtl" checkout --quiet "$RTL_SHA"
[ "$(git -C "$WORK/rtl" rev-parse HEAD)" = "$RTL_SHA" ] || { echo "!! rtl-sdr is not at $RTL_SHA"; exit 1; }
git -C "$WORK/rtl" apply "$CPP/sdr-kit/librtlsdr-android.patch"
cmake -S "$WORK/rtl" -B "$WORK/rtlb" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=$ABI \
  -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release \
  -DLIBUSB_INCLUDE_DIRS="$KIT/include" -DLIBUSB_LIBRARIES="$KIT/lib/libusb1.0.so" \
  -DDETACH_KERNEL_DRIVER=OFF -DENABLE_ZEROCOPY=OFF >/dev/null
cmake --build "$WORK/rtlb" --target rtlsdr -j8 >/dev/null
cp "$WORK/rtlb/src/librtlsdr.so" "$KIT/lib/"

echo "==> opus 1.5.2 @ android-$API"
# ★★ PINNED SHA-256 (audit 2026-10-03). The tarball was `curl -sL` with no -f and no check, so an
#    error page or a swapped file went straight into a library we ship. Value = xiph's published
#    SHA256SUMS for opus-1.5.2.tar.gz (also Homebrew's pin for 1.5.2) — written in offline, so if it
#    ever refuses, compare the "got" hash with https://downloads.xiph.org/releases/opus/SHA256SUMS.txt
#    before changing it. Same pattern as vibeserver/mac/build-deps.sh fetch().
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"   # opus-1.5.2.tar.gz
curl -fsSL --retry 3 "https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz" -o "$WORK/opus.tgz"
GOT="$(shasum -a 256 "$WORK/opus.tgz" | awk '{print $1}')"
[ "$GOT" = "$OPUS_SHA256" ] || { echo "!! SHA-256 MISMATCH for opus-1.5.2.tar.gz: got $GOT, pinned $OPUS_SHA256 — refusing"; exit 1; }
tar -xzf "$WORK/opus.tgz" -C "$WORK"
cmake -S "$WORK/opus-1.5.2" -B "$WORK/opusb" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=$ABI \
  -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF >/dev/null
cmake --build "$WORK/opusb" --target opus -j8 >/dev/null
cp "$WORK/opusb/libopus.a" "$HERE/opus/lib/$ABI/"

# ★ THE GATE: verify the artefacts, not the exit codes. None may want the API-23 `stderr` symbol,
#   and librtlsdr must carry the fd entry point and the V4L tuner.
for f in "$KIT/lib/libusb1.0.so" "$KIT/lib/librtlsdr.so" "$HERE/opus/lib/$ABI/libopus.a"; do
  n="$("$NM" --undefined-only "$f" 2>/dev/null | grep -c -w stderr || true)"
  [ "$n" = "0" ] || { echo "!! $f still wants the 'stderr' symbol ($n) — not API $API clean"; exit 1; }
done
[ "$("$NM" -D --defined-only "$KIT/lib/librtlsdr.so" | grep -c rtlsdr_open_sys_dev || true)" -gt 0 ] || { echo "!! rtlsdr_open_sys_dev missing"; exit 1; }
[ "$(strings "$KIT/lib/librtlsdr.so" | grep -c 'Blog V4L' || true)" -gt 0 ] || { echo "!! V4L support missing"; exit 1; }
echo "==> OK: $(ls -la "$KIT/lib" "$HERE/opus/lib/$ABI" | grep -E '\.so|\.a' | awk '{print $NF" "$5}' | tr '\n' ' ')"
