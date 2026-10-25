#!/usr/bin/env bash
# image/sbc/boot-test.sh — boot a FINISHED card image as an emulated container and check the Portable comes up
# (2026-10-25). Emulation only: no U-Boot, no kernel of the board, no real Ethernet or Wi-Fi — it proves the root's
# systemd boot, our units, the setup front door and the setup name, nothing about the hardware.
#
#   sudo image/sbc/boot-test.sh <vibeserver-portable-<board>-<date>.img.xz> <work-dir>
#   (or: run-in-docker.sh boot-test <img.xz> <out>). Board cards and Raspberry Pi cards alike.
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
# ★ Any card of ours (2026-10-25): a board card (root p1, vibedata p2, VIBESETUP p3) or a Raspberry Pi card (FAT boot p1,
#   root p2, vibedata p3). Root = the ext4 partition holding /etc/os-release; settings = LABEL=vibedata.
mkdir -p "$R"
while read -r N S Z; do
  L="$(losetup -f --show -o $((S*512)) --sizelimit $((Z*512)) "$IMG")"
  T="$(blkid -p -o value -s TYPE "$L" 2>/dev/null || true)"; LB="$(blkid -p -o value -s LABEL "$L" 2>/dev/null || true)"
  if [ "$LB" = vibedata ] && [ -z "$LOOP2" ]; then LOOP2="$L"; continue; fi
  if [ "$T" = ext4 ] && [ -z "$LOOP1" ] && mount -o ro "$L" "$R" 2>/dev/null; then
    if [ -f "$R/etc/os-release" ]; then umount "$R"; LOOP1="$L"; continue; fi
    umount "$R"
  fi
  losetup -d "$L"
# ★ [^ ]* then ONE OR MORE digits: with `.*[^0-9]\([0-9]*\)` the greedy .* left the number EMPTY (a Pi card's first run,
#   2026-10-25: "could not find the root and vibedata partitions").
done < <(sfdisk -d "$IMG" | sed -n 's/^[^ ]*[^0-9]\([0-9][0-9]*\) *: *start= *\([0-9]*\), *size= *\([0-9]*\).*/\1 \2 \3/p')
[ -n "$LOOP1" ] && [ -n "$LOOP2" ] || { echo "!! could not find the root and vibedata partitions"; exit 1; }
mount "$LOOP1" "$R"
case "$(readelf -h "$R/usr/bin/true" 2>/dev/null | sed -n 's/^ *Machine: *//p')" in
  *AArch64*) ARCH=arm64 ;;
  *ARM*)     ARCH=arm ;;
  *) echo "!! unknown architecture for $R/usr/bin/true"; exit 1 ;;
esac
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
# ★★★ THE FIRST BOOT SEALS AND REBOOTS (11.0.11 on a board card: vibeserver-pocket-seal writes extraargs=overlayroot=tmpfs
#     into /boot/armbianEnv.txt and reboots — and a reboot ENDS an nspawn container, so the first 11.0.11 board tests
#     never reached the front door, 2026-10-25). So: boot once and let the seal run; prove it wrote the switch; then mark
#     this COPY no-seal (nspawn cannot boot the kernel argument anyway) and boot again for everything else.
SEAL="not applicable (no armbianEnv.txt — a Raspberry Pi card: its seal writes cmdline.txt on the boot partition, not mounted here)"
if [ -f "$R/boot/armbianEnv.txt" ]; then
  say "first boot: the read-only seal (it reboots the box when it has written the switch)"
  timeout 900 systemd-nspawn -q --register=no --keep-unit --private-network -bD "$R" --bind="$LOOP2" \
    > "$WD/boot-test-seal.log" 2>&1 || true
  if grep -qE '^extraargs=(.*[[:space:]])?overlayroot=tmpfs([[:space:]]|$)' "$R/boot/armbianEnv.txt"; then
    SEAL="ok — first boot wrote extraargs=overlayroot=tmpfs into /boot/armbianEnv.txt and rebooted"
  else
    SEAL="FAILED — no overlayroot=tmpfs in /boot/armbianEnv.txt after the first boot"
  fi
  say "seal: $SEAL"
  touch "$R/etc/vibeserver-pocket/no-seal"
fi
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
  echo "read-only seal: $SEAL"
  echo "vibeserver package: $(run dpkg-query -W -f '${Version} ${Architecture}' vibeserver)"
  for u in vibeserver vibeserver-pocket vibeserver-pocket-data vibeserver-pocket-seal vibeserver-pocket-cpu NetworkManager avahi-daemon systemd-networkd ssh; do
    printf '%-24s %s\n' "$u" "$(run systemctl is-active "$u.service")"
  done
  echo "--- vibeserver-pocket (journal)"; run journalctl -b -u vibeserver-pocket --no-pager -n 15
  echo "--- vibeserver-pocket-seal"; run journalctl -b -u vibeserver-pocket-seal --no-pager -n 5
  echo "--- vibeserver-pocket-data"; run journalctl -b -u vibeserver-pocket-data --no-pager -n 8
  echo "--- vibeserver-pocket-cpu"; run journalctl -b -u vibeserver-pocket-cpu --no-pager -n 5
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
case "$SEAL" in FAILED*) echo "!! the read-only seal did not write its switch"; exit 1 ;; esac
say "boot test passed (emulation only)"
