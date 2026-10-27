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
# ★★★ THE RESOLVER IS A LADDER, NOT THREE PUBLIC SERVERS (2026-10-27, Kiko's TV box: outbound UDP 53 to the
#     internet dropped, so 1.1.1.1:53 timed out and the tunnel never came up). The patch adds vibedns/: the
#     device's own DNS servers first (the app passes them in VIBE_DNS_SERVERS), then public UDP/TCP 53, then
#     certificate-verified DNS-over-TLS. Its tests run on THIS machine before the cross-compile, so a broken
#     ladder never reaches an APK. VIBE_CF_SKIP_NET_TESTS=1 runs them with -short (no 1.1.1.1:853 / 8.8.8.8:853).
#
# Usage:  ./build.sh [output-path]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# ★★ ABSOLUTE, ALWAYS. The build below runs inside the temp clone, so a RELATIVE -o path resolves
#    there and the binary is deleted with the scratch directory — leaving an empty destination and
#    a script that reported success. (Caught 2026-08-22.)
OUT="${1:-$HERE/libcloudflared.so}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
mkdir -p "$(dirname "$OUT")"
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

echo "==> testing the DNS ladder (vibedns) on the host"
# ★ Two CPUs at most, niced: this runs on the development Mac. Every wait in the tests is bounded.
SHORT=""; [ "${VIBE_CF_SKIP_NET_TESTS:-0}" = "1" ] && SHORT="-short"
( cd "$WORK/src" && GOTOOLCHAIN=local GOMAXPROCS=2 nice go test -p 1 -count=1 -timeout 120s $SHORT ./vibedns/ )

ARCH="${VIBE_CF_ARCH:-arm64}"
echo "==> building android/$ARCH"
# ★ THE MAIN APP IS arm64 ONLY. armeabi-v7a is not built for it: the switch simply does not appear
#   there, per AGENTS.md ("a control that only works in one scenario should be removed rather than
#   left dead").
# ★ CGO_ENABLED=0 keeps it a single static binary with no NDK toolchain needed.
# ★★ VIBE_CF_ARCH=arm is VibeServer Lite's (2026-09-18: a 2017 Fire 7 is 32-bit only). Go will not
#    link android/arm internally — it needs the NDK's clang as the external linker, hence cgo.
if [ "$ARCH" = "arm" ]; then
  NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
  CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang"
  [ -x "$CC" ] || { echo "NDK clang not found at $CC — set ANDROID_NDK"; exit 1; }
  ( cd "$WORK/src" && GOTOOLCHAIN=local GOOS=android GOARCH=arm GOARM=7 CGO_ENABLED=1 CC="$CC" \
      go build -trimpath -ldflags "-s -w" -o "$OUT" ./cmd/cloudflared )
else
  ( cd "$WORK/src" && GOTOOLCHAIN=local GOOS=android GOARCH=arm64 CGO_ENABLED=0 \
      go build -trimpath -ldflags "-s -w" -o "$OUT" ./cmd/cloudflared )
fi

echo "==> $(ls -lh "$OUT" | awk '{print $5}')  ->  $OUT"

# ★★ NAMED libcloudflared.so ON PURPOSE. An app's filesDir is NOT executable on modern Android;
#    files packaged as lib*.so in nativeLibraryDir are. It is a Go binary, not a shared library —
#    the name is what makes Android extract it and let us exec it.
# ★★ Needs useLegacyPackaging / extractNativeLibs so it lands on disk as a real file rather than
#    staying compressed inside the APK.
echo
# ★ Where the two builds pick it up (both gitignored): android/app/build.gradle and lite/android/app/build.gradle.
echo "Next: place at android/app/src/main/cpp/cloudflared/arm64-v8a/libcloudflared.so (main app, arm64)"
echo "   or lite/native/cloudflared/armeabi-v7a/libcloudflared.so (VibeServer Lite, VIBE_CF_ARCH=arm)"
