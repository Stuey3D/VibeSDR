#!/usr/bin/env bash
# Fetch the official cloudflared binaries we SHIP, one per platform we package for.
#
# ★★★ WE BUNDLE IT, WE DO NOT ASK FOR IT. "Needs cloudflared installed" is a fine sentence for a
#     developer and a dead end for the owner of a Raspberry Pi who wants their receiver listed:
#     it is not in Debian's repositories, so the instruction is really "go and find a .deb from
#     Cloudflare". A one-switch feature cannot have a manual prerequisite (Stuart, 2026-08-23:
#     "I thought we were bundling cloudflare").
#
# ★★★ AND UNLIKE ANDROID, NO PATCH IS NEEDED HERE. tools/cloudflared-android/build.sh compiles
#     from source because Go's resolver reads /etc/resolv.conf and Android does not ship one. Linux
#     and macOS do, so the OFFICIAL RELEASE BINARY works untouched — and taking Cloudflare's own
#     build means we are not shipping a toolchain's worth of difference from what they test.
#
# ★★ Apache-2.0, which is why any of this is allowed: redistribution is fine WITH THE LICENCE
#    ALONGSIDE, so the licence text is fetched with the binary and packaged next to it. A bundled
#    binary without its licence is the one way to turn a permitted act into an infringing one.
#
# ★ Pinned to a version rather than "latest": a package that builds a different dependency each
#   time it is built is not reproducible, and "it worked last week" stops being a useful sentence.
set -euo pipefail

VERSION="${CLOUDFLARED_VERSION:-2026.8.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/bin}"
BASE="https://github.com/cloudflare/cloudflared/releases/download/${VERSION}"

mkdir -p "$OUT"

fetch() {                       # fetch <asset> <dest-name>
  local asset="$1" dest="$2"
  if [ -s "$OUT/$dest" ]; then echo "==> have $dest"; return; fi
  echo "==> fetching $asset"
  # ★ -f so a 404 is a FAILURE, not a zero-byte file that packages perfectly and cannot run.
  curl -fsSL --retry 3 "$BASE/$asset" -o "$OUT/$dest.tmp"
  chmod +x "$OUT/$dest.tmp"
  mv "$OUT/$dest.tmp" "$OUT/$dest"
}

fetch cloudflared-linux-arm64  cloudflared-linux-arm64
fetch cloudflared-linux-amd64  cloudflared-linux-amd64
fetch cloudflared-darwin-arm64.tgz cloudflared-darwin-arm64.tgz
if [ ! -s "$OUT/cloudflared-darwin-arm64" ]; then
  tar -xzf "$OUT/cloudflared-darwin-arm64.tgz" -C "$OUT"
  mv "$OUT/cloudflared" "$OUT/cloudflared-darwin-arm64"
  chmod +x "$OUT/cloudflared-darwin-arm64"
fi

if [ ! -s "$OUT/LICENSE" ]; then
  echo "==> fetching the licence"
  curl -fsSL --retry 3 \
    "https://raw.githubusercontent.com/cloudflare/cloudflared/${VERSION}/LICENSE" \
    -o "$OUT/LICENSE"
fi

echo "==> cloudflared ${VERSION} ready in $OUT"
ls -lh "$OUT" | sed 's/^/    /'
