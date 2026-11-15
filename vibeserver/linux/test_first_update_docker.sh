#!/bin/bash
# test_first_update_docker.sh — run test_first_update_inner.sh (a new card's one update check, real apt + dpkg) in a
# throwaway Debian bookworm container with NO network. Needs docker (colima on the Mac). Silent; ~2 minutes.
#   vibeserver/linux/test_first_update_docker.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMG=${IMG:-vibeserver-build:bookworm-arm64}   # bookworm + python3, dpkg-dev, apt-utils (apt-ftparchive), procps
docker image inspect "$IMG" >/dev/null 2>&1 || IMG=debian:bookworm
exec docker run --rm --network none --cpus 2 -v "$REPO/vibeserver/linux:/src/vibeserver/linux:ro" "$IMG" \
  nice -n 10 bash /src/vibeserver/linux/test_first_update_inner.sh
