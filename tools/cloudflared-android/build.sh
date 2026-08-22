#!/usr/bin/env bash
# Build cloudflared for Android, patched so it can actually resolve DNS on a phone.
#
# ★★★ WHY THIS EXISTS. Go's pure-Go resolver reads /etc/resolv.conf to find a nameserver. ANDROID
#     DOES NOT SHIP THAT FILE, so a stock cloudflared fails every lookup with
#     "connection refused" and the tunnel never dials (cloudflared issue #425).
#
# ★★★ THE CIRCULATING WORKAROUND HEX-PATCHES THE SHIPPED BINARY — replacing the "/etc/resolv.conf"
#     string with a same-length path fed on stdin. WE DO NOT DO THAT. cloudflared is Apache-2.0 and
#     we have to compile for android/arm64 anyway, so the fix lives in SOURCE where it can be read,
#     reviewed and re-applied to a newer tag.
#
# ★★ Verified on a Moto g35 5G (arm64-v8a), 2026-08-22: a stock build fails exactly as issue #425
#    describes, this build creates a live Quick Tunnel and serves real traffic through it.
#
# Usage:  ./build.sh [output-path]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/libcloudflared.so}"
PINNED="$(cat "$HERE/UPSTREAM-COMMIT")"

command -v go >/dev/null || { echo "go is not installed (brew install go)"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> cloning cloudflared at the pinned commit"
git clone --quiet https://github.com/cloudflare/cloudflared.git "$WORK/src"
git -C "$WORK/src" checkout --quiet "$PINNED"

echo "==> applying android-dns.patch"
# ★ --3way so a moved line in a newer upstream is a conflict to look at, not a silent no-op.
git -C "$WORK/src" apply --3way "$HERE/android-dns.patch"

echo "==> building android/arm64"
# ★ arm64 ONLY. armeabi-v7a is not built: the switch simply does not appear there, per AGENTS.md
#   ("a control that only works in one scenario should be removed rather than left dead").
# ★ CGO_ENABLED=0 keeps it a single static binary with no NDK toolchain needed.
( cd "$WORK/src" && GOOS=android GOARCH=arm64 CGO_ENABLED=0 \
    go build -trimpath -ldflags "-s -w" -o "$OUT" ./cmd/cloudflared )

echo "==> $(ls -lh "$OUT" | awk '{print $5}')  ->  $OUT"

# ★★ NAMED libcloudflared.so ON PURPOSE. An app's filesDir is NOT executable on modern Android;
#    files packaged as lib*.so in nativeLibraryDir are. It is a Go binary, not a shared library —
#    the name is what makes Android extract it and let us exec it.
# ★★ Needs useLegacyPackaging / extractNativeLibs so it lands on disk as a real file rather than
#    staying compressed inside the APK.
echo
echo "Next: place at app/src/main/jniLibs/arm64-v8a/libcloudflared.so"
