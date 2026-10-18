#!/usr/bin/env bash
# build-deps.sh — every library the Linux VibeServer links, FROM PINNED SOURCE, as static archives.
#
#   build-deps.sh <prefix> [native|armhf|armv6]
#
# Run inside the build images (Dockerfile.build, Dockerfile.cross-armhf). Output:
#   <prefix>/lib/{libusb-1.0.a,libopus.a,librtlsdr.a}
#   <prefix>/include/{libusb-1.0/libusb.h,opus/*.h,rtl-sdr.h,rtl-sdr_export.h}
#   <prefix>/BUILT-FROM                  what went in, one line per library
# — the same layout vibeserver/mac/build-deps.sh produces, so CMake treats the two prefixes alike.
#
# ★★★ WHY (Stuart, 2026-10-18): the package must need NOTHING from the system for SDR support or
#     libraries — only glibc, FFmpeg (Recommends, DAB+) and the SDRplay API (closed, dlopen'd, the
#     owner installs it). Linux used to link Debian's libusb, libopus, libhackrf, ncurses and the
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

PREFIX="${1:?usage: build-deps.sh <prefix> [native|armhf|armv6]}"
PROFILE="${2:-native}"
JOBS="${JOBS:-$(nproc)}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

USB_VER=1.0.30
USB_SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf    # = mac/build-deps.sh
OPUS_VER=1.6.1
OPUS_SHA=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1   # = mac/build-deps.sh
RTL_SHA=797f8143266d983c56d8f35d2d442527529dd8a5                             # = mac/build-deps.sh

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
  *) echo "!! profile is native, armhf or armv6"; exit 1 ;;
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
# ★ libusb BY PATH, pkg-config disabled — the same arrangement as the Mac build, so it cannot resolve
#   to a system libusb. DETACH_KERNEL_DRIVER as the shipped .so had it (a dongle the DVB driver holds).
CC_ONLY="${CC%% *}"   # cmake wants the compiler alone; the cross profiles' flags come from the toolchain file
cmake -S "$WORK/rtl" -B "$WORK/rtl/b" -DCMAKE_BUILD_TYPE=Release "${CMAKE_TC[@]}" \
      $([ "$PROFILE" = native ] && echo "-DCMAKE_C_COMPILER=$CC_ONLY") \
      -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
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

# ── every object for the architecture the profile promises ───────────────────────────────────
X="$WORK/x"; mkdir -p "$X"
for a in libusb-1.0 libopus librtlsdr; do
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
  echo "profile $PROFILE, CFLAGS '$CFLAGS', $CCVER"; } > "$PREFIX/BUILT-FROM"
ls -l "$PREFIX/lib"
echo "==> deps ($PROFILE): done"
