#!/bin/bash
# build-deb.sh — build the vibeserver .deb from THIS checkout for the pocket image. BUILD ONLY.
#
# ★★★ NOT A RELEASE. No signing, no apt index, no commit, no push — scripts/publish-apt-docker.sh is
#     the release path and this deliberately shares none of its side effects (even its --dry-run
#     signs and commits into the apt repository checkout). The .deb lands in image/pocket/out/debs/
#     and is only ever installed into an image by build-image.sh.
# ★★ Same build containers as a release (Dockerfile.build for arm64, the armhf CROSS-compiler on the
#    Mac's own arm64 — never an emulated arm/v7 container; see armhf_one_vibeserver).
# ★★ CPU: --cpus 2, nice 15, -j2, and only the `vibeserver` target (no test binaries) — the Mac has
#    overheated on full builds before. ONE build at a time: run arm64, then armhf, never both.
# ★ The ARMv6 fallback binary is NOT built: every pocket board (Pi 3 A+, Zero 2 W, Pi 4/5) has NEON.
#
# Usage: image/pocket/build-deb.sh arm64|armhf
#   POCKET_DEB_REV (default 900): the Debian revision. 900 keeps this local build ABOVE the published
#   revisions of the same upstream version, so "Install updates" on a test box cannot swap it for an
#   official build that has no pocket code; the first release with a higher upstream version replaces it.
set -euo pipefail
A="${1:-}"
case "$A" in arm64|armhf) ;; *) echo "usage: $0 arm64|armhf"; exit 2 ;; esac
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$SRC/image/pocket/out/debs"
mkdir -p "$OUT"
REV="${POCKET_DEB_REV:-900}"
# ★ The bundled cloudflared (the tunnel) is NOT in git — a worktree has none, and a STRICT build
#   rightly refuses to ship a package that would remove the tunnel. Take it from this checkout, or
#   from the main checkout this worktree belongs to, or from POCKET_CLOUDFLARED_BIN.
CF="${POCKET_CLOUDFLARED_BIN:-$SRC/tools/cloudflared-desktop/bin}"
if [ ! -e "$CF/cloudflared-linux-arm64" ]; then
  MAIN="$(cd "$(git -C "$SRC" rev-parse --git-common-dir)/.." && pwd)"
  CF="$MAIN/tools/cloudflared-desktop/bin"
fi
[ -e "$CF/cloudflared-linux-arm64" ] || { echo "!! no cloudflared binaries (run tools/cloudflared-desktop/fetch.sh)"; exit 1; }

if [ "$A" = armhf ]; then
  IMAGE=vibeserver-cross:bookworm-armhf; DOCKERFILE="$SRC/vibeserver/linux/Dockerfile.cross-armhf"
else
  IMAGE=vibeserver-build:bookworm-arm64; DOCKERFILE="$SRC/vibeserver/linux/Dockerfile.build"
fi
docker image inspect "$IMAGE" >/dev/null 2>&1 || \
  nice -n 15 docker build --platform linux/arm64 -q -f "$DOCKERFILE" -t "$IMAGE" "$SRC/vibeserver/linux" >/dev/null

echo "==> [$A] building vibeserver (rev $REV) — 2 CPUs, niced"
docker run --rm --platform linux/arm64 --cpus 2 \
  -v "$SRC":/src:ro -v "$CF":/cf:ro -v "$OUT":/out -e A="$A" -e REV="$REV" \
  "$IMAGE" /bin/bash -euo pipefail -c '
  mkdir -p /build
  rsync -a --exclude .git --exclude node_modules --exclude Pods --exclude ios --exclude tvos --exclude spike \
        --exclude web/dist --exclude .cxx --exclude "build" --exclude "build-*" --exclude .claude --exclude .gradle \
        --exclude image/pocket/out --exclude "*.raw" --exclude "*.iq16" --exclude "*.cf32" --exclude "*.zip" \
        /src/ /build/VibeSDR/
  mkdir -p /build/VibeSDR/tools/cloudflared-desktop/bin
  cp -a /cf/cloudflared-linux-arm64 /cf/cloudflared-linux-arm /cf/LICENSE /build/VibeSDR/tools/cloudflared-desktop/bin/ 2>/dev/null || true
  cd /build/VibeSDR/vibeserver
  CROSS=""
  if [ "$A" = armhf ]; then
    CROSS="-DCMAKE_TOOLCHAIN_FILE=/opt/armhf-toolchain.cmake"
    export DEB_HOST_ARCH=armhf DEB_HOST_MULTIARCH=arm-linux-gnueabihf DEB_HOST_GNU_TYPE=arm-linux-gnueabihf
  fi
  # ★ STRICT: a package missing the wizard or a radio driver is not one to put on a card either.
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVIBESERVER_DEB_REV="$REV" -DVIBESERVER_STRICT_RADIOS=ON \
        -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON $CROSS >/dev/null
  nice -n 15 cmake --build build --target vibeserver -j2 2>&1 | tail -3
  cd build && nice -n 15 cpack 2>&1 | tail -2
  DEB=$(ls -t vibeserver_*.deb | head -1)
  cp "$DEB" /out/       # ★ out FIRST: a failed check below must not throw away a 10-minute build
  # ★★ PROVE THE POCKET CODE IS IN IT — a build is not the artefact (mac_only_compile_is_not_a_build).
  # ★★★ COUNT, NEVER grep -q: under pipefail, -q exits at the first match, dpkg-deb dies of SIGPIPE
  #     and a SUCCESSFUL match is reported as a failure (play_16kb_page_size — it bit here too).
  n1=$(dpkg-deb -c "$DEB" | grep -c "usr/lib/vibeserver/vibeserver-pocket$" || true)
  n2=$(dpkg-deb -c "$DEB" | grep -c "lib/systemd/system/vibeserver-pocket.service$" || true)
  n3=$(dpkg-deb --fsys-tarfile "$DEB" | grep -ac "/vibeserver/pocket/claim" || true)
  [ "${n1:-0}" -ge 1 ] || { echo "!! no vibeserver-pocket in $DEB"; exit 1; }
  [ "${n2:-0}" -ge 1 ] || { echo "!! no pocket unit in $DEB"; exit 1; }
  [ "${n3:-0}" -ge 1 ] || { echo "!! the binary has no pocket routes"; exit 1; }
  echo "==> verified: pocket helper, pocket unit, pocket routes in the binary"
  echo "==> $(basename "$DEB") ($(dpkg-deb -f "$DEB" Architecture), $(du -h "$DEB" | cut -f1))"
'
ls -la "$OUT"
