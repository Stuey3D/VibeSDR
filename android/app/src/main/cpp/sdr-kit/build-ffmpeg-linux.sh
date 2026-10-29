#!/usr/bin/env bash
# build-ffmpeg-linux.sh — the FFmpeg the LINUX VibeServer packages carry (/usr/lib/vibeserver/ffmpeg), V12 2026-10-29.
#
#   android/app/src/main/cpp/sdr-kit/build-ffmpeg-linux.sh            # build whatever is missing or stale
#   android/app/src/main/cpp/sdr-kit/build-ffmpeg-linux.sh --check    # exit 0 only if all five are present and current
#   android/app/src/main/cpp/sdr-kit/build-ffmpeg-linux.sh --force    # rebuild all five
#
# Output (gitignored, like tools/cloudflared-desktop/bin): tools/ffmpeg-linux/bin/
#   ffmpeg-linux-amd64 · ffmpeg-linux-i386 · ffmpeg-linux-arm64 · ffmpeg-linux-armhf (ARMv7+NEON) · ffmpeg-linux-armv6
#   COPYING.LGPLv2.1 · NOTICE.ffmpeg · BUILDINFO (version, SHA-256, every configure line) · manifest.sha256
#   source/ffmpeg-<VER>.tar.xz + .asc + this script + Dockerfile.ffmpeg — THE CORRESPONDING SOURCE, to publish beside the
#   packages (see docs/v12/BUNDLED-FFMPEG.md). .debug/ holds the unstripped binaries for profiling; never packaged.
#
# ★★★ WHY IT IS BUNDLED (Stuart, 2026-10-29): "bundle FFmpeg on the Linux servers … use FAAD2 for DRM and DAB+ by default
#     and FFmpeg where needed." Where it is needed is xHE-AAC (USAC): DRM stations that use it, and nothing FAAD2 can
#     decode. A distribution's ffmpeg is too old for that on most of the machines that run us (Pi OS bookworm 5.1,
#     Ubuntu 24.04 6.1), so the package carries its own and Depends/Recommends no longer name ffmpeg at all.
# ★★★ LGPL-2.1-or-later ONLY: no --enable-gpl, no --enable-nonfree, NO external libraries (--disable-autodetect), so the
#     binary is FFmpeg's own LGPL code plus glibc. Linked statically against FFmpeg's own libraries (one file), dynamically
#     against glibc only. The obligations that come with shipping it — the licence text, a notice, and the exact source
#     with this script — are met by what this script leaves in bin/ (see the doc). Any patch we ever apply to FFmpeg is a
#     MODIFICATION under the LGPL and must be published with the source the same way.
# ★★★ FULL SIMD, NEVER --disable-asm (Stuart): x86 via nasm with FFmpeg's runtime CPU detection (SSE2 is the i386 floor,
#     SSSE3/AVX paths are picked at run time); arm64 NEON; armhf --enable-neon (ARMv7 hard-float, the Pi 2) — the script
#     REFUSES a build in which the NEON / x86 SIMD objects are missing. armv6 (Pi Zero / Pi 1) has no NEON: VFP + ARMv6 media.
# ★★ FFmpeg 8.1.x, NOT 7.1.x: 7.1 was the first with a USAC decoder, but 7.1.5 refuses every USAC stream that uses
#    parametric stereo (stereoConfigIndex > 0, "AAC USAC Mps212" — common in low-rate stereo, which is DRM's territory);
#    8.1 parses Mps212 and decodes those streams (the core downmix), and carries a year of USAC fixes (bounds checks among
#    them). Neither release decodes eSBR (harmonic SBR / PVC / inter-TES) or the LPD (ACELP/TCX) core — those streams
#    still fail, and the listener is told so. See docs/v12/BUNDLED-FFMPEG.md.
# ★ Only what VibeServer feeds it: the aac decoder (USAC is part of it) and parser, the aac (ADTS — DAB+) and matroska (DRM
#   xHE-AAC, the config as CodecPrivate: FFmpeg cannot take a USAC config in-band in LOAS — drm_audio.h MkvStream) demuxers,
#   f32le/s16le out, pipe + file, the resampler. DAB+'s MP2 is our own decoder (vibe_dab_mp2.h), so no mp2/mp3.
# ★ CPU (2026-10-29: other agents share this Mac): ONE container, 2 CPUs, nice 19, make -j2, one architecture at a time.
#   Built ONLY on the Mac — a native arm64 container that cross-compiles all five (never an emulated container; qemu-user
#   runs only the FINISHED binaries, for the smoke test).
set -euo pipefail

VER=8.1.3
SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
KEY_FPR=FCF986EA15E6E293A5644F10B4322F04D67658D8      # "FFmpeg release signing key <ffmpeg-devel@ffmpeg.org>"
ARCHES="amd64 i386 arm64 armhf armv6"
EXTRA_VERSION=vibeserver1                             # ★ `ffmpeg -version` says "8.1.3-vibeserver1": the bundled one, visibly

# ════════════════════════════════════ INSIDE THE CONTAINER ════════════════════════════════════
if [ "${1:-}" = --in-container ]; then
  shift
  SRC_TAR=/in/ffmpeg-$VER.tar.xz; OUT=/out; J=2
  count() { grep -c "$@" || true; }   # ★ never grep -q under pipefail
  fail() { echo "!! [$A] $*"; exit 1; }
  COMMON=(--prefix=/nonexistent --enable-cross-compile --target-os=linux --extra-version="$EXTRA_VERSION"
    --disable-everything --disable-autodetect --disable-iconv --disable-network --disable-doc
    --disable-ffplay --disable-ffprobe --enable-ffmpeg --disable-avdevice --disable-swscale
    --enable-static --disable-shared --enable-runtime-cpudetect --enable-swresample --enable-avfilter
    --enable-decoder=aac --enable-parser=aac --enable-demuxer=aac,matroska
    --enable-muxer=pcm_f32le,pcm_s16le --enable-encoder=pcm_f32le,pcm_s16le --enable-protocol=pipe,file
    --enable-filter=aresample,aformat,anull,atrim,abuffer,abuffersink)
  for A in "$@"; do
    W=/tmp/b-$A; rm -rf "$W"; mkdir -p "$W"; tar -xJf "$SRC_TAR" -C "$W" --strip-components 1; cd "$W"
    QEMU=(); WANT=""; SIMD_OBJ=""
    case "$A" in
      amd64) ARCHF=(--arch=x86_64 --cross-prefix=x86_64-linux-gnu- --x86asmexe=nasm)
             QEMU=(qemu-x86_64 -L /usr/x86_64-linux-gnu); WANT="x86-64"; SIMD_OBJ="libavcodec/x86/*.o" ;;
      # ★ SSE2 is the floor (vibeserver's own i386 line: -march=i686 -msse2 -mfpmath=sse). NOT PIE: FFmpeg's 32-bit x86
      #   asm needs the register PIC would take (configure drops some of it under PIC); a non-PIE executable keeps it all.
      i386)  ARCHF=(--arch=x86_32 --cpu=i686 --cross-prefix=i686-linux-gnu- --x86asmexe=nasm --disable-pic
                    --extra-cflags="-msse2 -mfpmath=sse -mtune=generic -fno-pie" --extra-ldflags="-no-pie")
             QEMU=(qemu-i386 -L /usr/i686-linux-gnu); WANT="Intel 80386"; SIMD_OBJ="libavcodec/x86/*.o" ;;
      arm64) ARCHF=(--arch=aarch64 --cross-prefix=aarch64-linux-gnu- --enable-neon)   # ★ the container's own compiler
             WANT="ARM aarch64"; SIMD_OBJ="libavcodec/aarch64/*neon*.o" ;;
      # ★★ THE PI 2: Cortex-A7, ARMv7 hard-float, NEON, and ARM mode — the -marm finding in armhf-toolchain.cmake (Thumb-2
      #    cost the DSP loops ~8 % on the Pi 2).
      armhf) ARCHF=(--arch=arm --cpu=cortex-a7 --cross-prefix=arm-linux-gnueabihf- --enable-neon --enable-vfp
                    --extra-cflags="-marm -mfpu=neon-vfpv4 -mfloat-abi=hard")
             QEMU=(qemu-arm -cpu cortex-a7 -L /usr/arm-linux-gnueabihf); WANT="ARM, EABI5"; SIMD_OBJ="libavcodec/arm/*neon*.o" ;;
      # ★★ PI ZERO / PI 1: clang against RASPBIAN's runtime objects (armv6-toolchain.cmake: Debian's armhf crt/libgcc are
      #    ARMv7 Thumb-2 and would kill an ARMv6 core). No NEON; VFPv2 + the ARMv6 media instructions. -no-pie as there.
      armv6) T="--target=arm-linux-gnueabihf -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm --sysroot=/opt/rpi-sysroot --gcc-toolchain=/opt/rpi-sysroot/usr"
             ARCHF=(--arch=arm --cpu=armv6 --cc=clang --cxx=clang++ --ar=arm-linux-gnueabihf-ar --nm=arm-linux-gnueabihf-nm
                    --strip=arm-linux-gnueabihf-strip --ranlib=arm-linux-gnueabihf-ranlib --disable-neon --enable-vfp --enable-armv6 --disable-armv6t2
                    --disable-pic --extra-cflags="$T -fno-pie" --extra-ldflags="$T -fuse-ld=lld -no-pie")
             QEMU=(qemu-arm -cpu arm1176 -L /opt/rpi-sysroot); WANT="ARM, EABI5"; SIMD_OBJ="libavutil/arm/*vfp*.o" ;;
      *) fail "unknown architecture" ;;
    esac
    echo "==> [$A] configure"
    ./configure "${COMMON[@]}" "${ARCHF[@]}" > "$OUT/.configure-$A.log" 2>&1 || { tail -20 config.log; fail "configure failed"; }
    # ★★ arc4random_buf is glibc 2.36 — one call in libavutil/random_seed.c would put GLIBC_2.36 into the binary and refuse
    #    Ubuntu 22.04 (2.35), the floor vibeserver itself holds. FFmpeg falls back to /dev/urandom without it.
    sed -i 's/^#define HAVE_ARC4RANDOM_BUF 1$/#define HAVE_ARC4RANDOM_BUF 0/' config.h
    { echo "[$A] ./configure $(printf '%q ' "${COMMON[@]}" "${ARCHF[@]}")"; } > "$OUT/.configure-line-$A"
    grep -E '^(ARCH|CPU|x86 assembler|NEON enabled|VFP enabled|ARMv6 enabled|runtime cpu detection|SSE enabled|SSSE3 enabled|AVX enabled|AVX2 enabled|AVX-512 enabled)' "$OUT/.configure-$A.log" | sed "s/^/  [$A] /" || true
    echo "==> [$A] make -j$J"
    make -j"$J" ffmpeg > "$OUT/.make-$A.log" 2>&1 || { tail -30 "$OUT/.make-$A.log"; fail "make failed"; }
    # ── the gates ──────────────────────────────────────────────────────────────────────────────
    [ "$(file -b ffmpeg | count "$WANT")" -gt 0 ] || fail "ffmpeg is not $WANT: $(file -b ffmpeg)"
    # shellcheck disable=SC2086
    NSIMD=$(ls $SIMD_OBJ 2>/dev/null | wc -l)
    [ "$NSIMD" -gt 0 ] || fail "no SIMD objects ($SIMD_OBJ) — the assembly was not built"
    NSYM=0; case "$A" in
      armhf) [ "$(readelf -A ffmpeg | count 'Tag_Advanced_SIMD_arch: NEON')" -gt 0 ] || fail "no NEON attribute"
             NSYM=$(nm ffmpeg_g | count -i "_neon"); [ "$NSYM" -gt 10 ] || fail "only $NSYM NEON functions in the binary" ;;
      armv6) [ "$(readelf -A ffmpeg | count 'Tag_CPU_arch: v6')" -gt 0 ] || fail "not ARMv6: $(readelf -A ffmpeg | grep Tag_CPU_arch:)"
             [ "$(readelf -A ffmpeg | count 'Tag_Advanced_SIMD_arch')" -eq 0 ] || fail "NEON in the ARMv6 binary" ;;
      amd64|i386) NSYM=$(nm ffmpeg_g | count -E "_(sse|sse2|ssse3|sse4|avx|avx2|fma3)$"); [ "$NSYM" -gt 50 ] || fail "only $NSYM x86 SIMD functions in the binary" ;;
      arm64) NSYM=$(nm ffmpeg_g | count -i "_neon"); [ "$NSYM" -gt 10 ] || fail "only $NSYM NEON functions in the binary" ;;
    esac
    NEED="$(readelf -d ffmpeg | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')"
    for l in $NEED; do case "$l" in libc.so.6|libm.so.6|ld-linux*) ;; *) fail "links $l — only glibc is allowed" ;; esac; done
    G="$(readelf -V ffmpeg | grep -oE 'Name: GLIBC_[0-9.]+' | sed 's/Name: GLIBC_//' | sort -V | tail -1)"
    [ "$(printf '%s\n2.35\n' "$G" | sort -V | tail -1)" = 2.35 ] || fail "needs GLIBC_$G (> 2.35, Ubuntu 22.04)"
    # ── smoke test: the finished binary runs and has what VibeServer probes for (qemu-user runs it; nothing compiles under it)
    RUN=("${QEMU[@]}" ./ffmpeg)
    "${RUN[@]}" -hide_banner -version | head -1 | grep -F "ffmpeg version $VER-$EXTRA_VERSION" >/dev/null || fail "-version"
    D="$("${RUN[@]}" -hide_banner -decoders 2>/dev/null)"
    [ "$(echo "$D" | count ' aac ')" -gt 0 ] || fail "no aac decoder"
    F="$("${RUN[@]}" -hide_banner -formats 2>/dev/null)"
    [ "$(echo "$F" | count -E ' D +matroska')" -gt 0 ] && [ "$(echo "$F" | count -E ' D +aac ')" -gt 0 ] || fail "no matroska / aac demuxer"
    mkdir -p "$OUT/.debug"; cp ffmpeg_g "$OUT/.debug/ffmpeg_g-$A"
    cp ffmpeg "$OUT/ffmpeg-linux-$A"
    SZ=$(stat -c %s ffmpeg)
    echo "==> [$A] ffmpeg-linux-$A  $SZ bytes  needs: $NEED GLIBC_$G  SIMD objects: $NSIMD, SIMD functions: $NSYM"
    echo "$A $SZ" >> "$OUT/.sizes"
    cp COPYING.LGPLv2.1 "$OUT/COPYING.LGPLv2.1"
    cd /; rm -rf "$W"
  done
  chmod -R a+rwX "$OUT"
  exit 0
fi

# ═══════════════════════════════════════ ON THE MAC ═══════════════════════════════════════════
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../../../.." && pwd)"
OUT="$ROOT/tools/ffmpeg-linux/bin"
LINUX="$ROOT/vibeserver/linux"
SELF_SHA="$(cat "$0" "$LINUX/Dockerfile.ffmpeg" | shasum -a 256 | cut -c1-16)"
STAMP="ffmpeg $VER sha256 $SHA recipe $SELF_SHA"

current() {   # every binary present, the recipe unchanged, and every file what the manifest says
  [ -f "$OUT/BUILDINFO" ] && [ "$(head -1 "$OUT/BUILDINFO")" = "$STAMP" ] || return 1
  for A in $ARCHES; do [ -x "$OUT/ffmpeg-linux-$A" ] || return 1; done
  [ -f "$OUT/COPYING.LGPLv2.1" ] && [ -f "$OUT/source/ffmpeg-$VER.tar.xz" ] || return 1
  (cd "$OUT" && shasum -a 256 -c --quiet manifest.sha256 >/dev/null 2>&1)
}
case "${1:-}" in
  --check) current && { echo "bundled FFmpeg $VER: current ($OUT)"; exit 0; }
           echo "!! bundled FFmpeg $VER missing or stale in $OUT — run $0"; exit 1 ;;
  --force) ;;
  "") current && { echo "bundled FFmpeg $VER: current ($OUT)"; exit 0; } ;;
  *) echo "usage: $0 [--check|--force]"; exit 1 ;;
esac

# ── the source: pinned SHA-256 AND FFmpeg's release signature, against the committed key ─────────
mkdir -p "$OUT/source"
TAR="$OUT/source/ffmpeg-$VER.tar.xz"
if [ ! -f "$TAR" ] || [ "$(shasum -a 256 "$TAR" | cut -d' ' -f1)" != "$SHA" ]; then
  rm -f "$TAR" "$TAR.asc"
  curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz" -o "$TAR.part"
  got="$(shasum -a 256 "$TAR.part" | cut -d' ' -f1)"
  [ "$got" = "$SHA" ] || { rm -f "$TAR.part"; echo "!! ffmpeg-$VER.tar.xz sha256 $got, want $SHA — refusing"; exit 1; }
  mv "$TAR.part" "$TAR"
fi
[ -f "$TAR.asc" ] || curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz.asc" -o "$TAR.asc"
GH="$(mktemp -d)"; trap 'rm -rf "$GH"' EXIT
gpg --homedir "$GH" --quiet --import "$HERE/ffmpeg-release-key.asc" 2>/dev/null
fpr="$(gpg --homedir "$GH" --with-colons --list-keys 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
[ "$fpr" = "$KEY_FPR" ] || { echo "!! committed FFmpeg key is $fpr, expected $KEY_FPR — refusing"; exit 1; }
gpg --homedir "$GH" --status-fd 1 --verify "$TAR.asc" "$TAR" 2>/dev/null | grep -q "VALIDSIG $KEY_FPR" \
  || { echo "!! ffmpeg-$VER.tar.xz: no valid signature by $KEY_FPR — refusing"; exit 1; }
echo "==> ffmpeg-$VER.tar.xz: sha256 OK, signed by $KEY_FPR"

# ── the build: one native arm64 container, 2 CPUs, nice 19 ────────────────────────────────────────
command -v docker >/dev/null && docker info >/dev/null 2>&1 || { echo "!! Docker is not running (colima start)"; exit 1; }
[ "$(docker info --format '{{.Architecture}}')" = aarch64 ] || { echo "!! this Docker is not arm64 — refusing (it would emulate)"; exit 1; }
( cd "$LINUX" && nice -n 19 docker build --platform linux/arm64 -q -f Dockerfile.ffmpeg -t vibeserver-ffmpeg:bookworm . >/dev/null )
rm -f "$OUT"/ffmpeg-linux-* "$OUT/.sizes" "$OUT"/.configure-line-*
docker rm -f vibe-build-ffmpeg >/dev/null 2>&1 || true
docker run --rm --name vibe-build-ffmpeg --platform linux/arm64 --cpus 2 \
  -v "$OUT/source":/in:ro -v "$HERE":/kit:ro -v "$OUT":/out \
  vibeserver-ffmpeg:bookworm nice -n 19 bash /kit/build-ffmpeg-linux.sh --in-container $ARCHES

# ── what travels with the binaries: the corresponding source, the licence, the notice, the record ─
cp "$0" "$OUT/source/build-ffmpeg-linux.sh"
cp "$LINUX/Dockerfile.ffmpeg" "$OUT/source/Dockerfile.ffmpeg"
cp "$HERE/ffmpeg-NOTICE.txt" "$OUT/NOTICE.ffmpeg"
{
  echo "$STAMP"
  echo "FFmpeg $VER (https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz), unmodified, signed by $KEY_FPR"
  echo "Built $(date -u +%Y-%m-%dT%H:%MZ) in vibeserver-ffmpeg:bookworm (vibeserver/linux/Dockerfile.ffmpeg)"
  echo "Sizes (bytes): $(tr '\n' ' ' < "$OUT/.sizes")"
  echo
  cat "$OUT"/.configure-line-*
  echo "(every arch: then HAVE_ARC4RANDOM_BUF set to 0 in config.h — the glibc 2.35 floor; see the script)"
} > "$OUT/BUILDINFO"
(cd "$OUT" && shasum -a 256 ffmpeg-linux-* COPYING.LGPLv2.1 NOTICE.ffmpeg > manifest.sha256)
echo "==> bundled FFmpeg $VER → $OUT"
cat "$OUT/.sizes"
