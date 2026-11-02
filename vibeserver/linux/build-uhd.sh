#!/usr/bin/env bash
# build-uhd.sh <download-cache> <libusb-prefix> <out-prefix> <images-dir-compiled-in>
#
# UHD (Ettus USRP driver), B2xx ONLY, as ONE STATIC ARCHIVE, plus the Boost subset it needs and the B2xx
# firmware + FPGA images — for VibeServer. V12, 2026-11-02 — docs/v12/USRP.md.
# Called by vibeserver/linux/build-deps.sh (amd64 and arm64 only) and vibeserver/mac/build-deps.sh, with CC / CXX /
# AR / RANLIB / CFLAGS / CXXFLAGS already set for the target, the same contract as build-libxaac.sh.
#
# ★★★ PLUG AND PLAY (Stuart, 2026-11-02): an owner installs NOTHING for a USRP — no uhd-host, no
#     uhd_images_downloader, no SoapySDR. The driver is linked into vibeserver, and the images are fetched HERE, at
#     build time, SHA-256 checked against UHD's own images/manifest.txt for the pinned release, and shipped in the
#     package. Nothing is downloaded on the owner's machine, ever.
# ★★★ HIGH-END HOSTS ONLY (Stuart, 2026-11-02): amd64, arm64 and the Mac. NOT armhf (Pi 2) or i386 — the caller does
#     not run this there, and the server compiles no USRP source there (CMakeLists.txt). A B2xx needs USB 3 and a host
#     that can carry its sample rates.
#
# Pins (move them together — the images must match the release's FPGA/firmware compat numbers):
#   UHD    4.9.0.1  (tag v4.9.0.1 = commit 9ec1f58234c5), GPL-3.0-or-later — GitHub tag tarball, SHA-256 below
#   Boost  1.83.0   BSL-1.0 — chrono date_time filesystem program_options serialization thread (+ test, which UHD's
#                   CMake insists on finding although nothing we build uses it; atomic and system come along)
#   images b2xx_*_fpga_default-gc37b318, b2xx_common_fw_default-g7f7d016 — the exact archives 4.9.0.1's manifest names
#
# Output:
#   <out>/lib/libuhd.a                    every device but B2xx compiled OUT (see UHD_OFF below)
#   <out>/lib/libboost_*.a                static, release, multi-threaded
#   <out>/include/{uhd,boost}/...         headers (only usrp_source.cpp includes them)
#   <out>/images/usrp_b2*_fpga.bin, usrp_b200_fw.hex   the images, flat, as UHD names them
#   <out>/share/licences/                 LICENSE.uhd, LICENSE.uhd-fpga (LGPL-3), LICENSE_1_0.txt (Boost), IMAGES.txt
#   <out>/BUILT-FROM                      what went in
#
# Environment:
#   UHD_B2_TOOLSET   gcc (Linux) or clang (macOS) — Boost.Build's toolset name
#   UHD_CMAKE_TC     optional: "-DCMAKE_TOOLCHAIN_FILE=…" for a cross build
#   UHD_CMAKE_EXTRA  optional: more cmake arguments (the Mac passes its deployment target / arch here)
#   JOBS             parallel jobs
set -euo pipefail
CACHE="${1:?download cache}"; USBP="${2:?libusb prefix}"; OUT="${3:?out prefix}"; IMGDIR="${4:?compiled-in images dir}"
: "${CC:?CC}" "${CXX:?CXX}" "${AR:?AR}" "${UHD_B2_TOOLSET:?UHD_B2_TOOLSET (gcc|clang)}"
JOBS="${JOBS:-4}"
mkdir -p "$CACHE"
B="$(mktemp -d)"; trap 'rm -rf "$B"' EXIT

UHD_VER=4.9.0.1
UHD_SHA=0be26a139f23041c1fb6e9666d84cba839460e3c756057dc48dc067cc356a7bc     # github.com/EttusResearch/uhd/archive/refs/tags/v4.9.0.1.tar.gz
BOOST_VER=1.83.0
BOOST_SHA=6478edfe2f3305127cffe8caf73ea0176c53769f4bf1585be237eb30798c3b8e   # archives.boost.io boost_1_83_0.tar.bz2 (= its .json)
# ★ name  url-under-files.ettus.com/binaries/cache/  sha256 — copied from uhd-4.9.0.1/images/manifest.txt, and CHECKED
#   against that file below, so a hand-edit here that drifts from the release is refused.
IMAGES=(
  "b2xx_b200_fpga_default     b2xx/uhd-c37b318/b2xx_b200_fpga_default-gc37b318.zip     35542b8a2b6a6dfd3aa155c35364af85c2599cfb692b0cf94013c5342c945448"
  "b2xx_b210_fpga_default     b2xx/uhd-c37b318/b2xx_b210_fpga_default-gc37b318.zip     ab1cdbe93090fdcf590a79cbe938749f8a5fbd7b45422b7833b8c841aa2106b4"
  "b2xx_b200mini_fpga_default b2xx/uhd-c37b318/b2xx_b200mini_fpga_default-gc37b318.zip c1865c6e574cf453a688c6bfb8fe5e3646cd53458ae403cf11d3eeb7dc6a9918"
  "b2xx_b205mini_fpga_default b2xx/uhd-c37b318/b2xx_b205mini_fpga_default-gc37b318.zip dce96120e3512d6e21bb061811c4021cbc90ac4d787248ad821337b7389ff3c4"
  "b2xx_common_fw_default     b2xx/uhd-7f7d016/b2xx_common_fw_default-g7f7d016.zip     22ac0e54cc90d53d8494d481439447a0c89b9bbaced6fabe24df260eda3ce3a3"
)

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }
fetch() {   # fetch <url> <file> <sha256> — download once into the cache, verify EVERY time
  local out="$CACHE/$2"
  [ -s "$out" ] || { echo "    downloading $2"; curl -fsSL --retry 3 "$1" -o "$out.tmp" && mv "$out.tmp" "$out"; }
  local got; got="$(sha256 "$out")"
  [ "$got" = "$3" ] || { echo "!! SHA-256 MISMATCH for $2: got $got, pinned $3 — refusing"; rm -f "$out"; exit 1; }
}
count() { grep -c "$@" || true; }   # ★ never grep -q under pipefail

rm -rf "$OUT"; mkdir -p "$OUT/lib" "$OUT/include" "$OUT/images" "$OUT/share/licences"

# ── Boost 1.83.0, static, the subset UHD links ────────────────────────────────────────────────────────────
echo "==> Boost $BOOST_VER (static subset) — $($CXX --version | head -1)"
fetch "https://archives.boost.io/release/$BOOST_VER/source/boost_${BOOST_VER//./_}.tar.bz2" "boost_${BOOST_VER//./_}.tar.bz2" "$BOOST_SHA"
mkdir -p "$B/boost" && tar -xjf "$CACHE/boost_${BOOST_VER//./_}.tar.bz2" -C "$B/boost" --strip-components 1
# ★★ b2 itself is built with the HOST compiler (bootstrap), the libraries with the TARGET one: a user-config.jam names
#    $CXX as toolset "<UHD_B2_TOOLSET>-vibe", so a cross g++ and the Mac's clang++ are the same one line.
# ★ CXXFLAGS (the caller's arch / deployment-target flags) go in as cxxflags; -fvisibility stays Boost's default.
( cd "$B/boost" && ./bootstrap.sh >"$B/boost-bootstrap.log" 2>&1 ) || { tail -30 "$B/boost-bootstrap.log"; echo "!! Boost bootstrap failed"; exit 1; }
CXX_ONLY="${CXX%% *}"; CXX_REST="${CXX#"$CXX_ONLY"}"
echo "using $UHD_B2_TOOLSET : vibe : $CXX_ONLY : <archiver>$AR ;" > "$B/boost/user-config.jam"
( cd "$B/boost" && ./b2 -q -d0 -j"$JOBS" --user-config="$B/boost/user-config.jam" toolset="$UHD_B2_TOOLSET-vibe" \
      variant=release link=static runtime-link=shared threading=multi cxxstd=17 --layout=system \
      cxxflags="${CXXFLAGS:-} $CXX_REST -fPIC" cflags="${CFLAGS:-} -fPIC" linkflags="${LDFLAGS:-}" \
      --with-chrono --with-date_time --with-filesystem --with-program_options --with-serialization \
      --with-thread --with-test --with-system --with-atomic \
      --prefix="$B/boost-inst" install >"$B/boost.log" 2>&1 ) \
  || { tail -40 "$B/boost.log"; echo "!! Boost build failed"; exit 1; }
for l in chrono date_time filesystem program_options serialization thread atomic system unit_test_framework; do
  [ -s "$B/boost-inst/lib/libboost_$l.a" ] || { ls "$B/boost-inst/lib"; echo "!! libboost_$l.a missing — refusing"; exit 1; }
done
cp "$B/boost/LICENSE_1_0.txt" "$OUT/share/licences/LICENSE_1_0.txt.boost"

# ── UHD 4.9.0.1, B2xx only, static ───────────────────────────────────────────────────────────────────────
echo "==> UHD $UHD_VER (B2xx only, static)"
fetch "https://github.com/EttusResearch/uhd/archive/refs/tags/v$UHD_VER.tar.gz" "uhd-$UHD_VER.tar.gz" "$UHD_SHA"
mkdir -p "$B/uhd"
# ★ host/ (the library) and images/manifest.txt (the image pins) only — the FPGA source is 130 MB we do not compile.
tar -xzf "$CACHE/uhd-$UHD_VER.tar.gz" -C "$B/uhd" --strip-components 1 \
    "uhd-$UHD_VER/host" "uhd-$UHD_VER/images/manifest.txt" "uhd-$UHD_VER/LICENSE.md" "uhd-$UHD_VER/fpga/usrp3/LICENSE.md"
# ★★ ONE EDIT TO UHD's BUILD, NONE TO ITS CODE: Boost.System named in its component list. The static library links
#    the Boost::system target (lib/CMakeLists.txt), but the list has it commented out ("causes errors on some
#    systems"), and with Boost's own CMake package an unrequested component has no target — a configure error.
#    Boost.System has been header-only since 1.69, so naming it adds no code.
sed -i.orig 's|^    #system Boost.System is used.*$|    system|' "$B/uhd/host/CMakeLists.txt"
[ "$(count -E '^    system$' "$B/uhd/host/CMakeLists.txt")" -eq 1 ] || { echo "!! could not name Boost.System in UHD's component list — refusing"; exit 1; }
# ★★ EVERY DEVICE BUT B2xx OFF, and everything that is not the library OFF. A component left ON is code nobody here can
#    test, compiled into every server; MPMD alone would pull in rpclib and a network discovery thread.
UHD_OFF=(-DENABLE_C_API=OFF -DENABLE_PYTHON_API=OFF -DENABLE_PYMOD_UTILS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF
         -DENABLE_TESTS=OFF -DENABLE_MANUAL=OFF -DENABLE_DOXYGEN=OFF -DENABLE_MAN_PAGES=OFF -DENABLE_RFNOC_DEV=OFF
         -DENABLE_B100=OFF -DENABLE_USRP1=OFF -DENABLE_USRP2=OFF -DENABLE_X300=OFF -DENABLE_MPMD=OFF -DENABLE_SIM=OFF
         -DENABLE_N300=OFF -DENABLE_N320=OFF -DENABLE_E320=OFF -DENABLE_E300=OFF -DENABLE_X400=OFF
         -DENABLE_OCTOCLOCK=OFF -DENABLE_DPDK=OFF)
# ★ libusb BY PATH (ours: 1.0.30, static, no udev on Linux). pkg-config is BLINDED rather than disabled: UHD's
#   FindLIBUSB.cmake calls pkg_check_modules unconditionally, so CMAKE_DISABLE_FIND_PACKAGE_PkgConfig is a configure
#   error; an empty PKG_CONFIG_LIBDIR means it can find nothing, and the paths given below win.
# ★★ UHD_IMAGES_DIR is COMPILED IN: b200_find() searches it for the firmware, and the server also passes the files'
#    absolute paths in the device args (usrp_source.cpp), so the images are ours whatever the owner's environment says.
# shellcheck disable=SC2086
PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH= \
cmake -S "$B/uhd/host" -B "$B/uhd/b" -DCMAKE_BUILD_TYPE=Release ${UHD_CMAKE_TC:-} ${UHD_CMAKE_EXTRA:-} \
      -DCMAKE_C_COMPILER="${CC%% *}" -DCMAKE_CXX_COMPILER="$CXX_ONLY" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DENABLE_LIBUHD=ON -DENABLE_STATIC_LIBS=ON -DENABLE_USB=ON -DENABLE_B200=ON "${UHD_OFF[@]}" \
      -DBOOST_ROOT="$B/boost-inst" -DBoost_NO_SYSTEM_PATHS=ON -DBoost_USE_STATIC_LIBS=ON -DBoost_NO_BOOST_CMAKE=OFF \
      -DCMAKE_FIND_ROOT_PATH="$B/boost-inst;$USBP" \
      -DLIBUSB_INCLUDE_DIRS="$USBP/include/libusb-1.0" -DLIBUSB_LIBRARIES="$USBP/lib/libusb-1.0.a" \
      -DUHD_IMAGES_DIR="$IMGDIR" \
      >"$B/uhd.log" 2>&1 \
  && cmake --build "$B/uhd/b" --target uhd_static -j"$JOBS" >>"$B/uhd.log" 2>&1 \
  || { tail -60 "$B/uhd.log"; echo "!! UHD build failed"; exit 1; }
# ★ The FEATURE, not the file: the B200 is in, and the devices we turned off are not (checked on the symbols below).
UA="$(find "$B/uhd/b" -name libuhd.a | head -1)"
[ -s "$UA" ] || { echo "!! libuhd.a not produced — refusing"; exit 1; }
cp "$UA" "$OUT/lib/libuhd.a"
NM_="${NM:-nm}"
[ "$("$NM_" -C "$OUT/lib/libuhd.a" 2>/dev/null | count 'b200_impl::b200_impl')" -gt 0 ] || { echo "!! libuhd.a has no B200 driver — refusing"; exit 1; }
[ "$("$NM_" -C "$OUT/lib/libuhd.a" 2>/dev/null | count -E 'x300_impl::|mpmd_impl::|usrp2_impl::')" -eq 0 ] || { echo "!! libuhd.a contains devices that should be compiled out — refusing"; exit 1; }
cp "$B/boost-inst/lib/"libboost_{chrono,date_time,filesystem,program_options,serialization,thread,atomic,system}.a "$OUT/lib/"
cp -R "$B/boost-inst/include/boost" "$OUT/include/"
cp -R "$B/uhd/host/include/uhd" "$OUT/include/"
# ★ Generated headers (version.hpp, config.h …) live in the build tree.
cp -R "$B/uhd/b/include/uhd/." "$OUT/include/uhd/" 2>/dev/null || true
find "$OUT/include/uhd" \( -name 'CMakeLists.txt' -o -name '*.in' -o -name 'CMakeFiles' -o -name '*.cmake' \) -prune -exec rm -rf {} + 2>/dev/null || true
[ -f "$OUT/include/uhd/version.hpp" ] || { echo "!! generated uhd/version.hpp missing — refusing"; exit 1; }
cp "$B/uhd/LICENSE.md" "$OUT/share/licences/LICENSE.uhd"
cp "$B/uhd/fpga/usrp3/LICENSE.md" "$OUT/share/licences/LICENSE.uhd-fpga"

# ── the B2xx images: firmware + FPGA, pinned to THIS release's manifest ─────────────────────────────────────
echo "==> B2xx images (UHD $UHD_VER manifest)"
MAN="$B/uhd/images/manifest.txt"
: > "$OUT/share/licences/IMAGES.txt"
for line in "${IMAGES[@]}"; do
  read -r name path sha <<<"$line"
  # ★★ The pin here must be the line in the release's own manifest, byte for byte (name, path, sha).
  [ "$(awk -v n="$name" -v p="$path" -v s="$sha" '$1==n && $3==p && $4==s' "$MAN" | count .)" -eq 1 ] \
    || { echo "!! $name is not pinned as uhd-$UHD_VER/images/manifest.txt says — refusing"; exit 1; }
  fetch "https://files.ettus.com/binaries/cache/$path" "$(basename "$path")" "$sha"
  mkdir -p "$B/img/$name"
  ( cd "$B/img/$name" && unzip -q -o "$CACHE/$(basename "$path")" ) || { echo "!! could not unpack $path"; exit 1; }
  echo "$name $path sha256 $sha" >> "$OUT/share/licences/IMAGES.txt"
done
for f in usrp_b200_fw.hex usrp_b200_fpga.bin usrp_b210_fpga.bin usrp_b200mini_fpga.bin usrp_b205mini_fpga.bin; do
  src="$(find "$B/img" -name "$f" | head -1)"
  [ -s "$src" ] || { echo "!! $f is not in the image archives — refusing"; exit 1; }
  cp "$src" "$OUT/images/$f"
done
{ echo
  echo "These are the Ettus Research USRP B2xx images for UHD $UHD_VER, unmodified, as published at"
  echo "https://files.ettus.com/binaries/cache/ (the archives above, SHA-256 checked against UHD's images/manifest.txt)."
  echo "FPGA images: built from uhd/fpga/usrp3 (LGPL-3.0, see LICENSE.uhd-fpga). FX3 firmware: built from"
  echo "uhd/firmware/fx3 (GPL-3.0-or-later) with Cypress's FX3 SDK. Corresponding source:"
  echo "https://github.com/EttusResearch/uhd/tree/v$UHD_VER (fpga/usrp3/top/b200, fpga/usrp3/top/b200mini, firmware/fx3/b200)."; } \
  >> "$OUT/share/licences/IMAGES.txt"

{ echo "uhd $UHD_VER sha256 $UHD_SHA (B2xx only, static; GPL-3.0-or-later)"
  echo "boost $BOOST_VER sha256 $BOOST_SHA (static: chrono date_time filesystem program_options serialization thread atomic system)"
  for line in "${IMAGES[@]}"; do read -r name path sha <<<"$line"; echo "image $name sha256 $sha"; done
  echo "compiled-in UHD_IMAGES_DIR $IMGDIR; $($CXX --version | head -1)"; } > "$OUT/BUILT-FROM"
ls -l "$OUT/lib" "$OUT/images"
echo "==> UHD $UHD_VER: done"
