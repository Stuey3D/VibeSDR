#!/usr/bin/env bash
# shellcheck disable=SC2015  # A && B || die is meant
# image/sbc/build.sh — a VibeServer PORTABLE card image for a non-Raspberry-Pi board, from that board's official
# Armbian image (2026-10-25).
#
#   sudo image/sbc/build.sh <board> <Armbian_…_<Board>_trixie_…_minimal.img.xz> <output-dir>
#   DRY_RUN=1 image/sbc/build.sh <board> [<base.img.xz>] [<output-dir>]     # the plan and the checks; no root needed
#   image/sbc/run-in-docker.sh <board> <base.img.xz> <output-dir>           # the same, in a privileged container
#
# ★★★ THE RASPBERRY PI PORTABLE, ON ANOTHER BOARD. image/pocket/build-image.sh takes Raspberry Pi OS Lite exactly as
#     Raspberry Pi publish it, chroots in and runs ONE customisation script. This does the same with the board's
#     official Armbian minimal image: verified (checksum + Armbian's signature), grown, chrooted (under QEMU on an x86
#     host), customised by customise-armbian.sh pre → the Pi image's OWN customise.sh → customise-armbian.sh post,
#     shrunk, and given the same small settings partition (LABEL=vibedata, 256 MB) straight after root. The box it
#     makes is the same Portable: open "VibeServer" setup hotspot when the board has Wi-Fi, VibeServerSetup.local:48000
#     on Ethernet always, the same setup page, the same updates (Install updates on the admin page), the same baked
#     High Detail Maps. See docs/SBC-IMAGES-PLAN.md.
# ★★ NOTHING BOARD-SPECIFIC IS TOUCHED. Armbian's U-Boot lives OUTSIDE the partition (the MBR's boot code and the SPL
#    from 8 KiB up to the partition start): fingerprinted before, compared after, and the build fails if one byte moved.
#    The kernel, the device tree and armbianEnv.txt are Armbian's.
# ★★ NO PASSWORD IS MADE OR STORED. root keeps Armbian's documented default, still flagged for Armbian's forced change at
#    the first login; sshd is OFF (the first network is an OPEN hotspot) and the converge masks the console gettys, as
#    on the Pi image. The VibeServer admin password is typed by the owner in setup (vibeserver/pocket.cpp, claim).
#
# Needs, on a Linux host, as root: losetup, sfdisk, e2fsck, resize2fs, dumpe2fs, mkfs.ext4, blkid, xz, sha256sum,
# gpg, curl, readelf, and qemu-user-static (qemu-arm / qemu-aarch64) — run-in-docker.sh installs all of them in a
# throwaway Debian container. A qemu binfmt entry is registered for this run only if none exists, and removed at exit.
#   VIBESERVER_DEB=/path/to.deb   install this local package instead of apt.vibesdr.net's stable one (a branch build)
#   COUNTRY=GB                    the Wi-Fi country (setup can change it)
#   NO_MAPS=1                     leave the High Detail Maps out (a smaller image; the server can fetch them later)
#   KEEP_WORK=1                   keep the work directory
# ★ CPU: everything heavy runs under nice -n 19 and xz -T2. ONE build at a time.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
POCKET_FILES="$REPO/image/pocket/stage-pocket/00-pocket/files"
BOARD="${1:?usage: build.sh <board> <armbian-minimal.img.xz> <output-dir>  (DRY_RUN=1 for the plan)}"
BASE_XZ="${2:-}"
OUT="${3:-}"
COUNTRY="${COUNTRY:-GB}"
IMAGE_VERSION="${IMAGE_VERSION:-1}"
# Armbian's image signing key (Igor Pecovnik) — every dl.armbian.com .asc is made with it (as image/soft66ip-dietpi).
ARMBIAN_IMAGE_KEY="DF00FAF1C577104B50BF1D0093D6889F9F0E78D5"
GROW_MIB=3072             # working room for NetworkManager, VibeServer, ffmpeg and the maps; shrunk again afterwards
HEADROOM_MIB=1024         # left free in root after the shrink — room for an Update in the read-only root (as the Pi)
DATA_MIB=256              # the settings partition, never grown (as the Pi)

say()  { printf '==> %s\n' "$*"; }
die()  { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

# ── 0. The board ──────────────────────────────────────────────────────────────────────────────────────────────────
LINE="$(awk -v b="$BOARD" '$1 == b { print; exit }' "$HERE/boards.txt")"
[ -n "$LINE" ] || die "unknown board '$BOARD' — boards.txt lists: $(awk '!/^#/ && NF { printf "%s ", $1 }' "$HERE/boards.txt")"
read -r _ FAMILY ARCH NEON WIFI RAM_MB CHANNEL STATUS NAME <<< "$LINE"
case "$ARCH" in
  armhf) QEMU=qemu-arm;     ELF_MAGIC='\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00'
         ELF_MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff' ;;
  arm64) QEMU=qemu-aarch64; ELF_MAGIC='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
         ELF_MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff' ;;
  *) die "boards.txt: unknown arch '$ARCH' for $BOARD" ;;
esac
ONBOARDING="Ethernet first: VibeServerSetup.local:48000 (no onboard Wi-Fi; a USB Wi-Fi adapter gets the setup hotspot)"
[ "$WIFI" = none ] || ONBOARDING="open \"VibeServer\" setup hotspot on the onboard $WIFI, and VibeServerSetup.local:48000 on Ethernet"
OUT_NAME="vibeserver-portable-$BOARD-$(date -u +%Y%m%d)"

if [ "${DRY_RUN:-0}" = 1 ]; then
  cat <<EOF
board ........ $BOARD — $NAME
family ....... $FAMILY ($ARCH, NEON: $NEON, smallest RAM: $RAM_MB MB, Armbian channel: $CHANNEL, status: $STATUS)
base ......... ${BASE_XZ:-<Armbian_…_${BOARD}_trixie_current_…_minimal.img.xz with .sha and .asc>} — from https://dl.armbian.com/$BOARD/
onboarding ... $ONBOARDING
package ...... ${VIBESERVER_DEB:-vibeserver from https://apt.vibesdr.net stable (the newest $ARCH build)}
steps ........ verify sha + Armbian signature → decompress → grow root +$GROW_MIB MB → chroot under $QEMU
               → customise-armbian.sh pre → image/pocket customise.sh → customise-armbian.sh post
               → shrink root to used + $HEADROOM_MIB MB → settings partition $DATA_MIB MB (LABEL=vibedata)
               → prove boot region unchanged, layout, NEON build, units → xz
output ....... ${OUT:-<output-dir>}/$OUT_NAME.img.xz (+ .sha256, manifest)
EOF
  for f in "$HERE/customise-armbian.sh" "$POCKET_FILES/customise.sh" "$POCKET_FILES/config.json" "$POCKET_FILES/pocket.conf" \
           "$POCKET_FILES/fetch-mapgl-detail.sh"; do
    [ -f "$f" ] && echo "  ok  $f" || die "missing $f"
  done
  bash -n "$HERE/customise-armbian.sh" && bash -n "$POCKET_FILES/customise.sh" && echo "  ok  both customisation scripts parse"
  exit 0
fi

[ -n "$BASE_XZ" ] && [ -n "$OUT" ] || die "usage: build.sh <board> <armbian-minimal.img.xz> <output-dir>"
[ "$(id -u)" = 0 ] || die "run as root (it loop-mounts the image)"
for t in losetup sfdisk e2fsck resize2fs dumpe2fs mkfs.ext4 blkid xz sha256sum gpg curl mount umount chroot readelf; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
[ -f "$BASE_XZ" ] && [ -f "$BASE_XZ.sha" ] && [ -f "$BASE_XZ.asc" ] || die "need $BASE_XZ with .sha and .asc beside it"
case "$(basename "$BASE_XZ")" in
  *_[Tt]rixie_*_minimal.img.xz) ;;
  *) die "expected an Armbian Trixie minimal image (…_trixie_…_minimal.img.xz), got $(basename "$BASE_XZ")" ;;
esac
[ -z "${VIBESERVER_DEB:-}" ] || [ -f "$VIBESERVER_DEB" ] || die "VIBESERVER_DEB=$VIBESERVER_DEB does not exist"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$OUT/work-$BOARD"
rm -rf "$WORK"; mkdir -p "$WORK"
IMG="$WORK/stage.img"
R="$WORK/root"
LOOP=""
BINFMT_OURS=""
cleanup() {
  set +e
  for m in "$R/proc/cpuinfo" "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run" "$R"; do
    mountpoint -q "$m" 2>/dev/null && umount "$m"
  done
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  if [ -n "$BINFMT_OURS" ] && [ -e "/proc/sys/fs/binfmt_misc/$BINFMT_OURS" ]; then
    echo -1 > "/proc/sys/fs/binfmt_misc/$BINFMT_OURS" && echo "==> removed the temporary $BINFMT_OURS binfmt entry"
  fi
  [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
}
trap cleanup EXIT
loop_p1() {   # attach just partition 1 of $IMG (offset loop: no partition scanning needed inside a container)
  local s z
  read -r s z <<< "$(sfdisk -d "$IMG" | sed -n 's/.*[^0-9]1 *: *start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2/p')"
  LOOP="$(losetup -f --show -o $((s * 512)) --sizelimit $((z * 512)) "$IMG")"
}

# ── 1. The base image: checksum and Armbian's signature ─────────────────────────────────────────────────────────────
say "$NAME — base $(basename "$BASE_XZ")"
( cd "$(dirname "$BASE_XZ")" && sha256sum -c "$(basename "$BASE_XZ").sha" ) || die "checksum does not match"
GNUPGHOME="$WORK/gnupg"; export GNUPGHOME; mkdir -m 700 "$GNUPGHOME"
curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$ARMBIAN_IMAGE_KEY" -o "$WORK/armbian-image.key"
gpg --batch -q --import "$WORK/armbian-image.key"
gpg --batch --status-fd 1 --verify "$BASE_XZ.asc" "$BASE_XZ" 2>/dev/null > "$WORK/gpg.status" || true
N="$(grep -c "^\[GNUPG:\] VALIDSIG $ARMBIAN_IMAGE_KEY " "$WORK/gpg.status" || true)"
[ "${N:-0}" -ge 1 ] || die "the image is not signed by Armbian's key $ARMBIAN_IMAGE_KEY"
say "signature: good, Armbian image key $ARMBIAN_IMAGE_KEY"
nice -n 19 xz -dc -T2 "$BASE_XZ" > "$IMG"
[ "$(sfdisk -d "$IMG" | grep -c 'start=' || true)" = 1 ] || die "expected ONE partition (an Armbian minimal image)"
S1="$(sfdisk -d "$IMG" | sed -n 's/.*start= *\([0-9]*\),.*/\1/p' | head -1)"
[ -n "$S1" ] && [ "$S1" -gt 16 ] || die "unexpected partition layout"
# ★ The boot loader is OUTSIDE the partition: MBR boot code (bytes 0-445) and U-Boot (sector 1 up to the partition).
boot_region_sums() {
  { dd if="$1" bs=1 count=446 status=none | sha256sum | cut -d' ' -f1
    dd if="$1" bs=512 skip=1 count=$(( S1 - 1 )) status=none | sha256sum | cut -d' ' -f1; } | paste -sd' '
}
BOOT_BEFORE="$(boot_region_sums "$IMG")"
say "boot region (MBR code, sectors 1-$((S1 - 1))): $BOOT_BEFORE"

# ── 2. Working room ────────────────────────────────────────────────────────────────────────────────────────────────
truncate -s "+${GROW_MIB}M" "$IMG"
echo ', +' | sfdisk -q --no-reread -N 1 "$IMG"
loop_p1
e2fsck -fy "$LOOP" >/dev/null || [ $? -le 1 ] || die "e2fsck failed"
resize2fs "$LOOP" >/dev/null 2>&1
ROOT_UUID="$(blkid -p -o value -s UUID "$LOOP")"
mkdir -p "$R"; mount "$LOOP" "$R"
cp "$R/etc/armbian-release" "$OUT/$OUT_NAME.armbian-release.txt"
REL_BOARD="$(sed -n 's/^BOARD=//p' "$R/etc/armbian-release" | head -1)"
REL_FAMILY="$(sed -n 's/^LINUXFAMILY=//p' "$R/etc/armbian-release" | head -1)"
[ "$REL_BOARD" = "$BOARD" ] || die "this image is for '$REL_BOARD', not '$BOARD'"
[ "$REL_FAMILY" = "$FAMILY" ] || die "boards.txt says family $FAMILY, the image says $REL_FAMILY"
grep -q "UUID=$ROOT_UUID" "$R/boot/armbianEnv.txt" || die "armbianEnv.txt does not boot root by UUID $ROOT_UUID"

# ── 3. Emulation, for this run only ────────────────────────────────────────────────────────────────────────────────
if ! chroot "$R" /bin/true 2>/dev/null; then
  Q="$(command -v "$QEMU-static" || command -v "$QEMU" || true)"
  [ -n "$Q" ] || die "no $QEMU-static (install qemu-user-static)"
  mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
  BINFMT_OURS="$QEMU-vibesbc"
  # ★ F (fix binary): the kernel opens the interpreter now, so it works inside the chroot without copying it in.
  printf '%s' ":$BINFMT_OURS:M::$ELF_MAGIC:$ELF_MASK:$Q:F" > /proc/sys/fs/binfmt_misc/register
  say "registered $QEMU with binfmt_misc for this run ($Q)"
  chroot "$R" /bin/true || die "cannot execute $ARCH binaries in the chroot"
fi
for d in dev dev/pts proc sys run; do mkdir -p "$R/$d"; mount --bind "/$d" "$R/$d"; done
# ★★★ THE PROCESSOR THE PACKAGE SEES. The armhf package carries two builds — /usr/bin/vibeserver (ARMv7 + NEON) and
#     lib/vibeserver/armv6 (scalar) — and its postinst picks by grepping /proc/cpuinfo for neon|asimd. In this chroot
#     /proc is the BUILD HOST's: an x86 cpuinfo has neither word, so the Pi 2-speed build would be swapped for the
#     scalar one on a board that has NEON. So the chroot sees an ARMv7-with-NEON cpuinfo for the install, and step 6
#     checks the result. (A board family without NEON would need the armv6 build — none is in boards.txt.)
if [ "$ARCH" = armhf ] && [ "$NEON" = yes ]; then
  cat > "$WORK/cpuinfo" <<'EOF'
processor	: 0
model name	: ARMv7 Processor rev 5 (v7l)
Features	: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm
CPU implementer	: 0x41
CPU architecture: 7
CPU variant	: 0x0
CPU part	: 0xc07
CPU revision	: 5

Hardware	: Allwinner sun8i Family (image build: image/sbc/build.sh)
EOF
  mount --bind "$WORK/cpuinfo" "$R/proc/cpuinfo"
fi
RESOLV_LINK="$(readlink "$R/etc/resolv.conf" || true)"
[ -e "$R/etc/resolv.conf.vibe-keep" ] || mv "$R/etc/resolv.conf" "$R/etc/resolv.conf.vibe-keep" 2>/dev/null || true
cp -L /etc/resolv.conf "$R/etc/resolv.conf"
# ★ Nothing starts inside the chroot (a package's postinst would otherwise try to start its service).
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 0755 "$R/usr/sbin/policy-rc.d"

# ── 4. Customise: Armbian → the Pi Portable's own customise.sh → Armbian ───────────────────────────────────────────
mkdir -p "$R/tmp/pocket-files" "$R/tmp/pocket-debs"
cp "$POCKET_FILES/customise.sh" "$POCKET_FILES/config.json" "$POCKET_FILES/pocket.conf" "$R/tmp/pocket-files/"
cp "$HERE/customise-armbian.sh" "$R/tmp/pocket-files/"
if [ -n "${VIBESERVER_DEB:-}" ]; then cp "$VIBESERVER_DEB" "$R/tmp/pocket-debs/"; fi
if [ "${NO_MAPS:-0}" != 1 ]; then
  MAPGL_H="$REPO/android/app/src/main/cpp/vibe_mapgl.h"
  MAPGL_URL=$(sed -n '/DETAIL_URL =/{n;s/.*"\(https[^"]*\)".*/\1/p;}' "$MAPGL_H")
  MAPGL_BYTES=$(sed -n 's/.*DETAIL_BYTES = \([0-9]*\);.*/\1/p' "$MAPGL_H")
  [ -n "$MAPGL_URL" ] && [ -n "$MAPGL_BYTES" ] || die "could not read DETAIL_URL / DETAIL_BYTES from vibe_mapgl.h"
  MAPS="$(bash "$POCKET_FILES/fetch-mapgl-detail.sh" "$OUT/cache" "$MAPGL_URL" "$MAPGL_BYTES")"
  install -d -m 0755 "$R/usr/lib/vibeserver/mapgl-detail"
  install -m 0644 "$MAPS" "$R/usr/lib/vibeserver/mapgl-detail/vibemap-detail.pmtiles"
  say "High Detail Maps baked in ($MAPGL_BYTES bytes, PMTiles)"
fi
say "customising (emulated $ARCH: slow)"
nice -n 19 chroot "$R" /bin/bash /tmp/pocket-files/customise-armbian.sh pre
nice -n 19 chroot "$R" /usr/bin/env POCKET_COUNTRY="$COUNTRY" POCKET_HOSTNAME=vibeserversetup \
    /bin/bash /tmp/pocket-files/customise.sh
nice -n 19 chroot "$R" /bin/bash /tmp/pocket-files/customise-armbian.sh post
chroot "$R" /usr/bin/dpkg-query -W -f '${Package} ${Version} ${Architecture}\n' \
    vibeserver network-manager avahi-daemon overlayroot ffmpeg 'linux-image-*' 'linux-u-boot-*' 'linux-dtb-*' \
    > "$OUT/$OUT_NAME.packages.txt" 2>/dev/null || true
rm -f "$R/usr/sbin/policy-rc.d"
rm -rf "$R/tmp/pocket-files" "$R/tmp/pocket-debs"
rm -f "$R/etc/resolv.conf"
if [ -n "$RESOLV_LINK" ]; then ln -s "$RESOLV_LINK" "$R/etc/resolv.conf"; rm -f "$R/etc/resolv.conf.vibe-keep"
else mv -f "$R/etc/resolv.conf.vibe-keep" "$R/etc/resolv.conf" 2>/dev/null || true; fi
# ★ Made on the box, not shared by every card (Armbian's first run makes new SSH host keys itself).
: > "$R/etc/machine-id"
rm -f "$R/var/lib/dbus/machine-id"

# ── 5. Prove the root is a Portable ────────────────────────────────────────────────────────────────────────────────
check() { if eval "$2"; then echo "  ok  $1"; else die "CHECK FAILED: $1"; fi; }
wants() { [ -e "$R/etc/systemd/system/multi-user.target.wants/$1" ] || [ -e "$R/etc/systemd/system/$2.wants/$1" ]; }
say "checking the root"
check "the Portable marker"                     '[ -f "$R/etc/vibeserver-pocket/enabled" ]'
check "hostname vibeserversetup"                '[ "$(cat "$R/etc/hostname")" = vibeserversetup ]'
check "first-boot config (not configured)"      'grep -q "\"configured\": false" "$R/etc/vibeserver/config.json"'
check "vibeserver-pocket enabled"               'wants vibeserver-pocket.service multi-user.target'
check "vibeserver-pocket-data enabled"          'wants vibeserver-pocket-data.service multi-user.target'
check "vibeserver-pocket-seal enabled"          'wants vibeserver-pocket-seal.service multi-user.target'
check "vibeserver enabled"                      'wants vibeserver.service multi-user.target'
check "NetworkManager enabled"                  'wants NetworkManager.service multi-user.target'
check "avahi-daemon enabled"                    'wants avahi-daemon.service multi-user.target'
check "systemd-networkd off"                    '! wants systemd-networkd.service multi-user.target'
check "sshd off"                                '! wants ssh.service multi-user.target'
check "netplan hands Ethernet to NetworkManager" 'grep -q "renderer: NetworkManager" "$R/etc/netplan/10-vibeserver-networkmanager.yaml"'
check "Armbian root resize off"                 '[ -f "$R/root/.no_rootfs_resize" ]'
check "nmcli, iw, dnsmasq, nft, avahi present"  '[ -x "$R/usr/bin/nmcli" ] && [ -x "$R/usr/sbin/iw" -o -x "$R/usr/bin/iw" ] && [ -x "$R/usr/sbin/dnsmasq" ] && [ -x "$R/usr/sbin/nft" ] && [ -x "$R/usr/sbin/avahi-daemon" ]'
check "overlayroot-chroot present"              '[ -x "$R/usr/sbin/overlayroot-chroot" ]'
check "root's password is NOT ours (Armbian's, forced change pending)" '[ -f "$R/root/.not_logged_in_yet" ]'
check "the U-Boot package is the board's"       'grep -q "linux-u-boot-$BOARD-" "$OUT/$OUT_NAME.packages.txt"'
if [ "$NEON" = yes ] && [ "$ARCH" = armhf ]; then
  check "/usr/bin/vibeserver is the ARMv7 + NEON build" \
        'readelf -A "$R/usr/bin/vibeserver" | grep -q "Tag_CPU_arch: v7" && readelf -A "$R/usr/bin/vibeserver" | grep -q "Tag_Advanced_SIMD_arch: NEON"'
fi
grep -E '^vibeserver ' "$OUT/$OUT_NAME.packages.txt" | sed 's/^/  ok  installed: /'
df -h "$R" | tail -1 | awk '{print "==> root: " $3 " used"}'
for d in dev/pts dev proc/cpuinfo proc sys run; do mountpoint -q "$R/$d" && umount "$R/$d"; done
umount "$R"
e2fsck -fy "$LOOP" >/dev/null 2>&1 || true

# ── 6. Shrink root to what it holds + headroom; the settings partition after it; cut the file there ─────────────────
resize2fs -M "$LOOP" >/dev/null 2>&1
BS="$(dumpe2fs -h "$LOOP" 2>/dev/null | sed -n 's/^Block size: *//p')"
BC="$(dumpe2fs -h "$LOOP" 2>/dev/null | sed -n 's/^Block count: *//p')"
ALIGN=$(( 4 * 1024 * 1024 ))
NEWROOT=$(( ( (BC * BS + HEADROOM_MIB * 1024 * 1024) + ALIGN - 1) / ALIGN * ALIGN ))
resize2fs "$LOOP" "$(( NEWROOT / 1024 ))K" >/dev/null 2>&1
e2fsck -fy "$LOOP" >/dev/null 2>&1 || true
losetup -d "$LOOP"; LOOP=""
N1=$(( NEWROOT / 512 ))
echo "$S1,$N1" | sfdisk -q --no-reread -N 1 "$IMG"
S2=$(( (S1 + N1 + 8191) / 8192 * 8192 )); Z2=$(( DATA_MIB * 1024 * 1024 / 512 ))
[ $(( (S2 + Z2) * 512 )) -le "$(stat -c %s "$IMG")" ] || die "LAYOUT: root + settings do not fit the working file — raise GROW_MIB"
echo "$S2,$Z2,83" | sfdisk -q --no-reread --append "$IMG"
truncate -s $(( (S2 + Z2) * 512 )) "$IMG"
LOOP="$(losetup -f --show -o $(( S2 * 512 )) --sizelimit $(( Z2 * 512 )) "$IMG")"
mkfs.ext4 -q -F -L vibedata "$LOOP"
losetup -d "$LOOP"; LOOP=""

# ── 7. Prove the finished file ─────────────────────────────────────────────────────────────────────────────────────
[ "$(sfdisk -d "$IMG" | grep -c 'start=' || true)" = 2 ] || { sfdisk -d "$IMG"; die "LAYOUT: not 2 partitions"; }
T2="$(sfdisk -d "$IMG" | sed -n 's/.*[^0-9]2 *: *start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2/p')"
[ "$T2" = "$S2 $Z2" ] || die "LAYOUT: partition 2 is '$T2', expected '$S2 $Z2'"
CHK="$(losetup -f --show -r -o $(( S2 * 512 )) --sizelimit $(( Z2 * 512 )) "$IMG")"
LBL="$(blkid -p -o value -s LABEL "$CHK" 2>/dev/null || true)"; losetup -d "$CHK"
[ "$LBL" = vibedata ] || die "LAYOUT: no vibedata filesystem at partition 2 (found '$LBL')"
loop_p1
U2="$(blkid -p -o value -s UUID "$LOOP")"; e2fsck -fn "$LOOP" >/dev/null 2>&1 || die "root filesystem check failed"
losetup -d "$LOOP"; LOOP=""
[ "$U2" = "$ROOT_UUID" ] || die "root UUID changed ($ROOT_UUID -> $U2): armbianEnv.txt would not find it"
BOOT_AFTER="$(boot_region_sums "$IMG")"
[ "$BOOT_BEFORE" = "$BOOT_AFTER" ] || die "the boot region changed: $BOOT_BEFORE -> $BOOT_AFTER"
say "boot region unchanged; layout: root $(( NEWROOT / 1024 / 1024 )) MB at $S1, vibedata $DATA_MIB MB at $S2; $(( (S2 + Z2) / 2048 )) MB raw"
sfdisk -d "$IMG" > "$OUT/$OUT_NAME.partition-table.txt"

# ── 8. Compress ────────────────────────────────────────────────────────────────────────────────────────────────────
{
  echo "VibeServer Portable for $NAME ($BOARD) — built $(date -u '+%Y-%m-%d %H:%M UTC') by image/sbc/build.sh"
  echo "base: $(basename "$BASE_XZ") (sha256 $(cut -d' ' -f1 "$BASE_XZ.sha"), signed by Armbian key $ARMBIAN_IMAGE_KEY)"
  echo "boot region: $BOOT_AFTER (identical to the base image)"
  echo "onboarding: $ONBOARDING"
  echo "packages:"; sed 's/^/  /' "$OUT/$OUT_NAME.packages.txt"
} > "$OUT/$OUT_NAME.manifest.txt"
mv "$IMG" "$OUT/$OUT_NAME.img"
nice -n 19 xz -T2 -6 -f "$OUT/$OUT_NAME.img"
( cd "$OUT" && sha256sum "$OUT_NAME.img.xz" > "$OUT_NAME.img.xz.sha256" )
say "built $OUT/$OUT_NAME.img.xz ($(du -h "$OUT/$OUT_NAME.img.xz" | cut -f1))"
cat "$OUT/$OUT_NAME.img.xz.sha256"
