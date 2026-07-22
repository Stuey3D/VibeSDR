#!/usr/bin/env bash
# Sign, notarise, staple and package VibeServer.app for distribution — so a downloaded copy opens
# without a Gatekeeper warning ("unidentified developer" / "damaged and can't be opened").
#
# Prerequisites (one-time):
#   1. A "Developer ID Application" certificate in the login keychain. Create it in
#      Xcode ▸ Settings ▸ Accounts ▸ Manage Certificates ▸ + ▸ Developer ID Application.
#   2. The App Store Connect API key at ~/.appstoreconnect/private_keys/AuthKey_NG46B3P48N.p8
#      (already present). Key id + issuer are passed to notarytool below.
#
# Usage:  vibeserver/mac/notarise-and-release.sh [path/to/VibeServer.app]
# Output: a stapled, zipped VibeServer-<ver>.zip ready to attach to a GitHub release.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="${1:-$HOME/Desktop/VibeServer.app}"
KEY_ID="NG46B3P48N"
ISSUER="340c3b5f-a208-4c2f-a68b-4ca12851b769"
KEY_FILE="$HOME/.appstoreconnect/private_keys/AuthKey_${KEY_ID}.p8"

[ -d "$APP" ] || { echo "!! No app at $APP"; exit 1; }
[ -f "$KEY_FILE" ] || { echo "!! No ASC key at $KEY_FILE"; exit 1; }

# The Developer ID Application identity (there should be exactly one).
IDENT=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk '{print $2}')
[ -n "$IDENT" ] || { echo "!! No 'Developer ID Application' certificate found. Create it in Xcode ▸ Settings ▸ Accounts."; exit 1; }
echo "==> Signing identity: $IDENT"

VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist" 2>/dev/null || echo "0.1.0")

# ── Sign ────────────────────────────────────────────────────────────────────
# Hardened runtime is REQUIRED for notarisation. Sign inside-out: nested code first, then the app.
# --options runtime enables the hardened runtime; --timestamp gets a secure timestamp.
# Strip extended attributes (resource forks, Finder info, quarantine) — codesign refuses to sign a
# bundle carrying them ("resource fork … not allowed"). Copying to the Desktop is enough to pick
# some up.
echo "==> Cleaning extended attributes…"
xattr -cr "$APP"

echo "==> Signing (hardened runtime)…"
find "$APP/Contents" -type f \( -name "*.dylib" -o -name "*.so" \) -print0 2>/dev/null \
  | xargs -0 -I{} codesign --force --timestamp --options runtime --sign "$IDENT" {} || true
codesign --force --timestamp --options runtime --sign "$IDENT" "$APP/Contents/MacOS/VibeServer"
codesign --force --timestamp --options runtime --sign "$IDENT" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

# ── Notarise ────────────────────────────────────────────────────────────────
ZIP="$ROOT/VibeServer-${VER}.zip"
echo "==> Zipping for submission → $ZIP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"

echo "==> Submitting to Apple notary service (this can take a few minutes)…"
xcrun notarytool submit "$ZIP" \
  --key "$KEY_FILE" --key-id "$KEY_ID" --issuer "$ISSUER" \
  --wait

# ── Staple + repackage ──────────────────────────────────────────────────────
# Staple the notarisation ticket to the .app so it validates OFFLINE, then re-zip the stapled app.
echo "==> Stapling…"
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute --verbose=4 "$APP" || true   # informational: should say "accepted, Notarized Developer ID"

echo "==> Re-zipping the stapled app → $ZIP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"

echo "==> DONE. Notarised, stapled, packaged: $ZIP"
