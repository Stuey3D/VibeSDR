#!/bin/bash
# Build "VibeSDR Flasher.app" (universal), sign it (Developer ID, hardened runtime), notarise it, staple it, zip it.
#   tools/r36s-flasher/build-mac.sh [out-dir]     (default ~/Desktop/VibeSDR-Flasher)
# Notarisation uses the App Store Connect API key in ~/.appstoreconnect/config (KEY_ID, ISSUER) — as VibeIQ's build does.
set -euo pipefail
cd "$(dirname "$0")"
OUT=${1:-$HOME/Desktop/VibeSDR-Flasher}
WAILS=${WAILS:-$HOME/go/bin/wails}
echo "==> Tests"
go test ./... -timeout 30m
echo "==> Build (universal)"
MACOSX_DEPLOYMENT_TARGET=13.0 CGO_CFLAGS="-mmacosx-version-min=13.0" CGO_LDFLAGS="-mmacosx-version-min=13.0" \
  "$WAILS" build -platform darwin/universal -clean -s -skipbindings
APP="build/bin/VibeSDR Flasher.app"
IDENT=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk '{print $2}')
[ -n "$IDENT" ] || { echo "no Developer ID Application identity"; exit 1; }
echo "==> Sign"
codesign --force --timestamp --options runtime --identifier net.vibesdr.flasher --sign "$IDENT" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
mkdir -p "$OUT"
ZIP="$OUT/VibeSDR-Flasher-macOS.zip"
rm -f "$ZIP"; ditto -c -k --keepParent "$APP" "$ZIP"
echo "==> Notarise"
# shellcheck source=/dev/null
source "$HOME/.appstoreconnect/config"
KEY_FILE="$HOME/.appstoreconnect/private_keys/AuthKey_${KEY_ID}.p8"
xcrun notarytool submit "$ZIP" --key "$KEY_FILE" --key-id "$KEY_ID" --issuer "$ISSUER" --wait
echo "==> Staple"
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute -vv "$APP"
rm -f "$ZIP"; ditto -c -k --keepParent "$APP" "$ZIP"
rm -rf "$OUT/VibeSDR Flasher.app"; ditto "$APP" "$OUT/VibeSDR Flasher.app"
echo "==> $ZIP"
echo "==> $OUT/VibeSDR Flasher.app"
