#!/bin/bash
# customise.sh — turn a Raspberry Pi OS Lite root filesystem into a POCKET VIBESERVER (2026-10-10).
#
# ★★★ ONE SCRIPT, BOTH ARCHITECTURES, BOTH BUILD ROUTES. It runs INSIDE the target root (a chroot),
#     as root, and is the only place the pocket image's contents are decided:
#       · pi-gen:       stage-pocket/00-pocket/01-run.sh pipes it to on_chroot;
#       · official img: build-image.sh loop-mounts Raspberry Pi OS Lite and chroots into it.
#     arm64 and armhf differ only in which .deb is installed — dpkg picks by architecture.
# ★★★ MINIMAL ON PURPOSE (Stuart, 2026-10-10: "Raspberry Pi OS Lite ... runs the Pi 2 with great
#     success and low RAM use. Keep the footprint minimal"). It ADDS only what the hotspot and the
#     captive portal need, and switches OFF what a headless radio in a pocket never uses.
# ★ Idempotent: running it twice leaves the same box.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
POCKET_DEBS=${POCKET_DEBS:-/tmp/pocket-debs}
POCKET_FILES=${POCKET_FILES:-/tmp/pocket-files}
COUNTRY=${POCKET_COUNTRY:-GB}
HOSTNAME_NEW=${POCKET_HOSTNAME:-vibeserversetup}
say() { echo "pocket-image: $*"; }

ARCH=$(dpkg --print-architecture)
say "customising a $ARCH root ($(. /etc/os-release; echo "$PRETTY_NAME"))"

# ── 1. Packages: only what the hotspot needs that Lite may not already have ──────────────────────
# ★ NetworkManager is Lite's default since Bookworm — checked, never installed over the top.
#   dnsmasq-base: NM's shared (hotspot) mode runs it for DHCP + the captive DNS rule — a binary, no
#   service of its own. nftables: the :80 → front-door redirect (its own service stays disabled).
#   iw: station count + the no-drop scan. python3: the root Wi-Fi service (Lite ships it).
apt-get update
dpkg -s network-manager >/dev/null 2>&1 || { say "!! NetworkManager is not on this image — refusing (Lite has had it since Bookworm)"; exit 1; }
apt-get install -y --no-install-recommends dnsmasq-base nftables iw rfkill python3 ca-certificates curl gnupg

# ── 2. The VibeSDR apt repository, so the box updates like every other VibeServer ─────────────
install -d -m 0755 /usr/share/keyrings
if [ -f "$POCKET_FILES/vibesdr.gpg" ]; then
  install -m 0644 "$POCKET_FILES/vibesdr.gpg" /usr/share/keyrings/vibesdr.gpg
else
  curl -fsSL https://apt.vibesdr.net/KEY.gpg | gpg --dearmor --yes -o /usr/share/keyrings/vibesdr.gpg
fi
echo "deb [arch=arm64,amd64,armhf signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main" \
  > /etc/apt/sources.list.d/vibesdr.list
apt-get update

# ── 3. VibeServer itself ────────────────────────────────────────────────────────────────────────
# ★★ A LOCAL .deb WINS when one is provided for this architecture — the pocket code must be in the
#    package, and until a release carries it the image is built from this branch (build-deb.sh).
LOCAL_DEB=$(ls -1 "$POCKET_DEBS"/vibeserver_*_"$ARCH".deb 2>/dev/null | sort -V | tail -1 || true)
if [ -n "$LOCAL_DEB" ]; then
  say "installing the local build $(basename "$LOCAL_DEB")"
  apt-get install -y --no-install-recommends "$LOCAL_DEB"
else
  say "installing vibeserver from apt.vibesdr.net"
  apt-get install -y --no-install-recommends vibeserver
fi
[ -x /usr/lib/vibeserver/vibeserver-pocket ] || { say "!! this vibeserver package has no pocket support (vibeserver-pocket missing) — build it from the pocket branch: image/pocket/build-deb.sh"; exit 1; }
# ★★★ ffmpeg — HOW DAB+ IS DECODED ON LINUX (Stuart, 2026-10-10: "does it contain FFMPEG ready for DAB+?" — it did not).
#     The package only RECOMMENDS it, and everything here installs --no-install-recommends, so the first images had no
#     DAB+ audio at all with an RTL-SDR plugged in. Named here explicitly, still without ITS recommends (no X, no extras).
say "installing ffmpeg (DAB+ audio)"
apt-get install -y --no-install-recommends ffmpeg
command -v ffmpeg >/dev/null || { say "!! ffmpeg did not install — DAB+ would have no audio"; exit 1; }

# ── 4. The pocket marker + defaults (root's directory: the daemon cannot create the marker) ──────
install -d -o root -g root -m 0755 /etc/vibeserver-pocket
echo "1" > /etc/vibeserver-pocket/enabled
chmod 0644 /etc/vibeserver-pocket/enabled
if [ ! -f /etc/vibeserver-pocket/pocket.conf ]; then
  install -m 0644 "$POCKET_FILES/pocket.conf" /etc/vibeserver-pocket/pocket.conf
  sed -i "s/^COUNTRY=.*/COUNTRY=$COUNTRY/" /etc/vibeserver-pocket/pocket.conf
fi

# ── 5. First-boot server config: a Full-mode front door with no radio and no password yet ────────
# ★★ Without it the daemon would start as a single-radio server and look for a dongle that is not
#    there; with it, the front door serves the setup page from the first boot. The admin password
#    is chosen in that page (vibeserver/pocket.cpp, claim) — never baked into an image.
if [ ! -s /etc/vibeserver/config.json ]; then
  install -d -m 0755 /etc/vibeserver
  install -m 0600 "$POCKET_FILES/config.json" /etc/vibeserver/config.json
  chown vibeserver:vibeserver /etc/vibeserver/config.json /etc/vibeserver
fi

# ★★ NETWORKMANAGER'S CONNECTIVITY CHECK — the tunnel's gate. NM decides "full" (real internet) by
#    fetching this when a connection comes up and every 5 minutes after; vibeserver-pocket follows
#    the verdict by event (nmcli monitor), and the server starts its tunnel only on "full". A box on
#    its own hotspot is never "full", so it never spawns cloudflared there.
install -d -m 0755 /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/50-vibeserver-connectivity.conf <<'EOF'
# Written by the VibeServer pocket image: lets NetworkManager tell "joined Wi-Fi" from "has internet".
[connectivity]
uri=http://nmcheck.gnome.org/check_network_status.txt
response=NetworkManager is online
interval=300
EOF

# ── 6. Services: ours on, the ones a pocket radio never uses off ─────────────────────────────────
# ★★ postinst enables ours only when systemd is RUNNING ([ -d /run/systemd/system ]), which in a
#    chroot it is not — so an image would boot with VibeServer installed and never started. Enabled
#    here by hand, every one of them.
systemctl enable NetworkManager.service vibeserver.service vibeserver-radios.service \
                 vibeserver-maintenance.path vibeserver-pocket.service
# nftables.service would load /etc/nftables.conf at boot — we load our own table only while a
# hotspot is up, so the service itself stays off.
systemctl disable nftables.service 2>/dev/null || true
for u in bluetooth.service hciuart.service ModemManager.service triggerhappy.service triggerhappy.socket \
         apt-daily.timer apt-daily-upgrade.timer man-db.timer e2scrub_all.timer \
         rpi-display-backlight.service keyboard-setup.service; do
  systemctl disable "$u" 2>/dev/null || true
done
# ★★ NO CONSOLE LOGINS (Stuart's Pi 2 list, 2026-10-10). A pocket box has no screen and no keyboard,
#    and its only account is LOCKED (section 8) — so a getty on HDMI or on the UART could not log
#    anyone in anyway. With Bluetooth off, serial0 is the GPIO UART: no getty there either.
#  ★ If an owner sets a password with Imager and wants the HDMI console: systemctl enable getty@tty1.
systemctl mask serial-getty@ttyAMA0.service serial-getty@serial0.service serial-getty@ttyS0.service 2>/dev/null || true
systemctl disable getty@tty1.service 2>/dev/null || true
systemctl mask getty@tty1.service 2>/dev/null || true
# ★ udisks2 is D-Bus activated, so "disable" does not stop it: mask it. It automounts USB disks —
#   nothing to automount on a radio box, and it costs ~8 MB resident once anything wakes it.
systemctl mask udisks2.service 2>/dev/null || true
# ★ apt's daily timers are off: they wake a 512 MB box to download indexes over a hotspot that has
#   no internet, and VibeServer's own update schedule (admin page) is the one an owner sets.

# ── 7. Name, Wi-Fi country, boot config ──────────────────────────────────────────────────────────
# ★★★ "vibeserversetup" FIRST (Stuart, 2026-10-10): a new box answers as VibeServerSetup.local on
#     any network, with no router page. The root service retires it (→ "vibepocket") once setup is
#     finished, and the server then publishes the name chosen in setup.
# ★ Never "vibeserver": avahi publishes <hostname>.local and the server publishes <its name>.local —
#   the same label from two responders is a conflict avahi "solves" by renaming.
echo "$HOSTNAME_NEW" > /etc/hostname
sed -i "s/^127\.0\.1\.1.*/127.0.1.1\t$HOSTNAME_NEW/" /etc/hosts
grep -q "^127\.0\.1\.1" /etc/hosts || echo -e "127.0.1.1\t$HOSTNAME_NEW" >> /etc/hosts
# ★ The Pi's Wi-Fi is soft-blocked until a country is set. raspi-config's non-interactive form does
#   both halves (regulatory domain + unblock); the service also unblocks at start.
if command -v raspi-config >/dev/null; then raspi-config nonint do_wifi_country "$COUNTRY" || true; fi
for BOOTCFG in /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$BOOTCFG" ] || continue
  if ! grep -q "^# VibeServer pocket" "$BOOTCFG"; then
    cp -n "$BOOTCFG" "$BOOTCFG.bak-pocket" || true
    # ★★ HEADLESS: no display stack, no Bluetooth. The KMS driver reserves CMA for a framebuffer
    #    nobody looks at; on a 512 MB board that is RAM the radio wants. Bluetooth shares the
    #    Wi-Fi chip's antenna and UART on the Pi 3 — off, it can never step on the hotspot.
    sed -i 's/^\(dtoverlay=vc4-kms-v3d.*\)/#\1  # off: headless pocket box/' "$BOOTCFG"
    sed -i 's/^\(camera_auto_detect=1\)/#\1/; s/^\(display_auto_detect=1\)/#\1/' "$BOOTCFG"
    cat >> "$BOOTCFG" <<'EOF'

# VibeServer pocket — headless radio box
gpu_mem=16
dtoverlay=disable-bt
EOF
  fi
  break
done

# ── 7b. POWER-CUT HARDENING (Stuart, 2026-10-10: "the box WILL have its power pulled at any moment") ──
# ★★★ READ-ONLY ROOT. overlayroot (raspi-config's own "Overlay File System") is installed and its
#     initramfs built HERE, for every kernel on the image; the switch itself (overlayroot=tmpfs on the
#     command line) is thrown on the first boot by vibeserver-pocket-seal, once Pi OS has written its
#     one-time identity. Settings live on the small data=journal partition (vibeserver-pocket-data).
apt-get install -y --no-install-recommends overlayroot initramfs-tools
for BOOTCFG in /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$BOOTCFG" ] || continue
  grep -q '^auto_initramfs=1' "$BOOTCFG" || echo "auto_initramfs=1" >> "$BOOTCFG"
  break
done
update-initramfs -u -k all || update-initramfs -c -k all
ls -la /boot/firmware/initramfs* 2>/dev/null | sed 's/^/pocket-image: initramfs: /' || say "!! no initramfs in /boot/firmware"
for CMDF in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
  [ -f "$CMDF" ] || continue
  # ★★ NO ROOT RESIZE. Pi OS grows partition 2 to the end of the card on first boot; here partition 3
  #    (the settings) follows it, and the root is read-only anyway. vibeserver-pocket-data grows the
  #    settings partition instead — see the doc for why most of the card is left unallocated.
  sed -i 's/ resize\b//; s/^resize //' "$CMDF"
  say "cmdline: $(cat "$CMDF")"
  break
done
# ★ The boot partition read-only too (raspi-config's "boot partition read-only"): the seal and an update
#   remount it for the moment they need it.
sed -i -E 's#^([^#].*[[:space:]]/boot/firmware[[:space:]]+vfat[[:space:]]+)defaults([[:space:]])#\1defaults,ro\2#' /etc/fstab
grep -E '/boot/firmware' /etc/fstab | sed 's/^/pocket-image: fstab: /'
# ★★ VOLATILE BY DEFAULT. The journal lives in RAM (16 MB cap) — a pocket box's logs are for the session
#    in front of you, and a journal on the SD card is a write every few seconds.
install -d /etc/systemd/journald.conf.d
cat > /etc/systemd/journald.conf.d/60-vibeserver-pocket.conf <<'EOF'
# VibeServer pocket image: logs in RAM only — no SD writes, gone at power-off.
[Journal]
Storage=volatile
RuntimeMaxUse=16M
EOF
# ★ Swap: compressed RAM (zram) only, never a file on the card. rpi-swap is Trixie's swap manager.
install -d /etc/rpi/swap.conf.d
cat > /etc/rpi/swap.conf.d/60-vibeserver-pocket.conf <<'EOF'
# VibeServer pocket image: no swap file on the SD card — zram only.
[Main]
Mechanism=zram
EOF
systemctl disable dphys-swapfile.service 2>/dev/null || true
# ★ fake-hwclock (if present) would save the time to the root every hour; the settings partition keeps
#   systemd-timesyncd's clock file instead (written on each sync, not on a timer).
systemctl disable fake-hwclock.service 2>/dev/null || true
install -d -m 0755 /data /var/lib/vibeserver-pocket
systemctl enable vibeserver-pocket-data.service vibeserver-pocket-seal.service

# ── 8. A login nobody needs, so first boot never stops to ask for one ───────────────────────────
# ★ Raspberry Pi OS asks for a user on the first boot unless Raspberry Pi Imager's settings set
#   one. A pocket box has no screen to answer on. So: one account, password LOCKED (no login at
#   all until the owner sets one with Imager or on the card), SSH off unless Imager turns it on.
if ! getent passwd 1000 >/dev/null; then
  useradd -m -u 1000 -s /bin/bash -G sudo,video,plugdev,netdev vibe 2>/dev/null \
    || useradd -m -u 1000 -s /bin/bash vibe
  passwd -l vibe >/dev/null
fi
systemctl disable userconfig.service 2>/dev/null || true
rm -f /etc/ssh/sshd_config.d/rename_user.conf 2>/dev/null || true

# ── 9. Tidy ─────────────────────────────────────────────────────────────────────────────────────
apt-get clean
rm -rf /var/lib/apt/lists/* /tmp/pocket-debs /tmp/pocket-files
say "done — first boot raises the open \"VibeServer\" hotspot"
