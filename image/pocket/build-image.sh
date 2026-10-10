#!/bin/bash
# build-image.sh — make a flashable POCKET VIBESERVER card image. ONE script, both architectures.
#
#   image/pocket/build-image.sh arm64|armhf            # default route: customise the official image
#   image/pocket/build-image.sh arm64|armhf --pigen    # full pi-gen build with stage-pocket
#
# ★★★ TWO ROUTES, ONE CUSTOMISATION. Both run stage-pocket/00-pocket/files/customise.sh inside the
#     target root, so the box they produce is the same:
#   · DEFAULT — start from Raspberry Pi OS Lite exactly as Raspberry Pi publish it, grow it, chroot
#     in, customise, compress. Needs ~5 GB free on the Mac and none of the Docker VM's disk (the work
#     files live in image/pocket/out, shared into the container). This is the route that fits the
#     Mac (2026-10-10: the colima VM had 5 GB free, a full pi-gen run wants 8+).
#   · --pigen — pi-gen (arm64 branch for 64-bit, master for 32-bit) with stage2 + stage-pocket. The
#     reproducible, from-scratch build; wants a Linux host or a Docker VM with ≥ 15 GB free.
# ★★ armhf on Apple Silicon: the chroot's armhf programs run under qemu-arm (M-series CPUs have no
#    AArch32). Only apt/dpkg run there — no compiling — which qemu handles; the qemu-arm binfmt entry
#    this registers in the Docker VM is REMOVED again at the end.
# ★★ CPU: --cpus 2 and nice on every container, xz -T2. ONE image at a time — never both in parallel.
#
# Inputs: image/pocket/out/debs/vibeserver_*_<arch>.deb (from build-deb.sh) — REQUIRED until a
#   published release carries the pocket code (customise.sh refuses a package without it).
# Output: image/pocket/out/vibeserver-pocket-<arch>-<date>.img.xz (+ .sha256)
set -euo pipefail
A="${1:-}"; MODE="${2:-}"
case "$A" in arm64|armhf) ;; *) echo "usage: $0 arm64|armhf [--pigen]"; exit 2 ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"; mkdir -p "$OUT/cache" "$OUT/work"
STAMP="$(date +%Y%m%d)"
NAME="vibeserver-pocket-$A-$STAMP"
COUNTRY="${POCKET_COUNTRY:-GB}"
ls "$OUT"/debs/vibeserver_*_"$A".deb >/dev/null 2>&1 || {
  echo "!! no $A package in $OUT/debs — run: image/pocket/build-deb.sh $A"; exit 1; }

if [ "$MODE" = "--pigen" ]; then
  # ── pi-gen ──────────────────────────────────────────────────────────────────────────────────
  BRANCH=arm64; [ "$A" = armhf ] && BRANCH=master
  PG="$OUT/work/pi-gen-$A"
  [ -d "$PG" ] || git clone --depth 1 --branch "$BRANCH" https://github.com/RPi-Distro/pi-gen.git "$PG"
  rm -rf "$PG/stage-pocket"
  cp -R "$HERE/stage-pocket" "$PG/stage-pocket"
  mkdir -p "$PG/stage-pocket/00-pocket/files/debs"
  cp "$OUT"/debs/vibeserver_*_"$A".deb "$PG/stage-pocket/00-pocket/files/debs/"
  touch "$PG/stage2/SKIP_IMAGES"        # ★ only OUR stage exports an image
  # ★ The login exists so first boot never stops to ask for one; its password is random and not
  #   kept — the owner sets their own with Raspberry Pi Imager (or never needs one: SSH is off).
  PASS="$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 24)"
  cat > "$PG/config" <<EOF
IMG_NAME=vibeserver-pocket-$A
RELEASE=trixie
DEPLOY_COMPRESSION=xz
COMPRESSION_LEVEL=6
LOCALE_DEFAULT=en_GB.UTF-8
KEYBOARD_KEYMAP=gb
KEYBOARD_LAYOUT="English (UK)"
TIMEZONE_DEFAULT=Europe/London
TARGET_HOSTNAME=vibepocket
FIRST_USER_NAME=vibe
FIRST_USER_PASS=$PASS
DISABLE_FIRST_BOOT_USER_RENAME=1
ENABLE_SSH=0
WPA_COUNTRY=$COUNTRY
STAGE_LIST="stage0 stage1 stage2 stage-pocket"
EOF
  (cd "$PG" && PIGEN_DOCKER_OPTS="--cpus 2" nice -n 15 ./build-docker.sh)
  ls -la "$PG/deploy"
  exit 0
fi

# ── Official Raspberry Pi OS Lite, customised ───────────────────────────────────────────────────
URL="https://downloads.raspberrypi.com/raspios_lite_${A}_latest"
docker run --rm --privileged --cpus 2 --platform linux/arm64 \
  -v "$OUT":/out -v "$HERE/stage-pocket/00-pocket/files":/pocket-files:ro \
  -e A="$A" -e URL="$URL" -e NAME="$NAME" -e COUNTRY="$COUNTRY" \
  debian:trixie-slim /bin/bash -euo pipefail -c '
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends ca-certificates curl xz-utils fdisk e2fsprogs dosfstools \
      util-linux qemu-user-static >/dev/null
  cd /out/work
  # ── 1. The base image, verified ──
  REAL=$(curl -fsSLI -o /dev/null -w "%{url_effective}" "$URL")
  BASE=/out/cache/$(basename "$REAL")
  [ -s "$BASE" ] || curl -fL --retry 3 -o "$BASE" "$REAL"
  WANT=$(curl -fsSL "$REAL.sha256" | cut -d" " -f1)
  GOT=$(sha256sum "$BASE" | cut -d" " -f1)
  [ "$WANT" = "$GOT" ] || { echo "!! checksum mismatch for $(basename "$BASE")"; rm -f "$BASE"; exit 1; }
  echo "==> base: $(basename "$BASE") (sha256 ok)"
  IMG=/out/work/$NAME.img
  nice -n 15 xz -dc "$BASE" > "$IMG"
  # ── 2. Room for VibeServer and its maps: +1.5 GB on the root partition ──
  truncate -s +1536M "$IMG"
  echo ", +" | sfdisk -q -N 2 "$IMG"
  # ★ Partition 1 = boot (FAT), 2 = root (ext4), in 512-byte sectors from sfdisk'"'"'s dump.
  PARTS=$(sfdisk -d "$IMG" | sed -n "s/.*start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2/p")
  read -r S1 Z1 S2 Z2 <<< "$(echo $PARTS)"
  S1=$((S1*512)); Z1=$((Z1*512)); S2=$((S2*512)); Z2=$((Z2*512))
  [ "$Z2" -gt 0 ] || { echo "!! could not read the partition table"; exit 1; }
  ROOTDEV=$(losetup -f --show -o "$S2" --sizelimit "$Z2" "$IMG")
  BOOTDEV=$(losetup -f --show -o "$S1" --sizelimit "$Z1" "$IMG")
  cleanup() {
    set +e
    for m in /mnt/r/dev/pts /mnt/r/dev /mnt/r/proc /mnt/r/sys /mnt/r/boot/firmware /mnt/r; do mountpoint -q $m && umount $m; done
    losetup -d "$BOOTDEV" "$ROOTDEV" 2>/dev/null
    [ -n "${BINFMT:-}" ] && [ -e /proc/sys/fs/binfmt_misc/qemu-arm-pocket ] && echo -1 > /proc/sys/fs/binfmt_misc/qemu-arm-pocket
  }
  trap cleanup EXIT
  e2fsck -fy "$ROOTDEV" >/dev/null || true
  resize2fs "$ROOTDEV" >/dev/null
  mkdir -p /mnt/r && mount "$ROOTDEV" /mnt/r
  mkdir -p /mnt/r/boot/firmware && mount "$BOOTDEV" /mnt/r/boot/firmware
  for d in dev dev/pts proc sys; do mount --bind /$d /mnt/r/$d; done
  # ── 3. armhf runs under qemu-arm (no AArch32 on Apple Silicon) — registered for this run only ──
  if [ "$A" = armhf ]; then
    mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
    if [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm-pocket ]; then
      printf "%s" ":qemu-arm-pocket:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-arm-static:F" \
        > /proc/sys/fs/binfmt_misc/register
      BINFMT=1
    fi
  fi
  # ── 4. Customise, inside the image ──
  cp /mnt/r/etc/resolv.conf /tmp/resolv.keep 2>/dev/null || true
  rm -f /mnt/r/etc/resolv.conf; cp /etc/resolv.conf /mnt/r/etc/resolv.conf
  mkdir -p /mnt/r/tmp/pocket-files /mnt/r/tmp/pocket-debs
  cp /pocket-files/customise.sh /pocket-files/config.json /pocket-files/pocket.conf /mnt/r/tmp/pocket-files/
  cp /out/debs/vibeserver_*_"$A".deb /mnt/r/tmp/pocket-debs/
  nice -n 15 chroot /mnt/r /usr/bin/env POCKET_COUNTRY="$COUNTRY" POCKET_HOSTNAME=vibepocket \
      bash /tmp/pocket-files/customise.sh
  rm -f /mnt/r/etc/resolv.conf
  [ -f /tmp/resolv.keep ] && cp /tmp/resolv.keep /mnt/r/etc/resolv.conf || true
  # ★ The SSH host keys and machine-id must be made on the box, not shared by every card.
  rm -f /mnt/r/etc/ssh/ssh_host_* 2>/dev/null || true
  : > /mnt/r/etc/machine-id
  du -sh /mnt/r/usr/lib/vibeserver 2>/dev/null | sed "s/^/==> vibeserver on the card: /"
  df -h /mnt/r | tail -1 | awk "{print \"==> root partition: \" \$3 \" used of \" \$2}"
  cleanup; trap - EXIT
  e2fsck -fy "$ROOTDEV" >/dev/null 2>&1 || true
  # ── 5. Compress ──
  nice -n 15 xz -T2 -6 -f "$IMG"
  sha256sum "$(basename "$IMG").xz" > "$(basename "$IMG").xz.sha256"
  mv "$IMG.xz" "$IMG.xz.sha256" /out/
  ls -la /out/$NAME.img.xz
'
echo "==> $OUT/$NAME.img.xz"
