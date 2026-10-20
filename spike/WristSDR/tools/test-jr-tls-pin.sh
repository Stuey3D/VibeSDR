#!/usr/bin/env bash
# test-jr-tls-pin.sh — VibeSDR Jr's per-server certificate pins (GitHub #52): compile Jr's OWN
# WristSDR/TlsPin.swift + TlsTrust.swift on the Mac and run tools/tls/main.swift against real self-signed
# servers on 127.0.0.1 (scripts/tls/tls-test-server.mjs — the phone's test server, an OWRX+-style listener).
# Security, Network and URLSession are the same code on watchOS and macOS.
# ★ Missing swiftc/openssl/node = exit 3, NOT RUN — never a pass.
set -uo pipefail
cd "$(dirname "$0")/../../.."
for t in swiftc openssl node; do command -v $t >/dev/null || { echo "   not run — needs $t"; exit 3; }; done
OUT="${TMPDIR:-/tmp}/jr-tls-pin"
rm -rf "$OUT"; mkdir -p "$OUT"

# The same three certificates as scripts/test-tls-pin-apple.sh: OWRX+-style (CN only, ten years, CA:TRUE),
# a localhost one with SANs, and an expired one.
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ss.key" -out "$OUT/ss.pem" -days 3650 -subj "/CN=openwebrx" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/lh.key" -out "$OUT/lh.pem" -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ex.key" -out "$OUT/ex.pem" -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost" -not_before 20200101000000Z -not_after 20210101000000Z 2>/dev/null
fp() { openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr 'A-F' 'a-f'; }
for c in ss lh ex; do openssl x509 -in "$OUT/$c.pem" -outform der -out "$OUT/$c.der"; done

P1=$((20000 + RANDOM % 20000)); P2=$((P1 + 1)); P3=$((P1 + 2))
node scripts/tls/tls-test-server.mjs "$OUT/ss.pem" "$OUT/ss.key" $P1 "$OUT/lh.pem" "$OUT/lh.key" $P2 \
  "$OUT/ex.pem" "$OUT/ex.key" $P3 > "$OUT/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 1 50); do [ "$(grep -c LISTEN "$OUT/server.log" || true)" -ge 3 ] && break; sleep 0.1; done

J=spike/WristSDR/WristSDR
if ! nice swiftc -O -swift-version 5 -o "$OUT/test" "$J/TlsPin.swift" "$J/TlsTrust.swift" spike/WristSDR/tools/tls/main.swift \
      > "$OUT/build.log" 2>&1; then
  echo "   did not build"; tail -20 "$OUT/build.log"; exit 1
fi
"$OUT/test" $P1 "$(fp "$OUT/ss.pem")" $P2 "$(fp "$OUT/lh.pem")" $P3 "$(fp "$OUT/ex.pem")" "$OUT/server.log" \
  "$OUT/ss.der" "$OUT/lh.der" "$OUT/ex.der"
rc=$?

# ★ Part two: Jr's REAL AudioSocket.swift around the decision (ask → trust → reopen; cancel → one failure).
#   Its TlsTrustCenter is a scripted stand-in (tools/tls/socket/main.swift) — the real one is SwiftUI.
#   The P2 server's certificate names localhost, so the probe says "untrusted" (not "hostname").
echo
echo "AudioSocket"
if ! nice swiftc -O -swift-version 5 -o "$OUT/socktest" "$J/TlsPin.swift" "$J/TlsTrust.swift" "$J/AudioSocket.swift" \
      spike/WristSDR/tools/tls/socket/main.swift > "$OUT/build2.log" 2>&1; then
  echo "   did not build"; tail -20 "$OUT/build2.log"; exit 1
fi
"$OUT/socktest" $P2 || rc=1
exit $rc
