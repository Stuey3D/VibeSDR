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
# ★★ THE HIGH DETAIL MAPS, BAKED IN (Stuart, 2026-10-10) — their URL and exact size are read from the
#    server's own source, so the image and the server's installer can never disagree about them.
MAPGL_H="$HERE/../../android/app/src/main/cpp/vibe_mapgl.h"
MAPGL_URL=$(sed -n '/DETAIL_URL =/{n;s/.*"\(https[^"]*\)".*/\1/p;}' "$MAPGL_H")
MAPGL_BYTES=$(sed -n 's/.*DETAIL_BYTES = \([0-9]*\);.*/\1/p' "$MAPGL_H")
[ -n "$MAPGL_URL" ] && [ -n "$MAPGL_BYTES" ] || { echo "!! could not read DETAIL_URL / DETAIL_BYTES from vibe_mapgl.h"; exit 1; }
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
  cp "$("$HERE/stage-pocket/00-pocket/files/fetch-mapgl-detail.sh" "$OUT/cache" "$MAPGL_URL" "$MAPGL_BYTES")" \
     "$PG/stage-pocket/00-pocket/files/vibemap-detail.pmtiles"
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
TARGET_HOSTNAME=vibeserversetup
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
# ★★ THE BUILD HOST (2026-10-25): the Mac (arm64, colima) as before, or an x86-64 Linux box (the Lenovo). On x86 the
#    container is native amd64 and BOTH image architectures run under QEMU in the chroot (only apt/dpkg run there).
case "$(uname -m)" in x86_64|amd64) PLAT=linux/amd64 ;; *) PLAT=linux/arm64 ;; esac
docker run --rm --privileged --cpus 2 --platform "$PLAT" -v /dev:/dev \
  -v "$OUT":/out -v "$HERE/stage-pocket/00-pocket/files":/pocket-files:ro \
  -e A="$A" -e URL="$URL" -e NAME="$NAME" -e COUNTRY="$COUNTRY" \
  -e MAPGL_URL="$MAPGL_URL" -e MAPGL_BYTES="$MAPGL_BYTES" -e POCKET_ALLOW_NO_ARMV6="${POCKET_ALLOW_NO_ARMV6:-0}" \
  debian:trixie-slim /bin/bash -euo pipefail -c '
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends ca-certificates curl xz-utils fdisk e2fsprogs dosfstools \
      util-linux qemu-user-static binutils >/dev/null
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
  # ── 2. WORKING room for VibeServer, its packages and the maps: +4 GB on the root partition ──
  # ★★★ WORKING ROOM ONLY — step 5 shrinks root to what it holds + 1 GB, so the image is no bigger.
  #     It was +1.5 GB until 2026-10-10: once ffmpeg (~400 MB of libraries) was added, used + 1 GB no
  #     longer fitted, sfdisk could not place partitions 2 and 3 yet RETURNED SUCCESS, and the v6 image
  #     shipped with boot + root only and its settings filesystem outside any partition. Every setting
  #     then lived in RAM until the first restart. The layout check in step 5 now fails such a build.
  truncate -s +4G "$IMG"
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
    for m in /mnt/r/proc/cpuinfo /mnt/r/dev/pts /mnt/r/dev /mnt/r/proc /mnt/r/sys /mnt/r/boot/firmware /mnt/r; do mountpoint -q $m && umount $m; done
    losetup -d "$BOOTDEV" "$ROOTDEV" 2>/dev/null
    for b in qemu-arm-pocket qemu-aarch64-pocket; do
      [ -n "${BINFMT:-}" ] && [ -e /proc/sys/fs/binfmt_misc/$b ] && echo -1 > /proc/sys/fs/binfmt_misc/$b
    done
  }
  trap cleanup EXIT
  e2fsck -fy "$ROOTDEV" >/dev/null || true
  resize2fs "$ROOTDEV" >/dev/null
  mkdir -p /mnt/r && mount "$ROOTDEV" /mnt/r
  mkdir -p /mnt/r/boot/firmware && mount "$BOOTDEV" /mnt/r/boot/firmware
  for d in dev dev/pts proc sys; do mount --bind /$d /mnt/r/$d; done
  # ── 3. armhf runs under qemu-arm (no AArch32 on Apple Silicon); on an x86 host arm64 does too — registered for
  #       this run only, and only when the chroot cannot already run the programs of the image ──
  if ! chroot /mnt/r /bin/true 2>/dev/null; then
    mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
    if [ "$A" = armhf ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm-pocket ]; then
      printf "%s" ":qemu-arm-pocket:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-arm-static:F" \
        > /proc/sys/fs/binfmt_misc/register
      BINFMT=1
    fi
    if [ "$A" = arm64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64-pocket ]; then
      printf "%s" ":qemu-aarch64-pocket:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:F" \
        > /proc/sys/fs/binfmt_misc/register
      BINFMT=1
    fi
    chroot /mnt/r /bin/true || { echo "!! cannot run $A programs in the chroot"; exit 1; }
  fi
  # ★★★ THE PROCESSOR THE PACKAGE SEES (2026-10-25). The armhf package picks its build in postinst from /proc/cpuinfo —
  #     here the BUILD HOST cpuinfo: a Mac VM says asimd (NEON build kept, by luck), an x86 host says neither (the SCALAR
  #     build would go on the card, and a Pi 2 would run at ARMv6 speed for ever). So the install sees an ARMv7+NEON
  #     cpuinfo, the card always carries the NEON build, and vibeserver-pocket-cpu swaps in the ARMv6 one at boot on a
  #     Pi 0 / Pi 1 — the 0/1/2 card. Checked with readelf after the customisation.
  if [ "$A" = armhf ]; then
    printf "processor\t: 0\nmodel name\t: ARMv7 Processor rev 5 (v7l)\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\nCPU architecture: 7\n\nHardware\t: BCM2835 (image build: image/pocket/build-image.sh)\n" > /tmp/cpuinfo.neon
    mount --bind /tmp/cpuinfo.neon /mnt/r/proc/cpuinfo
  fi
  # ── 4. Customise, inside the image ──
  cp /mnt/r/etc/resolv.conf /tmp/resolv.keep 2>/dev/null || true
  rm -f /mnt/r/etc/resolv.conf; cp /etc/resolv.conf /mnt/r/etc/resolv.conf
  mkdir -p /mnt/r/tmp/pocket-files /mnt/r/tmp/pocket-debs
  cp /pocket-files/customise.sh /pocket-files/config.json /pocket-files/pocket.conf /mnt/r/tmp/pocket-files/
  cp /out/debs/vibeserver_*_"$A".deb /mnt/r/tmp/pocket-debs/
  MAPS=$(bash /pocket-files/fetch-mapgl-detail.sh /out/cache "$MAPGL_URL" "$MAPGL_BYTES")
  install -d -m 0755 /mnt/r/usr/lib/vibeserver/mapgl-detail
  install -m 0644 "$MAPS" /mnt/r/usr/lib/vibeserver/mapgl-detail/vibemap-detail.pmtiles
  echo "==> High Detail Maps baked in: $(du -h /mnt/r/usr/lib/vibeserver/mapgl-detail/vibemap-detail.pmtiles | cut -f1) (verified: $MAPGL_BYTES bytes, PMTiles)"
  nice -n 15 chroot /mnt/r /usr/bin/env POCKET_COUNTRY="$COUNTRY" POCKET_HOSTNAME=vibeserversetup \
      bash /tmp/pocket-files/customise.sh
  if [ "$A" = armhf ]; then
    umount /mnt/r/proc/cpuinfo
    [ "$(readelf -A /mnt/r/usr/bin/vibeserver | grep -c -e "Tag_CPU_arch: v7" -e "Tag_Advanced_SIMD_arch: NEON" || true)" = 2 ] \
      || { echo "!! /usr/bin/vibeserver on the card is not the ARMv7+NEON build"; exit 1; }
    echo "==> /usr/bin/vibeserver: ARMv7 + NEON (the Pi 2 build)"
    if [ -x /mnt/r/usr/lib/vibeserver/armv6/vibeserver ] && [ -f /mnt/r/lib/systemd/system/vibeserver-pocket-cpu.service ]; then
      echo "==> ARMv6 build + vibeserver-pocket-cpu present: a Pi 0 / Pi 1 picks the scalar build at boot"
    elif [ "$POCKET_ALLOW_NO_ARMV6" = 1 ]; then
      echo "!! NO ARMv6 build or no vibeserver-pocket-cpu in this package — this card will NOT run on a Pi 0 / Pi 1 (allowed by POCKET_ALLOW_NO_ARMV6=1)"
    else
      echo "!! this package lacks the ARMv6 build or vibeserver-pocket-cpu (11.0.11+) — the card would not run on a Pi 0 / Pi 1."
      echo "   Use the published package (image/pocket/fetch-release-deb.sh armhf), or POCKET_ALLOW_NO_ARMV6=1 for a test card."
      exit 1
    fi
  fi
  rm -f /mnt/r/etc/resolv.conf
  [ -f /tmp/resolv.keep ] && cp /tmp/resolv.keep /mnt/r/etc/resolv.conf || true
  # ★ The SSH host keys and machine-id must be made on the box, not shared by every card.
  rm -f /mnt/r/etc/ssh/ssh_host_* 2>/dev/null || true
  : > /mnt/r/etc/machine-id
  du -sh /mnt/r/usr/lib/vibeserver 2>/dev/null | sed "s/^/==> vibeserver on the card: /"
  df -h /mnt/r | tail -1 | awk "{print \"==> root partition: \" \$3 \" used of \" \$2}"
  cleanup; trap - EXIT
  # ── 5. Shrink root to what it holds + 1 GB (room for an update in the read-only root), then add the
  #       small settings partition (LABEL=vibedata, 256 MB — never grown) after it, and cut the file
  #       there. Everything after it on the card stays UNALLOCATED, kept for a future gallery partition.
  ROOTDEV=$(losetup -f --show -o "$S2" --sizelimit "$Z2" "$IMG")
  e2fsck -fy "$ROOTDEV" >/dev/null 2>&1 || true
  resize2fs -M "$ROOTDEV" >/dev/null 2>&1
  BS=$(dumpe2fs -h "$ROOTDEV" 2>/dev/null | awk -F: "/^Block size/{gsub(/ /,\"\",\$2);print \$2}")
  BC=$(dumpe2fs -h "$ROOTDEV" 2>/dev/null | awk -F: "/^Block count/{gsub(/ /,\"\",\$2);print \$2}")
  ALIGN=$((4*1024*1024))
  NEWROOT=$(( ( (BC*BS + 1024*1024*1024) + ALIGN - 1) / ALIGN * ALIGN ))
  resize2fs "$ROOTDEV" "$((NEWROOT/1024))K" >/dev/null
  e2fsck -fy "$ROOTDEV" >/dev/null 2>&1 || true
  losetup -d "$ROOTDEV"
  S2S=$((S2/512)); N2S=$((NEWROOT/512))
  [ "$((S2S + N2S))" -le "$(( $(stat -c %s "$IMG") / 512 ))" ] \
    || { echo "!! LAYOUT: root needs $((NEWROOT/1024/1024)) MB, more than the working room in step 2 — raise it"; exit 1; }
  echo "$S2S,$N2S" | sfdisk -q --no-reread -N 2 "$IMG"
  [ "$(sfdisk -d "$IMG" | sed -n "s/.*[^0-9]2 *: *start= *[0-9]*, *size= *\([0-9]*\).*/\1/p")" = "$N2S" ] \
    || { echo "!! LAYOUT: partition 2 was not resized to $N2S sectors"; exit 1; }
  S3S=$(( (S2S + N2S + 8191) / 8192 * 8192 )); Z3S=$((256*1024*1024/512))
  echo "$S3S,$Z3S,83" | sfdisk -q --no-reread --append "$IMG"
  truncate -s $(( (S3S + Z3S) * 512 )) "$IMG"
  DATADEV=$(losetup -f --show -o $((S3S*512)) --sizelimit $((Z3S*512)) "$IMG")
  mkfs.ext4 -q -F -L vibedata "$DATADEV"
  losetup -d "$DATADEV"
  # ★★★ PROVE THE LAYOUT ON THE FINISHED FILE (the v6 card of 2026-10-10 had boot + root only:
  #     every setting sat in RAM until the first restart). Three partitions; partition 3 at S3S, 256 MB;
  #     and LABEL=vibedata read back AT THE OFFSET THE TABLE GIVES, not the one we meant to write.
  T3=$(sfdisk -d "$IMG" | sed -n "s/.*[^0-9]3 *: *start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2/p")
  [ "$(sfdisk -d "$IMG" | grep -c "start=")" = 3 ] || { echo "!! LAYOUT: the image does not have 3 partitions"; sfdisk -d "$IMG"; exit 1; }
  [ "$T3" = "$S3S $Z3S" ] || { echo "!! LAYOUT: partition 3 is \"$T3\", expected \"$S3S $Z3S\""; exit 1; }
  CHK=$(losetup -f --show -r -o $((S3S*512)) --sizelimit $((Z3S*512)) "$IMG")
  LBL=$(blkid -p -o value -s LABEL "$CHK" 2>/dev/null || true); losetup -d "$CHK"
  [ "$LBL" = vibedata ] || { echo "!! LAYOUT: no vibedata filesystem at partition 3 (found \"$LBL\")"; exit 1; }
  echo "==> layout verified: partition 3 at sector $S3S, 256 MB, LABEL=vibedata"
  sfdisk -l "$IMG" | sed "s/^/==> /" | tail -4
  echo "==> image: $(( (S3S + Z3S) * 512 / 1024 / 1024 )) MB raw (root $((NEWROOT/1024/1024)) MB, settings 256 MB)"
  # ── 5. Compress ──
  nice -n 15 xz -T2 -6 -f "$IMG"
  sha256sum "$(basename "$IMG").xz" > "$(basename "$IMG").xz.sha256"
  mv "$IMG.xz" "$IMG.xz.sha256" /out/
  ls -la /out/$NAME.img.xz
'
echo "==> $OUT/$NAME.img.xz"
