#!/usr/bin/env bash
# shellcheck disable=SC2015,SC2016  # A && B || die is meant; ${Package} is dpkg-query's format, not ours
# VibeServer for the Soft66IP / Orange Pi Zero (Allwinner H2+/H3, armv7, 256 MB) — a DietPi image.
#
#   sudo ./build.sh <Armbian_…_Orangepizero_…_minimal.img.xz> <output-dir>
#
# ★★★ WHY ARMBIAN UNDERNEATH. DietPi publishes no image for the ORIGINAL Orange Pi Zero (checked
#     2026-10-24: dietpi.com/downloads/images/ has the Zero 2W/3/3W only, and the hardware list in
#     DietPi's dietpi-build has no H2+ Zero). DietPi's documented route for a board it has no image
#     for is its own installer, `dietpi-installer`, run "on the running target system, or when booting
#     the original image as a container" (DietPi docs, "DietPi-Installer"), choosing hardware model 25,
#     "Generic Allwinner H3". So this script does exactly what DietPi's own image builder
#     (.build/images/dietpi-build) does for its boards: boot the board's official Armbian image as a
#     QEMU-emulated systemd-nspawn container and run dietpi-installer inside it, non-interactively.
#     The installer KEEPS Armbian's kernel, device tree and U-Boot for this board (its "Armbian"
#     branch); nothing here touches a binary. The result boots as DietPi.
# ★★ THEN ONLY DIETPI'S DOCUMENTED FIRST-BOOT AUTOMATION: values in /boot/dietpi.txt
#    (dietpi.txt.overrides) and /boot/Automation_Custom_Script.sh, which runs our official installer.
# ★★ NO PASSWORDS ARE MADE OR STORED. See dietpi.txt.overrides.
#
# Needs, on an x86-64 or arm Linux host, as root: systemd-nspawn (package systemd-container),
# qemu-arm registered with binfmt_misc using the F (fix-binary) flag (package qemu-user-static does
# this), losetup, sfdisk, e2fsck, resize2fs, dumpe2fs, xz, sha256sum, gpg, curl.
#   NSPAWN=/path/to/systemd-nspawn          use a systemd-nspawn that is not on PATH
#   QEMU_ARM_BINFMT=/path/to/arm-binfmt-P    register qemu-arm with binfmt_misc for this run only
#                                            (removed again at exit), if none is registered. This is
#                                            the wrapper qemu-user-static ships in
#                                            /usr/libexec/qemu-binfmt/ (a copy extracted from the .deb
#                                            works without installing it)
#   TEST_INSTALL=1                           afterwards, on a throwaway copy: run the custom script for
#                                            real inside the container (installs VibeServer over the
#                                            network) and run the installed binary under QEMU
#   KEEP_WORK=1                              keep the work directory
# The Armbian image's .sha and .asc must sit beside it; the signature must be by Armbian's image key.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_XZ="${1:?usage: build.sh <armbian-orangepizero-minimal.img.xz> <output-dir>}"
OUT="${2:?usage: build.sh <armbian-orangepizero-minimal.img.xz> <output-dir>}"
NSPAWN="${NSPAWN:-systemd-nspawn}"
IMAGE_VERSION="${IMAGE_VERSION:-1}"
OUT_NAME="VibeServer-Soft66IP-OrangePiZero-DietPi-${IMAGE_VERSION}"
# Armbian's image signing key (Igor Pecovnik), the key every dl.armbian.com .asc is made with.
ARMBIAN_IMAGE_KEY="DF00FAF1C577104B50BF1D0093D6889F9F0E78D5"
GROW_TO_MIB=4096          # room for dietpi-installer's apt work; shrunk again afterwards
SLACK_MIB=64              # left free after the shrink; DietPi grows the root partition at first boot anyway

say()  { printf '==> %s\n' "$*"; }
die()  { printf 'build.sh: %s\n' "$*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (it loop-mounts the image and boots it in a container)"
for t in losetup sfdisk e2fsck resize2fs dumpe2fs xz sha256sum gpg curl mount umount; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
command -v "$NSPAWN" >/dev/null || [ -x "$NSPAWN" ] || die "systemd-nspawn not found (set NSPAWN=)"
[ -f "$BASE_XZ" ] && [ -f "$BASE_XZ.sha" ] && [ -f "$BASE_XZ.asc" ] || die "need $BASE_XZ with .sha and .asc beside it"
for f in dietpi.txt.overrides Automation_Custom_Script.sh; do [ -f "$HERE/$f" ] || die "missing $HERE/$f"; done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$OUT/work"
rm -rf "$WORK"; mkdir -p "$WORK"
IMG="$WORK/stage.img"
ROOTFS="$WORK/rootfs"
LOOP=""
BINFMT_OURS=0
APPARMOR_LOCAL=""

cleanup() {
  set +e
  mountpoint -q "$ROOTFS" 2>/dev/null && umount -R "$ROOTFS"
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  if [ "$BINFMT_OURS" = 1 ] && [ -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
    echo -1 > /proc/sys/fs/binfmt_misc/qemu-arm && echo "==> removed the temporary qemu-arm binfmt entry"
  fi
  if [ -n "$APPARMOR_LOCAL" ]; then
    rm -f "$APPARMOR_LOCAL" && apparmor_parser -r /etc/apparmor.d/unix-chkpwd 2>/dev/null
    echo "==> host AppArmor unix-chkpwd profile restored"
  fi
  [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
}
trap cleanup EXIT

attach() {   # loop-attach the image with partitions, mount p1 at $ROOTFS
  LOOP="$(losetup -f --show -P "$IMG")"
  udevadm settle 2>/dev/null || true
  [ -b "${LOOP}p1" ] || die "no ${LOOP}p1"
  mkdir -p "$ROOTFS"
  mount "${LOOP}p1" "$ROOTFS"
}
detach() { umount -R "$ROOTFS"; losetup -d "$LOOP"; LOOP=""; }

# ── 1. The base image: checksum and Armbian's signature ──────────────────────────────────────────
say "checking $BASE_XZ"
( cd "$(dirname "$BASE_XZ")" && sha256sum -c "$(basename "$BASE_XZ").sha" ) || die "checksum does not match"
GNUPGHOME="$WORK/gnupg"; export GNUPGHOME; mkdir -m 700 "$GNUPGHOME"
curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$ARMBIAN_IMAGE_KEY" -o "$WORK/armbian-image.key"
gpg --batch -q --import "$WORK/armbian-image.key"
gpg --batch --status-fd 1 --verify "$BASE_XZ.asc" "$BASE_XZ" 2>/dev/null > "$WORK/gpg.status" || true
grep -q "^\[GNUPG:\] VALIDSIG $ARMBIAN_IMAGE_KEY " "$WORK/gpg.status" || die "the image is not signed by Armbian's key $ARMBIAN_IMAGE_KEY"
say "signature: good, Armbian image key $ARMBIAN_IMAGE_KEY"

say "decompressing"
xz -dc -T2 "$BASE_XZ" > "$IMG"
START="$(sfdisk -d "$IMG" | sed -n 's/.*start= *\([0-9]*\),.*/\1/p' | head -1)"
[ -n "$START" ] && [ "$START" -gt 16 ] || die "unexpected partition layout"
[ "$(sfdisk -d "$IMG" | grep -c 'start=')" = 1 ] || die "expected ONE partition (Armbian minimal)"
# ★ The boot loader lives OUTSIDE the partition: MBR boot code (bytes 0-445) and U-Boot's SPL at 8 KiB,
#   up to the partition start. Fingerprinted now, compared at the end — it must not change.
boot_region_sums() {
  { dd if="$1" bs=1 count=446 status=none | sha256sum | cut -d' ' -f1
    dd if="$1" bs=512 skip=1 count=$(( START - 1 )) status=none | sha256sum | cut -d' ' -f1; } | paste -sd' '
}
BOOT_BEFORE="$(boot_region_sums "$IMG")"
say "boot region (MBR code, sectors 1-$((START-1))): $BOOT_BEFORE"

# ── 2. Room to work ──────────────────────────────────────────────────────────────────────────────
truncate -s "${GROW_TO_MIB}M" "$IMG"
echo ', +' | sfdisk -q --no-reread -N 1 "$IMG"
LOOP="$(losetup -f --show -P "$IMG")"; udevadm settle 2>/dev/null || true
e2fsck -fy "${LOOP}p1" >/dev/null || [ $? -le 1 ] || die "e2fsck failed"
resize2fs "${LOOP}p1" >/dev/null
losetup -d "$LOOP"; LOOP=""

attach
# What we are building on — kept for the README's source references before the installer removes it.
cp "$ROOTFS/etc/armbian-release" "$OUT/armbian-release.txt" 2>/dev/null || true
cp "$ROOTFS/etc/armbian-image-release" "$OUT/armbian-image-release.txt" 2>/dev/null || true

# ── 3. Emulation ─────────────────────────────────────────────────────────────────────────────────
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
  [ -n "${QEMU_ARM_BINFMT:-}" ] && [ -x "$QEMU_ARM_BINFMT" ] || die "qemu-arm is not registered with binfmt_misc (install qemu-user-static, or set QEMU_ARM_BINFMT=)"
  [ -w /proc/sys/fs/binfmt_misc/register ] || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
  # Exactly the entry Ubuntu's qemu-user-static installs (/usr/lib/binfmt.d/qemu-arm.conf): the magic
  # and mask for 32-bit little-endian ARM ELF, flags OPF.
  #  ★★ O IS NOT OPTIONAL. F alone (the kernel opens the interpreter now, so it works inside the
  #     container) booted the container's systemd and then EVERY service failed in 2 ms with status 1:
  #     systemd 257 starts services through systemd-executor by file descriptor, and without O the
  #     interpreter is handed a /dev/fd path that is already closed. Found on the first build, 2026-10-24.
  printf '%s' ':qemu-arm:M::\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:'"$QEMU_ARM_BINFMT"':OPF' \
    > /proc/sys/fs/binfmt_misc/register
  BINFMT_OURS=1
  say "registered qemu-arm with binfmt_misc for this run"
fi
FLAGS="$(sed -n 's/^flags: *//p' /proc/sys/fs/binfmt_misc/qemu-arm)"
case "$FLAGS" in *O*) ;; *) die "qemu-arm binfmt entry has flags '$FLAGS': it needs O and F (as qemu-user-static sets them)" ;; esac
case "$FLAGS" in *F*) ;; *) die "qemu-arm binfmt entry has flags '$FLAGS': it needs O and F (as qemu-user-static sets them)" ;; esac
chroot "$ROOTFS" /bin/true || die "cannot execute ARM binaries"
chroot "$ROOTFS" /usr/bin/dpkg-query -W -f '${Package} ${Version}\n' 'linux-image-*' 'linux-dtb-*' 'linux-u-boot-*' armbian-firmware \
  > "$OUT/base-boot-packages.txt" 2>/dev/null || true

# ── 4. dietpi-installer in the container (as DietPi's dietpi-build does) ────────────────────────
# Workarounds copied in spirit from dietpi-build, each undone afterwards:
#  - services with ImportCredential/LoadCredential fail under QEMU (qemu#1962, systemd#31219);
#  - console-getty would steal the controlling terminal;
#  - ARMv7 Trixie: chpasswd reads /proc/sys/vm/mmap_min_addr, which a host's AppArmor may refuse
#    (opt-in host change below).
for s in "$ROOTFS"/lib/systemd/system/*.service; do
  [ -f "$s" ] || continue
  grep -Eq '^(Import|Load)Credential=' "$s" || continue
  d="$ROOTFS/etc/systemd/system/$(basename "$s").d"; mkdir -p "$d"
  printf '[Service]\nImportCredential=\nLoadCredential=\n' > "$d/dietpi-no-credentials.conf"
done
ln -sf /dev/null "$ROOTFS/etc/systemd/system/console-getty.service"
# ★★ chpasswd UNDER EMULATION, WITHOUT TOUCHING THE HOST. dietpi-installer resets root's password to
#    DietPi's documented default with `chpasswd <<< 'root:dietpi'`. Under QEMU, chpasswd's PAM helper
#    (unix_chkpwd) is caught by the HOST's AppArmor profile for that path and fails ("Authentication
#    token manipulation error"). DietPi's builder relaxes the host profile; we do not change the host.
#    The installer puts /usr/local/sbin first in PATH, so a shim there sets the same hash with
#    usermod -p (no PAM helper). It is removed straight after the container stops. On the board, DietPi
#    uses the real chpasswd.
cat > "$ROOTFS/usr/local/sbin/chpasswd" <<'SHIM'
#!/usr/bin/perl
# Build-time stand-in for chpasswd (see build.sh). Reads user:password lines; sets a SHA-512 crypt hash.
use strict; use warnings;
my @c = ('.', '/', 0..9, 'A'..'Z', 'a'..'z');
open(my $r, '<', '/dev/urandom') or die "urandom: $!";
while (my $line = <STDIN>) {
  chomp $line; next if $line eq '';
  my ($u, $p) = split(/:/, $line, 2);
  die "chpasswd shim: bad line\n" unless defined $p && $u =~ /^[a-z_][a-z0-9_-]*$/;
  read($r, my $b, 16) == 16 or die "urandom short read";
  my $salt = join('', map { $c[ord($_) % 64] } split(//, $b));
  my $h = crypt($p, '$6$' . $salt . '$');
  die "chpasswd shim: crypt failed\n" unless defined $h && $h =~ /^\$6\$/;
  system('/usr/sbin/usermod', '-p', $h, $u) == 0 or die "chpasswd shim: usermod failed for $u\n";
}
SHIM
chmod 0755 "$ROOTFS/usr/local/sbin/chpasswd"
# ★ The host is never changed for this, by default. Only with HOST_APPARMOR_WORKAROUND=1, on a
#   dedicated build machine, does it add the one rule DietPi's builder adds (and removes it at exit).
if [ "${HOST_APPARMOR_WORKAROUND:-0}" = 1 ] && systemctl -q is-active apparmor 2>/dev/null \
   && [ -f /etc/apparmor.d/unix-chkpwd ] && [ ! -e /etc/apparmor.d/local/unix-chkpwd ]; then
  APPARMOR_LOCAL=/etc/apparmor.d/local/unix-chkpwd
  echo '/proc/sys/vm/mmap_min_addr r,' > "$APPARMOR_LOCAL"
  apparmor_parser -r /etc/apparmor.d/unix-chkpwd
  say "host AppArmor: unix-chkpwd may read mmap_min_addr for this run (removed at exit)"
fi

# ★ Read as data, never sourced: it comes from the downloaded image. Held to a safe character set,
#   because it is written into a script below.
rel() { sed -n "s/^$1=//p" "$OUT/armbian-release.txt" 2>/dev/null | head -1 | tr -d "\"'" | tr -cd 'A-Za-z0-9 ._+-'; }
PREIMAGE="Armbian $(rel VERSION) $(rel BOARD_NAME), $(basename "$BASE_XZ" .img.xz | tr -cd 'A-Za-z0-9._+-')"
cat > "$ROOTFS/dietpi-build.sh" <<EOF
#!/bin/dash -e
{
# ★ dumb, always: the container console is not a terminal the installer can size, and with any other
#   TERM dietpi-globals asks tput for the width, gets nothing and dies in its arithmetic (2026-10-24).
export TERM=dumb
export SYSTEMD_PAGER='' PAGER=cat
export GITOWNER='MichaIng' GITBRANCH='master' HW_MODEL='25' IMAGE_CREATOR='VibeSDR project' PREIMAGE_INFO='$PREIMAGE' WIFI_REQUIRED=1 DISTRO_TARGET=8 TMPDIR='/tmp'
echo '[ INFO ] Running DietPi-Installer (MichaIng/master), hardware model 25: Generic Allwinner H3'
bash -c "\$(curl -sSf 'https://raw.githubusercontent.com/MichaIng/DietPi/master/.build/images/dietpi-installer' || echo false)" || { echo '[FAILED] DietPi-Installer failed'; journalctl --no-pager -n 40; df -h; exit 1; }
rm -v /etc/systemd/system/multi-user.target.wants/dietpi-build.service /etc/systemd/system/dietpi-build.service /dietpi-build.sh /etc/systemd/system/console-getty.service
> /success
exit 0
}
EOF
chmod +x "$ROOTFS/dietpi-build.sh"
cat > "$ROOTFS/etc/systemd/system/dietpi-build.service" <<'EOF'
[Unit]
After=multi-user.target

[Service]
Type=idle
StandardOutput=tty
ExecStart=-/dietpi-build.sh
ExecStopPost=/usr/bin/systemctl --no-block start poweroff.target
EOF
ln -sf ../dietpi-build.service "$ROOTFS/etc/systemd/system/multi-user.target.wants/dietpi-build.service"

say "booting the Armbian image in a container and running dietpi-installer (emulated: slow)"
# ★ Only THIS image's loop device and its partition are visible in the container (DietPi's builder
#   also binds the host's /dev/disk, for GRUB on PC images; an Armbian board image needs neither).
abind=("--bind=$LOOP")
for p in "${LOOP}p"*; do abind+=("--bind=$p"); done
# ★ DNS: Armbian's /etc/resolv.conf points at systemd-resolved's stub, which does not run in the
#   container (the first build failed "Could not resolve host"). replace-uplink puts the host's real
#   upstream servers there for the run; DietPi's installer writes its own resolv.conf anyway.
"$NSPAWN" -q --register=no --resolv-conf=replace-uplink -bD "$ROOTFS" "${abind[@]}" || die "systemd-nspawn failed"
[ -f "$ROOTFS/success" ] || die "dietpi-installer did not finish (no /success)"
rm -f "$ROOTFS/success"
for c in "$ROOTFS"/etc/systemd/system/*.service.d/dietpi-no-credentials.conf; do
  [ -f "$c" ] || continue; rm "$c"; rmdir --ignore-fail-on-non-empty "$(dirname "$c")"
done
rm -f "$ROOTFS/etc/systemd/system/console-getty.service"
rm -f "$ROOTFS/usr/local/sbin/chpasswd"
rm -Rf "${ROOTFS:?}"/{dev,proc,run,sys,tmp,var/log}/{,.??,.[^.]}* 2>/dev/null || true
[ -f "$ROOTFS/boot/dietpi/.version" ] || die "no /boot/dietpi/.version — not a DietPi system"
cp "$ROOTFS/boot/dietpi/.version" "$OUT/dietpi-version.txt"
chroot "$ROOTFS" /usr/bin/dpkg-query -W -f '${Package} ${Version}\n' 'linux-image-*' 'linux-dtb-*' 'linux-u-boot-*' armbian-firmware \
  > "$OUT/boot-packages.txt" 2>/dev/null || true

# ── 5. Our part: dietpi.txt values and the first-boot script ────────────────────────────────────
DTXT="$ROOTFS/boot/dietpi.txt"
[ -f "$DTXT" ] || die "no /boot/dietpi.txt"
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue ;; esac
  key="${line%%=*}"; val="${line#*=}"
  n="$(grep -c "^${key}=" "$DTXT" || true)"
  [ "$n" = 1 ] || die "dietpi.txt has $n lines for $key (expected exactly 1) — DietPi changed its keys?"
  sed -i "s|^${key}=.*|${key}=${val}|" "$DTXT"
done < "$HERE/dietpi.txt.overrides"
install -m 0755 "$HERE/Automation_Custom_Script.sh" "$ROOTFS/boot/Automation_Custom_Script.sh"
cp "$DTXT" "$OUT/dietpi.txt.as-built"
# Read back every value.
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue ;; esac
  grep -qxF "$line" "$DTXT" || die "dietpi.txt read-back: '$line' not found"
done < "$HERE/dietpi.txt.overrides"
say "dietpi.txt: every override read back"
grep -q '^AUTO_SETUP_GLOBAL_PASSWORD=dietpi$' "$DTXT" || die "AUTO_SETUP_GLOBAL_PASSWORD is not DietPi's default — this image must not carry a password of ours"

detach

# ── 6. Shrink to the filesystem (DietPi grows it to the card at first boot) ─────────────────────
LOOP="$(losetup -f --show -P "$IMG")"; udevadm settle 2>/dev/null || true
e2fsck -fy "${LOOP}p1" >/dev/null || [ $? -le 1 ] || die "e2fsck failed"
resize2fs -M "${LOOP}p1" >/dev/null 2>&1
BC="$(dumpe2fs -h "${LOOP}p1" 2>/dev/null | sed -n 's/^Block count: *//p')"
BS="$(dumpe2fs -h "${LOOP}p1" 2>/dev/null | sed -n 's/^Block size: *//p')"
losetup -d "$LOOP"; LOOP=""
SECTORS=$(( (BC * BS + 511) / 512 + SLACK_MIB * 2048 ))
SECTORS=$(( (SECTORS + 2047) / 2048 * 2048 ))
echo ",$SECTORS" | sfdisk -q --no-reread -N 1 "$IMG"
truncate -s $(( (START + SECTORS) * 512 )) "$IMG"
LOOP="$(losetup -f --show -P "$IMG")"; udevadm settle 2>/dev/null || true
e2fsck -fn "${LOOP}p1" >/dev/null || die "filesystem check after shrink failed"
losetup -d "$LOOP"; LOOP=""

BOOT_AFTER="$(boot_region_sums "$IMG")"
[ "$BOOT_BEFORE" = "$BOOT_AFTER" ] || die "the boot region changed: $BOOT_BEFORE -> $BOOT_AFTER"
say "boot region unchanged: $BOOT_AFTER"
sfdisk -d "$IMG" > "$OUT/partition-table.txt"

# ── 7. Optional: prove the first-boot script on a throwaway copy ────────────────────────────────
if [ "${TEST_INSTALL:-0}" = 1 ]; then
  say "TEST_INSTALL: running Automation_Custom_Script.sh in a container on a copy"
  cp --sparse=always "$IMG" "$WORK/test.img"
  ORIG_IMG="$IMG"; IMG="$WORK/test.img"
  # The shipped image has only $SLACK_MIB MiB free (DietPi grows it on the card); give the copy room.
  truncate -s +1G "$IMG"
  echo ', +' | sfdisk -q --no-reread -N 1 "$IMG"
  LOOP="$(losetup -f --show -P "$IMG")"; udevadm settle 2>/dev/null || true
  resize2fs "${LOOP}p1" >/dev/null 2>&1
  losetup -d "$LOOP"; LOOP=""
  attach
  "$NSPAWN" -q --register=no --resolv-conf=replace-uplink -D "$ROOTFS" --pipe /bin/bash /boot/Automation_Custom_Script.sh 2>&1 | tee "$OUT/test-install.log"
  chroot "$ROOTFS" /usr/bin/vibeserver --version 2>&1 | tee -a "$OUT/test-install.log" || true
  chroot "$ROOTFS" /usr/bin/dpkg-query -W -f '${Package} ${Version} ${Architecture}\n' vibeserver | tee -a "$OUT/test-install.log"
  detach
  IMG="$ORIG_IMG"; rm -f "$WORK/test.img"
fi

# ── 8. Compress ──────────────────────────────────────────────────────────────────────────────────
say "compressing"
mv "$IMG" "$OUT/$OUT_NAME.img"
sha256sum "$OUT/$OUT_NAME.img" | sed "s|  .*/|  |" > "$OUT/$OUT_NAME.img.sha256"
xz -T3 -9 -f "$OUT/$OUT_NAME.img"
( cd "$OUT" && sha256sum "$OUT_NAME.img.xz" > "$OUT_NAME.img.xz.sha256" )
say "built $OUT/$OUT_NAME.img.xz"
cat "$OUT/$OUT_NAME.img.xz.sha256"
