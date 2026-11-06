#!/bin/bash
# verify.sh IMAGE [ROOT-TREE BOOT-TREE] — check a finished R36S card image offline; exits non-zero on the first failure.
# ★ Called by build-image.sh before compressing, and runnable on its own on an .img (gunzip it first):
#     image/r36s/build.sh --verify <image.img.gz>
# ★ Reads the partitions straight out of the file (debugfs, mtools) — no loop device, nothing mounted.
set -euo pipefail
IMG=$1
ok()   { printf '  ok  %s\n' "$*"; }
fail() { printf '  !!  %s\n' "$*" >&2; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
echo "==> verifying $(basename "$IMG")"

# ── partition table ──
sfdisk -d "$IMG" > "$T/pt"
N=$(grep -c ' : start=' "$T/pt")
[ "$N" = 3 ] || fail "expected 3 partitions (boot, root, state), found $N"
read -r S1 Z1 <<< "$(sed -n 's/.*1 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p' "$T/pt")"
read -r S2 Z2 <<< "$(sed -n 's/.*2 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p' "$T/pt")"
read -r S3 Z3 <<< "$(sed -n 's/.*3 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p' "$T/pt")"
[ "$S1" = 32768 ] && grep -q '1 : .*type=c, bootable' "$T/pt" || fail "p1 is not a bootable FAT32 at 16 MiB"
[ $((S1 + Z1)) -le "$S2" ] && [ $((S2 + Z2)) -le "$S3" ] || fail "partitions overlap"
[ $(( (S3 + Z3) * 512 )) -le "$(stat -c %s "$IMG")" ] || fail "p3 runs past the end of the file"
DISKID=$(sed -n 's/^label-id: 0x//p' "$T/pt")
ok "MBR: id $DISKID, p1 boot $((Z1/2048)) MiB @16 MiB, p2 root $((Z2/2048)) MiB, p3 state $((Z3/2048)) MiB; VIBESDR comes on first boot"

# ── the loader ──
dd if="$IMG" bs=32K skip=1 count=511 status=none | sha256sum | grep -q '^689724d71558cd9a3d2490c89344b9fbdcf16f2f92cfeff6e39f8bdfe981fab4' \
  || fail "loader at 32 KiB is not ROCKNIX's b_uboot.bin.uart5"
ok "loader: b_uboot.bin.uart5 at 32 KiB (the bytes ROCKNIX wrote on Stuart's unit)"

# ── boot partition ──
dd if="$IMG" of="$T/boot.vfat" bs=512 skip="$S1" count="$Z1" status=none
mdir -i "$T/boot.vfat" -/ -b :: > "$T/boot.ls"
for f in ::/KERNEL ::/boot.scr ::/rk3326-gameconsole-eeclone.dtb ::/extlinux/extlinux.conf ::/overlays/mipi-panel.dtbo \
         ::/overlays/vibe-r36s.dtbo ::/stock/rf3536k3ka.dtb ::/SOURCES.txt; do
  grep -qx "$f" "$T/boot.ls" || fail "boot partition lacks ${f#::}"
done
mtype -i "$T/boot.vfat" ::/extlinux/extlinux.conf > "$T/ext"
grep -q "root=PARTUUID=$DISKID-02 " "$T/ext" || fail "extlinux root= does not name this card's p2"
grep -q "rdinit=/vibesdr-no-initramfs" "$T/ext" && grep -q " ro " "$T/ext" && grep -q "init=/usr/lib/vibesdr-r36s/overlay-init" "$T/ext" \
  || fail "extlinux APPEND is missing rdinit= / ro / init="
grep -q "console=tty0" "$T/ext" && fail "console=tty0 would print over the splash"
grep -q "^  INITRD /vibe-initrd$" "$T/ext" || fail "extlinux has no INITRD /vibe-initrd (the kernel cannot mount root without /root)"
mcopy -n -i "$T/boot.vfat" ::/vibe-initrd ::/boot.scr "$T/"
[ "$(cpio -it < "$T/vibe-initrd" 2>/dev/null | sort | tr '\n' ' ')" = ". dev dev/console root " ] || fail "vibe-initrd is not exactly /dev, /dev/console, /root"
grep -aq '^setenv ramdisk_addr_r "0x0c000000"$' "$T/boot.scr" || fail "boot.scr does not put the initrd above the KERNEL"
grep -q ' $' "$T/ext" && fail "extlinux.conf has a trailing space"
mcopy -n -i "$T/boot.vfat" ::/overlays/mipi-panel.dtbo ::/overlays/vibe-r36s.dtbo ::/rk3326-gameconsole-eeclone.dtb "$T/"
fdtoverlay -i "$T/rk3326-gameconsole-eeclone.dtb" -o "$T/m.dtb" "$T/mipi-panel.dtbo" "$T/vibe-r36s.dtbo" || fail "overlays do not apply"
dtc -q -I dtb -O dts "$T/m.dtb" > "$T/m.dts"
cmp -s "$T/mipi-panel.dtbo" /repo/image/r36s/board/mipi-panel.rocknix.dtbo || fail "the panel overlay is not ROCKNIX's, unchanged"
grep -q 'M clock=30000 horizontal=640,46,2,44 vertical=480,16,2,14 default=1' "$T/m.dts" || fail "the panel's default is not ROCKNIX's 80 Hz mode"
[ "$(grep -o 'default=1' "$T/m.dts" | wc -l)" = 1 ] || fail "more than one default panel mode"
for f in 408000000 600000000 816000000 1008000000 1104000000 1200000000 1248000000 1296000000; do
  grep -q "opp-$f {" "$T/m.dts" || fail "CPU step $f missing"
done
awk '/opp-1416000000 \{/{f=1} f&&/status = "disabled"/{print; exit}' "$T/m.dts" | grep -q disabled || fail "1416 MHz is not disabled"
grep -q 'default-brightness-level = <0x78>' "$T/m.dts" || fail "backlight boot level is not 120"
ok "boot: KERNEL, eeclone DTB, overlays apply (panel = ROCKNIX's 80 Hz overlay, unchanged, CPU 408–1296 MHz, 1416 off, backlight 120), extlinux → PARTUUID=$DISKID-02"

# ── root filesystem ──
dd if="$IMG" of="$T/root.ext4" bs=512 skip="$S2" count="$Z2" status=none
dfs() { debugfs -R "$1" "$T/root.ext4" 2>/dev/null; }
exists() { dfs "stat $1" | grep -q 'Inode:'; }
link()   { dfs "stat $1" | sed -n 's/^Fast link dest: "\(.*\)"$/\1/p'; }
e2fsck -fn "$T/root.ext4" >/dev/null 2>&1 || fail "root filesystem has errors"
[ "$(dumpe2fs -h "$T/root.ext4" 2>/dev/null | sed -n 's/^Filesystem volume name: *//p')" = vibe-root ] || fail "root label"
for f in /usr/lib/vibesdr-r36s/vibe-hwd /usr/lib/vibesdr-r36s/vibe-splash /usr/lib/vibesdr-r36s/vibe-card \
         /usr/lib/vibesdr-r36s/vibe-bootkeys /usr/lib/vibesdr-r36s/vibe-clock /usr/lib/vibesdr-r36s/overlay-init \
         /usr/share/vibesdr-r36s/splash.bgrx.z /usr/share/vibesdr-r36s/splash-font.bin /usr/bin/cog /usr/bin/vibeserver \
         /usr/lib/modules/7.1.2/kernel/drivers/gpu/drm/panfrost/panfrost.ko /usr/lib/modules/7.1.2/modules.dep.bin \
         /etc/udev/rules.d/70-vibe-r36s-gamepad.rules /etc/modprobe.d/vibe-r36s-gpu.conf /usr/share/doc/vibesdr-r36s/SOURCES \
         /media/root-ro /media/root-rw /media/VIBESDR /var/lib/vibe-state /usr/lib/clock-epoch; do
  exists "$f" || fail "root lacks $f"
done
exists /usr/lib/vibeserver/cloudflared && fail "cloudflared is on the card"
WANTS=/etc/systemd/system/multi-user.target.wants
for u in vibe-hwd vibe-bootkeys vibe-card vibeserver-handheld vibe-kiosk; do
  exists "$WANTS/$u.service" || fail "$u.service is not enabled"
done
exists /etc/systemd/system/sysinit.target.wants/vibe-splash.service || fail "vibe-splash is not enabled"
for u in vibe-clock vibe-state; do exists /etc/systemd/system/sysinit.target.wants/$u.service || fail "$u is not enabled"; done
for u in vibeserver.service getty@tty1.service apt-daily.timer apt-daily-upgrade.timer systemd-networkd.service \
         systemd-networkd.socket systemd-networkd-wait-online.service; do
  [ "$(link "/etc/systemd/system/$u")" = /dev/null ] || fail "$u is not masked"
done
# ★★★ NO SSH (Stuart 2026-11-05): no server package, no daemon binary, no unit, no config, no keys
for p in openssh-server openssh-sftp-server dropbear tinysshd telnetd; do
  dfs "cat /var/lib/dpkg/status" | grep -qx "Package: $p" && fail "$p is installed"
done
for f in /usr/sbin/sshd /usr/lib/systemd/system/ssh.service /usr/lib/systemd/system/ssh.socket /etc/ssh/sshd_config; do
  exists "$f" && fail "$f is on the card"
done
dfs "cat /etc/shadow" | grep -E '^root:' | cut -d: -f2 | grep -qx '\*' || fail "root has a password in the image"
dfs "cat /etc/shadow" | awk -F: '$2 !~ /^[*!]/ {print $1}' | grep -q . && fail "an account has a password in the image"
dfs "cat /etc/udev/rules.d/70-vibe-r36s-gamepad.rules" | grep -q 'ENV{ID_INPUT_TABLET_PAD}=""' || fail "gamepad udev rule"
dfs "cat /etc/modprobe.d/vibe-r36s-gpu.conf" | grep -q '^blacklist mali_kbase' || fail "mali_kbase not blacklisted"
dfs "cat /usr/lib/systemd/system/vibe-kiosk.service" | grep -q -- '--gamepad=manette' || fail "Cog without the gamepad (the A press starts the audio)"
dfs "cat /usr/lib/systemd/system/vibe-kiosk.service" | grep -q '^Environment=VIBE_ENGINE_URL=http://127.0.0.1:48000/?handheld=1$' \
  || fail "Cog's URL lacks ?handheld=1 (the page boots as a web client: auth probe 503)"
dfs "cat /usr/lib/systemd/system/vibe-cpu.service" | grep -q schedutil || fail "governor"
dfs "cat /etc/systemd/logind.conf.d/vibe-r36s.conf" | grep -q '^HandlePowerKey=ignore' || fail "logind would take the power key"
[ -z "$(dfs 'cat /etc/machine-id')" ] || fail "machine-id is baked in"
dfs "ls /etc/apt/sources.list.d" | grep -q '\.list' && fail "apt sources left on the card"
# ★★★ THE HIGH-DETAIL MAPS ARE ON THE CARD (no internet on the handheld): the package, and files where the engine looks
dfs "cat /var/lib/dpkg/status" | grep -qx "Package: vibeserver-mapdata-detail" || fail "the map detail pack (vibeserver-mapdata-detail) is not installed"
MAPN=$(dfs "ls -p /usr/lib/vibeserver/mapdata" | tr ' ' '\n' | grep -c '\.json\|\.png\|\.gz' || true)
[ "$MAPN" -ge 100 ] || fail "only $MAPN map files in /usr/lib/vibeserver/mapdata — the detail layers are missing"
exists /usr/lib/vibeserver/mapdata/index.json || fail "no map index.json (the basic pack's manifest)"
ok "maps: vibeserver-mapdata-detail installed, $MAPN files in /usr/lib/vibeserver/mapdata (where vibe_mapdata.h looks first)"
for p in cloudflared unattended-upgrades avahi-daemon; do
  dfs "cat /var/lib/dpkg/status" | grep -qx "Package: $p" && fail "$p is installed"
done
ok "root: services enabled, server units + getty + apt timers masked, no SSH at all, no network services, no passwords, udev rule, Panfrost, schedutil, Cog with the gamepad"

# ── state partition ──
dd if="$IMG" of="$T/state.ext4" bs=512 skip="$S3" count="$Z3" status=none
e2fsck -fn "$T/state.ext4" >/dev/null 2>&1 || fail "state filesystem has errors"
[ "$(dumpe2fs -h "$T/state.ext4" 2>/dev/null | sed -n 's/^Filesystem volume name: *//p')" = vibestate ] || fail "state label"
for d in vibeserver kiosk; do debugfs -R "stat /$d" "$T/state.ext4" 2>/dev/null | grep -q 'Type: directory' || fail "state lacks /$d"; done
ok "state: ext4 'vibestate' with vibeserver/, kiosk/ (vibe-state's bind mounts work from the first boot)"

# ── units parse (systemd-analyze verify on the real tree) ──
if [ -n "${2:-}" ] && [ -d "$2" ] && command -v systemd-analyze >/dev/null; then
  U="vibe-splash.service vibe-clock.service vibe-cpu.service vibe-state.service vibe-hwd.service vibe-bootkeys.service vibe-card.service vibeserver-handheld.service vibe-kiosk.service"
  OUT=$(cd "$2/usr/lib/systemd/system" && SYSTEMD_LOG_LEVEL=notice systemd-analyze verify --root="$2" $U 2>&1 | grep -v -E 'Cannot add dependency job|is not executable|Failed to (connect|put)|bpf|kmsg|No such file or directory.*(dbus|proc)' || true)
  if [ -n "$OUT" ]; then echo "$OUT" | sed 's/^/      /'; fail "systemd-analyze verify reported problems"; fi
  ok "systemd-analyze verify: the image's units parse"

  # ★★★ WHAT MAY LISTEN (Stuart 2026-11-05: nothing but the engine on 127.0.0.1). Offline, from the tree itself: every
  #     ENABLED socket unit's Listen…= lines (only AF_UNIX paths / netlink / abstract allowed), and every enabled
  #     service's executable — a network daemon among them fails the build. The engine's own bind address is its
  #     handheld profile's (§0.1: 127.0.0.1 only); the live proof is `ss -tulnp` on the booted root (README "Tests").
  echo "      listening sockets the image would open at boot (enabled .socket units):"
  BAD=""
  # ★ enabled = linked from a .wants/ (admin's /etc and the packages' static /usr/lib ones), and NOT masked in /etc
  masked() { [ "$(readlink "$2/etc/systemd/system/$1" 2>/dev/null)" = /dev/null ]; }
  for s in $(find "$2/etc/systemd/system" "$2/usr/lib/systemd/system" -path '*.wants/*.socket' 2>/dev/null | sort -u); do
    u=$(basename "$s"); masked "$u" "$2" && continue
    # ★ the links are absolute INSIDE the image (or relative): resolve them under the tree, never on the build host
    f="$2/usr/lib/systemd/system/$u"; [ -f "$f" ] || f="$2/etc/systemd/system/$u"
    grep -E '^Listen[A-Za-z]*=' "$f" 2>/dev/null | while read -r l; do
      v=${l#*=}
      case "$l" in ListenNetlink=*|ListenSpecial=*|ListenFIFO=*) k=ok ;; *) case "$v" in /*|@*) k=ok ;; *) k=NETWORK ;; esac ;; esac
      printf '        %-34s %-48s %s\n' "$u" "$l" "$k"
    done
    grep -E '^Listen(Stream|Datagram|SequentialPacket)=' "$f" 2>/dev/null | cut -d= -f2 | grep -vqE '^(/|@)' && BAD="$BAD $u"
  done
  [ -z "$BAD" ] || fail "socket unit(s) listen on the network:$BAD"
  SV=$(for x in $(find "$2/etc/systemd/system" "$2/usr/lib/systemd/system" -path '*.wants/*.service' -exec basename {} \; 2>/dev/null | sort -u); do
         masked "$x" "$2" || printf '%s ' "$x"; done)
  for d in sshd dropbear telnetd avahi-daemon cloudflared systemd-networkd systemd-resolved systemd-timesyncd \
           rpcbind cupsd exim4 nginx lighttpd apache2 smbd; do
    echo " $SV " | grep -q " $d.service " && fail "$d.service is enabled"
  done
  echo "      enabled services: $SV"
  ok "listening: no enabled socket unit and no enabled service opens a network port — the engine (127.0.0.1) is the only listener"
fi
echo "==> verify: all checks passed"
