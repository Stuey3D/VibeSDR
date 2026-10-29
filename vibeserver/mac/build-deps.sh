#!/usr/bin/env bash
# Build every third-party library the Mac app links — and the cloudflared it ships — FROM PINNED
# SOURCE, for the macOS version the app promises (vibeserver/mac/MACOS_MINIMUM).
#
# ★★★ WHY NOT HOMEBREW. build-app.sh used to link Homebrew's static archives. A Homebrew bottle is
#     built for the OS of whoever installed it — on this Mac, macOS 26 bottles on a macOS 27 beta —
#     so 5.6.78 shipped librtlsdr, libusb, libopus and libhackrf objects built for macOS 26 inside
#     an app whose Info.plist says 14.0. ld warned ("built for newer macOS version (26.0) than
#     being linked (14.0)") into /dev/null, stamped the final binary 14.0 anyway, and the result is
#     code that may call APIs a 14 or 15 Mac does not have. A driver that was not built for the
#     user's Mac is exactly how a Mac build "locked up" once before and a user had to report it
#     (Stuart, 2026-09-28). The fix is to own the build: pinned source, SHA-256 checked, compiled
#     here with MACOSX_DEPLOYMENT_TARGET = the app's minimum, and verified object by object.
# ★★ SAME VERSIONS AS BEFORE, ON PURPOSE. Each library is the exact release the Mac already
#    shipped (and, for librtlsdr, the exact commit Linux and Android build): only the deployment
#    target changes. Moving a version at the same time would be two changes and one radio to blame.
#      librtlsdr  osmocom/rtl-sdr 797f814 = v2.0.3 — Linux Dockerfile.build, Android sdr-kit, and
#                 Homebrew's librtlsdr 2.0.3 are all this commit (has the RTL-SDR Blog V4L)
#      libusb     1.0.30 — what Homebrew gave the Mac. (Android is pinned at 1.0.26, Linux takes
#                 Debian's; the Mac's darwin backend has had fixes since 1.0.26, so it stays put.)
#      libopus    1.6.1  — what Homebrew gave the Mac. (Android vendors 1.5.2.)
#      cloudflared 2026.10.4 — the version tools/cloudflared-desktop/fetch.sh pins, but COMPILED
#                 (see the cloudflared section: Cloudflare's own binary is minos 15.0).
#    NOT here, because CMakeLists.txt compiles them into libvibeserver_core.a from the vendored
#    tree, so they get the deployment target with the rest of the core:
#      libairspyhf (cpp/libairspyhf, patched) · libairspy (cpp/libairspy) · libhackrf 2026.01.3
#      (cpp/libhackrf, the same source Android compiles). SDRplay is dlopen()ed, never linked.
#
# Output (outside git — vibeserver/.gitignore has mac/deps/):
#   vibeserver/mac/deps/prefix/{lib,include}   static .a + headers, for CMake and swiftc
#   vibeserver/mac/deps/cloudflared/           cloudflared + LICENSE
#   vibeserver/mac/deps/src/                   downloaded tarballs (small; kept so a rebuild is offline)
# ★ IDEMPOTENT: each piece writes a stamp holding everything it was built from (version, hash,
#   minimum macOS, arch, compiler). Same stamp = skipped in a second. Change any input = rebuilt.
# ★ Intermediates go in deps/work/ and are deleted as each piece finishes — this Mac filled its disk
#   once, and a Go build cache alone is ~400 MB.
#
# Usage:  vibeserver/mac/build-deps.sh            (build-app.sh runs it for you)
#         FORCE=1 vibeserver/mac/build-deps.sh    rebuild everything
set -euo pipefail

MAC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="${VIBE_MAC_DEPS_DIR:-$MAC/deps}"
PREFIX="$DEPS/prefix"
SRC="$DEPS/src"
WORK="$DEPS/work"
CFDIR="$DEPS/cloudflared"
MIN="$(tr -d '[:space:]' < "$MAC/MACOS_MINIMUM")"
# ★ arm64 only, matching build-app.sh (swiftc -target arm64-apple-macos…, and the core's CMake
#   builds for the host). If the app ever goes universal, this list and build-app.sh move together.
ARCH=arm64
JOBS="$(sysctl -n hw.ncpu)"
[ -n "$MIN" ] || { echo "!! build-deps: MACOS_MINIMUM is empty"; exit 1; }

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$SRC" "$CFDIR"

# ── A clean, pinned toolchain environment ────────────────────────────────────────────────────
# ★ Apple's clang, never a Homebrew llvm on PATH; no inherited include/library search paths; and
#   pkg-config (if anyone installs it later) sees ONLY our prefix, so a configure script cannot
#   wander into /opt/homebrew and pick a bottle back up.
export CC=/usr/bin/clang CXX=/usr/bin/clang++
export MACOSX_DEPLOYMENT_TARGET="$MIN"
export CFLAGS="-arch $ARCH -mmacosx-version-min=$MIN -O2"
export CXXFLAGS="$CFLAGS" LDFLAGS="-arch $ARCH -mmacosx-version-min=$MIN"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH SDKROOT 2>/dev/null || true
CCVER="$($CC --version | head -1)"
CMAKE_COMMON=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN"
              -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_C_COMPILER="$CC"
              -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;/opt/local"
              -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON)

stamp_ok() {  # stamp_ok <name> <stamp> <output-file> — same inputs AND the output still there
  [ "${FORCE:-0}" != 1 ] && [ -s "$3" ] && [ -f "$PREFIX/.stamp-$1" ] \
    && [ "$(cat "$PREFIX/.stamp-$1")" = "$2" ]
}
check_log() {  # clang's "only available on macOS N" = an API the minimum lacks, compiled anyway
  local n; n="$(grep -ciE 'only available on macOS|unguarded-availability' "$1" || true)"
  [ "$n" -eq 0 ] || { grep -iE 'only available on macOS|unguarded-availability' "$1" | head -5
                      echo "!! $n availability warnings in $(basename "$1") — refusing"; exit 1; }
}
fetch() {    # fetch <url> <file> <sha256> — download once, verify EVERY time
  local url="$1" out="$SRC/$2" want="$3" got
  if [ ! -s "$out" ]; then
    echo "    downloading $2"
    curl -fsSL --retry 3 "$url" -o "$out.tmp" && mv "$out.tmp" "$out"
  fi
  got="$(shasum -a 256 "$out" | awk '{print $1}')"
  [ "$got" = "$want" ] || { echo "!! SHA-256 MISMATCH for $2: got $got, pinned $want — refusing"; rm -f "$out"; exit 1; }
}
git_at() {   # git_at <repo> <full-commit-sha> <dir> — fetch exactly that commit, prove it
  rm -rf "$3"; git init -q "$3"
  git -C "$3" fetch -q --depth 1 "$1" "$2"
  git -C "$3" checkout -q FETCH_HEAD
  [ "$(git -C "$3" rev-parse HEAD)" = "$2" ] || { echo "!! $1 is not at $2 — refusing"; exit 1; }
}
verify_archive() {  # every object in the archive at or below the minimum, or stop here
  local out; out="$("$MAC/check-bundle.sh" --min "$MIN" --archive "$1")" || { echo "$out"; exit 1; }
  echo "$out" | sed -n '3p'
}

# ── libusb 1.0.30 ────────────────────────────────────────────────────────────────────────────
USB_VER=1.0.30
USB_SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf   # = Homebrew's pin
USB_STAMP="libusb $USB_VER $USB_SHA min=$MIN arch=$ARCH $CCVER"
if stamp_ok libusb "$USB_STAMP" "$PREFIX/lib/libusb-1.0.a"; then echo "==> libusb $USB_VER: up to date"; else
  echo "==> libusb $USB_VER: building for macOS $MIN"
  fetch "https://github.com/libusb/libusb/releases/download/v$USB_VER/libusb-$USB_VER.tar.bz2" \
        "libusb-$USB_VER.tar.bz2" "$USB_SHA"
  rm -rf "$WORK/libusb"; mkdir -p "$WORK/libusb"
  tar -xjf "$SRC/libusb-$USB_VER.tar.bz2" -C "$WORK/libusb" --strip-components 1
  rm -rf "$PREFIX/include/libusb-1.0" "$PREFIX/lib/libusb-1.0".*
  # ★★★ ac_cv_func_pipe2=no — FOUND BY THIS SCRIPT ON ITS FIRST RUN (2026-09-28). pipe2() is new in
  #     macOS 27 and this Mac's SDK is 27, so configure's link test finds it and libusb calls it in
  #     usbi_create_event() — every libusb_init(). With a 14.0 target clang only WARNS ("'pipe2' is
  #     only available on macOS 27.0 or newer"), the symbol is weak, and on macOS 14/15/26 it is
  #     NULL: libusb_init() would jump to address 0 and take the whole server down the moment it
  #     looked for a dongle. Autoconf's function probes declare the function themselves, so they
  #     cannot see availability; the answer has to be given. libusb then uses pipe() + FD_CLOEXEC,
  #     its path for every macOS before 27. check_log below fails the build if another one appears.
  ( cd "$WORK/libusb" \
    && ./configure --prefix="$WORK/libusb/inst" --disable-shared --enable-static \
                   --disable-dependency-tracking ac_cv_func_pipe2=no >"$WORK/libusb.log" 2>&1 \
    && make -j"$JOBS" install >>"$WORK/libusb.log" 2>&1 ) \
    || { tail -30 "$WORK/libusb.log"; echo "!! libusb build failed"; exit 1; }
  cp "$WORK/libusb/inst/lib/libusb-1.0.a" "$PREFIX/lib/"
  mkdir -p "$PREFIX/include/libusb-1.0"
  cp "$WORK/libusb/inst/include/libusb-1.0/libusb.h" "$PREFIX/include/libusb-1.0/"
  check_log "$WORK/libusb.log"
  verify_archive "$PREFIX/lib/libusb-1.0.a"
  rm -rf "$WORK/libusb" "$WORK/libusb.log"
  echo "$USB_STAMP" > "$PREFIX/.stamp-libusb"
fi

# ── librtlsdr 2.0.3 (osmocom/rtl-sdr 797f814) ────────────────────────────────────────────────
RTL_SHA=797f8143266d983c56d8f35d2d442527529dd8a5
RTL_PATCH="$(cd "$(dirname "$0")" && pwd)/../linux/librtlsdr-cancel-wait.patch"
RTL_STAMP="librtlsdr $RTL_SHA patch=$(shasum -a 256 "$RTL_PATCH" | cut -c1-12) libusb=$USB_VER min=$MIN arch=$ARCH $CCVER"
if stamp_ok librtlsdr "$RTL_STAMP" "$PREFIX/lib/librtlsdr.a"; then echo "==> librtlsdr 2.0.3: up to date"; else
  echo "==> librtlsdr 2.0.3 ($RTL_SHA): building for macOS $MIN"
  git_at https://github.com/osmocom/rtl-sdr.git "$RTL_SHA" "$WORK/rtl"
  # ★★ OUR ONE PATCH TO librtlsdr (V12, 2026-10-24): rtlsdr_read_async's cancel waits for every transfer to come home
    #    before freeing any (upstream freed them with URBs still in the kernel; the next control transfer could then lock a
    #    destroyed mutex — the usbi_mutex_lock abort seen on live rate changes in September). Not reproducible since on any
    #    host (500+ live rate changes, 2026-10-24), so it is a GUARD, not a proven fix. See librtlsdr-cancel-wait.patch.
  git -C "$WORK/rtl" apply "$RTL_PATCH" || { echo "!! librtlsdr-cancel-wait.patch did not apply"; exit 1; }
  # ★ The static target only: the rtl_* tools and the dylib are not shipped, so building them would
  #   be work (and a dylib lying around to be linked by mistake) for nothing.
  # ★ libusb passed BY PATH — PkgConfig is disabled above, so this cannot resolve to Homebrew's.
  # ★ DETACH_KERNEL_DRIVER / ZEROCOPY are Linux-only and default OFF, exactly as Homebrew built it.
  cmake -S "$WORK/rtl" -B "$WORK/rtl/b" "${CMAKE_COMMON[@]}" \
        -DLIBUSB_INCLUDE_DIRS="$PREFIX/include/libusb-1.0" \
        -DLIBUSB_LIBRARIES="$PREFIX/lib/libusb-1.0.a" >"$WORK/rtl.log" 2>&1 \
    && cmake --build "$WORK/rtl/b" --target rtlsdr_static -j"$JOBS" >>"$WORK/rtl.log" 2>&1 \
    || { tail -30 "$WORK/rtl.log"; echo "!! librtlsdr build failed"; exit 1; }
  cp "$WORK/rtl/b/src/librtlsdr.a" "$PREFIX/lib/"
  cp "$WORK/rtl/include/rtl-sdr.h" "$WORK/rtl/include/rtl-sdr_export.h" "$PREFIX/include/"
  # ★★ VERIFY THE FEATURE, not just the file: a librtlsdr without V4L support is why the Linux
  #    build owns its librtlsdr at all. COUNT under pipefail, never grep -q (SIGPIPE = false fail).
  V4L="$(strings "$PREFIX/lib/librtlsdr.a" | grep -c 'Blog V4L' || true)"
  [ "$V4L" -gt 0 ] || { echo "!! librtlsdr has no RTL-SDR Blog V4L support — refusing"; exit 1; }
  check_log "$WORK/rtl.log"
  verify_archive "$PREFIX/lib/librtlsdr.a"
  rm -rf "$WORK/rtl" "$WORK/rtl.log"
  echo "$RTL_STAMP" > "$PREFIX/.stamp-librtlsdr"
fi

# ── libopus 1.6.1 ────────────────────────────────────────────────────────────────────────────
OPUS_VER=1.6.1
OPUS_SHA=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1  # = xiph SHA256SUMS = Homebrew
OPUS_STAMP="libopus $OPUS_VER $OPUS_SHA min=$MIN arch=$ARCH $CCVER"
if stamp_ok libopus "$OPUS_STAMP" "$PREFIX/lib/libopus.a"; then echo "==> libopus $OPUS_VER: up to date"; else
  echo "==> libopus $OPUS_VER: building for macOS $MIN"
  fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz" "opus-$OPUS_VER.tar.gz" "$OPUS_SHA"
  rm -rf "$WORK/opus"; mkdir -p "$WORK/opus"
  tar -xzf "$SRC/opus-$OPUS_VER.tar.gz" -C "$WORK/opus" --strip-components 1
  rm -rf "$PREFIX/include/opus" "$PREFIX/lib/libopus".*
  # ★ configure, as Homebrew builds it (and so as the Mac has always run it) — same defaults,
  #   same NEON paths; only the deployment target differs.
  ( cd "$WORK/opus" \
    && ./configure --prefix="$WORK/opus/inst" --disable-shared --enable-static --disable-doc \
                   --disable-extra-programs --disable-dependency-tracking >"$WORK/opus.log" 2>&1 \
    && make -j"$JOBS" install >>"$WORK/opus.log" 2>&1 ) \
    || { tail -30 "$WORK/opus.log"; echo "!! libopus build failed"; exit 1; }
  cp "$WORK/opus/inst/lib/libopus.a" "$PREFIX/lib/"
  mkdir -p "$PREFIX/include/opus"; cp "$WORK/opus/inst/include/opus/"*.h "$PREFIX/include/opus/"
  check_log "$WORK/opus.log"
  verify_archive "$PREFIX/lib/libopus.a"
  rm -rf "$WORK/opus" "$WORK/opus.log"
  echo "$OPUS_STAMP" > "$PREFIX/.stamp-libopus"
fi

# ── FAAD2 2.11.3 (libfaad_drm), static — V12 DRM's AAC, decoded on the server ─────────────────────────────────
# ★★★ GPL-2.0-or-later: the VibeServer MAC APP may contain it (it is GPL, notarised, not App Store). The App Store app's
#     archive never does — modules/vibe-local-sdr/build_ios.sh fails on any FAAD2 symbol. COPYING → Resources/COPYING.faad2.
FAAD_VER=2.11.3
FAAD_SHA=860ab62087e336c1844a70e33196c1790b525fb9a9e7b6ac4fab1a1a4e4d5ce8  # GitHub tag tarball = linux/build-deps.sh
FAAD_STAMP="faad2 $FAAD_VER $FAAD_SHA min=$MIN arch=$ARCH $CCVER"
if stamp_ok faad2 "$FAAD_STAMP" "$PREFIX/lib/libfaad_drm.a"; then echo "==> FAAD2 $FAAD_VER: up to date"; else
  echo "==> FAAD2 $FAAD_VER (DRM build): building for macOS $MIN"
  fetch "https://github.com/knik0/faad2/archive/refs/tags/$FAAD_VER.tar.gz" "faad2-$FAAD_VER.tar.gz" "$FAAD_SHA"
  rm -rf "$WORK/faad"; mkdir -p "$WORK/faad"
  tar -xzf "$SRC/faad2-$FAAD_VER.tar.gz" -C "$WORK/faad" --strip-components 1
  cmake -S "$WORK/faad" -B "$WORK/faad/b" "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF \
        -DFAAD_APPLY_DRC=OFF >"$WORK/faad.log" 2>&1 \
    && cmake --build "$WORK/faad/b" --target faad_drm -j"$JOBS" >>"$WORK/faad.log" 2>&1 \
    || { tail -30 "$WORK/faad.log"; echo "!! FAAD2 build failed"; exit 1; }
  cp "$WORK/faad/b/libfaad_drm.a" "$PREFIX/lib/"
  cp "$WORK/faad/include/neaacdec.h" "$PREFIX/include/"
  mkdir -p "$PREFIX/share/licences"; cp "$WORK/faad/COPYING" "$PREFIX/share/licences/COPYING.faad2"
  [ "$(nm "$PREFIX/lib/libfaad_drm.a" 2>/dev/null | grep -c 'T .*NeAACDecInitDRM' || true)" -gt 0 ] \
    || { echo "!! libfaad_drm.a has no NeAACDecInitDRM — refusing"; exit 1; }
  check_log "$WORK/faad.log"
  verify_archive "$PREFIX/lib/libfaad_drm.a"
  rm -rf "$WORK/faad" "$WORK/faad.log"
  echo "$FAAD_STAMP" > "$PREFIX/.stamp-faad2"
fi

# ── FAAD2 2.11.3 again: the PLAIN build, every global renamed vibedab_ (libfaad_dab) — V12 DAB+ audio in process ──────
# ★★★ The DRM build above refuses DAB+, and the plain build exports the same names — see linux/build-deps.sh, which does
#     the same thing the same way: a forced include #defines every global (read off a first plain build) to vibedab_<name>,
#     and the archive is CHECKED to define nothing unprefixed. vibe_dab_faad.h calls it under the prefixed names.
# ★★ V12: optimised, with VibeSDR's patch series — see linux/build-deps.sh (the same flag, the same series, in order).
FAAD_DAB_OPT="-funsafe-math-optimizations"
FAAD_PATCHES="$MAC/../linux/faad2-patches"
FAAD_SERIES_SUM="$( { [ -f "$FAAD_PATCHES/series" ] && cat "$FAAD_PATCHES/series" && grep -v '^#' "$FAAD_PATCHES/series" | while IFS= read -r p; do [ -n "$p" ] && cat "$FAAD_PATCHES/$p"; done; } 2>/dev/null | shasum -a 256 | cut -c1-16)"
FAADD_STAMP="faad2-dab $FAAD_VER $FAAD_SHA min=$MIN arch=$ARCH $CCVER opt=$FAAD_DAB_OPT patches=$FAAD_SERIES_SUM"
if stamp_ok faad2-dab "$FAADD_STAMP" "$PREFIX/lib/libfaad_dab.a"; then echo "==> FAAD2 $FAAD_VER (DAB+ build): up to date"; else
  echo "==> FAAD2 $FAAD_VER (plain build, prefixed, for DAB+): building for macOS $MIN"
  rm -rf "$WORK/faadd"; mkdir -p "$WORK/faadd"
  tar -xzf "$SRC/faad2-$FAAD_VER.tar.gz" -C "$WORK/faadd" --strip-components 1
  if [ -f "$FAAD_PATCHES/series" ]; then
    while IFS= read -r p; do
      case "$p" in ""|\#*) continue ;; esac
      patch -d "$WORK/faadd" -p1 -s < "$FAAD_PATCHES/$p" || { echo "!! FAAD2 patch $p did not apply"; exit 1; }
      echo "    FAAD2 patch: $p"
    done < "$FAAD_PATCHES/series"
  fi
  # ★ TWO PASSES: the plain build defines globals the DRM one does not (LTP, MAIN prediction — measured), so the list is
  #   read off a first, unprefixed plain build, and the second build is the one kept.
  cmake -S "$WORK/faadd" -B "$WORK/faadd/b0" "${CMAKE_COMMON[@]}" -DCMAKE_C_FLAGS="$CFLAGS" \
        -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DFAAD_APPLY_DRC=OFF >"$WORK/faadd.log" 2>&1 \
    && cmake --build "$WORK/faadd/b0" --target faad -j"$JOBS" >>"$WORK/faadd.log" 2>&1 \
    || { tail -30 "$WORK/faadd.log"; echo "!! FAAD2 (plain, first pass) failed"; exit 1; }
  { echo "/* generated by build-deps.sh from the plain libfaad.a — FAAD2 $FAAD_VER, globals renamed for the DAB+ build */"
    nm -gU "$WORK/faadd/b0/libfaad.a" 2>/dev/null | awk 'NF >= 3 && $2 ~ /^[TDBSRC]$/ {print $3}' | sed 's/^_//' | sort -u \
      | awk '{printf "#define %s vibedab_%s\n", $1, $1}'; } > "$WORK/faadd/faad_dab_prefix.h"
  [ "$(grep -c '^#define' "$WORK/faadd/faad_dab_prefix.h" || true)" -gt 50 ] || { echo "!! FAAD2 symbol list is short — refusing"; exit 1; }
  cmake -S "$WORK/faadd" -B "$WORK/faadd/b" "${CMAKE_COMMON[@]}" -DCMAKE_C_FLAGS="$CFLAGS $FAAD_DAB_OPT -include $WORK/faadd/faad_dab_prefix.h" \
        -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DFAAD_APPLY_DRC=OFF >>"$WORK/faadd.log" 2>&1 \
    && cmake --build "$WORK/faadd/b" --target faad -j"$JOBS" >>"$WORK/faadd.log" 2>&1 \
    || { tail -30 "$WORK/faadd.log"; echo "!! FAAD2 (DAB+ build) failed"; exit 1; }
  cp "$WORK/faadd/b/libfaad.a" "$PREFIX/lib/libfaad_dab.a"
  [ "$(nm "$PREFIX/lib/libfaad_dab.a" 2>/dev/null | grep -c 'T _vibedab_NeAACDecDecode$' || true)" -gt 0 ] \
    || { echo "!! libfaad_dab.a has no vibedab_NeAACDecDecode — refusing"; exit 1; }
  unpref="$(nm -gU "$PREFIX/lib/libfaad_dab.a" 2>/dev/null | awk 'NF >= 3 && $2 ~ /^[TDBSRC]$/ && $3 !~ /^_vibedab_/ {print $3}')"
  [ -z "$unpref" ] || { echo "!! libfaad_dab.a defines unprefixed globals (they would collide with libfaad_drm.a):"; echo "$unpref" | head; exit 1; }
  check_log "$WORK/faadd.log"
  verify_archive "$PREFIX/lib/libfaad_dab.a"
  rm -rf "$WORK/faadd" "$WORK/faadd.log"
  echo "$FAADD_STAMP" > "$PREFIX/.stamp-faad2-dab"
fi

# ── libxaac 0.1.14 (decoder only), static — V12 xHE-AAC in process (docs/v12/LIBXAAC.md) ─────────────────────────────
# ★★ The same pinned release and the same builder as Linux (../linux/build-libxaac.sh). Apache-2.0; LICENSE + NOTICE go
#    into the app's Resources (CMakeLists.txt / build-app.sh) as LICENSE.libxaac / NOTICE.libxaac. Server only — the
#    iPhone/iPad app never links it.
# ★ The C code path ("c" — see build-libxaac.sh): libxaac's AArch64 assembly is written for ELF/GNU as, not Mach-O. The Mac has the CPU.
XAAC_VER=0.1.14
XAAC_SHA=f467a6e5bcf3941ce5d1a4d811442835fba7eb3744ae986f3ea37e110584b611  # GitHub tag tarball = linux/build-deps.sh
XAAC_SCRIPT="$MAC/../linux/build-libxaac.sh"
XAAC_STAMP="libxaac $XAAC_VER $XAAC_SHA min=$MIN arch=$ARCH $CCVER script=$(shasum -a 256 "$XAAC_SCRIPT" | cut -c1-12)"
if stamp_ok libxaac "$XAAC_STAMP" "$PREFIX/lib/libxaacdec.a"; then echo "==> libxaac $XAAC_VER: up to date"; else
  echo "==> libxaac $XAAC_VER: building for macOS $MIN"
  fetch "https://github.com/ittiam-systems/libxaac/archive/refs/tags/v$XAAC_VER.tar.gz" "libxaac-$XAAC_VER.tar.gz" "$XAAC_SHA"
  rm -rf "$WORK/xaac"; mkdir -p "$WORK/xaac"
  tar -xzf "$SRC/libxaac-$XAAC_VER.tar.gz" -C "$WORK/xaac" --strip-components 1
  AR=/usr/bin/ar NM=/usr/bin/nm bash "$XAAC_SCRIPT" "$WORK/xaac" "$PREFIX" c >"$WORK/xaac.log" 2>&1 \
    || { tail -30 "$WORK/xaac.log"; echo "!! libxaac build failed"; exit 1; }
  tail -1 "$WORK/xaac.log"
  check_log "$WORK/xaac.log"
  verify_archive "$PREFIX/lib/libxaacdec.a"
  rm -rf "$WORK/xaac" "$WORK/xaac.log"
  echo "$XAAC_STAMP" > "$PREFIX/.stamp-libxaac"
fi

# ── cloudflared 2026.10.4, compiled ───────────────────────────────────────────────────────────
# ★★★ Cloudflare's darwin release binary is minos 15.0 (their CI links it with a macOS 15 SDK
#     through cgo), so on a macOS 14 Mac the tunnel switch starts nothing. Compiled here with
#     CGO_ENABLED=0 Go links it INTERNALLY and stamps Go's own floor (macOS 13 for Go 1.27), and
#     links nothing but libSystem/libresolv/CoreFoundation/Security — all system. Same source tag,
#     same -mod=readonly as upstream's Makefile (2026.10.x no longer keeps a usable vendor/ — its
#     modules.txt disagrees with go.mod — so modules come from the Go proxy, each checked against the
#     tag's go.sum), same main.Version/BuildTime stamps as upstream's Makefile.
# ★ -s -w strip debug info, as the Android build (tools/cloudflared-android/build.sh) does.
# ★ No self-update concern: directory.cpp always runs it with --no-autoupdate, which matters —
#   a self-updated binary inside a signed bundle would break the bundle's signature.
CF_VER=2026.10.4
CF_SHA=dc3b264da64a801bc3a8d5301a6db21401910ad5                   # refs/tags/2026.10.4^{}
# ★★★ GO 1.27.3 OR NEWER IS THE FIX ITSELF (eMudhra intermediates distrusted, 2026-10-26) — and
#     GOTOOLCHAIN=local below means an older Go fails on go.mod's "go 1.27.3" rather than building.
command -v go >/dev/null || { echo "!! go is not installed (brew install go) — cloudflared must be compiled"; exit 1; }
GOVER="$(GOTOOLCHAIN=local go version)"
CF_STAMP="cloudflared $CF_VER $CF_SHA $GOVER"
if [ "${FORCE:-0}" != 1 ] && [ -x "$CFDIR/cloudflared" ] && [ -f "$CFDIR/.stamp" ] \
   && [ "$(cat "$CFDIR/.stamp")" = "$CF_STAMP" ]; then echo "==> cloudflared $CF_VER: up to date"; else
  echo "==> cloudflared $CF_VER: compiling ($GOVER)"
  git_at https://github.com/cloudflare/cloudflared.git "$CF_SHA" "$WORK/cf"
  CF_DATE="$(TZ=UTC git -C "$WORK/cf" log -1 --format=%cd --date=format-local:'%Y-%m-%d-%H:%M UTC')"
  rm -f "$CFDIR/cloudflared" "$CFDIR/.stamp"
  ( cd "$WORK/cf" && GOCACHE="$WORK/gocache" GOMODCACHE="$WORK/gomod" GOTOOLCHAIN=local GOFLAGS= \
      CGO_ENABLED=0 GOOS=darwin GOARCH="$ARCH" \
      go build -mod=readonly -trimpath \
        -ldflags "-s -w -X \"main.Version=$CF_VER\" -X \"main.BuildTime=$CF_DATE\"" \
        -o "$CFDIR/cloudflared" ./cmd/cloudflared )
  cp "$WORK/cf/LICENSE" "$CFDIR/LICENSE"
  # ★ Prove it: it runs, it is the version we pinned, and it is not above the minimum.
  "$CFDIR/cloudflared" --version | grep -c "version $CF_VER " >/dev/null \
    || { echo "!! compiled cloudflared does not report $CF_VER"; exit 1; }
  CFMIN="$(otool -l "$CFDIR/cloudflared" | awk '/LC_BUILD_VERSION/{b=1} b&&$1=="minos"{print $2;exit}')"
  echo "    cloudflared: $("$CFDIR/cloudflared" --version), minos $CFMIN"
  # ★ GOCACHE is ~400 MB and GOMODCACHE holds a module download cache; neither is needed again.
  chmod -R u+w "$WORK/gomod" 2>/dev/null || true
  rm -rf "$WORK/cf" "$WORK/gocache" "$WORK/gomod"
  echo "$CF_STAMP" > "$CFDIR/.stamp"
fi

rmdir "$WORK" 2>/dev/null || true
echo "==> deps ready in $DEPS (macOS $MIN, $ARCH)"
