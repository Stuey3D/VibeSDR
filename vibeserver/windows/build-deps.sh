#!/usr/bin/env bash
# Build the libraries the Windows VibeServer links — FROM PINNED SOURCE, static, for one target.
# ★ Runs INSIDE the llvm-mingw container (scripts/build-windows.sh starts it); never on the Mac.
# ★★★ THE SAME VERSIONS AS THE MAC AND LINUX (vibeserver/mac/build-deps.sh is the reference):
#       libusb 1.0.30 · librtlsdr osmocom 797f814 (= 2.0.3, has the RTL-SDR Blog V4) · libopus 1.6.1
#     A new platform is a new test rig; a new library version at the same time would be two changes
#     and one radio to blame.
# ★★ All STATIC: vibeserver.exe needs nothing beside it but Windows itself (Stuart's rule — no
#    external runtime dependencies; the SDRplay API stays the owner's own install, loaded at run time).
# ★ Idempotent, like the Mac script: a stamp per library holds everything it was built from.
#
# Usage (in the container):  build-deps.sh <triple> <deps-dir>
set -euo pipefail
TRIPLE="${1:?triple, e.g. x86_64-w64-mingw32}"
DEPS="${2:?deps dir}"
PREFIX="$DEPS/$TRIPLE/prefix"
SRC="$DEPS/src"
WORK="$DEPS/$TRIPLE/work"
JOBS="${JOBS:-3}"
mkdir -p "$PREFIX/lib" "$PREFIX/include" "$SRC" "$WORK"
export CC="$TRIPLE-clang" CXX="$TRIPLE-clang++" AR="$TRIPLE-ar" RANLIB="$TRIPLE-ranlib"
CCVER="$($CC --version | head -1)"
TC="$(cd "$(dirname "$0")" && pwd)/windows-toolchain.cmake"
CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC"
              -DVIBE_WIN_TRIPLE="$TRIPLE" -DVIBE_WIN_DEPS="$PREFIX"
              -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON)

stamp_ok() { [ "${FORCE:-0}" != 1 ] && [ -s "$3" ] && [ -f "$PREFIX/.stamp-$1" ] \
             && [ "$(cat "$PREFIX/.stamp-$1")" = "$2" ]; }
fetch() {    # fetch <url> <file> <sha256> — download once, verify EVERY time
  local url="$1" out="$SRC/$2" want="$3" got
  [ -s "$out" ] || { echo "    downloading $2"; curl -fsSL --retry 3 "$url" -o "$out.tmp" && mv "$out.tmp" "$out"; }
  got="$(sha256sum "$out" | awk '{print $1}')"
  [ "$got" = "$want" ] || { echo "!! SHA-256 MISMATCH for $2: got $got, pinned $want — refusing"; rm -f "$out"; exit 1; }
}
git_at() {   # git_at <repo> <full-commit-sha> <dir>
  rm -rf "$3"; git init -q "$3"
  git -C "$3" fetch -q --depth 1 "$1" "$2"
  git -C "$3" checkout -q FETCH_HEAD
  [ "$(git -C "$3" rev-parse HEAD)" = "$2" ] || { echo "!! $1 is not at $2 — refusing"; exit 1; }
}

# ── libusb 1.0.30 ─────────────────────────────────────────────────────────────────────────────
# ★★ Its Windows backend carries BOTH drivers M1 has to prove: WinUSB (default, and libusbK/libusb0
#    through libusbK.dll when one is installed) and UsbDk (LIBUSB_OPTION_USE_USBDK, run-time; it
#    LoadLibrary()s UsbDkHelper.dll, so nothing of UsbDk's is linked here).
USB_VER=1.0.30
USB_SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
USB_STAMP="libusb $USB_VER $USB_SHA $TRIPLE $CCVER"
if stamp_ok libusb "$USB_STAMP" "$PREFIX/lib/libusb-1.0.a"; then echo "==> libusb $USB_VER: up to date"; else
  echo "==> libusb $USB_VER ($TRIPLE)"
  fetch "https://github.com/libusb/libusb/releases/download/v$USB_VER/libusb-$USB_VER.tar.bz2" \
        "libusb-$USB_VER.tar.bz2" "$USB_SHA"
  rm -rf "$WORK/libusb"; mkdir -p "$WORK/libusb"
  tar -xjf "$SRC/libusb-$USB_VER.tar.bz2" -C "$WORK/libusb" --strip-components 1
  # ★ NOT --disable-dependency-tracking here: libusb's .rc rule writes .deps/*.Tpo regardless, and
  #   without the directory windres fails ("error opening .deps/libusb-1.0.Tpo").
  ( cd "$WORK/libusb" && ./configure --host="$TRIPLE" --prefix="$WORK/libusb/inst" --disable-shared \
        --enable-static >"$WORK/libusb.log" 2>&1 \
    && make -j"$JOBS" install >>"$WORK/libusb.log" 2>&1 ) \
    || { tail -30 "$WORK/libusb.log"; echo "!! libusb build failed"; exit 1; }
  rm -rf "$PREFIX/include/libusb-1.0"; mkdir -p "$PREFIX/include/libusb-1.0"
  cp "$WORK/libusb/inst/lib/libusb-1.0.a" "$PREFIX/lib/"
  cp "$WORK/libusb/inst/include/libusb-1.0/libusb.h" "$PREFIX/include/libusb-1.0/"
  cp "$WORK/libusb/COPYING" "$PREFIX/COPYING.libusb"
  rm -rf "$WORK/libusb" "$WORK/libusb.log"
  echo "$USB_STAMP" > "$PREFIX/.stamp-libusb"
fi

# ── librtlsdr 2.0.3 (osmocom/rtl-sdr 797f814) ──────────────────────────────────────────────────
RTL_SHA=797f8143266d983c56d8f35d2d442527529dd8a5
RTL_STAMP="librtlsdr $RTL_SHA libusb=$USB_VER $TRIPLE $CCVER known-devices-1"
if stamp_ok librtlsdr "$RTL_STAMP" "$PREFIX/lib/librtlsdr.a"; then echo "==> librtlsdr 2.0.3: up to date"; else
  echo "==> librtlsdr 2.0.3 ($RTL_SHA, $TRIPLE)"
  git_at https://github.com/osmocom/rtl-sdr.git "$RTL_SHA" "$WORK/rtl"
  # ★ The static library only — the rtl_* tools need getopt shims on Windows and are not shipped.
  cmake -S "$WORK/rtl" -B "$WORK/rtl/b" "${CMAKE_COMMON[@]}" \
        -DLIBUSB_INCLUDE_DIRS="$PREFIX/include/libusb-1.0" \
        -DLIBUSB_LIBRARIES="$PREFIX/lib/libusb-1.0.a" -DTHREADS_PTHREADS_WIN32_LIBRARY=pthread \
        >"$WORK/rtl.log" 2>&1 \
    && cmake --build "$WORK/rtl/b" --target rtlsdr_static -j"$JOBS" >>"$WORK/rtl.log" 2>&1 \
    || { tail -30 "$WORK/rtl.log"; echo "!! librtlsdr build failed"; exit 1; }
  # ★ On Windows the static target keeps its own name (librtlsdr_static.a); consumers must define
  #   rtlsdr_STATIC, or rtl-sdr_export.h declares every function dllimport.
  cp "$WORK/rtl/b/src/librtlsdr_static.a" "$PREFIX/lib/librtlsdr.a"
  cp "$WORK/rtl/include/rtl-sdr.h" "$WORK/rtl/include/rtl-sdr_export.h" "$PREFIX/include/"
  cp "$WORK/rtl/COPYING" "$PREFIX/COPYING.librtlsdr"
  # ★★ ONE RULE, TWO READERS: the VID:PID list Windows' driver scan matches (windows/win_usb.cpp) is
  #    GENERATED from the librtlsdr we link, never typed by hand — a dongle librtlsdr opens is a dongle
  #    the scan recognises, by construction (docs/WINDOWS-PORT-PLAN.md §3.1).
  python3 - "$WORK/rtl/src/librtlsdr.c" > "$PREFIX/include/rtl_known_devices.h" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
body = src[src.index("known_devices[]"):]
body = body[:body.index("};")]
rows = re.findall(r'\{\s*0x([0-9a-fA-F]{4})\s*,\s*0x([0-9a-fA-F]{4})\s*,\s*"([^"]*)"\s*\}', body)
assert len(rows) > 20, "known_devices[] not found where expected"
print("/* GENERATED by vibeserver/windows/build-deps.sh from librtlsdr.c known_devices[] (osmocom rtl-sdr). Do not edit. */")
print("#pragma once")
print("struct VibeRtlKnown { unsigned short vid, pid; const char* name; };")
print("static const VibeRtlKnown kVibeRtlKnown[] = {")
for v, p, n in rows:
    print('    { 0x%s, 0x%s, "%s" },' % (v.lower(), p.lower(), n.replace("\\", "\\\\").replace('"', '\\"')))
print("};")
PY
  [ "$(grep -c '0x0bda, 0x2838' "$PREFIX/include/rtl_known_devices.h" || true)" -gt 0 ] \
    || { echo "!! rtl_known_devices.h has no 0bda:2838 — refusing"; exit 1; }
  # ★★ Verify the FEATURE, not the file. Count, never grep -q (SIGPIPE under pipefail).
  V4="$(strings "$PREFIX/lib/librtlsdr.a" | grep -c 'Blog V4' || true)"
  [ "$V4" -gt 0 ] || { echo "!! librtlsdr has no RTL-SDR Blog V4 support — refusing"; exit 1; }
  rm -rf "$WORK/rtl" "$WORK/rtl.log"
  echo "$RTL_STAMP" > "$PREFIX/.stamp-librtlsdr"
fi

# ── libopus 1.6.1 ─────────────────────────────────────────────────────────────────────────────
OPUS_VER=1.6.1
OPUS_SHA=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
OPUS_STAMP="libopus $OPUS_VER $OPUS_SHA $TRIPLE $CCVER"
if stamp_ok libopus "$OPUS_STAMP" "$PREFIX/lib/libopus.a"; then echo "==> libopus $OPUS_VER: up to date"; else
  echo "==> libopus $OPUS_VER ($TRIPLE)"
  fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz" "opus-$OPUS_VER.tar.gz" "$OPUS_SHA"
  rm -rf "$WORK/opus"; mkdir -p "$WORK/opus"
  tar -xzf "$SRC/opus-$OPUS_VER.tar.gz" -C "$WORK/opus" --strip-components 1
  ( cd "$WORK/opus" && ./configure --host="$TRIPLE" --prefix="$WORK/opus/inst" --disable-shared \
        --enable-static --disable-doc --disable-extra-programs --disable-dependency-tracking \
        >"$WORK/opus.log" 2>&1 \
    && make -j"$JOBS" install >>"$WORK/opus.log" 2>&1 ) \
    || { tail -30 "$WORK/opus.log"; echo "!! libopus build failed"; exit 1; }
  rm -rf "$PREFIX/include/opus"; mkdir -p "$PREFIX/include/opus"
  cp "$WORK/opus/inst/lib/libopus.a" "$PREFIX/lib/"
  cp "$WORK/opus/inst/include/opus/"*.h "$PREFIX/include/opus/"
  cp "$WORK/opus/COPYING" "$PREFIX/COPYING.opus"
  rm -rf "$WORK/opus" "$WORK/opus.log"
  echo "$OPUS_STAMP" > "$PREFIX/.stamp-libopus"
fi
rmdir "$WORK" 2>/dev/null || true
echo "==> deps ready in $PREFIX"
