#!/bin/bash -e
# pi-gen substage: make Raspberry Pi OS Lite (stage2) a pocket VibeServer.
# ★ Runs on the BUILD HOST with ROOTFS_DIR set; everything that happens inside the image is in
#   files/customise.sh — shared, unchanged, with build-image.sh's official-image route.
install -d "${ROOTFS_DIR}/tmp/pocket-files" "${ROOTFS_DIR}/tmp/pocket-debs"
install -m 0755 files/customise.sh "${ROOTFS_DIR}/tmp/pocket-files/"
install -m 0644 files/config.json files/pocket.conf "${ROOTFS_DIR}/tmp/pocket-files/"
[ -f files/vibesdr.gpg ] && install -m 0644 files/vibesdr.gpg "${ROOTFS_DIR}/tmp/pocket-files/"
# ★ Local packages (build-deb.sh) — the pocket code ships in them until a release carries it.
for d in files/debs/*.deb; do
	[ -e "$d" ] && install -m 0644 "$d" "${ROOTFS_DIR}/tmp/pocket-debs/"
done
# ★ The High Detail Maps (build-image.sh --pigen fetched and verified them into files/).
if [ -f files/vibemap-detail.pmtiles ]; then
	install -d -m 0755 "${ROOTFS_DIR}/usr/lib/vibeserver/mapgl-detail"
	install -m 0644 files/vibemap-detail.pmtiles "${ROOTFS_DIR}/usr/lib/vibeserver/mapgl-detail/"
fi
on_chroot << EOF
POCKET_COUNTRY="${WPA_COUNTRY:-GB}" POCKET_HOSTNAME="${TARGET_HOSTNAME:-vibepocket}" bash /tmp/pocket-files/customise.sh
EOF
