#!/usr/bin/env bash
# build-deb.sh — build ONE VibeServer .deb inside a bookworm build container. Every architecture, one recipe.
#
#   build-deb.sh <amd64|i386|armhf|arm64> <rev> <outdir>
#
# Run by scripts/build-debs-remote.sh (x86 on the Lenovo, ARM on the Mac), never by hand on a host — the image IS the build root:
#   amd64 / i386  Dockerfile.build, --platform linux/amd64 / linux/386 (native on an x86-64 CPU)
#   armhf         Dockerfile.cross-armhf on linux/arm64 (Mac) or linux/amd64 (cross either way; carries the ARMv6 build in lib/vibeserver/armv6)
#   arm64         Dockerfile.build on linux/arm64 (native, on the Mac) or Dockerfile.cross-arm64 on linux/amd64 (cross)
# The source is read from /work/VibeSDR (mounted read-only) and COPIED to /build first.
#
# Environment (all optional):
#   JOBS                 compile jobs (default 4 — the Lenovo is a live receiver)
#   VIBE_EXTRA_CFLAGS    extra C/C++ flags (Atom test builds)
#   VIBE_X86_32_FLAGS    replaces the i386 ISA line (see vibeserver/CMakeLists.txt)
#   VIBE_PGO             generate | use — profile-guided builds; the profile directory is mounted at /pgo
#
# ★★★ ONE RECIPE FOR ALL FOUR (2026-10-18). Until today the x86 cmake line lived in build-x86-debs.sh and the
#     arm64/armhf one in publish-apt.sh, run on the Mac. Two copies of a cmake invocation drift, and the one
#     that drifts is the one that quietly stops passing VIBESERVER_STRICT_RADIOS. Now the Lenovo builds every
#     architecture and they all come through here.
# ★★ `set -euo pipefail` AND NO `| tail` ON THE COMPILE: `cmake --build | tail -1` reports TAIL's status —
#    that almost shipped a package built from stale objects (2026-08-26; see publish-apt.sh).
set -euo pipefail

ARCH="${1:?usage: build-deb.sh <amd64|i386|armhf|arm64> <rev> <outdir>}"
REV="${2:?usage: build-deb.sh <arch> <rev> <outdir>}"
OUT="${3:?usage: build-deb.sh <arch> <rev> <outdir>}"
JOBS="${JOBS:-4}"
PGO="${VIBE_PGO:-}"
CF="${VIBE_EXTRA_CFLAGS:-}"
count() { grep -c "$@" || true; }   # ★ never grep -q under pipefail — SIGPIPE turns a match into a failure
fail() { echo "!! [$ARCH] $*"; exit 1; }

# ── what this architecture needs ────────────────────────────────────────────────────────────────
CROSS=(); WANT_ELF=""
case "$ARCH" in
  amd64|i386)
    # ★★ NATIVE: the container's own dpkg architecture must BE the target — a linux/amd64 image handed
    #    `i386` would build a 64-bit binary and CPack would stamp it amd64 (or worse, nothing would notice).
    [ "$(dpkg --print-architecture)" = "$ARCH" ] || fail "this container is $(dpkg --print-architecture), not $ARCH"
    [ "$ARCH" = amd64 ] && WANT_ELF="x86-64" || WANT_ELF="Intel 80386" ;;
  armhf)
    [ -f /opt/armhf-toolchain.cmake ] || fail "not the armhf cross image (Dockerfile.cross-armhf)"
    CROSS=(-DCMAKE_TOOLCHAIN_FILE=/opt/armhf-toolchain.cmake -DVIBE_ARMV6_STAGE=/build/armv6-stage)
    # ★ dpkg-shlibdeps reads these to resolve the TARGET's libc6 rather than the host's.
    export DEB_HOST_ARCH=armhf DEB_HOST_MULTIARCH=arm-linux-gnueabihf DEB_HOST_GNU_TYPE=arm-linux-gnueabihf
    WANT_ELF="ARM, EABI5" ;;
  arm64)
    # ★ TWO WAYS (2026-10-25): NATIVE in Dockerfile.build on Apple Silicon (the normal route again), or CROSS in
    #   Dockerfile.cross-arm64 on the Lenovo (VIBE_ARM_ON_LENOVO=1). The cross image is told apart by its toolchain
    #   file; anything else must itself be arm64, the same rule as the x86 branch above.
    if [ -f /opt/arm64-toolchain.cmake ]; then
      CROSS=(-DCMAKE_TOOLCHAIN_FILE=/opt/arm64-toolchain.cmake)
      export DEB_HOST_ARCH=arm64 DEB_HOST_MULTIARCH=aarch64-linux-gnu DEB_HOST_GNU_TYPE=aarch64-linux-gnu
    else
      [ "$(dpkg --print-architecture)" = arm64 ] || fail "neither the arm64 cross image nor an arm64 container"
    fi
    WANT_ELF="ARM aarch64" ;;
  *) fail "unknown architecture (amd64, i386, armhf, arm64)" ;;
esac
TARGET=()
case "$PGO" in
  "") ;;
  generate) CF="$CF -fprofile-generate=/pgo -fprofile-update=atomic"; TARGET=(--target vibeserver vibeserver-bench) ;;
  use)      CF="$CF -fprofile-use=/pgo -fprofile-partial-training -Wno-missing-profile" ;;
  *) fail "VIBE_PGO is generate or use" ;;
esac
CF="${CF# }"

# ── the copy ────────────────────────────────────────────────────────────────────────────────────
# ★★★ `rm -rf build build-*` IN THE COPY. rsync --delete never deletes an EXCLUDED path on the receiving
#     side, so a vibeserver/build from some earlier hand build on the host survives every sync — and its
#     amd64 CMakeCache.txt was picked up by the first i386 build (2026-10-17). Only the throwaway copy is
#     cleaned; the host's directories are left alone (they may be somebody's).
mkdir -p /build "$OUT"
cp -a /work/VibeSDR /build/VibeSDR
cd /build/VibeSDR/vibeserver
rm -rf build build-*

# ── armhf: the ARMv6 binary first (Pi Zero / Pi 1), packaged into lib/vibeserver/armv6 ─────────
# ★★ Built against the Raspbian sysroot with its own prefix; postinst picks by CPU. Moved here verbatim
#    from publish-apt.sh's armhf branch — that path built it on the Mac.
if [ "$ARCH" = armhf ]; then
  nice -n 19 cmake -S . -B build-armv6 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/armv6-toolchain.cmake \
        -DVIBE_LINUX_DEPS=/opt/vibe-deps-v6 > "$OUT/cmake-armv6.log"
  nice -n 19 cmake --build build-armv6 --target vibeserver -j"$JOBS" > "$OUT/build-armv6.log" 2>&1 \
    || { tail -30 "$OUT/build-armv6.log"; fail "ARMv6 build failed"; }
  [ "$(readelf -A build-armv6/vibeserver | count 'Tag_CPU_arch: v6')" -gt 0 ] || fail "the ARMv6 build is not ARMv6"
  mkdir -p /build/armv6-stage && cp build-armv6/vibeserver /build/armv6-stage/
fi

# ── the package build ───────────────────────────────────────────────────────────────────────────
# ★ The WHOLE tree, not --target vibeserver: cpack's install step builds `all` anyway — single-job and with
#   its output hidden in cpack.log — so a narrow target only moves the cost somewhere slower.
nice -n 19 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVIBESERVER_DEB_REV="$REV" \
      -DVIBESERVER_STRICT_RADIOS=ON "${CROSS[@]}" \
      ${CF:+"-DCMAKE_C_FLAGS=$CF" "-DCMAKE_CXX_FLAGS=$CF"} \
      ${VIBE_X86_32_FLAGS:+"-DVIBE_X86_32_FLAGS=$VIBE_X86_32_FLAGS"} > "$OUT/cmake.log"
nice -n 19 cmake --build build "${TARGET[@]}" -j"$JOBS" > "$OUT/build.log" 2>&1 \
  || { tail -30 "$OUT/build.log"; fail "build failed"; }
# ★★★ THE x86 FIR KERNELS ARE CHECKED WHERE THEY ARE COMPILED (2026-10-18): the SSE2 tap banks exist only
#     in the x86 builds, so this is the one place their output can be compared with the direct convolution
#     before a package carries them. A cross build cannot run its own tests (no emulation, deliberately).
if [ "$PGO" != generate ] && { [ "$ARCH" = amd64 ] || [ "$ARCH" = i386 ]; }; then
  (cd build && nice -n 19 ctest -R '^fir-simd$' --output-on-failure > "$OUT/ctest.log" 2>&1) \
    || { tail -30 "$OUT/ctest.log"; fail "fir-simd test failed"; }
fi
if [ "$PGO" = generate ]; then
  mkdir -p "$OUT/pgogen" && cp build/vibeserver build/vibeserver-bench "$OUT/pgogen/"
  chmod -R a+rwX "$OUT"; exit 0
fi
(cd build && nice -n 19 cpack > "$OUT/cpack.log") || { tail -30 "$OUT/cpack.log"; fail "cpack failed"; }
DEB="$(ls build/vibeserver_*_"$ARCH".deb 2>/dev/null | head -1)"
[ -n "$DEB" ] || fail "no vibeserver_*_$ARCH.deb came out of cpack ($(ls build/*.deb 2>/dev/null | tr '\n' ' '))"

# ── the gates — the same ones publish-apt.sh runs, so a bad package is stopped before it travels ──
[ "$(dpkg-deb -f "$DEB" Architecture)" = "$ARCH" ] || fail "package says Architecture: $(dpkg-deb -f "$DEB" Architecture)"
[ "$(dpkg-deb -f "$DEB" Version)" = "$(dpkg-deb -f "$DEB" Version | sed 's/-[^-]*$//')-$REV" ] \
  || fail "package version $(dpkg-deb -f "$DEB" Version) does not end in -$REV"
[ "$(dpkg-deb --fsys-tarfile "$DEB" | count -a 'First-time setup')" -gt 0 ] || fail "no settings wizard in the binary"
DEPENDS="$(dpkg-deb -f "$DEB" Depends)"
if echo "$DEPENDS" | tr , '\n' | grep -E 'libc6 \(>= 2\.(3[6-9]|[4-9])|libstdc\+\+6 \(>= 1[3-9]'; then
  fail "needs a newer glibc than 2.35 (Ubuntu 22.04)"
fi
# ★ Nothing but libc6 and the exec()d tools (2026-10-18) — every library is linked in.
X="$(echo "$DEPENDS" | tr , '\n' | sed 's/^ *//; s/ *(.*$//' | grep -vxE 'adduser|curl|gzip|libc6' || true)"
[ -z "$X" ] || fail "depends on system libraries: $X"

# ★★★ EVERY ELF IN THE PACKAGE IS FOR THE PACKAGE'S MACHINE. A cross build is exactly where a host binary
#     slips in (a helper built with the wrong compiler, the wrong cloudflared) — and nothing else would see
#     it until a Pi said "Exec format error".
# ★★ AND NO GLIBC SYMBOL VERSION ABOVE 2.35, read from the ELF version-needs (readelf -V) rather than from
#    Depends — the same floor check-deb-depends.cmake enforces, asked of the binaries themselves.
X="$(mktemp -d)"; dpkg-deb -x "$DEB" "$X"
ELVES=0; MAXG="none"
while IFS= read -r f; do
  d="$(file -b "$f")"
  case "$d" in ELF*) ;; *) continue ;; esac
  ELVES=$((ELVES + 1))
  case "$d" in *"$WANT_ELF"*) ;; *) fail "${f#"$X"} is not $WANT_ELF: $d" ;; esac
  g="$(readelf -V "$f" 2>/dev/null | grep -oE 'Name: GLIBC_[0-9.]+' | sed 's/Name: GLIBC_//' | sort -V | tail -1 || true)"
  if [ -n "$g" ]; then
    [ "$(printf '%s\n2.35\n' "$g" | sort -V | tail -1)" = "2.35" ] || fail "${f#"$X"} needs GLIBC_$g (> 2.35)"
    [ "$MAXG" = none ] && MAXG="$g" || MAXG="$(printf '%s\n%s\n' "$MAXG" "$g" | sort -V | tail -1)"
  fi
done < <(find "$X" -type f)
[ "$ELVES" -gt 0 ] || fail "no ELF binaries in the package"
# ★★★ THE BUNDLED RSP DRIVER IS IN IT, AND IS THE REAL THING (OpenRSP, 2026-10-21). Not on i386 (OpenRSP does not
#     build for i386 Linux yet — see vibeserver/CMakeLists.txt). Three ways it could be present and useless:
#     firmware that is not what its manifest says, a service compiled against the USB STUB (libusb not found —
#     it then finds no receiver, ever), and a library missing API entry points.
ORSP="$X/usr/lib/vibeserver/openrsp"
ORSP_LINE="none (i386)"
if [ "$ARCH" != i386 ]; then
  for f in libsdrplay_api.so.3.15 libsdrplay_api.so.3 sdrplay_apiService firmware/own/manifest.sha256; do
    [ -e "$ORSP/$f" ] || fail "no usr/lib/vibeserver/openrsp/$f in the package"
  done
  (cd "$ORSP/firmware/own" && sha256sum -c --quiet manifest.sha256) || fail "the RSP firmware does not match its manifest"
  [ "$(strings "$ORSP/sdrplay_apiService" | count libusb)" -gt 20 ] || fail "the RSP service has no libusb in it (USB stub?)"
  # ★ The C entry points sdrplay_source.cpp resolves (OpenRSP 3.15.2-20 exports 23 plain sdrplay_api_* of its 101).
  EXPORTS="$(readelf -W --dyn-syms "$ORSP/libsdrplay_api.so.3.15" | awk '$5=="GLOBAL" && $7!="UND" {print $8}')"
  for s in Open Close GetDevices SelectDevice ReleaseDevice GetDeviceParams Init Uninit Update \
           LockDeviceApi UnlockDeviceApi GetErrorString ApiVersion; do
    [ "$(echo "$EXPORTS" | count -x "sdrplay_api_$s")" -eq 1 ] || fail "libsdrplay_api.so.3.15 does not export sdrplay_api_$s"
  done
  NAPI="$(echo "$EXPORTS" | count '^sdrplay_api_')"
  [ -f "$X/usr/share/doc/vibeserver/COPYING.openrsp" ] || fail "OpenRSP's licence is not in the package"
  [ -f "$X/usr/share/doc/vibeserver/COPYING.faad2" ] || fail "FAAD2's licence is not in the package (V12 DRM links it)"
  ORSP_LINE="library ($NAPI sdrplay_api_ exports), service, firmware $(cut -c1-12 "$ORSP/firmware/own/manifest.sha256" | tr '\n' ' ')"
fi
# ★★★ libxaac IS LINKED IN, WITH ITS LICENCE (V12, 2026-10-29 — docs/v12/LIBXAAC.md). Without it xHE-AAC goes silent;
#     without LICENSE.libxaac + NOTICE.libxaac we would be shipping Apache-2.0 code without its terms (§4(a), §4(d)).
#     The binary is stripped, so the proof is the version string vibe_xaac.h builds into the listener's "decoded by" line.
for f in LICENSE.libxaac NOTICE.libxaac; do
  [ -f "$X/usr/share/doc/vibeserver/$f" ] || fail "no usr/share/doc/vibeserver/$f — libxaac's licence/notice"
done
XAAC_LINE="$(strings "$X/usr/bin/vibeserver" | grep -oE 'libxaac [0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
[ -n "$XAAC_LINE" ] || fail "usr/bin/vibeserver has no libxaac in it (xHE-AAC would be silent)"
# ★ and no FFmpeg left over from early V12: the package must not carry, or remove on upgrade, anything we no longer build
[ ! -e "$X/usr/lib/vibeserver/ffmpeg" ] || fail "usr/lib/vibeserver/ffmpeg is in the package — the FFmpeg bundle is gone (libxaac)"
{
  echo "package   $(basename "$DEB")"
  echo "Depends   $DEPENDS"
  echo "ELF files $ELVES, all $WANT_ELF; highest GLIBC_ version needed: $MAXG"
  echo "vibeserver: $(file -b "$X/usr/bin/vibeserver" 2>/dev/null || echo '?')"
  echo "OpenRSP   $ORSP_LINE"
  echo "libxaac   $XAAC_LINE (in process)"
  case "$ARCH" in
    armhf) echo "  $(readelf -A "$X/usr/bin/vibeserver" | grep -E 'Tag_CPU_arch:|Tag_Advanced_SIMD_arch:|Tag_FP_arch:' | tr -s ' ' | tr '\n' ' ')"
           [ -f "$X/usr/lib/vibeserver/armv6/vibeserver" ] || fail "no ARMv6 binary in lib/vibeserver/armv6"
           echo "  armv6: $(readelf -A "$X/usr/lib/vibeserver/armv6/vibeserver" | grep -E 'Tag_CPU_arch:' | tr -s ' ')" ;;
  esac
} | tee "$OUT/build-info.txt"
rm -rf "$X"
cp "$DEB" "$OUT/"
# ★ The container runs as root; a root-owned file or directory under ~/vibeout stops the next run from
#   emptying it (rm -rf), and the && chain over there would then skip the whole build without saying so.
chmod -R a+rwX "$OUT"
echo "==> [$ARCH] $(basename "$DEB")"
