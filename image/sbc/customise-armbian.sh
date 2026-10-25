#!/bin/bash
# customise-armbian.sh — the ARMBIAN half of a VibeServer Portable for a non-Raspberry-Pi board (2026-10-25).
#
#   customise-armbian.sh pre     before the Portable's own customise.sh
#   customise-armbian.sh post    after it
#
# Runs INSIDE the target root (a chroot made by image/sbc/build.sh), as root.
#
# ★★★ THE PORTABLE, NOT A NEW PRODUCT (Stuart, 2026-10-25: "Try and keep the ISO as close to Portable as we can").
#     Everything a Portable IS — the marker, the first-boot config, the hotspot/captive portal, VibeServerSetup.local,
#     the converge, the package list, the read-only root, the settings partition — comes from the SAME files the
#     Raspberry Pi image uses: image/pocket/stage-pocket/00-pocket/files/customise.sh and the vibeserver package's
#     vibeserver-pocket*. This script only makes an Armbian root look like the Raspberry Pi OS Lite root those files
#     were written for. Each step says which Pi OS fact it stands in for.
# ★★ ARMBIAN, NOT DIETPI, FOR THIS (see docs/SBC-IMAGES-PLAN.md): vibeserver-pocket is NetworkManager (nmcli) from end
#    to end, and DietPi runs ifupdown + its own network tools; and DietPi's first boot needs the internet, which a box
#    whose only way in is its own setup hotspot does not have. Armbian is the board's own OS, boots offline, and its
#    supported way to run NetworkManager is netplan's NetworkManager renderer — used here.
# ★ Idempotent: running it twice leaves the same root.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
say() { echo "sbc-image: $*"; }
PHASE="${1:-}"
[ -f /etc/armbian-release ] || { say "!! no /etc/armbian-release — this is not an Armbian root"; exit 1; }
# shellcheck disable=SC1091
BOARD=$(sed -n 's/^BOARD=//p' /etc/armbian-release | head -1)

case "$PHASE" in
pre)
  say "Armbian root for $BOARD ($(. /etc/os-release; echo "$PRETTY_NAME"))"
  apt-get update
  # ── 1. NetworkManager — Raspberry Pi OS Lite's network stack since Bookworm; Armbian minimal ships networkd ──────
  # ★★ --no-install-recommends, like everything on a Portable: NM's recommends pull ModemManager, ppp and more.
  #    wpasupplicant is on Armbian minimal already; named so a base without it still gets it. dnsmasq-base (the
  #    hotspot's DHCP) comes with the Portable's package list in customise.sh.
  apt-get install -y --no-install-recommends network-manager wpasupplicant
  # ★★ netplan: Armbian's own file hands every e*/lan*/wan* port to systemd-networkd. The same matches are handed to
  #    NetworkManager instead (Armbian's documented way to switch renderer), so Ethernet comes up by DHCP at boot as
  #    before — the LAN-only onboarding (VibeServerSetup.local:48000) depends on it — and vibeserver-pocket sees one
  #    stack that owns every interface. Wi-Fi is NOT listed: vibeserver-pocket makes its own NM profiles.
  if [ -d /etc/netplan ]; then
    for f in /etc/netplan/*.yaml; do
      [ -f "$f" ] || continue
      case "$f" in */10-vibeserver-networkmanager.yaml) continue ;; esac
      mv -f "$f" "$f.off-vibeserver"
      say "netplan: $(basename "$f") set aside (it gave Ethernet to systemd-networkd)"
    done
    cat > /etc/netplan/10-vibeserver-networkmanager.yaml <<'EOF'
# Written by the VibeServer Portable image (image/sbc): NetworkManager owns every interface, as on Raspberry Pi OS.
# Ethernet ports get DHCP at boot, exactly as Armbian's own file gave them under systemd-networkd.
network:
  version: 2
  renderer: NetworkManager
  ethernets:
    all-eth-interfaces:
      match:
        name: "e*"
      dhcp4: yes
      dhcp6: yes
    all-lan-interfaces:
      match:
        name: "lan[0-9]*"
      dhcp4: yes
      dhcp6: yes
    all-wan-interfaces:
      match:
        name: "wan[0-9]*"
      dhcp4: yes
      dhcp6: yes
EOF
    chmod 0600 /etc/netplan/10-vibeserver-networkmanager.yaml
  fi
  for u in systemd-networkd.service systemd-networkd.socket systemd-networkd-wait-online.service; do
    systemctl disable "$u" 2>/dev/null || true
  done
  systemctl enable NetworkManager.service
  # ── 2. No root resize — Pi OS's " resize" on cmdline.txt, which the Pi customise.sh removes ─────────────────────────
  # ★★★ The settings partition (LABEL=vibedata) sits straight after root, as on the Pi image; Armbian's first boot
  #     would otherwise grow root to the end of the card and over it. Armbian's own switch for that is this file.
  touch /root/.no_rootfs_resize
  # ── 3. SSH OFF — as on the Raspberry Pi Portable (SSH off unless Imager turns it on) ─────────────────────────────────
  # ★★★ Armbian enables sshd with root's documented default password. On a Portable the first network is an OPEN
  #     hotspot: anyone on it could log in first and choose the "new" password Armbian then asks for. So sshd is off,
  #     exactly as on the Pi image. root's default password is LEFT AS ARMBIAN SHIPS IT, still flagged for Armbian's
  #     forced change at the first login (/root/.not_logged_in_yet) — no password of ours is made or stored. The
  #     converge masks the console gettys (HDMI and serial) on a Portable, as on the Pi, so nobody logs in at all
  #     until an owner deliberately re-enables one.
  # ★★★ DISABLED WAS NOT OFF (Nick, G4IRX, real Orange Pi One, 2026-10-25: "I connected to root@vibeserversetup.local,
  #     pw 1234"; the boot tests had printed `ssh active` and 0.0.0.0:22 too — and passed). Nothing ENABLED sshd: Armbian
  #     STARTED it. armbian-firstrun runs `service ssh restart` when OPENSSHD_REGENERATE_HOST_KEYS=true (its default) —
  #     a restart starts a stopped unit whether it is enabled or not — and armbian-firstlogin runs
  #     `systemctl restart ssh.service` at the first console login. And because the seal could reboot under the first
  #     run (vibeserver-pocket-seal now waits for it), its self-disable could land in the RAM overlay, so it ran again
  #     on every boot. Every way sshd can start is now shut:
  #       · /etc/ssh/sshd_not_to_be_run — Debian's own switch: ssh.service and ssh.socket carry
  #         ConditionPathExists=! on it, so ANY start through systemd (first run, first login, a package upgrade, a
  #         hand) is skipped. The owner's "Enable SSH" (vibeserver-pocket SshAccess) removes it, and puts it back on off.
  #       · ssh.service, ssh.socket disabled; ssh.socket and sshd@.service (its per-connection
  #         child, which has no such condition) MASKED: socket activation never runs here.
  #         ssh.service itself is NOT masked: the owner's switch starts it, and a masked unit would make Armbian's first
  #         console login fail its `systemctl restart ssh.service || exit 1` and rerun at every login. The condition
  #         stops that restart without failing it.
  #       · OPENSSHD_REGENERATE_HOST_KEYS=false — the first run neither restarts sshd nor makes keys.
  #       · the image's host keys DELETED: every card was built from the same base, so every card had the SAME keys.
  #         The owner's switch makes the box's own (ssh-keygen -A) the first time SSH goes on, and keeps them.
  #       · systemd's ssh generator masked (AF_UNIX/vsock sshd sockets — local only, but nothing of ours uses them).
  #       · PermitRootLogin no (a drop-in, read before Armbian's own "PermitRootLogin yes"): with the owner's SSH on, the
  #         login is `vibe` with the owner's password — never root with Armbian's documented one.
  #     build.sh checks every one of these; boot-test.sh fails a card on which sshd runs.
  systemctl disable ssh.service ssh.socket 2>/dev/null || true
  rm -f /etc/systemd/system/sshd.service              # ★ the Alias= link `enable` made (gone with disable; belt and braces)
  systemctl mask ssh.socket sshd@.service
  install -d -m 0755 /etc/ssh /etc/ssh/sshd_config.d
  echo "VibeServer Portable: SSH is off until the owner switches it on (the admin page)" > /etc/ssh/sshd_not_to_be_run
  if [ -f /etc/default/armbian-firstrun ] && grep -q '^OPENSSHD_REGENERATE_HOST_KEYS=' /etc/default/armbian-firstrun; then
    sed -i 's/^OPENSSHD_REGENERATE_HOST_KEYS=.*/OPENSSHD_REGENERATE_HOST_KEYS=false/' /etc/default/armbian-firstrun
  else
    echo "OPENSSHD_REGENERATE_HOST_KEYS=false" >> /etc/default/armbian-firstrun
  fi
  rm -f /etc/ssh/ssh_host_*
  install -d -m 0755 /etc/systemd/system-generators
  ln -sfn /dev/null /etc/systemd/system-generators/systemd-ssh-generator
  cat > /etc/ssh/sshd_config.d/00-vibeserver-portable.conf <<'EOF'
# Written by the VibeServer Portable image (image/sbc). sshd keeps the FIRST value it reads, and this file is read
# before Armbian's "PermitRootLogin yes" in sshd_config: root never logs in over SSH. The owner's login is `vibe`.
PermitRootLogin no
EOF
  # ── 4. One log, in RAM — Pi OS Lite (Trixie) has no rsyslog; the converge puts journald in RAM ─────────────────────
  # ★ Armbian minimal runs rsyslog beside journald: the same lines twice, and the second copy is written to /var/log.
  systemctl disable rsyslog.service 2>/dev/null || true
  say "pre: NetworkManager owns the network, root resize off, sshd off (masked/condition/no keys), rsyslog off"
  ;;
post)
  # ── 5. The read-only root's initramfs: Armbian boots uInitrd, the U-Boot wrapper of initrd.img ───────────────────────
  # ★★ The Pi customise.sh ran update-initramfs; on Armbian the kernel package's post-update hook (99-uboot) turns
  #    initrd.img into /boot/uInitrd. Prove overlayroot is IN it — a seal without it would boot straight past it.
  INITRD=$(ls -1t /boot/initrd.img-* 2>/dev/null | head -1 || true)
  [ -n "$INITRD" ] || { say "!! no /boot/initrd.img-* — the read-only root cannot work"; exit 1; }
  N=$(lsinitramfs "$INITRD" 2>/dev/null | grep -c "overlayroot" || true)
  [ "${N:-0}" -ge 1 ] || { say "!! overlayroot is not in $INITRD"; exit 1; }
  [ -f /boot/uInitrd ] || { say "!! no /boot/uInitrd"; exit 1; }
  [ ! "$INITRD" -nt "$(readlink -f /boot/uInitrd)" ] \
    || { say "!! /boot/uInitrd is older than $INITRD — Armbian's 99-uboot hook did not run"; exit 1; }
  say "post: overlayroot is in $(basename "$INITRD") (uInitrd $(du -h "$(readlink -f /boot/uInitrd)" | cut -f1))"
  # ── 6. Nothing of the build left behind ───────────────────────────────────────────────────────────────────────────
  apt-get clean
  rm -rf /var/lib/apt/lists/*
  ;;
*)
  echo "usage: $0 pre|post"; exit 2 ;;
esac
