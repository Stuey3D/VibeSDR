#!/bin/bash
# Build VibeServer.app — a menu-bar app around the shared C++ core.
#
# No Xcode project on purpose: swiftc + a hand-assembled bundle is scriptable, diffable and works
# in CI, where a .xcodeproj is a binary blob that drifts. The same script will sign and notarise
# later; those are extra steps here, not a different pipeline.
#
#   ./vibeserver/mac/build-app.sh          → builds, and INSTALLS to /Applications
#   ./vibeserver/mac/build-app.sh --no-copy
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAC="$ROOT/vibeserver/mac"
BUILD="$ROOT/vibeserver/build"
APP="$BUILD/VibeServer.app"
LOGS="$BUILD/mac-logs"
mkdir -p "$LOGS"

# ★★★ ONE MINIMUM macOS, READ BY EVERY STEP. The Info.plist said 14.0 while the engine was built
#     for 27.0 and cloudflared for 15.0 (5.6.78): three numbers, only one of them typed on purpose.
#     MACOS_MINIMUM feeds the plist, swiftc's target, CMake's deployment target and build-deps.sh,
#     and check-bundle.sh verifies the result against the plist at the end.
MACOS_MIN="$(tr -d '[:space:]' < "$MAC/MACOS_MINIMUM")"
[ -n "$MACOS_MIN" ] || { echo "!! vibeserver/mac/MACOS_MINIMUM is empty"; exit 1; }
echo "==> minimum macOS $MACOS_MIN (vibeserver/mac/MACOS_MINIMUM)"

# ★★★ THIRD-PARTY LIBRARIES FROM OUR OWN BUILD, NOT HOMEBREW. A Homebrew bottle is built for the
#     OS of the Mac that installed it (macOS 26 objects inside the 14.0 5.6.78). build-deps.sh
#     builds librtlsdr/libusb/libopus and cloudflared from pinned, hash-checked source for
#     MACOS_MIN; it is idempotent, so on every build after the first this takes a fraction of a second.
"$MAC/build-deps.sh"
DEPS="$MAC/deps"

echo "==> Building the C++ core"
# ★★★ STRICT: a release build must contain every radio the release claims to contain. Without
#     this flag a missing brew formula removes a driver silently, leaving one cmake STATUS line as
#     the only trace — which is how 3.0.0-2 shipped with the setup wizard compiled out. It matters
#     most for the HackRF: the one person who can test it cannot tell a missing driver from a
#     broken one. It used to need `brew install hackrf librtlsdr libusb`; since 2026-09-28 it
#     needs nothing from Homebrew any more: the libraries come from build-deps.sh (VIBE_MAC_DEPS),
#     and libairspyhf / libairspy / libhackrf are vendored and compiled into the core.
# ★★★ CMAKE_OSX_DEPLOYMENT_TARGET — without it CMake builds for THE BUILD MAC'S OS. 5.6.78's
#     vibeserver-engine was minos 27.0 (this Mac runs a 27 beta), so Full mode could not start on
#     any real user's Mac, and nothing here noticed. arm64 matches swiftc's target below.
# ★★ Output goes to LOGS, not /dev/null: the linker's "built for newer macOS version" warning was
#    the one line that told the truth, and it was being thrown away. check-bundle.sh reads them.
cmake -S "$ROOT/vibeserver" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DVIBESERVER_STRICT_RADIOS=ON \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DVIBE_MAC_DEPS="$DEPS/prefix" >"$LOGS/configure.log" 2>&1 \
  || { tail -40 "$LOGS/configure.log"; echo "!! cmake configure failed"; exit 1; }
cmake --build "$BUILD" --target vibeserver_core -j >"$LOGS/core.log" 2>&1 \
  || { tail -40 "$LOGS/core.log"; echo "!! vibeserver_core build failed"; exit 1; }
# ★★★ AND THE FRONT-DOOR BINARY, WHICH FULL MODE SPAWNS. Simple mode runs the server IN-PROCESS
#     (vs_start) and needs none of this; Full mode is multi-process by design — a front door that
#     owns no radio, one process per radio — exactly as on Linux. Rather than re-implement that
#     model inside the app, the app starts the SAME binary the Pi runs, so "Full mode behaves
#     identically to Linux" is true by construction instead of by maintenance.
# ★ It must be built from the same tree in the same configuration as the core the app links, or
#   the two halves of one product drift apart between releases.
# ★ Relinked EVERY time (the rm), so the engine's link — and any ld warning in it — is in THIS
#   build's log. An up-to-date target is a skipped link and a log that proves nothing.
rm -f "$BUILD/vibeserver"
cmake --build "$BUILD" --target vibeserver -j >"$LOGS/engine.log" 2>&1 \
  || { tail -40 "$LOGS/engine.log"; echo "!! vibeserver (engine) build failed"; exit 1; }

echo "==> Assembling the bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# ★★★ THE VERSION COMES FROM CMakeLists.txt, NOT FROM HERE. It was typed into this heredoc, so the
#     Mac app carried its own copy of the number and could disagree with the server binary inside
#     it — which is exactly how the iOS app came to ship 10.2 while everything else said 10.3
#     (2026-08-15). One source, and it is the one that also stamps the Linux package.
VIBE_VER=$(sed -n 's/^project(vibeserver VERSION \([0-9.]*\).*/\1/p' "$ROOT/vibeserver/CMakeLists.txt")
[ -n "$VIBE_VER" ] || { echo "!! could not read the version from vibeserver/CMakeLists.txt"; exit 1; }
echo "==> version $VIBE_VER (from CMakeLists.txt)"

# ★★★ AND THE BUILD NUMBER COMES FROM IT TOO. This was hard-coded to 32 in the plist heredoc below
#     — the SAME fault the note above describes, fixed for the marketing version and missed for
#     this one, so 3.1.18 and 3.1.20 both called themselves build 32. macOS caches app metadata by
#     bundle version, and two different builds claiming one number is how a machine keeps showing
#     you the old one long after you replaced it.
# ★ Derived, not counted: major*10000 + minor*100 + patch. Monotonic while versions go up, needs
#   no state file, and cannot drift from the version the way a hand-typed counter does.
VIBE_BUILD=$(echo "$VIBE_VER" | awk -F. '{printf "%d", $1*10000 + $2*100 + $3}')
# ★ A pre-release (VIBESERVER_PRERELEASE "b1" in CMakeLists.txt) must number BELOW its release and above
#   the last one: 11.0.0 b1 = 110000 - 100 + 1 = 109901; 11.0.0 itself = 110000. The marketing version
#   stays plain 11.0.0 (Apple requires digits); the label rides on the GitHub release tag.
VIBE_PRE=$(sed -n 's/^set(VIBESERVER_PRERELEASE "\([A-Za-z0-9]*\)").*/\1/p' "$ROOT/vibeserver/CMakeLists.txt")
if [ -n "$VIBE_PRE" ]; then
  VIBE_PRE_N=$(echo "$VIBE_PRE" | tr -dc '0-9'); VIBE_PRE_N=${VIBE_PRE_N:-1}
  VIBE_BUILD=$((VIBE_BUILD - 100 + VIBE_PRE_N))
  echo "==> pre-release $VIBE_PRE"
fi
echo "==> build $VIBE_BUILD (derived from $VIBE_VER)"

cat > "$APP/Contents/Info.plist" <<PLIST
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
  <key>CFBundleShortVersionString</key><string>${VIBE_VER}</string>
  <key>CFBundleVersion</key>           <string>${VIBE_BUILD}</string>
  <key>LSMinimumSystemVersion</key>    <string>${MACOS_MIN}</string>
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
# ★★ BUT NOT WHEN WE BUILT OUR OWN. CMakeLists.txt compiles the vendored, PATCHED libairspyhf into
#    libvibeserver_core.a whenever this file exists (the same test it uses), and find_library still
#    caches Homebrew's UNPATCHED archive. Linking both leaves which airspyhf_close() wins to archive
#    order — the use-after-free the vendored copy exists to fix. Core already defines every symbol.
# ★ libairspy (Airspy R2/Mini) is vendored-only and lives inside core too, so it needs no line here.
if [ -f "$ROOT/android/app/src/main/cpp/libairspyhf/airspyhf.c" ]; then AHFLIB=""; fi
# ★★★ AND libhackrf — THE SECOND READER OF THE RADIO LIST. CMakeLists.txt links this into
# vibeserver_core, but the Swift app is linked BY HAND right here, so a driver added there is not a
# driver added here: the CLI `vibeserver` target linked fine while the .app failed with 14
# undefined _hackrf_* symbols. That is the "one rule, two readers" fault that caused nearly every
# HackRF bug on 2026-08-26, and this line is the sixth site of it. Any future driver needs adding
# in BOTH places.
HRFLIB=$(grep -m1 '^HACKRF_LIB:' "$BUILD/CMakeCache.txt" | cut -d= -f2)
# ★★ NO HOMEBREW FALLBACK ANY MORE. This used to swap each cached Homebrew path for the .a beside
#    it — right for "no dylib", wrong for "built for this Mac only": those archives were macOS 26
#    objects. The cache now holds build-deps.sh's archives (VIBE_MAC_DEPS forces them), and the gate
#    at the end FAILS the build if a package-manager path ever reaches this link line again.
# ★ The link inputs are written down so the gate can check exactly what was linked.
printf '%s\n' $LIBS "$RTLSDR" "$USBLIB" "$OPUSLIB" ${AHFLIB:+"$AHFLIB"} ${HRFLIB:+"$HRFLIB"} \
  > "$LOGS/swift-link-inputs.txt"

swiftc \
  -O -target "arm64-apple-macos$MACOS_MIN" \
  -parse-as-library \
  -import-objc-header "$ROOT/vibeserver/vibeserver_api.h" \
  -I "$ROOT/vibeserver" \
  "$MAC/VibeServerApp.swift" \
  "$MAC/EibiStations.swift" \
  "$MAC/FullMode.swift" \
  $LIBS "$RTLSDR" "$USBLIB" "$OPUSLIB" ${AHFLIB:+"$AHFLIB"} ${HRFLIB:+"$HRFLIB"} \
  -lc++ \
  `# ★ ONE RULE, TWO READERS: CMakeLists.txt links these for vibeserver/dab-offline; this line is the app's own.` \
  `# AudioToolbox + CoreAudio: the DAB+ decoder fallback in vibe_dab_aacdec.h (5.0.0 broke here without them).` \
  -framework IOKit -framework CoreFoundation -framework Security -framework AppKit -framework SwiftUI \
  -framework CoreLocation \
  -framework AudioToolbox -framework CoreAudio \
  -o "$APP/Contents/MacOS/VibeServer" >"$LOGS/swift.log" 2>&1 \
  || { cat "$LOGS/swift.log"; echo "!! swiftc failed"; exit 1; }
# Swift's own warnings used to reach the terminal; they still do.
[ -s "$LOGS/swift.log" ] && cat "$LOGS/swift.log"

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

# ★★★ cloudflared TRAVELS INSIDE THE APP. The public listing offers to make an address for an
#     owner with no port forward, and that offer cannot rest on a binary they have to go and find
#     — a menu-bar app with a manual prerequisite is not a one-switch feature (Stuart, 2026-08-23).
#  ★★ Apache-2.0: redistribution is permitted WITH THE LICENCE ALONGSIDE, so it ships beside it.
#  ★ Missing = an app without the tunnel, not a failed build — a fresh clone has not run fetch.sh.
# ★★★ COMPILED BY build-deps.sh, NOT DOWNLOADED. Cloudflare's own darwin binary (what
#     tools/cloudflared-desktop/fetch.sh fetches) is minos 15.0, so the tunnel could not run on the
#     macOS 14 the app promises. Same tag, built from source with CGO off (see build-deps.sh).
#  ★ REQUIRED now: build-deps.sh always builds it, so a missing one is a broken deps tree, not a
#    fresh clone — and a release that silently lost the tunnel is the kind that ships.
CF="$DEPS/cloudflared/cloudflared"
[ -x "$CF" ] && [ -s "$DEPS/cloudflared/LICENSE" ] \
  || { echo "!! no compiled cloudflared at $CF — run vibeserver/mac/build-deps.sh"; exit 1; }
# ★★★ IN MacOS/, NOT Resources/. The notarise script signs every executable in MacOS/ by
#     ENUMERATION — precisely so a helper added later cannot be missed — and an unsigned Mach-O
#     anywhere in the bundle fails notarisation. It also runs perfectly on the machine that built
#     it, so the break would surface only in the artefact somebody downloads. That has happened
#     here once already, with vibeserver-engine.
cp "$CF" "$APP/Contents/MacOS/cloudflared"
chmod +x "$APP/Contents/MacOS/cloudflared"
cp "$DEPS/cloudflared/LICENSE" "$APP/Contents/Resources/LICENSE.cloudflared"
echo "==> bundled cloudflared ($("$CF" --version | head -1))"

# ★★ THE GPU MAP'S FILES (renderer, style, glyphs, icons, basic + relief PMTiles) — served at /mapgl/
#    by vibe_mapgl.h, which looks for them at <exe>/../Resources/mapgl. Plain names from
#    directory/public/mapgl (fonts with spaces, .js) plus the style from web/mapkit. Data only, so
#    the app's own signature seals them; nothing here needs signing separately.
#  ★ Missing = an app whose maps fall back to the old renderer, not a failed build.
GL="$ROOT/directory/public/mapgl"
if [ -s "$GL/vibemap-basic.pmtiles" ] && [ -s "$ROOT/web/mapkit/vibemap-style.json" ]; then
  rm -rf "$APP/Contents/Resources/mapgl"
  mkdir -p "$APP/Contents/Resources/mapgl/vendor" "$APP/Contents/Resources/mapgl/icons"
  cp "$GL/vibemap-basic.pmtiles" "$GL/vibemap-relief.pmtiles" "$GL/vibemap-runways.pmtiles" "$APP/Contents/Resources/mapgl/"
  cp "$ROOT/web/mapkit/vibemap-style.json" "$APP/Contents/Resources/mapgl/"
  cp "$GL/vendor/"*.js "$GL/vendor/"*.css "$GL/vendor/"LICENSE* "$APP/Contents/Resources/mapgl/vendor/"
  cp "$GL/icons/"*.png "$APP/Contents/Resources/mapgl/icons/"
  cp -R "$GL/fonts" "$APP/Contents/Resources/mapgl/fonts"
  echo "==> bundled GPU map files"
else
  echo "==> GPU map files NOT bundled (build the packs, then node scripts/sync-mapgl-assets.mjs)"
fi

if [ -f "$MAC/AppIcon.icns" ]; then
  cp "$MAC/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
else
  echo "    (no AppIcon.icns yet — using the system default)"
fi

# Ad-hoc signature so macOS will run it locally. Developer ID signing + notarisation come later;
# without any signature at all the LAN permission prompt and Gatekeeper get awkward.
codesign --force --sign - --identifier com.stuey3d.vibeserver "$APP" >/dev/null 2>&1 || true

echo "==> Built $APP"

# ★★★ THE GATE. Every Mach-O in the bundle at or below LSMinimumSystemVersion, only system or
#     in-bundle dylibs, every linked archive's objects at or below it, no "built for newer macOS"
#     warning in any link, no package-manager path on any link line. A failure here stops the
#     build BEFORE it is installed anywhere.
GATE=(--app "$APP")
for a in $LIBS "$RTLSDR" "$USBLIB" "$OPUSLIB" ${AHFLIB:+"$AHFLIB"} ${HRFLIB:+"$HRFLIB"}; do
  GATE+=(--archive "$a")
done
for l in core.log engine.log swift.log; do GATE+=(--ldlog "$LOGS/$l"); done
GATE+=(--linkinput "$BUILD/CMakeFiles/vibeserver.dir/link.txt"
       --linkinput "$BUILD/CMakeFiles/vibeserver_core.dir/flags.make"
       --linkinput "$LOGS/swift-link-inputs.txt")
"$MAC/check-bundle.sh" "${GATE[@]}"

if [ "${1:-}" != "--no-copy" ]; then
  # ★★ /Applications, NOT the Desktop. A build on the Desktop and an older copy in /Applications
  #    are two VibeServers with the same icon, and the one you double-click is whichever you
  #    happened to reach for — so a fix "not working" can simply be the other copy running
  #    (Stuart, 2026-08-12: "gets confusing otherwise"). One install, one answer.
  # ★ The running app is quit first: cp over a live bundle leaves a half-replaced app, and macOS
  #   will keep executing the OLD code it already has mapped.
  DEST="/Applications/VibeServer.app"
  osascript -e 'tell application "VibeServer" to quit' >/dev/null 2>&1 || true
  for _ in 1 2 3 4 5 6; do pgrep -x VibeServer >/dev/null || break; sleep 0.5; done
  rm -rf "$DEST"
  cp -R "$APP" "$DEST"
  echo "==> Installed to $DEST"
fi
