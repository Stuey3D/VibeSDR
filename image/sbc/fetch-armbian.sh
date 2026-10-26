#!/usr/bin/env bash
# image/sbc/fetch-armbian.sh <board> <dir> — download a board's CURRENT Armbian Trixie minimal image with its .sha and
# .asc, named as Armbian names it, into <dir>. build.sh then checks the checksum and Armbian's signature.
#
# ★ Through dl.armbian.com's own redirector for all three files (…/Trixie_current_minimal, .sha, .asc): a community
#   (trunk) image lives on a signed, expiring GitHub asset URL, to which ".asc" cannot simply be appended.
# ★ The file name is the one the .sha names, so `sha256sum -c` works as Armbian intends. Nothing is trusted here —
#   verification is build.sh's first step — but a .sha that names a different board is refused at once.
set -euo pipefail
B="${1:?usage: fetch-armbian.sh <board> <dir>}"; D="${2:?dir}"
mkdir -p "$D"; cd "$D"
U="https://dl.armbian.com/$B/Trixie_current_minimal"
curl -fsSL --retry 3 -o "$B.sha.tmp" "$U.sha"
NAME="$(awk '{print $2}' "$B.sha.tmp" | sed 's/^\*//' | head -1)"
case "$NAME" in
  Armbian*_trixie_*_minimal.img.xz) ;;
  *) rm -f "$B.sha.tmp"; echo "!! $U.sha names '$NAME', not an Armbian Trixie minimal image"; exit 1 ;;
esac
# ★ tr, not ${NAME,,}: the Mac's /bin/bash is 3.2 (the builds moved there 2026-10-25).
lc() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
case "$(lc "$NAME")" in *"_$(lc "$B")_"*) true ;; *) false ;; esac || { rm -f "$B.sha.tmp"; echo "!! $U.sha names '$NAME', which is not for $B"; exit 1; }
mv -f "$B.sha.tmp" "$NAME.sha"
curl -fsSL --retry 3 -o "$NAME.asc" "$U.asc"
[ -s "$NAME" ] && sha256sum -c "$NAME.sha" >/dev/null 2>&1 && { echo "$D/$NAME (already here, checksum ok)"; exit 0; }
curl -fL --retry 3 -o "$NAME.part" "$U"
mv -f "$NAME.part" "$NAME"
echo "$D/$NAME"
