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
#      cloudflared 2026.8.0 — the version tools/cloudflared-desktop/fetch.sh pins, but COMPILED
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
RTL_STAMP="librtlsdr $RTL_SHA libusb=$USB_VER min=$MIN arch=$ARCH $CCVER"
if stamp_ok librtlsdr "$RTL_STAMP" "$PREFIX/lib/librtlsdr.a"; then echo "==> librtlsdr 2.0.3: up to date"; else
  echo "==> librtlsdr 2.0.3 ($RTL_SHA): building for macOS $MIN"
  git_at https://github.com/osmocom/rtl-sdr.git "$RTL_SHA" "$WORK/rtl"
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

# ── cloudflared 2026.8.0, compiled ───────────────────────────────────────────────────────────
# ★★★ Cloudflare's darwin release binary is minos 15.0 (their CI links it with a macOS 15 SDK
#     through cgo), so on a macOS 14 Mac the tunnel switch starts nothing. Compiled here with
#     CGO_ENABLED=0 Go links it INTERNALLY and stamps Go's own floor (macOS 13 for Go 1.27), and
#     links nothing but libSystem/libresolv/CoreFoundation/Security — all system. Same source tag,
#     same -mod=vendor (cloudflared vendors every dependency, so nothing is downloaded but the
#     tree itself), same main.Version/BuildTime stamps as upstream's Makefile.
# ★ -s -w strip debug info, as the Android build (tools/cloudflared-android/build.sh) does.
# ★ No self-update concern: directory.cpp always runs it with --no-autoupdate, which matters —
#   a self-updated binary inside a signed bundle would break the bundle's signature.
CF_VER=2026.8.0
CF_SHA=aba66df755b17b8e46c0b716ea69796b9b570758                   # refs/tags/2026.8.0^{}
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
      go build -mod=vendor -trimpath \
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
