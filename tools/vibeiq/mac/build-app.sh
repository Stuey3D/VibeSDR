#!/usr/bin/env bash
# Build VibeIQ.app (universal), sign it, notarise it, staple it, and zip it for the release.
#
#   tools/vibeiq/mac/build-app.sh            → out/VibeIQ-macOS.zip (signed + notarised + stapled)
#   tools/vibeiq/mac/build-app.sh --no-notarise
#
# Needs: Go, Xcode command-line tools, a Developer ID Application certificate in the keychain,
# and ~/.appstoreconnect/config (KEY_ID, ISSUER) + the .p8 beside it — the same setup as
# vibeserver/mac/notarise-and-release.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/.."
ROOT="$SRC/../.."
OUT="$SRC/out"; APP="$OUT/VibeIQ.app"
VER="${VIBEIQ_VERSION:-1.0.0}"
BUILD=$(echo "$VER" | awk -F. '{printf "%d", $1*10000 + $2*100 + $3}')
NOTARISE=1; [ "${1:-}" = "--no-notarise" ] && NOTARISE=0

rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$OUT/tmp"

echo "==> Go bridge (universal)"
(cd "$SRC" && GOOS=darwin GOARCH=arm64 CGO_ENABLED=0 go build -ldflags="-s -w" -o "$OUT/tmp/vibeiq-arm64" . \
            && GOOS=darwin GOARCH=amd64 CGO_ENABLED=0 go build -ldflags="-s -w" -o "$OUT/tmp/vibeiq-amd64" .)
# ★★★ The bridge is `vibeiq-bridge`, NOT `vibeiq`. The Mac's disk is case-INsensitive APFS, so
#   `vibeiq` and the Swift shell `VibeIQ` were ONE FILE: the shell overwrote the bridge, then
#   launched "the bridge" — itself — which launched itself… A fork bomb that took the whole Mac
#   down (jetsam 2026-09-09 20:45, Stuart had to hold the power button). Names inside a bundle
#   must differ by more than case.
lipo -create "$OUT/tmp/vibeiq-arm64" "$OUT/tmp/vibeiq-amd64" -output "$APP/Contents/MacOS/vibeiq-bridge"

echo "==> Swift shell (universal)"
for arch in arm64 x86_64; do
  swiftc -O -target "$arch-apple-macos14.0" -parse-as-library \
    "$HERE/VibeIQApp.swift" -o "$OUT/tmp/VibeIQ-$arch"
done
lipo -create "$OUT/tmp/VibeIQ-arm64" "$OUT/tmp/VibeIQ-x86_64" -output "$APP/Contents/MacOS/VibeIQ"

# ★ Prove the two executables are two files, and that each is what it claims to be. If this
#   ever fails, DO NOT launch the app — see the fork-bomb note above.
[ "$(ls "$APP/Contents/MacOS" | wc -l | tr -d ' ')" = 2 ] || { echo "!! Contents/MacOS must hold exactly two executables"; ls -l "$APP/Contents/MacOS"; exit 1; }
[ "$(stat -f %i "$APP/Contents/MacOS/VibeIQ")" != "$(stat -f %i "$APP/Contents/MacOS/vibeiq-bridge")" ] || { echo "!! shell and bridge are the same file"; exit 1; }
#   (grep -c, not grep -q: under pipefail a -q that matches early sends strings SIGPIPE and the
#   SUCCESSFUL match reports as failure — the librtlsdr trap.)
[ "$(strings -a "$APP/Contents/MacOS/VibeIQ" | grep -c "vibeiq-bridge" || true)" -gt 0 ] || { echo "!! VibeIQ is not the Swift shell"; exit 1; }
[ "$(strings -a "$APP/Contents/MacOS/vibeiq-bridge" | grep -c "VibeIQ window:" || true)" -gt 0 ] || { echo "!! vibeiq-bridge is not the Go bridge"; exit 1; }

# ★ The family icon, same as VibeServer's — one product family, one mark.
cp "$ROOT/vibeserver/mac/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>VibeIQ</string>
  <key>CFBundleDisplayName</key>       <string>VibeIQ</string>
  <key>CFBundleIdentifier</key>        <string>net.vibesdr.vibeiq</string>
  <key>CFBundleExecutable</key>        <string>VibeIQ</string>
  <key>CFBundleIconFile</key>          <string>AppIcon</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VER}</string>
  <key>CFBundleVersion</key>           <string>${BUILD}</string>
  <key>LSMinimumSystemVersion</key>    <string>14.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
  <key>NSHumanReadableCopyright</key>  <string>VibeSDR</string>
</dict>
</plist>
PLIST

IDENT=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk '{print $2}')
[ -n "$IDENT" ] || { echo "!! no Developer ID Application certificate"; exit 1; }
echo "==> Signing"
xattr -cr "$APP"
# ★ Strip first: a re-sign keeps the OLD identifier and seal of whatever was there (Go's build
#   cache hands back the same file), and the app then fails "invalid Info.plist" on that slice.
codesign --remove-signature "$APP/Contents/MacOS/vibeiq-bridge" 2>/dev/null || true
codesign --force --timestamp --options runtime --identifier net.vibesdr.vibeiq.bridge --sign "$IDENT" "$APP/Contents/MacOS/vibeiq-bridge"
codesign --force --timestamp --options runtime --sign "$IDENT" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

ZIP="$OUT/VibeIQ-macOS.zip"
rm -f "$ZIP"; ditto -c -k --keepParent "$APP" "$ZIP"
if [ "$NOTARISE" = 1 ]; then
  # shellcheck source=/dev/null
  source "$HOME/.appstoreconnect/config"
  KEY_FILE="$HOME/.appstoreconnect/private_keys/AuthKey_${KEY_ID}.p8"
  echo "==> Notarising"
  xcrun notarytool submit "$ZIP" --key "$KEY_FILE" --key-id "$KEY_ID" --issuer "$ISSUER" --wait
  echo "==> Stapling"
  xcrun stapler staple "$APP"
  xcrun stapler validate "$APP"
  rm -f "$ZIP"; ditto -c -k --keepParent "$APP" "$ZIP"
  spctl --assess --type execute -vv "$APP"
fi
echo "==> $ZIP"
