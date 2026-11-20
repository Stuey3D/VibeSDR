#!/usr/bin/env bash
# build-rade-android.sh — FreeDV's RADE V1 receiver (rade_c + Opus 1.6.1's DNN code and FARGAN vocoder + freedv-backend's
# reliable text — all BSD) for Android: the app and VibeServer Lite. V12, 2026-11-20 — docs/v12/FREEDV.md ("RADE").
# ★★★ THE SAME BUILDER AND THE SAME PINS AS THE LINUX AND MAC SERVERS. The pins are READ from vibeserver/linux/build-deps.sh
#     (never written twice, so they cannot drift) and the archive is made by vibeserver/linux/build-rade.sh itself, with the
#     NDK's clang for each target — so the Android receiver is the server's receiver: the one buffer change to Opus's
#     nnet.c, the decoder's runtime DNN dispatch, the hand-written pilot search, only vrade_* left global.
# ★★ STATIC, LINKED INTO libvibelocalsdr.so (CMakeLists.txt, VIBE_HAVE_RADE), as on the servers. Its private copy of Opus
#    1.6.1's DNN code is all LOCAL symbols, so it can never meet the app's own libopus.a (1.5.2, the audio codec).
# ★★ THE TRANSMITTER TOO (librade_vibe_tx.a, VIBE_HAVE_RADE_TX): the benchmark's dec_rade row decodes a real RADE over made
#    by rade_c's own transmitter, and that row's verdict is what sets the owner's "rade" switch OFF by default on a phone
#    that cannot carry it (Stuart, 2026-11-20: "add it because the benchmark will advise it to be disabled").
# ★★ THREE OUTPUTS, as build-codec2.sh:
#      sdr-kit/arm64-v8a, sdr-kit/armeabi-v7a   the app, android-24 like the rest of sdr-kit
#      lite/native/kit/armeabi-v7a               VibeServer Lite, android-21 (checked: nothing Android 5 lacks)
# ★ SIMD: build-rade.sh's runtime dispatch, chosen by Opus's own detection (/proc/cpuinfo on Android, as on Linux):
#   arm64 NEON → NEON + int8 dot product; armeabi-v7a NEON (ARM mode, -mfpu=neon, as the app's other C) → dot product
#   where a 64-bit core running the 32-bit app reports it.
#   ANDROID_NDK=… android/app/src/main/cpp/sdr-kit/build-rade-android.sh      (run it under .claude/build-lock.sh)
set -euo pipefail
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../../../.." && pwd)"
LITE_KIT="$ROOT/lite/native/kit"
DEPS="$ROOT/vibeserver/linux/build-deps.sh"
RADE_SH="$ROOT/vibeserver/linux/build-rade.sh"
JOBS="${JOBS:-4}"
[ -d "$NDK" ] || { echo "NDK not found at $NDK — set ANDROID_NDK"; exit 1; }
BIN="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# ── the pins, from the Linux server's own build ──────────────────────────────────────────────────────────────────────
pin() { local v; v="$(sed -n "s/^$1=\([0-9A-Za-z.]*\).*/\1/p" "$DEPS" | head -1)"; [ -n "$v" ] || { echo "!! $1 not found in $DEPS"; exit 1; }; echo "$v"; }
RADE_C_COMMIT="$(pin RADE_C_COMMIT)"; RADE_C_SHA="$(pin RADE_C_SHA)"
FDVB_COMMIT="$(pin FDVB_COMMIT)";     FDVB_SHA="$(pin FDVB_SHA)"
OPUS_VER="$(pin OPUS_VER)";           OPUS_SHA="$(pin OPUS_SHA)"
fetch() {   # fetch <url> <file> <sha256>
  curl -fsSL --retry 3 "$1" -o "$WORK/$2"
  local got; got="$(shasum -a 256 "$WORK/$2" | awk '{print $1}')"
  [ "$got" = "$3" ] || { echo "!! SHA-256 MISMATCH for $2: got $got, pinned $3 — refusing"; exit 1; }
}
fetch "https://github.com/freedv/rade_c/archive/$RADE_C_COMMIT.tar.gz" rade_c.tar.gz "$RADE_C_SHA"
fetch "https://github.com/freedv/freedv-backend/archive/$FDVB_COMMIT.tar.gz" freedv-backend.tar.gz "$FDVB_SHA"
fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz" opus.tar.gz "$OPUS_SHA"
mkdir -p "$WORK/rade_c" "$WORK/fdvb" "$WORK/opus"
tar -xzf "$WORK/rade_c.tar.gz" -C "$WORK/rade_c" --strip-components 1 '*/src/*' '*/LICENSE'   # ★ not its ~40 MB of WAVs
tar -xzf "$WORK/freedv-backend.tar.gz" -C "$WORK/fdvb" --strip-components 1
tar -xzf "$WORK/opus.tar.gz" -C "$WORK/opus" --strip-components 1
echo "==> RADE V1: rade_c $RADE_C_COMMIT, freedv-backend $FDVB_COMMIT, opus $OPUS_VER — NDK $(basename "$NDK")"

# build <abi> <api> <out dir>
build() {
  local abi=$1 api=$2 out=$3 triple cflags="" p="$WORK/prefix-$1-$2"
  case "$abi" in
    arm64-v8a)   triple=aarch64-linux-android ;;
    # ★ the app's armeabi-v7a C is ARM mode with NEON (ANDROID_ARM_MODE=arm, ANDROID_ARM_NEON=ON) — the same here
    armeabi-v7a) triple=armv7a-linux-androideabi; cflags="-march=armv7-a -mfloat-abi=softfp -mfpu=neon -marm" ;;
  esac
  CC="$BIN/clang --target=$triple$api" CXX="$BIN/clang++ --target=$triple$api" AR="$BIN/llvm-ar" NM="$BIN/llvm-nm" \
  LD_R="$BIN/ld.lld" OBJCOPY="$BIN/llvm-objcopy" READELF="$BIN/llvm-readelf" CFLAGS="$cflags" CXXFLAGS="$cflags" JOBS="$JOBS" \
    bash "$RADE_SH" "$WORK/rade_c" "$WORK/opus" "$WORK/fdvb" "$p" >"$WORK/log-$abi-$api" 2>&1 \
    || { tail -30 "$WORK/log-$abi-$api"; echo "!! RADE build failed ($abi, android-$api)"; exit 1; }
  grep -E '^==> RADE DNN code' "$WORK/log-$abi-$api" | sed "s/^/    $abi android-$api: /"
  # ★ THE GATE — the artefacts, not the exit code (build-rade.sh has already checked the globals and the dispatch tiers).
  local a
  for a in librade_vibe librade_vibe_tx; do
    local want=ARM; [ "$abi" != arm64-v8a ] || want=AArch64
    [ "$("$BIN/llvm-readelf" -h "$p/lib/$a.a" | grep -c -E "Machine: +$want\$" || true)" -gt 0 ] \
      || { echo "!! $abi: $a.a is not for $want"; exit 1; }
    # ★ no COMDAT group may survive (partial()'s dissolving — a group could be discarded for the app's own copy)
    [ "$("$BIN/llvm-readelf" -g "$p/lib/$a.a" 2>/dev/null | grep -c 'COMDAT group' || true)" -eq 0 ] \
      || { echo "!! $abi: $a.a has COMDAT groups"; exit 1; }
    if [ "$api" -lt 23 ]; then
      # ★ nothing Android 5 lacks: not `stderr`/`stdout` (a symbol from API 23), not a C99 complex-maths function
      local bad; bad="$("$BIN/llvm-nm" -u "$p/lib/$a.a" | awk 'NF {print $NF}' | grep -xE 'stderr|stdout|c(real|imag|onj|abs|arg|exp|log|pow|sqrt)[fl]?' || true)"
      [ -z "$bad" ] || { echo "!! $abi android-$api: $a.a wants $bad"; exit 1; }
    fi
  done
  mkdir -p "$out/lib" "$out/include"
  cp "$p/lib/librade_vibe.a" "$p/lib/librade_vibe_tx.a" "$out/lib/"
  cp "$p/include/vibe_rade.h" "$out/include/"
  echo "    $abi android-$api → ${out#$ROOT/}/lib: librade_vibe.a $(wc -c < "$p/lib/librade_vibe.a" | tr -d ' ') B," \
       "librade_vibe_tx.a $(wc -c < "$p/lib/librade_vibe_tx.a" | tr -d ' ') B"
  cp "$p/share/licences/"* "$WORK/" 2>/dev/null || true
}
build arm64-v8a 24 "$HERE/arm64-v8a"
build armeabi-v7a 24 "$HERE/armeabi-v7a"
build armeabi-v7a 21 "$LITE_KIT/armeabi-v7a"

for f in LICENSE.rade_c COPYING.opus LICENSE.freedv-backend; do
  cp "$WORK/$f" "$HERE/$f"; cp "$WORK/$f" "$LITE_KIT/$f"
done
{ echo "rade_c $RADE_C_COMMIT sha256 $RADE_C_SHA (github.com/freedv/rade_c archive), BSD-2-Clause"
  echo "opus $OPUS_VER sha256 $OPUS_SHA (downloads.xiph.org), BSD-3-Clause — its DNN code and the FARGAN vocoder only"
  echo "freedv-backend $FDVB_COMMIT sha256 $FDVB_SHA (github.com/freedv/freedv-backend archive), BSD-2-Clause — reliable text"
  echo "built by vibeserver/linux/build-rade.sh (sha256 $(shasum -a 256 "$RADE_SH" | awk '{print $1}')) — pins read from vibeserver/linux/build-deps.sh"
  echo "static archives librade_vibe.a (receiver) + librade_vibe_tx.a (transmitter, the benchmark's dec_rade row); only vrade_* global"
  echo "NDK $(basename "$NDK"); app android-24 (arm64-v8a, armeabi-v7a ARM+NEON), Lite android-21 (armeabi-v7a ARM+NEON)"
  for d in "$HERE/arm64-v8a" "$HERE/armeabi-v7a" "$LITE_KIT/armeabi-v7a"; do
    for a in librade_vibe.a librade_vibe_tx.a; do
      echo "sha256 $(shasum -a 256 "$d/lib/$a" | awk '{print $1}')  ${d#$ROOT/}/lib/$a"
    done
  done; } > "$HERE/RADE-BUILT-FROM"
cp "$HERE/RADE-BUILT-FROM" "$LITE_KIT/RADE-BUILT-FROM"
echo "==> RADE V1 → sdr-kit/{arm64-v8a,armeabi-v7a} (android-24) + lite/native/kit/armeabi-v7a (android-21)"
