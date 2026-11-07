#!/bin/bash
# build-image.sh — INSIDE the build container (build.sh starts it): make the VibeSDR R36S handheld card image.
#
# Mounts: /repo (this repository, read-only) · /cache (downloads, kept between builds) · /out (the finished image)
#         /work (a Docker volume: the root tree is built on the VM's own ext4, never on the Mac's shared folder —
#         ownership, device nodes and xattrs survive there) · /deb (optional: a local vibeserver .deb)
# Env:    VIBESERVER_DEB=/deb/<file>.deb   (else the newest arm64 vibeserver from apt.vibesdr.net suite $APT_SUITE)
#         MAPDATA_DEB=/mapdata/vibeserver-mapdata-detail_<v>_all.deb (the high-detail map pack — REQUIRED: no internet)
#         APT_SUITE=beta · SNAPSHOT=<YYYYMMDDTHHMMSSZ> (snapshot.debian.org, for a reproducible package set)
#         IMAGE_VERSION=<string> (default: <vibeserver version>-<UTC date>) · JOBS=6
#
# ★★★ THE BOOT CHAIN IS ROCKNIX's, THE OS IS OURS. From the ROCKNIX 20261101 "b" image (EE clones) we take the loader
#     (b_uboot.bin.uart5 — byte-identical to what ROCKNIX's own updater wrote on Stuart's unit), its boot.scr, the
#     KERNEL (mainline 7.1.2 + RK3326 patches, panel driver built in), the eeclone DTB, the kernel modules and the
#     firmware. Then the kernel boots OUR Debian root directly: see README.md "Boot chain" for why rdinit= and not
#     ROCKNIX's initramfs + SYSTEM squashfs.
# ★★ NO LOOP DEVICES. Every filesystem is made in a plain file (mkfs.ext4 -d, mkfs.vfat + mtools) and copied into the
#    image at its offset, so nothing of the host's /dev is ever bound into the build (the Lenovo /dev deletion of
#    2026-10-25, image/sbc/build.sh). The chroot gets a private tmpfs /dev with six device nodes.
set -euo pipefail
JOBS=${JOBS:-6}
APT_SUITE=${APT_SUITE:-beta}
ROCKNIX_TAG=20261101
ROCKNIX_COMMIT=cefc14339a4ae18c02bb69888f27835e50664b60
ROCKNIX_IMG=ROCKNIX-RK3326.aarch64-${ROCKNIX_TAG}-b.img.gz
ROCKNIX_URL=https://github.com/ROCKNIX/distribution/releases/download/${ROCKNIX_TAG}/${ROCKNIX_IMG}
ROCKNIX_SHA256=b0f2a35db7750a03773c128a1c178b88fc050bf85d2a2416f5be5fc2bb1a66a8
# ★ the loader ROCKNIX's update.sh chose for this board (UART5 console → b_uboot.bin.uart5), as read back off the card
LOADER_SHA256=689724d71558cd9a3d2490c89344b9fbdcf16f2f92cfeff6e39f8bdfe981fab4
KVER=7.1.2
BOOT_MIB=128      # FAT32 boot partition: KERNEL (24 MB) + DTB + overlays + room for a newer kernel
STATE_MIB=256     # ext4 state partition (never grown)
ROOT_HEAD_MIB=320 # free room left in the read-only root (developer hot-fixes, a newer engine)
H=/repo/image/r36s
W=/work
say() { printf '==> %s\n' "$*"; }
die() { printf '!! build-image.sh: %s\n' "$*" >&2; exit 1; }

mkdir -p /cache /out "$W"
R=$W/root
# ★ a SUBSHELL body: `set +e` in here must not switch error checking off for the rest of the build (it did, once)
cleanup() (
  set +e
  for m in "$R/dev/pts" "$R/dev" "$R/proc" "$R/sys" "$R/run"; do mountpoint -q "$m" && umount -l "$m"; done
  exit 0
)
trap cleanup EXIT
cleanup
# ★ nothing may be mounted under $W before it is emptied (image/sbc/build.sh: the /dev deletion)
if grep -q " $W/" /proc/mounts; then die "something is still mounted under $W"; fi
rm -rf --one-file-system "$W"/root "$W"/state "$W"/boot "$W"/rocknix "$W"/parts
mkdir -p "$W/rocknix" "$W/boot" "$W/parts"

# ── 1. ROCKNIX: verified, then the boot chain lifted out ─────────────────────────────────────────────────────────────
if [ ! -s "/cache/$ROCKNIX_IMG" ]; then
  say "downloading $ROCKNIX_IMG"
  curl -fL --retry 3 -o "/cache/$ROCKNIX_IMG.part" "$ROCKNIX_URL" && mv "/cache/$ROCKNIX_IMG.part" "/cache/$ROCKNIX_IMG"
fi
echo "$ROCKNIX_SHA256  /cache/$ROCKNIX_IMG" | sha256sum -c --quiet || die "$ROCKNIX_IMG checksum mismatch"
say "ROCKNIX $ROCKNIX_TAG image verified (sha256 $ROCKNIX_SHA256)"
RX=$W/rocknix
pigz -dc -p "$JOBS" "/cache/$ROCKNIX_IMG" > "$RX/rocknix.img"
read -r P1S P1Z <<< "$(sfdisk -d "$RX/rocknix.img" | sed -n 's/.*img1 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p')"
dd if="$RX/rocknix.img" of="$RX/p1.img" bs=512 skip="$P1S" count="$P1Z" status=none
rm -f "$RX/rocknix.img"
mcopy -n -i "$RX/p1.img" ::/KERNEL ::/SYSTEM ::/rk3326-gameconsole-eeclone.dtb "$RX/"
rm -f "$RX/p1.img"
unsquashfs -q -n -d "$RX/sys" "$RX/SYSTEM" usr/share/bootloader usr/lib/kernel-overlays/base/lib etc/os-release >/dev/null
rm -f "$RX/SYSTEM"
BL=$RX/sys/usr/share/bootloader
echo "$LOADER_SHA256  $BL/b_uboot.bin.uart5" | sha256sum -c --quiet || die "b_uboot.bin.uart5 is not the loader proven on the unit"
[ "$(stat -c %s "$BL/b_uboot.bin.uart5")" = 16744448 ] || die "unexpected loader size"
file -b "$RX/KERNEL" | grep -q "ARM64 boot executable Image" || die "KERNEL is not an arm64 Image"
cmp -s "$RX/rk3326-gameconsole-eeclone.dtb" "$BL/device_trees/rk3326-gameconsole-eeclone.dtb" || die "DTB mismatch between BOOT and SYSTEM"
KO=$RX/sys/usr/lib/kernel-overlays/base/lib
[ -d "$KO/modules/$KVER" ] || die "no modules for $KVER in SYSTEM"
say "boot chain: KERNEL $(stat -c %s "$RX/KERNEL") bytes, loader b_uboot.bin.uart5, $(find "$KO/modules/$KVER" -name '*.ko*' | wc -l) modules"

# ── 2. The Debian root ───────────────────────────────────────────────────────────────────────────────────────────────
MIRROR=http://deb.debian.org/debian
[ -n "${SNAPSHOT:-}" ] && MIRROR=https://snapshot.debian.org/archive/debian/$SNAPSHOT
say "debootstrap trixie arm64 (minbase) from $MIRROR"
debootstrap --variant=minbase --arch=arm64 --components=main --include=ca-certificates trixie "$R" "$MIRROR" >/dev/null \
  || { tail -30 "$R/debootstrap/debootstrap.log" 2>/dev/null; die "debootstrap failed"; }
# ★ the chroot's own /dev: a tmpfs with the six nodes apt and dpkg need — never the host's /dev
mount -t tmpfs -o mode=0755,size=1M tmpfs "$R/dev"
mknod -m 666 "$R/dev/null" c 1 3; mknod -m 666 "$R/dev/zero" c 1 5; mknod -m 666 "$R/dev/full" c 1 7
mknod -m 666 "$R/dev/random" c 1 8; mknod -m 666 "$R/dev/urandom" c 1 9; mknod -m 666 "$R/dev/tty" c 5 0
mkdir -p "$R/dev/pts" "$R/dev/shm"
mount -t devpts -o newinstance,ptmxmode=0666 devpts "$R/dev/pts"; ln -sf pts/ptmx "$R/dev/ptmx"
mount -t proc proc "$R/proc"; mount -t sysfs -o ro sysfs "$R/sys"; mount -t tmpfs tmpfs "$R/run"
cp /etc/resolv.conf "$R/etc/resolv.conf"
in_root() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8 "$@"; }
# ★ dpkg's excludes (no cloudflared, no docs) BEFORE any package lands; policy-rc.d: nothing starts in the chroot
install -D -m 0644 "$H/rootfs/etc/dpkg/dpkg.cfg.d/vibe-r36s" "$R/etc/dpkg/dpkg.cfg.d/vibe-r36s"
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 0755 "$R/usr/sbin/policy-rc.d"
echo "deb $MIRROR trixie main" > "$R/etc/apt/sources.list"
if [ -z "${SNAPSHOT:-}" ]; then
  echo "deb http://deb.debian.org/debian-security trixie-security main" >> "$R/etc/apt/sources.list"
  echo "deb http://deb.debian.org/debian trixie-updates main" >> "$R/etc/apt/sources.list"
fi
in_root apt-get update -qq
PKGS=$(sed 's/#.*//' "$H/packages.txt" | xargs)
say "installing: $PKGS"
in_root apt-get install -y -qq --no-install-recommends $PKGS >/dev/null

# ── 3. The engine ────────────────────────────────────────────────────────────────────────────────────────────────────
mkdir -p "$R/tmp/vdeb"
if [ -n "${VIBESERVER_DEB:-}" ]; then
  [ -f "$VIBESERVER_DEB" ] || die "VIBESERVER_DEB=$VIBESERVER_DEB not found"
  cp "$VIBESERVER_DEB" "$R/tmp/vdeb/"
  say "engine: local package $(basename "$VIBESERVER_DEB")"
else
  # ★ as image/pocket's customise.sh: the repository's own key, then the newest arm64 build of the suite
  curl -fsSL https://apt.vibesdr.net/KEY.gpg | gpg --dearmor --yes -o "$R/usr/share/keyrings/vibesdr.gpg"
  echo "deb [arch=arm64 signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net $APT_SUITE main" \
    > "$R/etc/apt/sources.list.d/vibesdr.list"
  in_root apt-get update -qq
  in_root sh -c 'cd /tmp/vdeb && apt-get download -qq vibeserver' 2>&1 | grep -v "_apt" || true
  rm -f "$R/etc/apt/sources.list.d/vibesdr.list"
  say "engine: $(ls "$R/tmp/vdeb") from apt.vibesdr.net $APT_SUITE (signature checked by apt)"
fi
# ★ by CONTENT, not by name: a branch build may be called anything (vibeserver-handheld-arm64.deb, 2026-11-05)
DEB=$(ls "$R"/tmp/vdeb/*.deb | head -1)
[ "$(dpkg-deb -f "$DEB" Package)" = vibeserver ] && [ "$(dpkg-deb -f "$DEB" Architecture)" = arm64 ] \
  || die "$(basename "$DEB") is not an arm64 vibeserver package"
DEB_SHA=$(sha256sum "$DEB" | cut -d' ' -f1)
cp "$DEB" "/cache/"
in_root apt-get install -y -qq --no-install-recommends "/tmp/vdeb/$(basename "$DEB")" >/dev/null
VS_VER=$(in_root dpkg-query -W -f '${Version}' vibeserver)
[ ! -e "$R/usr/lib/vibeserver/cloudflared" ] || die "cloudflared landed on the card"
# ── 3b. The high-detail map pack (the handheld has no internet: everything past zoom 7 must be on the card) ──
[ -n "${MAPDATA_DEB:-}" ] && [ -f "$MAPDATA_DEB" ] || die "no map detail pack (MAPDATA_DEB) — build.sh finds or builds it"
cp "$MAPDATA_DEB" "$R/tmp/vdeb/"
MAP_SHA=$(sha256sum "$MAPDATA_DEB" | cut -d' ' -f1)
in_root apt-get install -y -qq --no-install-recommends "/tmp/vdeb/$(basename "$MAPDATA_DEB")" >/dev/null
MAP_VER=$(in_root dpkg-query -W -f '${Version}' vibeserver-mapdata-detail)
# ★ the engine looks in <its binary>/../lib/vibeserver/mapdata first (vibe_mapdata.h dir()) — /usr/lib/vibeserver/mapdata;
#   both packs live there, file sets disjoint, index.json the basic pack's
MAP_N=$(in_root dpkg -L vibeserver-mapdata-detail | grep -c '^/usr/lib/vibeserver/mapdata/.')
[ "$MAP_N" -gt 0 ] || die "the map pack installed no files into /usr/lib/vibeserver/mapdata"
say "map detail pack $MAP_VER: $MAP_N files, $(du -sh "$R/usr/lib/vibeserver/mapdata" | cut -f1) in /usr/lib/vibeserver/mapdata (basic + detail)"
rm -rf "$R/tmp/vdeb"
IMAGE_VERSION=${IMAGE_VERSION:-$(echo "$VS_VER" | sed 's/-[0-9]*$//; s/~/-/g')-$(date -u +%Y%m%d)}
say "engine vibeserver $VS_VER — image version $IMAGE_VERSION"

# ── 4. The kernel's modules and firmware (ROCKNIX's, matching its KERNEL) ────────────────────────────────────────────
mkdir -p "$R/usr/lib/modules" "$R/usr/lib/firmware"
cp -a "$KO/modules/$KVER" "$R/usr/lib/modules/"
cp -a "$KO/firmware/." "$R/usr/lib/firmware/"
in_root depmod -a "$KVER"

# ── 4b. vibe-hhui, the native receiver UI (handheld-ui/) ─────────────────────────────────────────────────────────────
# ★ Compiled HERE, in the build container (Debian trixie arm64 — the card's own release, so the same libraries), never
#   in the image: no compiler and no -dev package lands on the card. Fonts: JetBrains Mono (OFL) for the UI, DSEG14
#   (OFL) for the frequency, as the app's VFD.
echo "==> vibe-hhui (the native receiver UI)"
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends g++ cmake make pkg-config \
  libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libgles-dev libegl-dev >/dev/null
rm -rf /work/hhui && cmake -S /repo/handheld-ui -B /work/hhui -DCMAKE_BUILD_TYPE=Release >/dev/null
make -C /work/hhui -j"${JOBS:-6}" >/dev/null || die "vibe-hhui did not build"
HHUI_BIN=/work/hhui/vibe-hhui

# ── 5. Our files, users and units ────────────────────────────────────────────────────────────────────────────────────
# ★ modes and times from the repository, ownership root's (the checkout belongs to the Mac's user)
cp -R --preserve=mode,timestamps "$H/rootfs/." "$R/"
chmod 0755 "$R"/usr/lib/vibesdr-r36s/*
install -m 0755 "$HHUI_BIN" "$R/usr/lib/vibesdr-r36s/vibe-hhui"
mkdir -p "$R/usr/share/vibesdr-r36s/fonts"
cp /repo/assets/mapfonts/JetBrainsMono-Bold.ttf /repo/assets/mapfonts/JetBrainsMono-Regular.ttf /repo/assets/mapfonts/OFL-JetBrainsMono.txt \
   /repo/assets/fonts/DSEG14Classic-BoldItalic.ttf /repo/assets/fonts/DSEG-LICENSE.txt \
   /repo/assets/fonts/AtkinsonHyperlegible-Regular.ttf /repo/assets/fonts/AtkinsonHyperlegible-Bold.ttf \
   /repo/assets/fonts/OFL-AtkinsonHyperlegible.txt "$R/usr/share/vibesdr-r36s/fonts/"   # ★ the native UI's face (Stuart 2026-11-06)
python3 "$H/art/make-splash.py" /repo "$R/usr/share/vibesdr-r36s" >/dev/null
rm -f "$R/usr/share/vibesdr-r36s/splash-base.png"
mv "$R/usr/share/vibesdr-r36s/splash-preview.png" /out/splash-preview.png
in_root useradd --system --user-group --home-dir /var/lib/vibe-kiosk --no-create-home --shell /usr/sbin/nologin \
  --groups video,render,input,audio vibe-kiosk
in_root id vibeserver >/dev/null || die "the vibeserver package did not create its user"
in_root usermod -a -G audio,plugdev vibeserver 2>/dev/null || true
# ★ root has NO password: locked ('*') — there is no login anywhere on the handheld (no SSH, no getty on the panel)
in_root usermod -p '*' root
mkdir -p "$R/media/root-ro" "$R/media/root-rw" "$R/media/VIBESDR" "$R/var/lib/vibe-state" "$R/var/lib/vibe-kiosk" \
  "$R/var/lib/vibeserver" "$R/boot/firmware" "$R/usr/share/doc/vibesdr-r36s"
cp "$H/README.md" "$R/usr/share/doc/vibesdr-r36s/README.md"
# the engine's own units: the network server, its radios starter and maintenance are not the handheld's
for u in vibeserver.service vibeserver-radios.service vibeserver-maintenance.path vibeserver-maintenance.service \
         vibeserver-pocket.service vibeserver-pocket-data.service vibeserver-pocket-seal.service \
         vibeserver-pocket-cpu.service; do
  in_root systemctl mask "$u" >/dev/null 2>&1 || true
done
# ★★ no console on the panel (Cog has it), no apt timers, and NO NETWORK SERVICES: networkd / resolved / timesyncd
#    masked — the handheld never configures a network (no SSH either: not installed, Stuart 2026-11-05)
for u in getty@tty1.service systemd-networkd.service systemd-networkd.socket systemd-networkd-wait-online.service \
         systemd-resolved.service systemd-network-generator.service apt-daily.timer apt-daily-upgrade.timer \
         apt-daily.service apt-daily-upgrade.service e2scrub_all.timer e2scrub_reap.service fstrim.timer \
         systemd-timesyncd.service systemd-backlight@backlight:backlight.service; do
  in_root systemctl mask "$u" >/dev/null 2>&1 || true
done
for u in vibe-splash.service vibe-clock.service vibe-cpu.service vibe-state.service vibe-hwd.service \
         vibe-bootkeys.service vibe-card.service vibeserver-handheld.service vibe-kiosk.service vibe-hhui.service \
         vibe-debuglog.service; do
  in_root systemctl enable "$u" >/dev/null
done
in_root systemctl set-default multi-user.target >/dev/null
echo vibesdr-r36s > "$R/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tvibesdr-r36s\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$R/etc/hosts"
ln -sf /usr/share/zoneinfo/Etc/UTC "$R/etc/localtime" 2>/dev/null || true
echo "Etc/UTC" > "$R/etc/timezone"
# ★ made on the device, never shared by every card
: > "$R/etc/machine-id"; rm -f "$R/var/lib/dbus/machine-id"
# ★ the clock floor (vibe-clock, and systemd's own): no clock on this card may be earlier than the build
touch "$R/usr/lib/clock-epoch"
echo "$IMAGE_VERSION" > "$R/etc/vibesdr-r36s-version"
# package manifest (and the SOURCES below) travel with the image
in_root dpkg-query -W -f '${Package}\t${Version}\t${Architecture}\n' | sort > "$R/usr/share/doc/vibesdr-r36s/packages.txt"
cp "$R/usr/share/doc/vibesdr-r36s/packages.txt" "/out/vibesdr-r36s-$IMAGE_VERSION.packages.txt"
# ★ the card is offline: no apt sources left to update from (no auto-updates; a new engine arrives by card swap)
rm -f "$R/etc/apt/sources.list" "$R"/etc/apt/sources.list.d/*.list "$R/usr/sbin/policy-rc.d" "$R/etc/resolv.conf"
: > "$R/etc/resolv.conf"      # ★ the engine is local; nothing on the handheld needs a name server
in_root apt-get clean
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/*.bin "$R"/var/log/*.log "$R"/var/log/apt/* "$R"/tmp/* \
       "$R"/var/cache/debconf/*-old "$R"/var/lib/dpkg/*-old "$R/debootstrap"
find "$R/usr/share/doc" -mindepth 1 -maxdepth 1 -type d ! -name vibesdr-r36s -exec sh -c \
  'find "$1" -mindepth 1 ! -name copyright -delete 2>/dev/null' sh {} \;
cleanup

# ── 6. The boot partition ────────────────────────────────────────────────────────────────────────────────────────────
B=$W/boot
mkdir -p "$B/overlays" "$B/stock" "$B/extlinux"
cp "$RX/KERNEL" "$B/KERNEL"
cp "$RX/rk3326-gameconsole-eeclone.dtb" "$B/"
# ★★ boot.scr = ROCKNIX's script + ONE line: ramdisk_addr_r above the 24 MB KERNEL (kernel_addr_r 0x09000000 + 24 MB
#    would run over U-Boot's default 0x0a200000). The legacy script image is a 64-byte header + an 8-byte length list.
dd if="$BL/b_boot.scr" of="$W/boot.txt" bs=1 skip=72 status=none
grep -q '^sysboot ' "$W/boot.txt" || die "ROCKNIX's boot.scr has no sysboot line"
sed -i 's|^sysboot |# VibeSDR: the initrd (/dev, /dev/console, /root) goes above the 24 MB KERNEL\nsetenv ramdisk_addr_r "0x0c000000"\nsysboot |' "$W/boot.txt"
mkimage -A arm64 -O linux -T script -C none -n "VibeSDR R36S boot" -d "$W/boot.txt" "$B/boot.scr" >/dev/null
# ★★★ vibe-initrd: /dev, /dev/console, /root — nothing else. ROCKNIX's built-in initramfs has no /root, so with rdinit=
#     skipping its init the kernel could not mount root= ("VFS: Cannot open root device … error -2", kernel panic,
#     reboot in 1 s: the first card, 2026-11-06). The kernel unpacks this on top of the built-in one.
rm -rf "$W/ird" && mkdir -p "$W/ird/dev" "$W/ird/root" && mknod -m 600 "$W/ird/dev/console" c 5 1
(cd "$W/ird" && find . | sort | cpio -o -H newc --owner 0:0 --quiet) > "$B/vibe-initrd"
# ★★★ ROCKNIX's panel overlay UNCHANGED — its 80 Hz default (clock=30000, 640,46,2,44 / 480,16,2,14). Proven on
#     Stuart's R36S 2026-11-06: both 60 Hz modes ROCKNIX lists put the picture off-centre or garbled on this panel
#     (23.04 MHz: ~80 px left; 22.92 MHz: left half only, columns of noise). The page draws at 30 fps whatever the
#     panel refreshes at, so 80 Hz costs no drawing. board/panel-60hz.py is kept only as the record of what failed.
cp "$H/board/mipi-panel.rocknix.dtbo" "$B/overlays/mipi-panel.dtbo"
dtc -q -@ -I dts -O dtb -o "$B/overlays/vibe-r36s.dtbo" "$H/board/vibe-r36s.dts"
cp "$H/board/rf3536k3ka.dtb" "$B/stock/rf3536k3ka.dtb"
# ★ prove the overlays apply to the DTB, as U-Boot will (same libfdt)
fdtoverlay -i "$B/rk3326-gameconsole-eeclone.dtb" -o "$W/merged.dtb" "$B/overlays/mipi-panel.dtbo" "$B/overlays/vibe-r36s.dtbo" \
  || die "the overlays do not apply to the eeclone DTB"
DISKID=$(printf '%s' "vibesdr-r36s-$IMAGE_VERSION" | sha256sum | cut -c1-8)
sed -e "s/@DISKID@/$DISKID/g" "$H/board/extlinux.conf" > "$B/extlinux/extlinux.conf"
grep -q ' $' "$B/extlinux/extlinux.conf" && die "extlinux.conf has a trailing space (U-Boot's parser breaks on it)"
cp "$H/rootfs/usr/share/vibesdr-r36s/VIBESDR-README.txt" "$B/README.txt"

# ── 7. SOURCES (GPL: where every binary came from) ───────────────────────────────────────────────────────────────────
S=$R/usr/share/doc/vibesdr-r36s/SOURCES
{
  echo "VibeSDR R36S handheld image $IMAGE_VERSION — sources of every binary on this card"
  echo "Built $(date -u '+%Y-%m-%d %H:%M UTC') by image/r36s/build.sh (https://github.com/Stuey3D/VibeSDR, branch v12)."
  echo
  echo "BOOT CHAIN — from ROCKNIX release $ROCKNIX_TAG, image $ROCKNIX_IMG"
  echo "  release:  https://github.com/ROCKNIX/distribution/releases/tag/$ROCKNIX_TAG  (sha256 $ROCKNIX_SHA256)"
  echo "  source:   https://github.com/ROCKNIX/distribution/tree/$ROCKNIX_COMMIT  (tag $ROCKNIX_TAG)"
  echo "  KERNEL    sha256 $(sha256sum "$B/KERNEL" | cut -d' ' -f1)"
  echo "            Linux $KVER (https://www.kernel.org/pub/linux/kernel/v7.x/linux-$KVER.tar.xz) + ROCKNIX's RK3326 patches,"
  echo "            rocknix-joypad and mainline-rockchip patch sets, panel-generic-dsi.c and its built-in initramfs,"
  echo "            built from projects/ROCKNIX/packages/linux at the commit above (GPL-2.0)."
  echo "  modules + firmware: /usr/lib/modules/$KVER and /usr/lib/firmware from the same image's SYSTEM"
  echo "            (usr/lib/kernel-overlays/base/lib), same build."
  echo "  rk3326-gameconsole-eeclone.dtb  sha256 $(sha256sum "$B/rk3326-gameconsole-eeclone.dtb" | cut -d' ' -f1)"
  echo "            projects/ROCKNIX/devices/RK3326/linux/dts/rockchip/ at the commit above (GPL-2.0 OR MIT)."
  echo "  loader (sectors 64…32767): b_uboot.bin.uart5  sha256 $LOADER_SHA256"
  echo "            U-Boot v2025.10 (https://github.com/u-boot/u-boot/archive/refs/tags/v2025.10.tar.gz), rk3326-handheld_defconfig,"
  echo "            ROCKNIX's projects/ROCKNIX/devices/RK3326/packages/u-boot patches (GPL-2.0+), packed with Rockchip's rkbin"
  echo "            blobs (DDR init tuned for UART5, miniloader, BL31) from https://github.com/rockchip-linux/rkbin at"
  echo "            74213af1e952c4683d2e35952507133b61394862 (Rockchip's redistributable binaries)."
  echo "  boot.scr  ROCKNIX's b_boot.scr + one line, ramdisk_addr_r (board detection by SARADC, then extlinux)."
  echo "  vibe-initrd  ours: three entries (/dev, /dev/console, /root) the kernel needs to mount the root."
  echo "  overlays/mipi-panel.dtbo  generated by ROCKNIX on this board from the stock vendor DTB (stock/rf3536k3ka.dtb);"
  echo "            unchanged (its 80 Hz default — the 60 Hz modes it lists do not display correctly on this panel)."
  echo "  overlays/vibe-r36s.dtbo   ours: image/r36s/board/vibe-r36s.dts (the stock CPU steps, backlight 120)."
  echo "  stock/rf3536k3ka.dtb      the vendor DTB from the unit's stock card (ArkOS, Rockchip BSP 4.4), kept for reference."
  echo
  echo "OPERATING SYSTEM — Debian 13 (trixie) arm64, from ${MIRROR}; the exact package versions are in packages.txt"
  echo "  beside this file. Debian's sources: https://snapshot.debian.org (by package and version) or https://sources.debian.org."
  echo
  echo "ENGINE — vibeserver $VS_VER  (package sha256 $DEB_SHA), https://github.com/Stuey3D/VibeSDR"
  echo "MAPS — vibeserver-mapdata-detail $MAP_VER  (package sha256 $MAP_SHA), built by scripts/build-mapdata-deb.sh from"
  echo "  assets/mapdata/v1 (Natural Earth and the other sources listed, with their licences, in scripts/gen-map-data.mjs)"
  echo
  echo "OURS — /usr/lib/vibesdr-r36s/* (vibe-hwd, vibe-splash, vibe-card, vibe-bootkeys, vibe-clock, overlay-init, …),"
  echo "  the units in /usr/lib/systemd/system/vibe*.service and /etc: image/r36s/ in the VibeSDR repository."
  echo "  Splash artwork: assets/icon.png and assets/fonts/AtkinsonHyperlegible-Regular.ttf (SIL OFL 1.1)."
} > "$S"
cp "$S" "$B/SOURCES.txt"
cp "$S" "/out/vibesdr-r36s-$IMAGE_VERSION.SOURCES.txt"

# ── 8. The state partition's first contents (so the fstab bind mounts work on the very first boot) ───────────────────
ST=$W/state
mkdir -p "$ST/vibeserver" "$ST/kiosk"
chown "$(in_root id -u vibeserver):$(in_root id -g vibeserver)" "$ST/vibeserver"
chown "$(in_root id -u vibe-kiosk):$(in_root id -g vibe-kiosk)" "$ST/kiosk"
chmod 0750 "$ST/vibeserver"

# ── 9. Filesystems in files, then the card image ─────────────────────────────────────────────────────────────────────
P=$W/parts
ROOT_KIB=$(du -sk --apparent-size "$R" | cut -f1)
ROOT_MIB=$(( (ROOT_KIB * 115 / 100 / 1024 + ROOT_HEAD_MIB + 3) / 4 * 4 ))
UUID_ROOT=$(printf '%s-root' "$DISKID" | sha256sum | sed -E 's/^(.{8})(.{4})(.{4})(.{4})(.{12}).*/\1-\2-\3-\4-\5/')
UUID_STATE=$(printf '%s-state' "$DISKID" | sha256sum | sed -E 's/^(.{8})(.{4})(.{4})(.{4})(.{12}).*/\1-\2-\3-\4-\5/')
say "root filesystem: $((ROOT_KIB / 1024)) MiB of files in a $ROOT_MIB MiB partition"
truncate -s "${ROOT_MIB}M" "$P/root.ext4"
mkfs.ext4 -q -L vibe-root -U "$UUID_ROOT" -E hash_seed="$UUID_ROOT",root_owner=0:0 -m 0 \
  -d "$R" "$P/root.ext4"
truncate -s "${STATE_MIB}M" "$P/state.ext4"
mkfs.ext4 -q -L vibestate -U "$UUID_STATE" -E hash_seed="$UUID_STATE",root_owner=0:0 -m 1 -d "$ST" "$P/state.ext4"
truncate -s "${BOOT_MIB}M" "$P/boot.vfat"
mkfs.vfat -F 32 -n VIBEBOOT -i "${DISKID}" "$P/boot.vfat" >/dev/null
mcopy -s -i "$P/boot.vfat" "$B"/* ::/
IMG=$W/vibesdr-r36s-$IMAGE_VERSION.img
S1=32768; S2=$((S1 + BOOT_MIB * 2048)); S3=$((S2 + ROOT_MIB * 2048)); END=$((S3 + STATE_MIB * 2048))
rm -f "$IMG"; truncate -s $((END * 512)) "$IMG"
sfdisk -q "$IMG" <<EOF
label: dos
label-id: 0x$DISKID
unit: sectors

start=$S1, size=$((BOOT_MIB * 2048)), type=c, bootable
start=$S2, size=$((ROOT_MIB * 2048)), type=83
start=$S3, size=$((STATE_MIB * 2048)), type=83
EOF
# ★ the loader in ONE aligned write at 32 KiB, as ROCKNIX's update.sh does — it ends exactly at 16 MiB, before p1
dd if="$BL/b_uboot.bin.uart5" of="$IMG" bs=32K seek=1 conv=notrunc status=none
dd if="$P/boot.vfat" of="$IMG" bs=1M seek=$((S1 / 2048)) conv=notrunc,sparse status=none
dd if="$P/root.ext4" of="$IMG" bs=1M seek=$((S2 / 2048)) conv=notrunc,sparse status=none
dd if="$P/state.ext4" of="$IMG" bs=1M seek=$((S3 / 2048)) conv=notrunc,sparse status=none
say "image: $(( END * 512 / 1048576 )) MiB (boot $BOOT_MIB, root $ROOT_MIB, state $STATE_MIB; VIBESDR made on first boot)"

# ── 10. Verify offline, compress ─────────────────────────────────────────────────────────────────────────────────────
bash "$H/verify.sh" "$IMG" "$R" "$B"
rm -f "/out/vibesdr-r36s-$IMAGE_VERSION.img.gz"
pigz -9 -n -p "$JOBS" -c "$IMG" > "/out/vibesdr-r36s-$IMAGE_VERSION.img.gz"
(cd /out && sha256sum "vibesdr-r36s-$IMAGE_VERSION.img.gz" > "vibesdr-r36s-$IMAGE_VERSION.img.gz.sha256")
echo "$IMAGE_VERSION" > /out/LATEST
say "done: /out/vibesdr-r36s-$IMAGE_VERSION.img.gz ($(du -h "/out/vibesdr-r36s-$IMAGE_VERSION.img.gz" | cut -f1))"
cat "/out/vibesdr-r36s-$IMAGE_VERSION.img.gz.sha256"
