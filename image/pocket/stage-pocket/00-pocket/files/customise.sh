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
# ★★★ ONLY WHAT A FLASH CAN DO LIVES HERE (Stuart, 2026-10-13: "after initial setup we need to make sure that VibeServer
#     Portables can be fully updated without needing a full ISO again"). Everything that should track a RELEASE — the
#     system packages (pocket-packages.txt), the services on/off/masked, NetworkManager's connectivity check, avahi on
#     every interface, the journal in RAM, zram, the config.txt lines, /boot read-only — is in the PACKAGE:
#     /usr/lib/vibeserver/vibeserver-pocket-converge, called below and again by every Update. What stays here is
#     genuinely one-time: the marker, the first-boot defaults, the name, the apt key, the initramfs build, the resize
#     removal and the locked login. See image/pocket/README.md.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
POCKET_DEBS=${POCKET_DEBS:-/tmp/pocket-debs}
POCKET_FILES=${POCKET_FILES:-/tmp/pocket-files}
COUNTRY=${POCKET_COUNTRY:-GB}
HOSTNAME_NEW=${POCKET_HOSTNAME:-vibeserversetup}
say() { echo "pocket-image: $*"; }

ARCH=$(dpkg --print-architecture)
say "customising a $ARCH root ($(. /etc/os-release; echo "$PRETTY_NAME"))"

# ── 1. What the next two steps need; the rest of the packages come from the package's own list (step 4b) ──
# ★ NetworkManager is Lite's default since Bookworm — checked, never installed over the top.
#   curl + gnupg + ca-certificates: the apt key below. python3: vibeserver-pocket-converge (Lite ships it).
apt-get update
dpkg -s network-manager >/dev/null 2>&1 || { say "!! NetworkManager is not on this image — refusing (Lite has had it since Bookworm)"; exit 1; }
apt-get install -y --no-install-recommends ca-certificates curl gnupg python3

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
CONVERGE=/usr/lib/vibeserver/vibeserver-pocket-converge
[ -x "$CONVERGE" ] || { say "!! this vibeserver package has no $CONVERGE — it predates full updates; build it from this checkout: image/pocket/build-deb.sh"; exit 1; }

# ── 4. The pocket marker + defaults (root's directory: the daemon cannot create the marker) ──────
install -d -o root -g root -m 0755 /etc/vibeserver-pocket
echo "1" > /etc/vibeserver-pocket/enabled
chmod 0644 /etc/vibeserver-pocket/enabled
# ★★★ A NEW CARD'S ONE UPDATE CHECK (Stuart, 2026-10-28: "Make the SBC cards check for a VibeServer update on first
#     install if connected to the internet … then the owner can choose auto updates thereafter"). This file ARMS it: once
#     the owner has set up the network, the setup page asks the root helper (vibeserver-maintenance first-update) to look
#     ONCE for a newer VibeServer and install it — the package only, never the OS. Only a card carries the file, so a box
#     that updated to this package from an older one never runs it. The helper records the run on the settings partition
#     and never runs it again (offline, failed or cancelled included). A package from before the feature: not armed.
if grep -q '^first_update()' /usr/lib/vibeserver/vibeserver-maintenance 2>/dev/null; then
  : > /etc/vibeserver-pocket/first-update
  chmod 0644 /etc/vibeserver-pocket/first-update
  say "armed the one update check at first setup"
else
  say "this vibeserver package has no first-update — the card will not check for an update at setup"
fi
# ★★ A CARD BUILT FROM A BETA looks in the BETA suite (Stuart: "stable suite unless the card was built from beta"). The same
#    file the admin page's "Receive beta updates" tick writes (vibeserver-maintenance beta_write), so the page shows it
#    ticked and the owner can untick it — the way back to stable is the one that already exists.
case "$(dpkg-query -W -f='${Version}' vibeserver 2>/dev/null)" in
  *~beta*)
    printf '%s\n' "# VibeServer BETA updates. Written by the admin page (Receive beta updates);" \
      "# untick it there, or delete this file and run apt update, to go back to stable releases only." \
      "deb [arch=arm64,amd64,armhf signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net beta main" \
      > /etc/apt/sources.list.d/vibesdr-beta.list
    chmod 0644 /etc/apt/sources.list.d/vibesdr-beta.list
    say "a beta package: the card follows the beta suite (the admin page shows Receive beta updates ticked)" ;;
esac
if [ ! -f /etc/vibeserver-pocket/pocket.conf ]; then
  install -m 0644 "$POCKET_FILES/pocket.conf" /etc/vibeserver-pocket/pocket.conf
  sed -i "s/^COUNTRY=.*/COUNTRY=$COUNTRY/" /etc/vibeserver-pocket/pocket.conf
fi

# ── 4b. The system packages — THE PACKAGE'S LIST, the same one every Update reads ───────────────
# ★★ dnsmasq-base (hotspot DHCP + captive DNS), nftables (the :80 redirect), iw, rfkill, avahi-daemon, overlayroot +
#    initramfs-tools (the read-only root), ffmpeg (★★★ DAB+ audio — Stuart, 2026-10-10: "does it contain FFMPEG ready
#    for DAB+?"; the package only RECOMMENDS it)… all in /usr/lib/vibeserver/pocket-packages.txt, which says why.
#    Without THEIR recommends (no X, no extras). After the marker: the converge does nothing without it.
MISSING=$("$CONVERGE" --missing-packages)
if [ -n "$MISSING" ]; then
  say "installing the Portable's system packages: $MISSING"
  # shellcheck disable=SC2086
  apt-get install -y --no-install-recommends $MISSING
fi
MISSING=$("$CONVERGE" --missing-packages)
[ -z "$MISSING" ] || { say "!! still missing after install: $MISSING"; exit 1; }
command -v ffmpeg >/dev/null || { say "!! ffmpeg did not install — DAB+ would have no audio"; exit 1; }

# ── 5. First-boot server config: a Full-mode front door with no radio and no password yet ────────
# ★★ Without it the daemon would start as a single-radio server and look for a dongle that is not
#    there; with it, the front door serves the setup page from the first boot. The admin password
#    is chosen in that page (vibeserver/pocket.cpp, claim) — never baked into an image.
if [ ! -s /etc/vibeserver/config.json ]; then
  install -d -m 0755 /etc/vibeserver
  install -m 0600 "$POCKET_FILES/config.json" /etc/vibeserver/config.json
  chown vibeserver:vibeserver /etc/vibeserver/config.json /etc/vibeserver
fi

# ── 6. THE OPERATING SYSTEM — the same converge every Update runs ─────────────────────────────────
# ★★★ Services on (ours, NetworkManager, avahi, the settings partition, the seal) and off (Bluetooth, ModemManager,
#     triggerhappy, apt's daily timers, nftables.service, dphys-swapfile, fake-hwclock), console gettys + udisks2
#     masked, NetworkManager's connectivity check (the tunnel's gate), avahi on every interface, journald in RAM, zram
#     only, config.txt's headless block (gpu_mem, disable-bt, audio off, no splash, auto_initramfs), /boot read-only in
#     fstab, /data. Each is explained where it is defined: vibeserver/linux/pocket/vibeserver-pocket-converge.
#  ★ Before the initramfs build below, so auto_initramfs=1 is in config.txt when it runs.
"$CONVERGE"
"$CONVERGE" --check >/dev/null || { "$CONVERGE" --check || true; say "!! the OS did not converge — refusing to build this image"; exit 1; }

# ── 7. Name, Wi-Fi country, boot config ──────────────────────────────────────────────────────────
# ★★★ "vibeserversetup" FIRST (Stuart, 2026-10-10): a new box answers as VibeServerSetup.local on
#     any network, with no router page. The root service retires it (→ "vibepocket") once setup is
#     finished, and the server then publishes the name chosen in setup.
# ★ Never "vibeserver": avahi publishes <hostname>.local and the server publishes <its name>.local —
#   the same label from two responders is a conflict avahi "solves" by renaming.
echo "$HOSTNAME_NEW" > /etc/hostname
sed -i "s/^127\.0\.1\.1.*/127.0.1.1\t$HOSTNAME_NEW/" /etc/hosts
grep -q "^127\.0\.1\.1" /etc/hosts || echo -e "127.0.1.1\t$HOSTNAME_NEW" >> /etc/hosts
# ★★★ THE NAME ON EVERY NETWORK (Stuart, 2026-10-13): avahi's allow/deny-interfaces lines are switched off by the
#     converge (step 6), and kept off by every Update.
# ★ The Pi's Wi-Fi is soft-blocked until a country is set. raspi-config's non-interactive form does
#   both halves (regulatory domain + unblock); the service also unblocks at start.
if command -v raspi-config >/dev/null; then raspi-config nonint do_wifi_country "$COUNTRY" || true; fi
# ★★ config.txt (headless: no display stack, no Bluetooth, no onboard audio) is the converge's too (step 6).
command -v vcgencmd >/dev/null || say "!! no vcgencmd — the display output will stay powered"

# ── 7b. POWER-CUT HARDENING (Stuart, 2026-10-10: "the box WILL have its power pulled at any moment") ──
# ★★★ READ-ONLY ROOT. overlayroot (raspi-config's own "Overlay File System") is installed and its
#     initramfs built HERE, for every kernel on the image; the switch itself (overlayroot=tmpfs on the
#     command line) is thrown on the first boot by vibeserver-pocket-seal, once Pi OS has written its
#     one-time identity. Settings live on the small data=journal partition (vibeserver-pocket-data).
# ★ overlayroot + initramfs-tools came from the package list (4b), auto_initramfs=1 from the converge (6). The explicit
#   build is IMAGE-ONLY: on a running box the overlayroot package's trigger and every kernel package rebuild it.
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
# ★ The boot partition read-only in fstab, the journal in RAM, zram-only swap, dphys-swapfile + fake-hwclock off, and the
#   settings partition's mount points: all the converge's (step 6). The settings partition itself is made by build-image.sh.
# ★ `|| true`: under pipefail a root with no /boot/firmware line (an Armbian board — image/sbc, where /boot is a directory
#   of root) would stop the build here on a line that only reports.
grep -E '/boot/firmware' /etc/fstab | sed 's/^/pocket-image: fstab: /' || true

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
