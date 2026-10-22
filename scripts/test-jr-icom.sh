#!/bin/bash
# test-jr-icom.sh — run Jr's Icom rules (spike/WristSDR/WristSDR/IcomRadio.swift) on this Mac, no simulator.
# Compiles the one file with two stand-ins (scripts/jr-icom-check/stubs.swift) and the checks in main.swift.
set -o pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
nice -n 19 xcrun swiftc -o "$OUT/jr-icom-check" scripts/jr-icom-check/stubs.swift \
  spike/WristSDR/WristSDR/IcomRadio.swift scripts/jr-icom-check/main.swift || exit 1
"$OUT/jr-icom-check"
RC=$?
# ★ The rules above are only half of it: each must be WIRED where Jr draws and sends (a static check — the views and
#   the client import WatchKit and cannot build on macOS).
W=spike/WristSDR/WristSDR
wired() { if grep -qF -- "$2" "$W/$1"; then echo "   ok   $3"; else echo "   FAIL $3"; RC=1; fi; }
wired ContentView.swift 'IcomRadio.vfoFraction(vfo: link.frequency, centre: link.scopeCentre, span: link.scopeSpan)' 'fixed scope: the carrier line is placed from the real edges'
wired ContentView.swift 'IcomRadio.edges(vfo: link.frequency' 'fixed scope: the band ticker uses the same edges'
wired SpikeLink.swift 'if scopeCentre != u.centerHz { scopeCentre = u.centerHz }' 'fixed scope: the config centre is mirrored'
wired UberClient.swift 'IcomRadio.sendViewAtOpen(isVibe: isVibe, knownIcom: isIcom, seenBefore: radioSeen)' 'first visit: no zoom before the radio is known'
wired UberClient.swift 'IcomRadio.sendHeldViewOnFirstConfig(isIcom: isIcom, shared: sharedView)' 'first visit: the held view is sent or adopted on the first config'
exit $RC
