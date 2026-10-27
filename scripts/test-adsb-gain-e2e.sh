#!/usr/bin/env bash
# test-adsb-gain-e2e.sh — the ADS-B gain rule (vibeadsb::GainControl) through the real server, on Stuart's FlightAware
# clip (filtered LNA feed, recorded at 49.6 dB) replayed by fake-rtl-tcp --adsb-gain, which scales it to the gain the
# server writes. The 10-25 A/B found the old message-only rule pinned that feed at 49.6 dB; dump1090-fa holds 38.6–40.2.
# Pass: the ramp ends at 38.6–40.2 dB, the reason names the noise floor, no feed warning, nothing decoded is lost
# (aircraft with position > 50). NOT RUN (exit 3) without VIBE_ADSB_CLIP (the 2.4 MS/s FlightAware recording).
set -uo pipefail
cd "$(dirname "$0")/.."
[ -n "${VIBE_ADSB_CLIP:-}" ] && [ -f "$VIBE_ADSB_CLIP" ] || { echo "test-adsb-gain-e2e: NOT RUN (set VIBE_ADSB_CLIP)"; exit 3; }
BIN="${VIBESERVER_BIN:-vibeserver/build/vibeserver}"
[ -x "$BIN" ] || { echo "test-adsb-gain-e2e: NOT RUN (no $BIN — set VIBESERVER_BIN)"; exit 3; }
T=$(mktemp -d); echo '{}' > "$T/config.json"; mkdir -p "$T/data"
cleanup() { kill "${SRV:-}" "${RTL:-}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
nice -n 15 node --max-old-space-size=256 vibeserver/fake-rtl-tcp.mjs --port 47341 --rate 2400000 --adsb "$VIBE_ADSB_CLIP" --adsb-gain > "$T/fake.log" 2>&1 & RTL=$!
sleep 1
VIBESERVER_CONFIG="$T/config.json" VIBESERVER_DATA_DIR="$T/data" nice -n 15 "$BIN" \
  --tcp 127.0.0.1:47341 --port 47342 --rate 2400000 --freq 96100000 > "$T/server.log" 2>&1 & SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null http://127.0.0.1:47342/ && break; sleep 0.5; done
OUT=$(node --max-old-space-size=256 scripts/probe-adsb-gain.mjs http://127.0.0.1:47342 "${VIBE_ADSB_GAIN_SECS:-90}" --stay)
echo "$OUT"
# ★★ A RECONNECT INSIDE THE 5 s GRACE (2026-10-26): the new session's plain landing retuned the tuner to 96.1 MHz under a
#    mode still on — the floor 13 dB up, no aircraft, the gain walked down. The second visit must find the sky as it was.
OUT2=$(node --max-old-space-size=256 scripts/probe-adsb-gain.mjs http://127.0.0.1:47342 15)
echo "$OUT2" | sed 's/^/   reconnect: /'
POS2=$(echo "$OUT2" | sed -n 's/^settled: .* pos \([0-9]*\) .*/\1/p')
grep -E "\[ADS-B\] gain" "$T/server.log" | sed 's/^/   server: /'
G=$(echo "$OUT" | sed -n 's/^settled: gain \([0-9.]*\) busy 0 .*/\1/p')
POS=$(echo "$OUT" | sed -n 's/^settled: .* pos \([0-9]*\) .*/\1/p')
if [ -n "$G" ] && awk -v g="$G" 'BEGIN{exit !(g>=38.5 && g<=40.3)}' && echo "$OUT" | grep -q "^why: noise floor at its target" \
   && echo "$OUT" | grep -q "^feedWarn: $" && [ "${POS:-0}" -gt 50 ] && [ "${POS2:-0}" -gt 50 ] \
   && grep -q "the receiver is in ADS-B, so the plain landing waits" "$T/server.log"; then
  echo "test-adsb-gain-e2e: ok (settled $G dB)"; rm -rf "$T"; exit 0
fi
echo "test-adsb-gain-e2e: FAIL (logs in $T)"; exit 1
