#!/bin/bash
# VibeServer for the Soft66IP / Orange Pi Zero (DietPi based, experimental) — the first-boot step.
#
# DietPi runs /boot/Automation_Custom_Script.sh ONCE, as root, at the end of its own first-run setup
# (dietpi.txt: AUTO_SETUP_CUSTOM_SCRIPT_EXEC=0). Its output is in DietPi's log,
# /var/lib/dietpi/logs/dietpi-firstrun-setup.log, and a copy is kept in /root/vibeserver-firstboot.log
# (/var/log is a RAM disk on DietPi and is cleared every hour).
#
# What it does, and nothing else:
#   1. Installs VibeServer with the official one-line installer — the same one every Debian/Ubuntu box
#      uses, so this server is an ordinary apt server from then on (Install updates on the admin page,
#      or apt upgrade). Stable channel, as the installer gives by default.
#   2. Frees the RTL-SDR from the DVB-T driver for this boot. The package's blacklist covers every
#      later boot.
#   3. Puts a one-line reminder at login until the server has been set up.
#
# ★ NO PASSWORDS HERE. DietPi asks for new root/dietpi passwords at the first login; the VibeServer
#   admin password is typed by the owner in the `vibeserver` setup wizard. Nothing is generated or
#   baked in.
# ★ Low-memory settings (256 MB): the CPU cap and zram are DietPi settings in dietpi.txt. The tunnel is
#   off on a new VibeServer anyway. The sample rate and DAB+ are per-radio settings, chosen in setup
#   once the radio is known — the README says what to pick. Nothing here invents a config key.
set -uo pipefail

LOG=/root/vibeserver-firstboot.log
exec > >(tee -a "$LOG") 2>&1

say() { printf '[VibeServer image] %s\n' "$*"; }
say "first boot step started $(date -u '+%Y-%m-%d %H:%M:%S') UTC"

# ── 1. VibeServer, from the official installer ────────────────────────────────────────────────────
# ★ The installer is ONE FUNCTION called on its last line, so a download cut off halfway runs nothing.
#   Exit codes (scripts/install.sh): 3 OS not supported, 4 CPU not supported, 5 privileges — retrying
#   cannot help those. 6 (download/key) and 7 (apt) are usually the network: try again.
INSTALL_URL="https://apt.vibesdr.net/install.sh"
ok=0
for attempt in 1 2 3 4 5; do
  say "installing VibeServer (attempt $attempt of 5)"
  curl -fsSL "$INSTALL_URL" | bash
  rc=$?
  if [ "$rc" = 0 ]; then ok=1; break; fi
  say "the installer stopped with exit code $rc"
  case "$rc" in 3|4|5) break ;; esac
  sleep 30
done
if [ "$ok" != 1 ]; then
  say "VibeServer did NOT install. Once the network is working, run it by hand:"
  say "  curl -fsSL $INSTALL_URL | sudo bash"
  exit 1
fi

# ── 2. The RTL-SDR: take it back from the DVB-T driver now ───────────────────────────────────────
# ★ The package ships /etc/modprobe.d/vibeserver-blacklist-dvb.conf, which only stops the driver
#   LOADING. If the dongle was already plugged in at boot the driver holds it until a reboot, and the
#   setup wizard would report the radio as busy. Removing it now saves that reboot. Harmless when the
#   module is not loaded (or the dongle is not plugged in yet).
if command -v modprobe >/dev/null 2>&1; then
  if modprobe -r dvb_usb_rtl28xxu 2>/dev/null; then say "unloaded the DVB-T driver (dvb_usb_rtl28xxu), if it was loaded"; fi
fi

# ── 3. A reminder at login until setup is done ───────────────────────────────────────────────────
# ★ The wizard writes /etc/vibeserver/config.json; from then on this says nothing. stat (-s) works
#   for any user because /etc/vibeserver is 0755, although the file itself is 0600.
cat > /etc/profile.d/zz-vibeserver-setup-hint.sh <<'HINT'
# Written by the Soft66IP VibeServer image's first boot. Shown at login until VibeServer is set up.
if [ ! -s /etc/vibeserver/config.json ] && command -v vibeserver >/dev/null 2>&1; then
  printf '\n  VibeServer is installed but not set up yet. Type:  vibeserver\n\n'
fi
HINT
chmod 0644 /etc/profile.d/zz-vibeserver-setup-hint.sh

say "done: $(dpkg-query -W -f='vibeserver ${Version} (${Architecture})' vibeserver 2>/dev/null || echo 'vibeserver not found?')"
say "next: log in as root, choose new passwords when DietPi asks, then type: vibeserver"
exit 0
