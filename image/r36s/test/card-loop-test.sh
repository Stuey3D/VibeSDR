#!/bin/bash
# card-loop-test.sh IMAGE.img — the first-boot card logic on a LOOP DEVICE, never a real card: the built image,
# grown to 4 GiB as if written to a bigger card, then `vibe-card boot` twice (first boot: capacity check → p4 →
# mkfs.exfat → folders; second boot: fsck + mount only), and a dirty-volume fsck. Needs root, losetup, exfat in the kernel.
# Run in a privileged container: see image/r36s/README.md "Tests".
# ★ With no IMAGE (e.g. on the R36S, inside the image root): a synthetic 4 GiB file with the image's three partitions.
set -euo pipefail
IMG=${1:-}
VC=${VC:-/repo/image/r36s/rootfs/usr/lib/vibesdr-r36s/vibe-card}
[ -f "$VC" ] || VC=/usr/lib/vibesdr-r36s/vibe-card
RM=/repo/image/r36s/rootfs/usr/share/vibesdr-r36s/VIBESDR-README.txt
[ -f "$RM" ] || RM=/usr/share/vibesdr-r36s/VIBESDR-README.txt
T=$(mktemp -d -p "${TMPBASE:-/tmp}")
if [ -n "$IMG" ]; then
  cp --sparse=always "$IMG" "$T/card.img"
else
  truncate -s 4G "$T/card.img"
  printf 'label: dos\nstart=32768, size=262144, type=c, bootable\nstart=294912, size=2752512, type=83\nstart=3047424, size=524288, type=83\n' \
    | sfdisk -q "$T/card.img"
fi
truncate -s 4G "$T/card.img"
L=$(losetup -f --show -P "$T/card.img")
trap 'umount "$T/mnt" 2>/dev/null; losetup -d "$L"; rm -rf "$T"' EXIT
mkdir -p "$T/state" "$T/mnt"
id vibeserver >/dev/null 2>&1 || useradd --system vibeserver
export VIBE_CARD_DISK=$L VIBE_STATE_DIR=$T/state VIBE_CARD_MOUNT=$T/mnt \
       VIBE_CARD_README=$RM
echo "== first boot"
time python3 -I "$VC" boot
sfdisk -d "$L" | grep ' : start='
echo "kernel size $(( $(cat /sys/class/block/$(basename "$L")/size) * 512 )) bytes; p4 ends at $(( ($(sfdisk -d "$L" | sed -n 's/.*4 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1+\2/p')) * 512 )) bytes"
blkid "${L}p4"
findmnt -n -o SOURCE,FSTYPE,OPTIONS "$T/mnt"
ls -la "$T/mnt"
head -3 "$T/mnt/README.txt"
cat "$T/state/card-capacity.json" | head -12
echo "== second boot (fsck + mount; nothing re-made)"
echo hello > "$T/mnt/Recordings/test.txt"; umount "$T/mnt"
python3 -I "$VC" boot
cat "$T/mnt/Recordings/test.txt"
echo "== a dirty volume: mounted, written, the 'power' cut (no umount, flag left dirty), then a boot"
umount "$T/mnt"
# set the exFAT VolumeDirty flag (VolumeFlags bit 1, offset 106 of the boot sector) as a power cut would leave it
printf '\x02' | dd of="${L}p4" bs=1 seek=106 conv=notrunc status=none
python3 -I "$VC" boot
echo "== mount -o flush (vfat's option) on exfat:"
umount "$T/mnt"; mount -t exfat -o flush "${L}p4" "$T/mnt" 2>&1 && echo "flush accepted" || echo "flush REJECTED by the exfat driver"
echo "== card-loop-test: PASS"
