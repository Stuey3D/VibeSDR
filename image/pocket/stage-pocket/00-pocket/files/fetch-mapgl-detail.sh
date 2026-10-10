#!/bin/bash
# fetch-mapgl-detail.sh <cache-dir> <url> <bytes> — fetch the High Detail Maps for the pocket image and
# verify them EXACTLY as the server's own installer does (vibe_mapgl.h verifyPart): curl -f, the exact
# byte count, and the 7-byte "PMTiles" magic. Prints the verified file's path.
# ★ URL and size come from vibe_mapgl.h (build-image.sh reads them), so the image can never bake a map
#   set the server would refuse — or one it would then try to download again.
# ★ Cached: the second image build reuses the file instead of fetching 177 MB again.
set -euo pipefail
CACHE="$1"; URL="$2"; BYTES="$3"
OUT="$CACHE/vibemap-detail.pmtiles"
ok() {
  [ -f "$1" ] || return 1
  [ "$(stat -c %s "$1" 2>/dev/null || stat -f %z "$1")" = "$BYTES" ] || return 1
  [ "$(head -c 7 "$1")" = "PMTiles" ] || return 1
}
if ! ok "$OUT"; then
  mkdir -p "$CACHE"
  curl -fsSL --retry 3 --connect-timeout 30 --speed-limit 1024 --speed-time 120 -o "$OUT.part" "$URL" >&2
  ok "$OUT.part" || { rm -f "$OUT.part"; echo "!! the High Detail Maps did not verify (size or PMTiles magic)" >&2; exit 1; }
  mv -f "$OUT.part" "$OUT"
fi
echo "$OUT"
