#!/usr/bin/env bash
# Build VibeIQ for every platform we ship, and package each one.
#
#   tools/vibeiq/build-all.sh              → out/dist/*  (all targets)
#   tools/vibeiq/build-all.sh linux        → just that family (linux | windows | darwin)
#
# The Mac .app is NOT built here — it needs a Developer ID certificate and a notarisation round
# trip, so it keeps its own script: tools/vibeiq/mac/build-app.sh. This one builds the bare
# darwin binaries (universal) for anyone who wants the bridge without the bundle.
#
# ★ Go, standard library only, CGO off: every artefact is one static binary that runs on a clean
#   machine with nothing installed. That is the whole distribution promise (see main.go).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."
OUT="$HERE/out"; DIST="$OUT/dist"; TMP="$OUT/tmp"
VER="${VIBEIQ_VERSION:-1.0.1}"
WANT="${1:-all}"

# ★ Only a full run clears the output. Asking for ONE family used to wipe `dist` first, so
#   `build-all.sh darwin` silently deleted the linux and windows packages built moments before —
#   found while assembling a release, with the artefacts already checksummed. A partial build must
#   never destroy what it is not rebuilding.
[ "$WANT" = all ] && rm -rf "$DIST"
mkdir -p "$DIST" "$TMP"
cd "$HERE"

# ★★ VERIFY THE ARTEFACT, not the exit status. A cross-build that quietly produced the wrong
#    thing has cost this project days more than once, so every binary is checked for a string
#    that only the real bridge has. grep -c, never grep -q: under `set -o pipefail` a -q that
#    matches early sends SIGPIPE upstream and the SUCCESSFUL match is reported as a FAILURE.
#    That trap is why librtlsdr could not be rebuilt for the 16 KB page work.
check() {
  local f="$1" what="$2"
  [ -s "$f" ] || { echo "!! $f was not produced"; exit 1; }
  [ "$(strings -a "$f" | grep -c "$what" || true)" -gt 0 ] || { echo "!! $f does not contain '$what' — wrong artefact"; exit 1; }
}

build() { # goos goarch outfile [extra ldflags]
  echo "==> $1/$2  $(basename "$3")"
  GOOS="$1" GOARCH="$2" CGO_ENABLED=0 go build -trimpath -ldflags="-s -w ${4:-}" -o "$3" .
}

# ── Linux ─────────────────────────────────────────────────────────────────────────────────────
# One binary: it is the GUI (a page on the loopback, opened with xdg-open) AND the command line,
# and it works out which is wanted — a desktop session gets the window, an SSH session gets asked
# for the code on the terminal. See headless() in main.go.
if [ "$WANT" = all ] || [ "$WANT" = linux ]; then
  for arch in amd64 arm64; do
    d="$TMP/vibeiq-linux-$arch"; rm -rf "$d"; mkdir -p "$d"
    build linux "$arch" "$d/vibeiq"
    check "$d/vibeiq" "VibeIQ window:"
    cp "$ROOT/assets/vibeserver-icon.png" "$d/vibeiq.png"
    # A desktop entry so it appears in the launcher when dropped in ~/.local/share/applications.
    # Terminal=false: double-clicking must open the window, not a console — the terminal path is
    # for people who typed the name, and they get it by typing it.
    cat > "$d/vibeiq.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=VibeIQ
Comment=Raw IQ from a VibeSDR receiver, as rtl_tcp on this machine
Exec=vibeiq
Icon=vibeiq
Terminal=false
Categories=HamRadio;Network;Utility;
DESK
    cat > "$d/README.txt" <<TXT
VibeIQ $VER — raw IQ from a VibeSDR receiver, as rtl_tcp on this machine.

  ./vibeiq            open the window (or, over SSH, ask for the code right here)
  ./vibeiq CODE       pair and stream, no window
  ./vibeiq --cli      the terminal even at a desk
  ./vibeiq --gui      the window even over SSH (forward the port it prints)

Then point SDR++, GQRX, DSD-FME or anything else that speaks rtl_tcp at 127.0.0.1:1234.
Tuning in that app moves the receiver's dial; the web client keeps playing audio.

To install for one user:
  install -Dm755 vibeiq      ~/.local/bin/vibeiq
  install -Dm644 vibeiq.png  ~/.local/share/icons/hicolor/512x512/apps/vibeiq.png
  install -Dm644 vibeiq.desktop ~/.local/share/applications/vibeiq.desktop
TXT
    tar -C "$TMP" -czf "$DIST/VibeIQ-$VER-linux-$arch.tar.gz" "vibeiq-linux-$arch"
  done
fi

# ── Windows ───────────────────────────────────────────────────────────────────────────────────
# TWO executables, and they may not differ only by case.
#
# ★★★ NTFS IS CASE-INSENSITIVE, exactly as the Mac's APFS is. `VibeIQ.exe` and `vibeiq.exe` would
#   be ONE FILE, and this is the second time that shape has come up in this app: on macOS the Swift
#   shell overwrote the Go bridge under the same name and then spawned itself forever, which took
#   the whole machine down (2026-09-09). The console build is `vibeiq-cli.exe`.
#
# ★ Why two at all: `-H windowsgui` detaches from the console, which is what stops a black cmd
#   window appearing behind the browser when someone double-clicks it — and it also means stdout
#   goes nowhere, so the same binary cannot serve the command line. One flag, two behaviours, so
#   two files.
if [ "$WANT" = all ] || [ "$WANT" = windows ]; then
  for arch in amd64 arm64; do
    d="$TMP/vibeiq-windows-$arch"; rm -rf "$d"; mkdir -p "$d"
    build windows "$arch" "$d/VibeIQ.exe" "-H windowsgui"
    build windows "$arch" "$d/vibeiq-cli.exe"
    check "$d/VibeIQ.exe"     "VibeIQ window:"
    check "$d/vibeiq-cli.exe" "VibeIQ window:"
    [ "$(ls "$d" | wc -l | tr -d ' ')" = 2 ] || { echo "!! two executables expected"; ls -l "$d"; exit 1; }
    cat > "$d/README.txt" <<TXT
VibeIQ $VER — raw IQ from a VibeSDR receiver, as rtl_tcp on this machine.

  VibeIQ.exe          double-click: opens the window in your browser. No console.
  vibeiq-cli.exe      the command line:  vibeiq-cli CODE
                      run it with no code and it asks for one.

Two files because Windows can only have one or the other: VibeIQ.exe is built without a console
so nothing black flashes up behind the browser, and a program without a console cannot print.

Then point SDR#, SDR++, HDSDR or anything else that speaks rtl_tcp at 127.0.0.1:1234.
Tuning in that app moves the receiver's dial; the web client keeps playing audio.
TXT
    (cd "$TMP" && zip -qr "$DIST/VibeIQ-$VER-windows-$arch.zip" "vibeiq-windows-$arch")
  done
fi

# ── macOS (bare binaries; the signed .app is mac/build-app.sh) ─────────────────────────────────
if [ "$WANT" = all ] || [ "$WANT" = darwin ]; then
  d="$TMP/vibeiq-macos"; rm -rf "$d"; mkdir -p "$d"
  build darwin arm64 "$TMP/vibeiq-darwin-arm64"
  build darwin amd64 "$TMP/vibeiq-darwin-amd64"
  lipo -create "$TMP/vibeiq-darwin-arm64" "$TMP/vibeiq-darwin-amd64" -output "$d/vibeiq"
  check "$d/vibeiq" "VibeIQ window:"
  # ★ Sign it if there is an identity to sign with, so the binary is at least attributable and
  #   `codesign -v` passes. It is NOT notarised: `stapler` can only attach a ticket to a bundle,
  #   dmg or pkg, so a loose executable has nowhere to carry one and Gatekeeper would fall back to
  #   an online check anyway. The quarantine line in the README is the honest answer for this file;
  #   people who want Gatekeeper to just work want VibeIQ.app, which IS notarised and stapled.
  IDENT=$(security find-identity -v -p codesigning 2>/dev/null | grep "Developer ID Application" | head -1 | awk '{print $2}' || true)
  if [ -n "${IDENT:-}" ]; then
    codesign --force --timestamp --options runtime --identifier net.vibesdr.vibeiq.cli --sign "$IDENT" "$d/vibeiq"
    codesign --verify --strict "$d/vibeiq"
    echo "    signed (not notarised — see README.txt)"
  else
    echo "    !! no Developer ID: shipping the macOS binary UNSIGNED"
  fi
  cat > "$d/README.txt" <<TXT
VibeIQ $VER — the command-line bridge, universal (Apple silicon + Intel).

Most people want VibeIQ.app instead; this is the same engine without the window, for scripts
and for anyone driving it over SSH.

  ./vibeiq            asks for the code (over SSH) or opens the window in your browser
  ./vibeiq CODE       pair and stream

It is not notarised, so the first run needs:  xattr -d com.apple.quarantine ./vibeiq
TXT
  tar -C "$TMP" -czf "$DIST/VibeIQ-$VER-macos-universal.tar.gz" "vibeiq-macos"
fi

echo
echo "==> $DIST"
ls -lh "$DIST"
