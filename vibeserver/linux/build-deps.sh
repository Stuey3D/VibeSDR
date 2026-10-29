#!/usr/bin/env bash
# build-deps.sh — every library the Linux VibeServer links, FROM PINNED SOURCE, as static archives.
#
#   build-deps.sh <prefix> [native|armhf|armv6|arm64]
#
# Run inside the build images (Dockerfile.build, Dockerfile.cross-armhf, Dockerfile.cross-arm64). Output:
#   <prefix>/lib/{libusb-1.0.a,libopus.a,librtlsdr.a,libfaad_drm.a,libfaad_dab.a,libxaacdec.a}
#   <prefix>/include/{libusb-1.0/libusb.h,opus/*.h,rtl-sdr.h,rtl-sdr_export.h}
#   <prefix>/BUILT-FROM                  what went in, one line per library
# — the same layout vibeserver/mac/build-deps.sh produces, so CMake treats the two prefixes alike.
#
# ★★★ WHY (Stuart, 2026-10-18): the package must need NOTHING from the system for SDR support or
#     libraries — only glibc and the SDRplay API (closed, dlopen'd, the owner installs it). xHE-AAC is libxaac,
#     linked in (V12, docs/v12/LIBXAAC.md), as is FAAD2 for DRM AAC and DAB+; VibeServer uses no FFmpeg (2026-10-29).
#     Linux used to link Debian's libusb, libopus, libhackrf, ncurses and the
#     C++ runtime, and dpkg-shlibdeps copied the BUILD image's versions into Depends — so the one
#     package refused Ubuntu 22.04 over a libhackrf0 (>= 2022.09.1) whose every call we use is
#     years older. A radio driver should arrive on OUR release cycle, not a distribution's.
# ★★ SAME VERSIONS AS THE MAC (vibeserver/mac/build-deps.sh), on purpose: one libusb, one libopus,
#    one librtlsdr commit for every desktop. Move a pin there and here together.
#      libusb   1.0.30   — built WITHOUT udev (see below)
#      libopus  1.6.1
#      librtlsdr osmocom/rtl-sdr 797f814 = v2.0.3 (RTL-SDR Blog V4L)
#    Compiled into the core from the vendored tree instead, by CMakeLists.txt: libairspyhf,
#    libairspy, libhackrf 2026.01.3. SDRplay is dlopen()ed, never linked.
# ★★★ libusb --disable-udev. Debian's libusb-1.0.a needs libudev's symbols, which is why a static
#     link failed on the Pi in 2026-07 and the Linux build went shared in the first place — and a
#     shared libusb means libudev1 in Depends. Without udev libusb enumerates through sysfs and
#     watches hotplug over NETLINK, which is what it does on every system without libudev; a
#     dongle plugged in later is still seen.
# ★★ librtlsdr STATIC, not the .so we used to ship in /usr/lib/vibeserver. That .so existed only
#    because a static librtlsdr needs a static libusb, and Debian's could not be linked statically.
#    With our own libusb there is nothing left to share: one binary, no RPATH, no private library
#    dir for dpkg-shlibdeps to be told about, and the ARMv6 build is one file too.
# ★ The CPU flags are the PACKAGE's floor, matching vibeserver/CMakeLists.txt: i386 SSE2 (-march=i686
#   -msse2 -mfpmath=sse), armhf ARMv7 NEON in ARM mode, ARMv6 scalar VFPv2. Opus keeps its own
#   runtime CPU detection where it has one (x86 SSE4.1/AVX2, ARM NEON), so a better CPU still gets it.
set -euo pipefail

PREFIX="${1:?usage: build-deps.sh <prefix> [native|armhf|armv6|arm64]}"
PROFILE="${2:-native}"
JOBS="${JOBS:-$(nproc)}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

USB_VER=1.0.30
USB_SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf    # = mac/build-deps.sh
OPUS_VER=1.6.1
OPUS_SHA=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1   # = mac/build-deps.sh
RTL_SHA=797f8143266d983c56d8f35d2d442527529dd8a5                             # = mac/build-deps.sh
FAAD_VER=2.11.3
FAAD_SHA=860ab62087e336c1844a70e33196c1790b525fb9a9e7b6ac4fab1a1a4e4d5ce8   # = mac/build-deps.sh (GitHub tag tarball)
XAAC_VER=0.1.14
XAAC_SHA=f467a6e5bcf3941ce5d1a4d811442835fba7eb3744ae986f3ea37e110584b611   # = mac/build-deps.sh (GitHub tag tarball, = commit 2fbadd57)

# ── the toolchain, per profile ───────────────────────────────────────────────────────────────
HOSTOPT=(); CMAKE_TC=(); OPUS_EXTRA=()
case "$PROFILE" in
  native)
    export CC=gcc AR=ar RANLIB=ranlib NM=nm
    case "$(dpkg --print-architecture)" in
      # ★ The i386 package's SSE2 floor — VIBE_X86_32_FLAGS' default in CMakeLists.txt. Debian's i386
      #   compiler would otherwise build x87 floating point, slower AND not what the binary assumes.
      i386) ARCHFLAGS="-march=i686 -msse2 -mfpmath=sse -mtune=generic" ;;
      *)    ARCHFLAGS="" ;;
    esac
    WANT_FILE="" ;;
  armhf)
    # ★ ARMv7 + NEON, ARM mode — what armhf-toolchain.cmake and the armv7 branch of CMakeLists.txt give
    #   the server itself (ARM mode measured faster than Thumb-2 on the Pi 2).
    export CC=arm-linux-gnueabihf-gcc AR=arm-linux-gnueabihf-ar RANLIB=arm-linux-gnueabihf-ranlib NM=arm-linux-gnueabihf-nm
    ARCHFLAGS="-marm -mfpu=neon-vfpv4"
    HOSTOPT=(--host=arm-linux-gnueabihf)
    CMAKE_TC=(-DCMAKE_TOOLCHAIN_FILE=/opt/armhf-toolchain.cmake)
    WANT_FILE="ARM, EABI5" ;;
  armv6)
    # ★★ clang against the RASPBIAN sysroot, exactly as armv6-toolchain.cmake: Debian's armhf crt/libgcc
    #    are ARMv7 Thumb-2 and an ARMv6 core dies on the first of them.
    # ★ SCALAR opus: no asm, no intrinsics, no runtime detection — there is no NEON to detect, and the
    #   ARMv6 media-instruction asm is not worth a second assembler path we cannot test on a Pi Zero.
    S=/opt/rpi-sysroot
    export CC="clang --target=arm-linux-gnueabihf -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm --gcc-toolchain=$S/usr --sysroot=$S"
    export AR=arm-linux-gnueabihf-ar RANLIB=arm-linux-gnueabihf-ranlib NM=arm-linux-gnueabihf-nm LDFLAGS="-fuse-ld=lld"
    ARCHFLAGS=""
    HOSTOPT=(--host=arm-linux-gnueabihf)
    CMAKE_TC=(-DCMAKE_TOOLCHAIN_FILE=/opt/armv6-toolchain.cmake)
    OPUS_EXTRA=(--disable-asm --disable-intrinsics --disable-rtcd)
    WANT_FILE="ARM, EABI5" ;;
  arm64)
    # ★★ arm64 CROSS from the Lenovo's amd64 container (2026-10-18) — arm64-toolchain.cmake. NO arch flags:
    #    the native arm64 build had none, so this is Debian's armv8-a default exactly as before (the
    #    Cortex-A53 floor). Opus's configure finds aarch64 NEON intrinsics the same way it did natively.
    export CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar RANLIB=aarch64-linux-gnu-ranlib NM=aarch64-linux-gnu-nm
    ARCHFLAGS=""
    HOSTOPT=(--host=aarch64-linux-gnu)
    CMAKE_TC=(-DCMAKE_TOOLCHAIN_FILE=/opt/arm64-toolchain.cmake)
    WANT_FILE="ARM aarch64" ;;
  *) echo "!! profile is native, armhf, armv6 or arm64"; exit 1 ;;
esac
export CFLAGS="-O2 $ARCHFLAGS"
# ★ pkg-config sees NOTHING: a configure script must not find a system library and quietly use it.
export PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH=""
CCVER="$($CC --version | head -1)"
echo "==> deps ($PROFILE) into $PREFIX — $CCVER, CFLAGS='$CFLAGS'"
rm -rf "$PREFIX"; mkdir -p "$PREFIX/lib" "$PREFIX/include"

fetch() {   # fetch <url> <file> <sha256>
  curl -fsSL --retry 3 "$1" -o "$WORK/$2"
  local got; got="$(sha256sum "$WORK/$2" | awk '{print $1}')"
  [ "$got" = "$3" ] || { echo "!! SHA-256 MISMATCH for $2: got $got, pinned $3 — refusing"; exit 1; }
}
count() { grep -c "$@" || true; }   # ★ never grep -q under pipefail — SIGPIPE turns a match into a failure

# ── libusb 1.0.30, no udev ───────────────────────────────────────────────────────────────────
fetch "https://github.com/libusb/libusb/releases/download/v$USB_VER/libusb-$USB_VER.tar.bz2" \
      "libusb.tar.bz2" "$USB_SHA"
mkdir -p "$WORK/usb" && tar -xjf "$WORK/libusb.tar.bz2" -C "$WORK/usb" --strip-components 1
( cd "$WORK/usb" && ./configure "${HOSTOPT[@]}" --prefix="$WORK/usb/inst" --disable-shared --enable-static \
      --disable-udev --disable-examples-build --disable-tests-build --disable-dependency-tracking \
      >"$WORK/usb.log" 2>&1 \
  && make -j"$JOBS" install >>"$WORK/usb.log" 2>&1 ) || { tail -40 "$WORK/usb.log"; echo "!! libusb build failed"; exit 1; }
cp "$WORK/usb/inst/lib/libusb-1.0.a" "$PREFIX/lib/"
mkdir -p "$PREFIX/include/libusb-1.0" && cp "$WORK/usb/inst/include/libusb-1.0/libusb.h" "$PREFIX/include/libusb-1.0/"
# ★★ PROVE the udev half is out: one undefined udev_* symbol is a link failure (or libudev1 in Depends).
U="$("$NM" -A "$PREFIX/lib/libusb-1.0.a" 2>/dev/null | count -E ' U udev_')"
[ "$U" -eq 0 ] || { echo "!! libusb-1.0.a still references udev ($U symbols) — refusing"; exit 1; }
N="$("$NM" -A "$PREFIX/lib/libusb-1.0.a" 2>/dev/null | count -E ' T linux_netlink_start_event_monitor')"
[ "$N" -gt 0 ] || { echo "!! libusb-1.0.a has no netlink hotplug monitor — refusing"; exit 1; }

# ── libopus 1.6.1 ────────────────────────────────────────────────────────────────────────────
fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz" "opus.tar.gz" "$OPUS_SHA"
mkdir -p "$WORK/opus" && tar -xzf "$WORK/opus.tar.gz" -C "$WORK/opus" --strip-components 1
( cd "$WORK/opus" && ./configure "${HOSTOPT[@]}" --prefix="$WORK/opus/inst" --disable-shared --enable-static \
      --disable-doc --disable-extra-programs --disable-dependency-tracking "${OPUS_EXTRA[@]}" \
      >"$WORK/opus.log" 2>&1 \
  && make -j"$JOBS" install >>"$WORK/opus.log" 2>&1 ) || { tail -40 "$WORK/opus.log"; echo "!! libopus build failed"; exit 1; }
cp "$WORK/opus/inst/lib/libopus.a" "$PREFIX/lib/"
mkdir -p "$PREFIX/include/opus" && cp "$WORK/opus/inst/include/opus/"*.h "$PREFIX/include/opus/"

# ── librtlsdr 2.0.3 (osmocom/rtl-sdr 797f814), static, against the libusb above ───────────────
git init -q "$WORK/rtl"
git -C "$WORK/rtl" fetch -q --depth 1 https://github.com/osmocom/rtl-sdr.git "$RTL_SHA"
git -C "$WORK/rtl" checkout -q FETCH_HEAD
[ "$(git -C "$WORK/rtl" rev-parse HEAD)" = "$RTL_SHA" ] || { echo "!! rtl-sdr is not at $RTL_SHA — refusing"; exit 1; }
# ★★ OUR ONE PATCH TO librtlsdr (V12, 2026-10-24): rtlsdr_read_async's cancel waits for every transfer to come home
#    before freeing any (upstream freed them with URBs still in the kernel; the next control transfer could then lock a
#    destroyed mutex — the usbi_mutex_lock abort seen on live rate changes in September). Not reproducible since on any
#    host (500+ live rate changes, 2026-10-24), so it is a GUARD, not a proven fix. See librtlsdr-cancel-wait.patch.
git -C "$WORK/rtl" apply "$(dirname "$0")/librtlsdr-cancel-wait.patch" || { echo "!! librtlsdr-cancel-wait.patch did not apply"; exit 1; }
# ★ libusb BY PATH, pkg-config disabled — the same arrangement as the Mac build, so it cannot resolve
#   to a system libusb. DETACH_KERNEL_DRIVER as the shipped .so had it (a dongle the DVB driver holds).
CC_ONLY="${CC%% *}"   # cmake wants the compiler alone; the cross profiles' flags come from the toolchain file
# ★★ CMAKE_C_FLAGS ONLY WHEN THERE ARE ARCH FLAGS. Given on the command line it REPLACES the toolchain
#    file's CMAKE_C_FLAGS_INIT — for ARMv6 that is the --target/--sysroot line, and without it clang
#    compiled for the aarch64 host (found on the first ARMv6 run). -O2 reaches CMake through $CFLAGS,
#    which it folds in AHEAD of the init flags; armhf's ARCHFLAGS repeat the toolchain's -marm.
cmake -S "$WORK/rtl" -B "$WORK/rtl/b" -DCMAKE_BUILD_TYPE=Release "${CMAKE_TC[@]}" \
      $([ "$PROFILE" = native ] && echo "-DCMAKE_C_COMPILER=$CC_ONLY") \
      ${ARCHFLAGS:+-DCMAKE_C_FLAGS="$ARCHFLAGS"} -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
      -DLIBUSB_INCLUDE_DIRS="$PREFIX/include/libusb-1.0" -DLIBUSB_LIBRARIES="$PREFIX/lib/libusb-1.0.a" \
      -DDETACH_KERNEL_DRIVER=ON >"$WORK/rtl.log" 2>&1 \
  && cmake --build "$WORK/rtl/b" --target rtlsdr_static -j"$JOBS" >>"$WORK/rtl.log" 2>&1 \
  || { tail -40 "$WORK/rtl.log"; echo "!! librtlsdr build failed"; exit 1; }
cp "$WORK/rtl/b/src/librtlsdr.a" "$PREFIX/lib/"
cp "$WORK/rtl/include/rtl-sdr.h" "$WORK/rtl/include/rtl-sdr_export.h" "$PREFIX/include/"
# ★★ The FEATURE, not the file: V4L support is the reason we own librtlsdr at all.
[ "$(strings "$PREFIX/lib/librtlsdr.a" | count 'Blog V4L')" -gt 0 ] \
  || { echo "!! librtlsdr has no RTL-SDR Blog V4L support — refusing"; exit 1; }
[ "$(strings "$PREFIX/lib/librtlsdr.a" | count 'Detached kernel driver')" -gt 0 ] \
  || { echo "!! librtlsdr was built without DETACH_KERNEL_DRIVER — refusing"; exit 1; }

# ── FAAD2 2.11.3, its DRM build (libfaad_drm), static — V12 DRM's AAC, decoded on the server ─────────────────
# ★★★ GPL-2.0-or-later. The server is GPL and may link it; the App Store app never does (build_ios.sh checks). Its
#     COPYING travels with the package (CMakeLists.txt installs share/licences/COPYING.faad2 into /usr/share/doc).
# ★ Float, not FAAD2's fixed-point variant: every profile has an FPU (ARMv6 VFPv2 included), and the float decoder is the
#   one measured (docs/v12/DRM-AUDIO-ROUTES.md). DRC off (DRM's AAC carries none); no CLI.
fetch "https://github.com/knik0/faad2/archive/refs/tags/$FAAD_VER.tar.gz" "faad2.tar.gz" "$FAAD_SHA"
mkdir -p "$WORK/faad" && tar -xzf "$WORK/faad2.tar.gz" -C "$WORK/faad" --strip-components 1
cmake -S "$WORK/faad" -B "$WORK/faad/b" -DCMAKE_BUILD_TYPE=Release "${CMAKE_TC[@]}" \
      $([ "$PROFILE" = native ] && echo "-DCMAKE_C_COMPILER=$CC_ONLY") \
      ${ARCHFLAGS:+-DCMAKE_C_FLAGS="$ARCHFLAGS"} -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DFAAD_APPLY_DRC=OFF \
      >"$WORK/faad.log" 2>&1 \
  && cmake --build "$WORK/faad/b" --target faad_drm -j"$JOBS" >>"$WORK/faad.log" 2>&1 \
  || { tail -40 "$WORK/faad.log"; echo "!! FAAD2 build failed"; exit 1; }
cp "$WORK/faad/b/libfaad_drm.a" "$PREFIX/lib/"
cp "$WORK/faad/include/neaacdec.h" "$PREFIX/include/"
mkdir -p "$PREFIX/share/licences" && cp "$WORK/faad/COPYING" "$PREFIX/share/licences/COPYING.faad2"
# ★ The FEATURE, not the file: the DRM entry point must be in it (a plain libfaad would build and play nothing).
[ "$("$NM" "$PREFIX/lib/libfaad_drm.a" 2>/dev/null | count 'T .*NeAACDecInitDRM')" -gt 0 ] \
  || { echo "!! libfaad_drm.a has no NeAACDecInitDRM — refusing"; exit 1; }

# ── FAAD2 2.11.3 AGAIN, its PLAIN build, every global renamed vibedab_ — V12 DAB+ audio in process ──────────────
# ★★★ THE DRM BUILD REFUSES DAB+ ("No standard extension payload allowed in DRM", measured 2026-10-26), and the plain
#     build exports exactly the same names, so the two cannot be linked side by side as they are. The plain one is
#     compiled with a FORCED INCLUDE that #defines every global it would export to vibedab_<name> — the list is read
#     off a first, unprefixed plain build — and the result is CHECKED: every defined global
#     in libfaad_dab.a must carry the prefix, or a DRM and a DAB+ decoder in one process would share code by accident.
#     vibe_dab_faad.h declares the six entry points it calls under their prefixed names. Same GPL-2.0-or-later licence
#     and COPYING as the DRM build above; same float decoder, DRC off, no CLI.
# ★★ V12 (2026-10-26, Stuart's Pi 2 — "FAAD2 · 11.9 % of a core" against FFmpeg's 6–10 %): THE DAB+ BUILD IS OPTIMISED.
#   1. -funsafe-math-optimizations: GCC will not put float loops on ARMv7 NEON without it (NEON flushes denormals, so
#      it is "not IEEE"), and reassociation lets reductions vectorise everywhere. Measured on the real unit logs: armhf
#      guest instructions −23.5 %; output against the stock build max |diff| 1.6e-6, SNR 132.5 dB, level and every
#      third octave identical to 0.0001 dB (docs/v12/DRM-AUDIO-ROUTES.md "DAB+ on its own thread").
#   2. VibeSDR's FAAD2 PATCH SERIES (faad2-patches/series): NEON (ARMv7 + AArch64) / SSE2 / scalar versions of the hot
#      stages, each documented in its own header, applied here in order — published with the source, never a silent
#      fork. Applied to the plain (DAB+) build only, AFTER the DRM build above has been made from the untouched tree.
FAAD_DAB_OPT="-funsafe-math-optimizations"
FAAD_PATCHES="$(cd "$(dirname "$0")" && pwd)/faad2-patches"
FAAD_SERIES=""
if [ -f "$FAAD_PATCHES/series" ]; then
  while IFS= read -r p; do
    case "$p" in ""|\#*) continue ;; esac
    patch -d "$WORK/faad" -p1 -s --no-backup-if-mismatch < "$FAAD_PATCHES/$p" || { echo "!! FAAD2 patch $p did not apply"; exit 1; }
    FAAD_SERIES="$FAAD_SERIES $p"
  done < "$FAAD_PATCHES/series"
fi
echo "==> FAAD2 (DAB+): $FAAD_DAB_OPT; patches:${FAAD_SERIES:- none}"
# ★ TWO PASSES: the plain build defines globals the DRM one does not (LTP, MAIN prediction — measured on the Mac), so
#   the list is read off a first, unprefixed plain build; the second, prefixed build is the one kept.
cmake -S "$WORK/faad" -B "$WORK/faad/b0" -DCMAKE_BUILD_TYPE=Release "${CMAKE_TC[@]}" \
      $([ "$PROFILE" = native ] && echo "-DCMAKE_C_COMPILER=$CC_ONLY") \
      ${ARCHFLAGS:+-DCMAKE_C_FLAGS="$ARCHFLAGS"} -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DFAAD_APPLY_DRC=OFF \
      >"$WORK/faad-dab.log" 2>&1 \
  && cmake --build "$WORK/faad/b0" --target faad -j"$JOBS" >>"$WORK/faad-dab.log" 2>&1 \
  || { tail -40 "$WORK/faad-dab.log"; echo "!! FAAD2 (plain, first pass) failed"; exit 1; }
{ echo "/* generated by build-deps.sh from the plain libfaad.a — FAAD2 $FAAD_VER, globals renamed for the DAB+ build */"
  "$NM" -g --defined-only "$WORK/faad/b0/libfaad.a" 2>/dev/null | awk 'NF >= 3 && $2 ~ /^[TDBSRC]$/ {print $3}' | sort -u \
    | awk '{printf "#define %s vibedab_%s\n", $1, $1}'; } > "$WORK/faad_dab_prefix.h"
[ "$(count '^#define' "$WORK/faad_dab_prefix.h")" -gt 50 ] || { echo "!! FAAD2 symbol list is short — refusing"; exit 1; }
# ★ The forced include goes in through the ENVIRONMENT's CFLAGS (which CMake ADDS to a toolchain's own flags), not
#   -DCMAKE_C_FLAGS (which REPLACES them): the ARMv6 toolchain carries --target there, and replacing it built x86 objects.
CFLAGS="$CFLAGS $FAAD_DAB_OPT -include $WORK/faad_dab_prefix.h" \
cmake -S "$WORK/faad" -B "$WORK/faad/bd" -DCMAKE_BUILD_TYPE=Release "${CMAKE_TC[@]}" \
      $([ "$PROFILE" = native ] && echo "-DCMAKE_C_COMPILER=$CC_ONLY") \
      -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF \
      -DFAAD_APPLY_DRC=OFF >>"$WORK/faad-dab.log" 2>&1 \
  && cmake --build "$WORK/faad/bd" --target faad -j"$JOBS" >>"$WORK/faad-dab.log" 2>&1 \
  || { tail -40 "$WORK/faad-dab.log"; echo "!! FAAD2 (DAB+ build) failed"; exit 1; }
cp "$WORK/faad/bd/libfaad.a" "$PREFIX/lib/libfaad_dab.a"
# ★ The FEATURE: the prefixed entry point is there, and NOTHING it defines is unprefixed.
[ "$("$NM" "$PREFIX/lib/libfaad_dab.a" 2>/dev/null | count 'T vibedab_NeAACDecDecode$')" -gt 0 ] \
  || { echo "!! libfaad_dab.a has no vibedab_NeAACDecDecode — refusing"; exit 1; }
# ★ i386 (2026-10-27): GCC's PIC helpers __x86.get_pc_thunk.{ax,bx,cx,dx,si,di,bp} are emitted into EVERY 32-bit x86
#   object as identical hidden COMDAT copies the linker folds into one — they cannot collide, so they are not FAAD2's to
#   prefix. Without this the i386 image refused to build and beta22 published nothing.
unpref="$("$NM" -g --defined-only "$PREFIX/lib/libfaad_dab.a" 2>/dev/null | awk 'NF >= 3 && $2 ~ /^[TDBSRC]$/ && $3 !~ /^vibedab_/ && $3 !~ /^__x86\.get_pc_thunk\./ {print $3}')"
[ -z "$unpref" ] || { echo "!! libfaad_dab.a defines unprefixed globals (they would collide with libfaad_drm.a):"; echo "$unpref" | head; exit 1; }

# ── libxaac 0.1.14 (decoder only), static — V12 xHE-AAC, decoded in process (docs/v12/LIBXAAC.md) ─────────────────
# ★★ Apache-2.0 (compatible with VibeServer's GPL-3.0); LICENSE + NOTICE travel with the package as LICENSE.libxaac and
#    NOTICE.libxaac (CMakeLists.txt installs them). Unmodified. Built by build-libxaac.sh, shared with the Mac.
# ★ Which of libxaac's code paths: armhf its ARMv7 NEON assembly (through clang's assembler — see build-libxaac.sh),
#   arm64 its AArch64 NEON assembly, amd64 / i386 / ARMv6 its C (it has no x86 SIMD; ARMv6 has no NEON).
fetch "https://github.com/ittiam-systems/libxaac/archive/refs/tags/v$XAAC_VER.tar.gz" "libxaac.tar.gz" "$XAAC_SHA"
mkdir -p "$WORK/xaac" && tar -xzf "$WORK/libxaac.tar.gz" -C "$WORK/xaac" --strip-components 1
case "$PROFILE" in
  armhf) XAAC_VARIANT=armv7; XAAC_ASCC="clang --target=arm-linux-gnueabihf -march=armv7-a -mfpu=neon-vfpv4" ;;
  arm64) XAAC_VARIANT=armv8; XAAC_ASCC="$CC" ;;
  native) if [ "$(dpkg --print-architecture)" = arm64 ]; then XAAC_VARIANT=armv8; else XAAC_VARIANT=c; fi; XAAC_ASCC="$CC" ;;
  *)     XAAC_VARIANT=c; XAAC_ASCC="$CC" ;;   # armv6 (and amd64 / i386 through "native")
esac
ASCC="$XAAC_ASCC" bash "$(cd "$(dirname "$0")" && pwd)/build-libxaac.sh" "$WORK/xaac" "$PREFIX" "$XAAC_VARIANT" >"$WORK/xaac.log" 2>&1 \
  || { tail -30 "$WORK/xaac.log"; echo "!! libxaac build failed"; exit 1; }
tail -1 "$WORK/xaac.log"

# ── every object for the architecture the profile promises ───────────────────────────────────
X="$WORK/x"; mkdir -p "$X"
for a in libusb-1.0 libopus librtlsdr libfaad_drm libfaad_dab libxaacdec; do
  rm -rf "$X"/*; ( cd "$X" && "$AR" x "$PREFIX/lib/$a.a" )
  if [ -n "$WANT_FILE" ]; then
    bad="$(file "$X"/*.o | count -v "$WANT_FILE")"
    [ "$bad" -eq 0 ] || { file "$X"/*.o | grep -v "$WANT_FILE" | head -3; echo "!! $a.a: $bad objects are not $WANT_FILE"; exit 1; }
  fi
  if [ "$PROFILE" = armv6 ]; then
    for o in "$X"/*.o; do
      [ "$(readelf -A "$o" | count -E 'Tag_CPU_arch: v6')" -gt 0 ] || { echo "!! $a.a: $(basename "$o") is not ARMv6"; exit 1; }
    done
  fi
done

{ echo "libusb $USB_VER sha256 $USB_SHA (no udev)"
  echo "libopus $OPUS_VER sha256 $OPUS_SHA ${OPUS_EXTRA[*]}"
  echo "librtlsdr osmocom/rtl-sdr $RTL_SHA (DETACH_KERNEL_DRIVER)"
  echo "faad2 $FAAD_VER sha256 $FAAD_SHA (libfaad_drm, float, GPL-2.0-or-later)"
  echo "faad2 $FAAD_VER sha256 $FAAD_SHA (libfaad_dab: the plain build, globals prefixed vibedab_, float, GPL-2.0-or-later; $FAAD_DAB_OPT; VibeSDR patches:${FAAD_SERIES:- none})"
  echo "libxaac $XAAC_VER sha256 $XAAC_SHA (decoder only, $XAAC_VARIANT code path, Apache-2.0)"
  echo "profile $PROFILE, CFLAGS '$CFLAGS', $CCVER"; } > "$PREFIX/BUILT-FROM"
ls -l "$PREFIX/lib"
echo "==> deps ($PROFILE): done"
