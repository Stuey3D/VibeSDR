#!/usr/bin/env bash
# image/sbc/run-in-docker.sh — run build.sh in a throwaway privileged Debian container on a Linux Docker host, so
# nothing is installed on the host itself. THE MAC (colima, arm64) from 2026-10-25 — arm64 boards then run natively
# in the chroot, armhf (Pi) under qemu-arm; loop devices on the virtiofs-shared output folder were checked. The
# Lenovo (192.168.86.13) still works, with qemu for both.
#
#   image/sbc/run-in-docker.sh boot-test <finished.img.xz> <output-dir>   boot a finished card under emulation
#   image/sbc/run-in-docker.sh <board> <base.img.xz> <output-dir>     (env passes through: VIBESERVER_DEB COUNTRY NO_MAPS KEEP_WORK)
#
# ★★ CPU: --cpus 2 and nice -n 19 inside (build.sh), xz -T2. ONE build at a time — the script refuses to start while
#    another container from this script is running on the host.
# ★★ THE HOST IS NOT CHANGED. --privileged is needed for loop devices and binfmt_misc; build.sh registers its qemu
#    binfmt entry only if none exists, and removes it at exit. No AppArmor change, no package on the host.
# ★★★ -v /dev:/dev IS THE HOST'S /dev (the VM's, on the Mac). build.sh bind-mounts it into the card's root, and a
#     delete through that bind mount emptied the Lenovo's /dev on 2026-10-25 — see the guard in build.sh's cleanup.
# ★ The repository is mounted read-only; only <output-dir> (and the base image's directory, read-only) is shared.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BOARD="${1:?usage: run-in-docker.sh <board> <base.img.xz> <output-dir>}"
BASE="$(cd "$(dirname "${2:?base image}")" && pwd)/$(basename "$2")"
mkdir -p "${3:?output dir}"; OUT="$(cd "$3" && pwd)"
[ -f "$BASE" ] || { echo "no $BASE"; exit 1; }
if [ -n "$(docker ps -q --filter label=vibesdr-sbc-build)" ]; then
  echo "!! another image/sbc build is running on this host — one at a time"; exit 1
fi
DEBMNT=(); DEBENV=()
if [ -n "${VIBESERVER_DEB:-}" ]; then
  D="$(cd "$(dirname "$VIBESERVER_DEB")" && pwd)"
  DEBMNT=(-v "$D":/deb:ro); DEBENV=(-e "VIBESERVER_DEB=/deb/$(basename "$VIBESERVER_DEB")")
fi
exec docker run --rm --privileged --cpus 2 --label vibesdr-sbc-build -v /dev:/dev \
  -v "$REPO":/repo:ro -v "$(dirname "$BASE")":/base:ro -v "$OUT":/out ${DEBMNT[@]+"${DEBMNT[@]}"} ${DEBENV[@]+"${DEBENV[@]}"} \
  -e COUNTRY="${COUNTRY:-GB}" -e NO_MAPS="${NO_MAPS:-0}" -e KEEP_WORK="${KEEP_WORK:-0}" -e IMAGE_VERSION="${IMAGE_VERSION:-1}" \
  debian:trixie-slim /bin/bash -euo pipefail -c '
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends ca-certificates curl xz-utils fdisk gdisk e2fsprogs dosfstools util-linux gnupg \
        binutils qemu-user-static >/dev/null
    # ★ "boot-test" in place of a board: boot a finished card (the second argument) under emulation — boot-test.sh.
    if [ "'"$BOARD"'" = boot-test ]; then
      apt-get install -y -qq --no-install-recommends systemd-container procps >/dev/null
      exec nice -n 19 /repo/image/sbc/boot-test.sh "/base/'"$(basename "$BASE")"'" /out/boot-test
    fi
    nice -n 19 /repo/image/sbc/build.sh "'"$BOARD"'" "/base/'"$(basename "$BASE")"'" /out
  '
