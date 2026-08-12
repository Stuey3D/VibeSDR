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
# ★ The BUILD OUTPUT, not the Desktop — build-app.sh installs to /Applications now, and there is
#   no Desktop copy to notarise (Stuart, 2026-08-12: two copies "gets confusing otherwise").
APP="${1:-$ROOT/vibeserver/build/VibeServer.app}"
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
# ★★ THE ENTITLEMENT MATTERS. Library validation (part of the hardened runtime) allows only
# libraries signed by our own Team ID — and SDRplay's API is signed by SDRPLAY LIMITED, so
# without disable-library-validation the dlopen() in sdrplay_source.cpp is refused and RSPs
# never appear. ★ Only on a SIGNED build: an unsigned dev build loads it happily, so this
# fails exclusively in the artefact you ship (2026-07-27).
ENT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/VibeServer.entitlements"
# ★★★ SIGN EVERY EXECUTABLE IN MacOS/, NOT JUST THE APP'S OWN. Full mode ships a SECOND Mach-O —
#     `vibeserver-engine`, the front door the app spawns — and this script predates it, so it went
#     out unsigned and Apple rejected the whole submission: "not signed with a valid Developer ID
#     certificate", "no secure timestamp", "hardened runtime not enabled", all three against that
#     one file (2026-08-11). Nothing local catches it: an unsigned nested binary runs perfectly on
#     the machine that built it and fails only in the artefact you ship.
#     ★★ So enumerate rather than name. A loop over MacOS/ cannot be out of date the next time a
#        helper binary is added — which is exactly how this one was missed.
for exe in "$APP/Contents/MacOS/"*; do
  [ -f "$exe" ] || continue
  echo "    signing $(basename "$exe")"
  codesign --force --timestamp --options runtime --entitlements "$ENT" --sign "$IDENT" "$exe"
done
codesign --force --timestamp --options runtime --entitlements "$ENT" --sign "$IDENT" "$APP"
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

# ★★★ AND PUT THE NOTARISED COPY IN /Applications. build-app.sh installs BEFORE this script runs,
#     so without this the installed app is the ad-hoc signed build: it reports the right version,
#     runs the right code, and is NOT the artefact anyone downloads — `spctl` rejects it and its
#     hash differs from the release. That is the worst kind of "same build", because the version
#     number agrees (Stuart, 2026-08-12: "is 3.0.2 the one in my apps folder?" — it said so, and it
#     was not). Testing a release means testing the thing that shipped.
if [ -d "/Applications/VibeServer.app" ]; then
  osascript -e 'tell application "VibeServer" to quit' >/dev/null 2>&1 || true
  for _ in 1 2 3 4 5 6; do pgrep -x VibeServer >/dev/null || break; sleep 0.5; done
  rm -rf "/Applications/VibeServer.app"
  cp -R "$APP" "/Applications/VibeServer.app"
  echo "==> Installed the NOTARISED build to /Applications"
fi

echo "==> DONE. Notarised, stapled, packaged: $ZIP"
