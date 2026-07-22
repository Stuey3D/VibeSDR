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
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key>           <string>1</string>
  <key>LSMinimumSystemVersion</key>    <string>14.0</string>
  <!-- Menu-bar resident: no Dock icon, no window on launch. -->
  <key>LSUIElement</key>               <true/>
  <!-- macOS asks before we can be reached on the LAN; explain why rather than letting the bare
       system prompt be the user's first experience of the app. -->
  <key>NSLocalNetworkUsageDescription</key>
  <string>VibeServer shares this Mac's radio with your phone, watch and browser on your local network.</string>
</dict>
</plist>
PLIST

echo "==> Compiling the Swift app"
# -import-objc-header pulls in the flat C API; Swift needs no C++ interop.
LIBS=$(cd "$BUILD" && ls libvibeserver_core.a libvibedsp.a 2>/dev/null | sed "s|^|$BUILD/|")
RTLSDR=$(grep -m1 '^RTLSDR_LIB:' "$BUILD/CMakeCache.txt" | cut -d= -f2)
USBLIB=$(grep -m1 '^USB_LIB:'    "$BUILD/CMakeCache.txt" | cut -d= -f2)

swiftc \
  -O -target arm64-apple-macos14.0 \
  -parse-as-library \
  -import-objc-header "$ROOT/vibeserver/vibeserver_api.h" \
  -I "$ROOT/vibeserver" \
  "$MAC/VibeServerApp.swift" \
  "$MAC/EibiStations.swift" \
  $LIBS "$RTLSDR" "$USBLIB" \
  -lc++ \
  -framework IOKit -framework CoreFoundation -framework Security -framework AppKit -framework SwiftUI \
  -o "$APP/Contents/MacOS/VibeServer"

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
