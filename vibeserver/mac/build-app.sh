#!/bin/bash
# Build VibeServer.app — a menu-bar app around the shared C++ core.
#
# No Xcode project on purpose: swiftc + a hand-assembled bundle is scriptable, diffable and works
# in CI, where a .xcodeproj is a binary blob that drifts. The same script will sign and notarise
# later; those are extra steps here, not a different pipeline.
#
#   ./vibeserver/mac/build-app.sh          → builds, and copies to the Desktop
#   ./vibeserver/mac/build-app.sh --no-copy
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAC="$ROOT/vibeserver/mac"
BUILD="$ROOT/vibeserver/build"
APP="$BUILD/VibeServer.app"

echo "==> Building the C++ core"
cmake -S "$ROOT/vibeserver" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target vibeserver_core -j >/dev/null
# ★★★ AND THE FRONT-DOOR BINARY, WHICH FULL MODE SPAWNS. Simple mode runs the server IN-PROCESS
#     (vs_start) and needs none of this; Full mode is multi-process by design — a front door that
#     owns no radio, one process per radio — exactly as on Linux. Rather than re-implement that
#     model inside the app, the app starts the SAME binary the Pi runs, so "Full mode behaves
#     identically to Linux" is true by construction instead of by maintenance.
# ★ It must be built from the same tree in the same configuration as the core the app links, or
#   the two halves of one product drift apart between releases.
cmake --build "$BUILD" --target vibeserver -j >/dev/null

echo "==> Assembling the bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>VibeServer</string>
  <key>CFBundleDisplayName</key>       <string>VibeServer</string>
  <key>CFBundleIdentifier</key>        <string>com.stuey3d.vibeserver</string>
  <key>CFBundleExecutable</key>        <string>VibeServer</string>
  <key>CFBundleIconFile</key>          <string>AppIcon</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <!-- ★★ THE APP IS V3 AND SAID 2.0.0. It launches the V3 front door, drives Full mode and ships
       the V3 core; the number had simply not moved since the alpha, so the About box and the
       GitHub release disagreed with the product (2026-08-11). -->
  <key>CFBundleShortVersionString</key><string>3.0.1</string>
  <key>CFBundleVersion</key>           <string>31</string>
  <key>LSMinimumSystemVersion</key>    <string>14.0</string>
  <!-- Menu-bar resident: no Dock icon, no window on launch. -->
  <key>LSUIElement</key>               <true/>
  <!-- macOS asks before we can be reached on the LAN; explain why rather than letting the bare
       system prompt be the user's first experience of the app. -->
  <key>NSLocalNetworkUsageDescription</key>
  <string>VibeServer shares this Mac's radio with your phone, watch and browser on your local network.</string>
  <!-- Optional, and only ever on an explicit button press: it fills in the receiver's Maidenhead
       LOCATOR (a square a few km across), never exact published coordinates. Listeners need a
       rough position for distances, bearings and the band plan's ITU region. -->
  <key>NSLocationWhenInUseUsageDescription</key>
  <string>Fills in this receiver's approximate location — a map square a few kilometres across — so listeners can see distances and bearings to the stations they hear. Your exact position is never published.</string>
  <!-- App Transport Security blocks cleartext HTTP by default, which silently killed the EiBi
       download — eibispace.de serves the schedule over plain http only (no https). Scope the
       exception to that one domain rather than allowing arbitrary loads; everything else stays
       https-only. -->
  <key>NSAppTransportSecurity</key>
  <dict>
    <key>NSExceptionDomains</key>
    <dict>
      <key>eibispace.de</key>
      <dict>
        <key>NSExceptionAllowsInsecureHTTPLoads</key>   <true/>
        <key>NSIncludesSubdomains</key>                 <true/>
      </dict>
    </dict>
  </dict>
</dict>
</plist>
PLIST

echo "==> Compiling the Swift app"
# -import-objc-header pulls in the flat C API; Swift needs no C++ interop.
LIBS=$(cd "$BUILD" && ls libvibeserver_core.a libvibedsp.a 2>/dev/null | sed "s|^|$BUILD/|")
RTLSDR=$(grep -m1 '^RTLSDR_LIB:' "$BUILD/CMakeCache.txt" | cut -d= -f2)
USBLIB=$(grep -m1 '^USB_LIB:'    "$BUILD/CMakeCache.txt" | cut -d= -f2)
OPUSLIB=$(grep -m1 '^OPUS_LIB:'  "$BUILD/CMakeCache.txt" | cut -d= -f2)
# ★ libairspyhf — BSD-3 and shipped as a real .a, so it links STATICALLY and the Airspy HF+ is
# plug-and-play with nothing for the user to install. Optional: a machine without it still
# builds, and simply reports no HF+ devices.
AHFLIB=$(grep -m1 '^AIRSPYHF_LIB:' "$BUILD/CMakeCache.txt" | cut -d= -f2)
# ★ FORCE THE STATIC ARCHIVE for EVERY Homebrew lib, next to whatever find_library cached. A notarised
# hardened-runtime app that links a Homebrew DYLIB crashes on launch for everyone: absent on machines
# without Homebrew, and even on the dev box the hardened runtime rejects it for a Team-ID mismatch
# (0.2.0: dyld "Library not loaded: librtlsdr.0.dylib"). librtlsdr/libusb had NO .a preference here —
# only opus did — so a stale cache pointing at the dylib shipped a broken app. Belt-and-braces even
# after wiping the cmake cache.
_rtldir=$(dirname "$RTLSDR"); [ -f "$_rtldir/librtlsdr.a" ]  && RTLSDR="$_rtldir/librtlsdr.a"
_usbdir=$(dirname "$USBLIB"); [ -f "$_usbdir/libusb-1.0.a" ] && USBLIB="$_usbdir/libusb-1.0.a"
_opusdir=$(dirname "$OPUSLIB"); [ -f "$_opusdir/libopus.a" ] && OPUSLIB="$_opusdir/libopus.a"
if [ -n "$AHFLIB" ]; then
  _ahfdir=$(dirname "$AHFLIB"); [ -f "$_ahfdir/libairspyhf.a" ] && AHFLIB="$_ahfdir/libairspyhf.a"
fi

swiftc \
  -O -target arm64-apple-macos14.0 \
  -parse-as-library \
  -import-objc-header "$ROOT/vibeserver/vibeserver_api.h" \
  -I "$ROOT/vibeserver" \
  "$MAC/VibeServerApp.swift" \
  "$MAC/EibiStations.swift" \
  "$MAC/FullMode.swift" \
  $LIBS "$RTLSDR" "$USBLIB" "$OPUSLIB" ${AHFLIB:+"$AHFLIB"} \
  -lc++ \
  -framework IOKit -framework CoreFoundation -framework Security -framework AppKit -framework SwiftUI \
  -framework CoreLocation \
  -o "$APP/Contents/MacOS/VibeServer"

# ★★ SHIP THE FRONT DOOR INSIDE THE BUNDLE. Contents/MacOS is the right home: it is code, it is
#    covered by the app's signature, and it is read-only once installed — an executable dropped in
#    Application Support would be neither signed nor trusted. Full mode looks it up with
#    Bundle.main.url(forAuxiliaryExecutable:), so a user who drags the .app anywhere still works.
# ★★★ NAMED vibeserver-engine, NOT vibeserver, AND THAT IS NOT COSMETIC. macOS filesystems are
#     CASE-INSENSITIVE by default, so "Contents/MacOS/vibeserver" IS "Contents/MacOS/VibeServer" —
#     the app's own binary. Copying it here overwrote the SwiftUI app with the command-line tool,
#     producing a bundle that launched the CLI when double-clicked. It built cleanly and the only
#     symptom was an .app that answered --list-radios. (Caught 2026-08-10, immediately.)
cp "$BUILD/vibeserver" "$APP/Contents/MacOS/vibeserver-engine"

# The web client the server hands to browsers is baked into the core, so there is nothing to copy.

echo "==> Icons"
# Regenerate from the family generator so the app can never drift from the brand artwork.
python3 "$MAC/make-icons.py" >/dev/null
cp "$MAC/Resources/"MenuBar*.png "$APP/Contents/Resources/"

if [ -f "$MAC/AppIcon.icns" ]; then
  cp "$MAC/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
else
  echo "    (no AppIcon.icns yet — using the system default)"
fi

# Ad-hoc signature so macOS will run it locally. Developer ID signing + notarisation come later;
# without any signature at all the LAN permission prompt and Gatekeeper get awkward.
codesign --force --sign - --identifier com.stuey3d.vibeserver "$APP" >/dev/null 2>&1 || true

echo "==> Built $APP"

if [ "${1:-}" != "--no-copy" ]; then
  DEST="$HOME/Desktop/VibeServer.app"
  rm -rf "$DEST"
  cp -R "$APP" "$DEST"
  echo "==> Copied to $DEST"
fi
