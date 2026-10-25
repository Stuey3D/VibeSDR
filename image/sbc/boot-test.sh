#!/usr/bin/env bash
# image/sbc/boot-test.sh — boot a FINISHED card image as an emulated container and check the Portable comes up
# (2026-10-25). Emulation only: no U-Boot, no kernel of the board, no real Ethernet or Wi-Fi — it proves the root's
# systemd boot, our units, the setup front door and the setup name, nothing about the hardware.
#
#   sudo image/sbc/boot-test.sh <vibeserver-portable-<board>-<date>.img.xz> <work-dir>
#   (or through run-in-docker.sh's container: BOOT_TEST=1)
#
# What it does: decompresses a THROWAWAY copy, attaches root + vibedata, boots root with systemd-nspawn (QEMU user
# mode; binfmt flags O+F — systemd 257 starts services by file descriptor) and a private network (only lo), waits for
# multi-user.target, then reads unit states, the hostname, and the front door on 127.0.0.1:48000. The copy is deleted.
set -euo pipefail
XZ="${1:?usage: boot-test.sh <image.img.xz> <work-dir>}"
WD="${2:?work dir}"; mkdir -p "$WD"; WD="$(cd "$WD" && pwd)"
IMG="$WD/boot-test.img"; R="$WD/boot-root"; LOOP1=""; LOOP2=""; BINFMT=""
say() { printf '==> %s\n' "$*"; }
cleanup() {
  set +e
  mountpoint -q "$R" && umount "$R"
  [ -n "$LOOP1" ] && losetup -d "$LOOP1"; [ -n "$LOOP2" ] && losetup -d "$LOOP2"
  [ -n "$BINFMT" ] && [ -e "/proc/sys/fs/binfmt_misc/$BINFMT" ] && echo -1 > "/proc/sys/fs/binfmt_misc/$BINFMT"
  rm -f "$IMG"; rmdir "$R" 2>/dev/null
}
trap cleanup EXIT
nice -n 19 xz -dc -T2 "$XZ" > "$IMG"
read -r S1 Z1 <<< "$(sfdisk -d "$IMG" | sed -n 's/.*[^0-9]1 *: *start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2/p')"
read -r S2 Z2 <<< "$(sfdisk -d "$IMG" | sed -n 's/.*[^0-9]2 *: *start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2/p')"
LOOP1="$(losetup -f --show -o $((S1*512)) --sizelimit $((Z1*512)) "$IMG")"
LOOP2="$(losetup -f --show -o $((S2*512)) --sizelimit $((Z2*512)) "$IMG")"
mkdir -p "$R"; mount "$LOOP1" "$R"
ARCH="$(sed -n 's/^ARCH=//p' "$R/etc/armbian-release")"
# ★★ THIS COPY ONLY (it is deleted afterwards): systemd's per-service sandbox cannot be set up in a container nested in
#    Docker under QEMU — the first run: "Failed to set up mount namespacing: Invalid argument" for NetworkManager and
#    vibeserver, and journald failed on its credentials (qemu#1962, systemd#31219, as image/soft66ip found). A drop-in for
#    EVERY service switches the sandbox off here so the units themselves can be seen running. The card is unchanged.
mkdir -p "$R/etc/systemd/system/service.d"
cat > "$R/etc/systemd/system/service.d/zz-boot-test-no-sandbox.conf" <<'EOF'
[Service]
ImportCredential=
LoadCredential=
ProtectSystem=no
ProtectHome=no
PrivateTmp=no
PrivateDevices=no
PrivateMounts=no
ProtectKernelTunables=no
ProtectKernelModules=no
ProtectKernelLogs=no
ProtectControlGroups=no
ProtectClock=no
ProtectHostname=no
ProtectProc=default
RestrictNamespaces=no
SystemCallFilter=
ReadWritePaths=
ReadOnlyPaths=
InaccessiblePaths=
EOF
if ! chroot "$R" /bin/true 2>/dev/null; then
  case "$ARCH" in
    arm)   Q=/usr/bin/qemu-arm-static; M='\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00' ;;
    arm64) Q=/usr/bin/qemu-aarch64-static; M='\x7f\x45\x4c\x46\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00' ;;
  esac
  K='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
  mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
  BINFMT=vibe-boot-test; printf '%s' ":$BINFMT:M::$M:$K:$Q:OCF" > /proc/sys/fs/binfmt_misc/register
fi
# ★ The settings partition by label, as on the card: udev in the container cannot see it, so its fstab-less mount is
#   done by vibeserver-pocket-data through blkid — the loop device is bound in for that.
say "booting the card's root (emulated $ARCH; a few minutes)"
LOG="$WD/boot-test.log"
timeout 900 systemd-nspawn -q --register=no --keep-unit --private-network -bD "$R" \
  --bind="$LOOP2" \
  > "$LOG" 2>&1 &
NS=$!
# Wait for the front door, from inside the container's network namespace.
LEADER=""
for _ in $(seq 1 120); do
  sleep 5
  # ★ The container's PID 1 is the descendant of nspawn whose network namespace is not ours (under QEMU its name
  #   may be qemu-arm-static, not systemd, so it is found by namespace, not by name).
  LEADER=""
  for p in $(pgrep -P "$NS" || true) $(for c in $(pgrep -P "$NS" || true); do pgrep -P "$c" || true; done); do
    [ "$(readlink "/proc/$p/ns/net" 2>/dev/null)" != "$(readlink /proc/self/ns/net)" ] && { LEADER=$p; break; }
  done
  [ -n "$LEADER" ] || continue
  if nsenter -t "$LEADER" -n curl -fsS -o /dev/null -m 5 http://127.0.0.1:48000/ 2>/dev/null; then break; fi
done
[ -n "$LEADER" ] || { tail -40 "$LOG"; echo "!! the container never started"; exit 1; }
CODE="$(nsenter -t "$LEADER" -n curl -s -o /dev/null -w '%{http_code}' -m 10 http://127.0.0.1:48000/ || true)"
run() { nsenter -t "$LEADER" -a "$@" 2>&1 || true; }
{
  echo "front door http://127.0.0.1:48000/ → HTTP $CODE"
  echo "hostname: $(run cat /etc/hostname)"
  for u in vibeserver vibeserver-pocket vibeserver-pocket-data vibeserver-pocket-seal NetworkManager avahi-daemon systemd-networkd ssh; do
    printf '%-24s %s\n' "$u" "$(run systemctl is-active "$u.service")"
  done
  echo "--- vibeserver-pocket (journal)"; run journalctl -b -u vibeserver-pocket --no-pager -n 15
  echo "--- vibeserver-pocket-seal"; run journalctl -b -u vibeserver-pocket-seal --no-pager -n 5
  echo "--- vibeserver-pocket-data"; run journalctl -b -u vibeserver-pocket-data --no-pager -n 8
  echo "--- ssh: units and TCP listeners (port 22 must NOT be listening)"
  run systemctl list-units --all --no-legend 'ssh*' 'sshd*'
  run ss -ltnp
  echo "--- vibeserver (journal)"; run journalctl -b -u vibeserver --no-pager -n 15
  echo "--- failed units"; run systemctl --failed --no-legend
  echo "--- memory"; run free -m
} | tee "$WD/boot-test-result.txt"
run systemctl poweroff >/dev/null
wait "$NS" 2>/dev/null || true
[ "$CODE" = 200 ] || [ "$CODE" = 302 ] || { echo "!! the front door did not answer"; exit 1; }
say "boot test passed (emulation only)"
