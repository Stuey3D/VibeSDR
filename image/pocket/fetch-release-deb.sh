#!/bin/bash
# fetch-release-deb.sh — put a PUBLISHED vibeserver package into image/pocket/out/debs, for a release image (2026-10-25).
#
#   image/pocket/fetch-release-deb.sh arm64|armhf [<version>]     # default: the newest on apt.vibesdr.net stable
#
# ★★★ WHY. build-deb.sh makes a package from THIS checkout (rev 900) — right while the pocket code was unreleased.
#     Every release since 11.0.0~rc38 carries it, so a Portable RELEASE image should carry the exact package an
#     ordinary apt server gets (Stuart, 2026-10-14: "Includes VibeServer 11.0.1 (the same build as apt 11.0.1-1)").
#     And only the published armhf package carries the ARMv6 build a Pi Zero / Pi 1 needs: build-deb.sh leaves it out.
# ★★ VERIFIED BY APT: the download happens in a throwaway Debian container that trusts only apt.vibesdr.net's key and
#    fetches through the signed index, so a package whose hash is not in the signed Packages file is refused.
# ★ Nothing is signed, published or pushed. One small container, 1 CPU.
set -euo pipefail
A="${1:-}"; VER="${2:-}"
case "$A" in arm64|armhf) ;; *) echo "usage: $0 arm64|armhf [<version>]"; exit 2 ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out/debs"; mkdir -p "$OUT"
docker run --rm --cpus 1 -v "$OUT":/out -e A="$A" -e VER="$VER" debian:trixie-slim /bin/bash -euo pipefail -c '
  apt-get update -qq && apt-get install -y -qq --no-install-recommends ca-certificates curl gnupg >/dev/null
  dpkg --add-architecture "$A"
  curl -fsSL https://apt.vibesdr.net/KEY.gpg | gpg --dearmor -o /usr/share/keyrings/vibesdr.gpg
  echo "deb [arch=$A signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main" > /etc/apt/sources.list.d/vibesdr.list
  # ★ Only our repository is needed for a download; Debian'"'"'s own indexes for the foreign arch are not.
  apt-get update -qq -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/vibesdr.list -o Dir::Etc::sourceparts=-
  [ -n "$VER" ] || VER=$(apt-cache madison "vibeserver:$A" | awk "NR==1{print \$3}")
  cd /out && apt-get download -q "vibeserver:$A=$VER"
  ls -la /out/vibeserver_*_"$A".deb
'
# ★ build-image.sh takes the NEWEST vibeserver_*_<arch>.deb here; say which one that will be.
echo "==> out/debs now holds: $(cd "$OUT" && ls -1 vibeserver_*_"$A".deb | sort -V | tr '\n' ' ')"
echo "    (customise.sh installs the newest by version — remove older ones, e.g. a rev-900 build, if it is not this one)"
