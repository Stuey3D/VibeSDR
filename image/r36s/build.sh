#!/usr/bin/env bash
# image/r36s/build.sh — build the VibeSDR R36S handheld card image (Debian 13 arm64 on ROCKNIX's boot chain).
#
#   image/r36s/build.sh [--deb PATH/vibeserver_…_arm64.deb] [--mapdata PATH/vibeserver-mapdata-detail_…_all.deb] [--out DIR]
#                                                                            build (≈ 10 min on the M5 Pro)
#   image/r36s/build.sh --verify DIR/vibesdr-r36s-<version>.img.gz           re-check a finished image offline
#   DRY_RUN=1 image/r36s/build.sh                                            the plan and the inputs; builds nothing
#
# Out: DIR/vibesdr-r36s-<version>.img.gz + .sha256, .packages.txt, .SOURCES.txt, splash-preview.png
#      (DIR defaults to ~/VibeSDR-r36s-build/out — NOT the repository).
# Env: APT_SUITE=beta (the engine's apt suite, when no --deb) · SNAPSHOT=<YYYYMMDDTHHMMSSZ> (pin Debian to
#      snapshot.debian.org for a reproducible package set) · IMAGE_VERSION=… · ROCKNIX_IMG=<local copy of the ROCKNIX
#      20261101 b image> (else downloaded once into the cache and checked against its pinned sha256) · JOBS=6
#
# ★★★ THE HIGH-DETAIL MAP PACK SHIPS ON THE CARD (Stuart 2026-11-05: the handheld has no internet). vibeserver only
#     Recommends vibeserver-mapdata-detail (~221 MB, the layers past zoom 7), and the image installs with
#     --no-install-recommends — so it is installed explicitly. --mapdata names the .deb; without it the newest one in
#     the cache is used, else scripts/build-mapdata-deb.sh builds it from the repository's assets/mapdata/v1.
#     verify.sh fails an image without it.
# ★★ ONE BUILD AT A TIME, ≤ 6 CPUs (another agent compiles at -j6 on the same Mac): --cpus 6, nice 10, pigz -p 6.
# ★★ DOCKER ON THE MAC (colima, arm64): native, no QEMU. colima shares only $HOME with its VM, so every folder handed to
#    the container lives under $HOME; a --out outside $HOME is filled by copying at the end.
# ★ The root tree is built in a Docker VOLUME (vibesdr-r36s-work) — the VM's own ext4 — never on the shared folder,
#   whose ownership mapping would hand every file to the Mac's user.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
STAGE="$HOME/VibeSDR-r36s-build"
CACHE="$STAGE/cache"
OUT="$STAGE/out"
DEB=""; VERIFY=""; MAPDATA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --deb) DEB="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    --mapdata) MAPDATA="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    --out) mkdir -p "$2"; OUT_FINAL="$(cd "$2" && pwd)"; shift 2 ;;
    --verify) VERIFY="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument $1"; exit 2 ;;
  esac
done
OUT_FINAL="${OUT_FINAL:-$OUT}"
case "$OUT_FINAL" in "$HOME"/*) OUT="$OUT_FINAL" ;; esac
mkdir -p "$CACHE" "$OUT"
TOOLS="ca-certificates curl gnupg debootstrap fdisk e2fsprogs dosfstools mtools squashfs-tools util-linux binutils file \
kmod device-tree-compiler pigz python3 python3-pil systemd cpio u-boot-tools"

if [ "${DRY_RUN:-0}" = 1 ]; then
  cat <<EOF
repository ... $REPO
engine ....... ${DEB:-vibeserver (newest arm64) from https://apt.vibesdr.net ${APT_SUITE:-beta}}
map pack ..... ${MAPDATA:-vibeserver-mapdata-detail: the newest in $CACHE/mapdata, else built by scripts/build-mapdata-deb.sh}
boot chain ... ROCKNIX 20261101 b (EE clones): loader b_uboot.bin.uart5, boot.scr, KERNEL 7.1.2, eeclone DTB, modules, firmware
board ........ $HERE/board: mipi-panel.rocknix.dtbo (→ 60 Hz default), vibe-r36s.dts (CPU 408–1296, backlight 120), extlinux.conf
OS ........... Debian 13 trixie arm64 minbase + $(sed 's/#.*//' "$HERE/packages.txt" | xargs | wc -w) packages (packages.txt)${SNAPSHOT:+, snapshot $SNAPSHOT}
layout ....... loader @32 KiB · p1 FAT32 VIBEBOOT 128 MiB @16 MiB · p2 ext4 root (read-only + RAM overlay) · p3 ext4 vibestate 256 MiB
               · p4 exFAT VIBESDR made on the first boot after the capacity check
output ....... $OUT_FINAL/vibesdr-r36s-<version>.img.gz (+ .sha256)
EOF
  for f in build-image.sh verify.sh packages.txt board/extlinux.conf board/vibe-r36s.dts board/panel-60hz.py \
           board/mipi-panel.rocknix.dtbo board/rf3536k3ka.dtb art/make-splash.py; do
    [ -f "$HERE/$f" ] && echo "  ok  $f" || { echo "  !!  missing $f"; exit 1; }
  done
  for f in "$HERE"/rootfs/usr/lib/vibesdr-r36s/*; do
    case "$(head -1 "$f")" in
      *python3*) python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$f" ;;
      *sh) bash -n "$f" ;;
    esac && echo "  ok  parses: ${f#$HERE/}"
  done
  exit 0
fi

command -v docker >/dev/null || { echo "!! docker not found (colima start --cpu 8 --memory 12)"; exit 1; }
if [ -n "$(docker ps -q --filter label=vibesdr-r36s-build)" ]; then
  echo "!! another R36S image build is running — one at a time"; exit 1
fi
if [ -n "${ROCKNIX_IMG:-}" ] && [ ! -s "$CACHE/$(basename "$ROCKNIX_IMG")" ]; then
  cp "$ROCKNIX_IMG" "$CACHE/"
fi

if [ -n "$VERIFY" ]; then
  case "$VERIFY" in "$HOME"/*) ;; *) echo "!! --verify needs a file under \$HOME (colima shares only that)"; exit 1 ;; esac
  exec docker run --rm --cpus 4 --label vibesdr-r36s-build -v "$REPO":/repo:ro -v "$(dirname "$VERIFY")":/v:ro \
    debian:trixie-slim /bin/bash -euo pipefail -c '
      apt-get update -qq && apt-get install -y -qq --no-install-recommends fdisk e2fsprogs mtools device-tree-compiler pigz cpio >/dev/null
      pigz -dc "/v/'"$(basename "$VERIFY")"'" > /tmp/card.img
      bash /repo/image/r36s/verify.sh /tmp/card.img'
fi

# ── the high-detail map pack (above) ──
if [ -z "$MAPDATA" ]; then
  mkdir -p "$CACHE/mapdata"
  MAPDATA=$(ls -t "$CACHE"/mapdata/vibeserver-mapdata-detail_*_all.deb 2>/dev/null | head -1 || true)
  if [ -z "$MAPDATA" ]; then
    echo "==> building the map detail pack (scripts/build-mapdata-deb.sh)"
    docker run --rm --cpus 4 -v "$REPO":/repo:ro -v "$CACHE/mapdata":/out debian:trixie-slim /bin/bash -euo pipefail -c '
      apt-get update -qq && apt-get install -y -qq --no-install-recommends python3-minimal >/dev/null
      bash /repo/scripts/build-mapdata-deb.sh --out /out >/dev/null'
    MAPDATA=$(ls -t "$CACHE"/mapdata/vibeserver-mapdata-detail_*_all.deb | head -1)
  fi
fi
[ -f "$MAPDATA" ] || { echo "!! no $MAPDATA"; exit 1; }
case "$MAPDATA" in "$HOME"/*) ;; *) mkdir -p "$CACHE/mapdata"; cp "$MAPDATA" "$CACHE/mapdata/"; MAPDATA="$CACHE/mapdata/$(basename "$MAPDATA")" ;; esac
echo "==> map pack: $(basename "$MAPDATA")"
DEBMNT=(-v "$(dirname "$MAPDATA")":/mapdata:ro); DEBENV=(-e "MAPDATA_DEB=/mapdata/$(basename "$MAPDATA")")
if [ -n "$DEB" ]; then
  [ -f "$DEB" ] || { echo "!! no $DEB"; exit 1; }
  case "$DEB" in "$HOME"/*) ;; *) cp "$DEB" "$CACHE/"; DEB="$CACHE/$(basename "$DEB")" ;; esac
  DEBMNT+=(-v "$(dirname "$DEB")":/deb:ro); DEBENV+=(-e "VIBESERVER_DEB=/deb/$(basename "$DEB")")
fi
docker volume create vibesdr-r36s-work >/dev/null
docker run --rm --privileged --cpus "${JOBS:-6}" --label vibesdr-r36s-build \
  -v "$REPO":/repo:ro -v "$CACHE":/cache -v "$OUT":/out -v vibesdr-r36s-work:/work \
  ${DEBMNT[@]+"${DEBMNT[@]}"} ${DEBENV[@]+"${DEBENV[@]}"} \
  -e APT_SUITE="${APT_SUITE:-beta}" -e SNAPSHOT="${SNAPSHOT:-}" -e IMAGE_VERSION="${IMAGE_VERSION:-}" -e JOBS="${JOBS:-6}" \
  debian:trixie-slim /bin/bash -euo pipefail -c "
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $TOOLS >/dev/null
    nice -n 10 bash /repo/image/r36s/build-image.sh
  "
V=$(cat "$OUT/LATEST")
if [ "$OUT_FINAL" != "$OUT" ]; then
  cp "$OUT/vibesdr-r36s-$V".* "$OUT/splash-preview.png" "$OUT_FINAL/"
fi
echo "==> $OUT_FINAL/vibesdr-r36s-$V.img.gz"
cat "$OUT_FINAL/vibesdr-r36s-$V.img.gz.sha256"
