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
#     does, so the OFFICIAL RELEASE BINARY works untouched (macOS: see the note at the bottom —
#     the Mac now compiles its own, for a reason that has nothing to do with DNS) — and taking Cloudflare's own
#     build means we are not shipping a toolchain's worth of difference from what they test.
#
# ★★ Apache-2.0, which is why any of this is allowed: redistribution is fine WITH THE LICENCE
#    ALONGSIDE, so the licence text is fetched with the binary and packaged next to it. A bundled
#    binary without its licence is the one way to turn a permitted act into an infringing one.
#
# ★ Pinned to a version rather than "latest": a package that builds a different dependency each
#   time it is built is not reproducible, and "it worked last week" stops being a useful sentence.
set -euo pipefail

VERSION="${CLOUDFLARED_VERSION:-2026.10.4}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/bin}"
BASE="https://github.com/cloudflare/cloudflared/releases/download/${VERSION}"

# ── PINNED SHA-256, CHECKED ON EVERY RUN (2026-10-03 security pass) ──────────────────────────
# ★★★ THESE BINARIES SHIP INSIDE OUR .deb AND RUN ON EVERY OWNER'S MACHINE. Fetched over HTTPS from
#     GitHub, but nothing checked WHAT arrived — and a file already in bin/ was reused unexamined
#     for ever, so one bad download (or one swapped file on the build machine) would be packaged
#     into every release after it. Now every file — freshly downloaded OR reused — must match the
#     pin below or the script FAILS, and a fresh download that fails is deleted, never moved into
#     place.
# ★★ THE PINS ARE FOR ${PINNED_VERSION} ONLY. Bumping VERSION without new pins is refused, not
#    waved through. To bump: download the new release's files, check them against the checksums
#    Cloudflare publishes on the GitHub release page, and replace every line here in one commit.
#    Hashes below are the SHA-256 Cloudflare lists on the 2026.10.4 release page (2026-10-26).
# ★★★ 2026.10.4, NOT 2026.8.0 (2026-10-26): Cloudflare's notice to every tunnel account — eight
#     eMudhra (emSign) intermediates have weak private keys, and cloudflared before 2026.10.2 still
#     trusts them. The fix is the Go 1.27.3 toolchain it is built with. Our tunnels all run under
#     Stuart's account, so every owner's bundled copy is in scope.
PINNED_VERSION="2026.10.4"
# ★ A case, not `declare -A`: macOS still ships bash 3.2, which has no associative arrays, and
#   this script runs on the build Mac.
pin_for() {
  case "$1" in
    cloudflared-linux-arm64) echo 2b7203b4fb0ba28a0c1e12f8855ce2c25f76d266a8d5edafac5540b6812acff6 ;;
    cloudflared-linux-arm)   echo f29b44e97ec18dd2971dfdd9bad0c6d460aaa78223b91b186219f02135c08d8c ;;
    cloudflared-linux-amd64) echo ae5e8add7884b0531d66bbca4570d1de491ff6da51e4ebe7eb160cdf241d9e42 ;;
    # ★ 386 added 2026-10-17 for the i386 package — checked against the SHA-256 Cloudflare lists on the
    #   release page. Go's 386 port needs SSE2,
    #   the same floor as our own i386 build, so the tunnel reaches every machine the server does.
    cloudflared-linux-386)   echo a759230628a894c9262cfc0535926ba1e0cc54260e0b35a6e6cf3833919c67f2 ;;
    LICENSE)                 echo 58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd ;;
    *)                       echo "" ;;
  esac
}

if [ "$VERSION" != "$PINNED_VERSION" ]; then
  echo "!! cloudflared $VERSION requested, but the SHA-256 pins in $0 are for $PINNED_VERSION." >&2
  echo "!! Update PINNED_VERSION and every PIN line for $VERSION first — refusing to fetch unverified binaries." >&2
  exit 1
fi

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

verify() {                      # verify <file> <pin-name> — exits the script on a mismatch
  local file="$1" name="$2" want got
  want="$(pin_for "$name")"
  if [ -z "$want" ]; then echo "!! no SHA-256 pin for $name — refusing" >&2; exit 1; fi
  got="$(sha256_of "$file")"
  if [ "$got" != "$want" ]; then
    echo "!! SHA-256 MISMATCH for $name" >&2
    echo "!!   expected $want" >&2
    echo "!!   got      $got" >&2
    return 1
  fi
}

mkdir -p "$OUT"

fetch() {                       # fetch <asset> <dest-name>
  local asset="$1" dest="$2"
  if [ -s "$OUT/$dest" ]; then
    # ★ Reused files are verified too — a cache that is trusted for ever is the hole this closes.
    verify "$OUT/$dest" "$dest" || { echo "!! $OUT/$dest does not match its pin — delete it and re-run" >&2; exit 1; }
    echo "==> have $dest (sha256 ok)"
    return
  fi
  echo "==> fetching $asset"
  rm -f "$OUT/$dest.tmp"
  # ★ -f so a 404 is a FAILURE, not a zero-byte file that packages perfectly and cannot run.
  curl -fsSL --proto '=https' --tlsv1.2 --retry 3 "$BASE/$asset" -o "$OUT/$dest.tmp"
  # ★★ Verified BEFORE it takes the real name, so a bad download can never be picked up later as
  #    "have $dest".
  if ! verify "$OUT/$dest.tmp" "$dest"; then rm -f "$OUT/$dest.tmp"; exit 1; fi
  chmod +x "$OUT/$dest.tmp"
  mv "$OUT/$dest.tmp" "$OUT/$dest"
}

fetch cloudflared-linux-arm64  cloudflared-linux-arm64
fetch cloudflared-linux-arm    cloudflared-linux-arm      # ★ 32-bit ARM — VibeServer Lite (Pi 2 etc.)
fetch cloudflared-linux-amd64  cloudflared-linux-amd64
fetch cloudflared-linux-386    cloudflared-linux-386      # ★ 32-bit x86 — the i386 package (Atom N270, Pentium M)
# ★★★ NO darwin BINARY ANY MORE (2026-09-28). Cloudflare's cloudflared-darwin-arm64 is built for
#     macOS 15.0 (LC_BUILD_VERSION minos 15.0), and VibeServer.app promises macOS 14 — so on a 14
#     Mac the tunnel switch started nothing. vibeserver/mac/build-deps.sh COMPILES the same pinned
#     tag instead (CGO off, Go's own floor), and build-app.sh bundles only that one. Fetching the
#     release binary here would only leave the wrong one lying around to be picked up again.

if [ -s "$OUT/LICENSE" ]; then
  verify "$OUT/LICENSE" LICENSE || { echo "!! $OUT/LICENSE does not match its pin — delete it and re-run" >&2; exit 1; }
else
  echo "==> fetching the licence"
  rm -f "$OUT/LICENSE.tmp"
  curl -fsSL --proto '=https' --tlsv1.2 --retry 3 \
    "https://raw.githubusercontent.com/cloudflare/cloudflared/${VERSION}/LICENSE" \
    -o "$OUT/LICENSE.tmp"
  if ! verify "$OUT/LICENSE.tmp" LICENSE; then rm -f "$OUT/LICENSE.tmp"; exit 1; fi
  mv "$OUT/LICENSE.tmp" "$OUT/LICENSE"
fi

echo "==> cloudflared ${VERSION} ready in $OUT"
ls -lh "$OUT" | sed 's/^/    /'
