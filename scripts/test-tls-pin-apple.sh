#!/usr/bin/env bash
# test-tls-pin-apple.sh — per-server certificate pins (GitHub #52), Apple side: compile the app's own
# modules/vibe-tls-trust/VibeTlsCore.m on the Mac and run scripts/tls/VibeTlsPinTest.m against real self-signed
# servers on 127.0.0.1 (scripts/tls/tls-test-server.mjs). Security.framework is the same code on iOS and macOS.
# ★ Missing clang/openssl/node = exit 3, NOT RUN — never a pass.
set -uo pipefail
cd "$(dirname "$0")/.."
for t in clang openssl node; do command -v $t >/dev/null || { echo "   not run — needs $t"; exit 3; }; done
OUT="${TMPDIR:-/tmp}/vibe-tls-pin-apple"
rm -rf "$OUT"; mkdir -p "$OUT"

# The OWRX+-style pair: self-signed, CN only (no SAN), ten years, CA:TRUE — what `openssl req -x509` makes, which is
# how the OWRX+ FAQ's cert.pem/key.pem are usually produced. Plus a localhost one and an expired one.
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ss.key" -out "$OUT/ss.pem" -days 3650 -subj "/CN=openwebrx" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/lh.key" -out "$OUT/lh.pem" -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ex.key" -out "$OUT/ex.pem" -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost" -not_before 20200101000000Z -not_after 20210101000000Z 2>/dev/null
fp() { openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr 'A-F' 'a-f'; }

P1=$((20000 + RANDOM % 20000)); P2=$((P1 + 1)); P3=$((P1 + 2))
node scripts/tls/tls-test-server.mjs "$OUT/ss.pem" "$OUT/ss.key" $P1 "$OUT/lh.pem" "$OUT/lh.key" $P2 \
  "$OUT/ex.pem" "$OUT/ex.key" $P3 > "$OUT/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 1 50); do [ "$(grep -c LISTEN "$OUT/server.log" || true)" -ge 3 ] && break; sleep 0.1; done

# ★ The WebSocket cases run the app's REAL VibePinnedSRPolicy through the macOS slice of the very SocketRocket the app
#   links (React Native's prebuilt ReactNativeDependencies) — present after `pod install`, else those cases say not run.
RND=ios/Pods/ReactNativeDependencies/framework/packages/react-native/ReactNativeDependencies.xcframework
SR=()
if [ -d "$RND/macos-arm64_x86_64/ReactNativeDependencies.framework" ]; then
  SR=(-DVIBE_WITH_SR -I"$RND/Headers" -F"$RND/macos-arm64_x86_64" -framework ReactNativeDependencies
      -Wl,-rpath,"$PWD/$RND/macos-arm64_x86_64" modules/vibe-tls-trust/VibePinnedSRPolicy.m)
fi
if ! nice clang -fobjc-arc -Wall -Imodules/vibe-tls-trust -framework Foundation -framework Security ${SR[@]+"${SR[@]}"} \
      modules/vibe-tls-trust/VibeTlsCore.m scripts/tls/VibeTlsPinTest.m -o "$OUT/test" > "$OUT/build.log" 2>&1; then
  echo "   did not build"; tail -20 "$OUT/build.log"; exit 1
fi
"$OUT/test" $P1 "$(fp "$OUT/ss.pem")" $P2 "$(fp "$OUT/lh.pem")" $P3 "$(fp "$OUT/ex.pem")" "$OUT/server.log"
