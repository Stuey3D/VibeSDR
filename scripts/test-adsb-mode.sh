#!/usr/bin/env bash
# test-adsb-mode.sh — ADS-B mode through the real server: fake-rtl-tcp replays a 1090 MHz recording, the built
# vibeserver decodes it, probe-adsb-mode.mjs drives it. NOT RUN (exit 3) unless VIBE_ADSB_CLIP names an rtl_sdr
# recording (unsigned 8-bit IQ) made at 2.4 MS/s on 1090 MHz — ~100 MB, not in the repo.
# ★ Also: no gain-step smoother events while in ADS-B (it read replies as +12 dB steps and turned the capture down).
set -uo pipefail
cd "$(dirname "$0")/.."
[ -n "${VIBE_ADSB_CLIP:-}" ] && [ -f "$VIBE_ADSB_CLIP" ] || { echo "test-adsb-mode: NOT RUN (set VIBE_ADSB_CLIP)"; exit 3; }
T=$(mktemp -d); echo '{}' > "$T/config.json"; mkdir -p "$T/data"
node vibeserver/fake-rtl-tcp.mjs --port 47331 --rate 2400000 --adsb "$VIBE_ADSB_CLIP" > "$T/fake.log" 2>&1 & RTL=$!
sleep 1
VIBESERVER_CONFIG="$T/config.json" VIBESERVER_DATA_DIR="$T/data" nice -n 19 vibeserver/build/vibeserver \
  --tcp 127.0.0.1:47331 --port 47332 --rate 2400000 --freq 96100000 ${VIBE_ADSB_EXTRA:-} > "$T/server.log" 2>&1 & SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null http://127.0.0.1:47332/ && break; sleep 0.5; done
OUT=$(node scripts/probe-adsb-mode.mjs http://127.0.0.1:47332)
kill $SRV $RTL 2>/dev/null
echo "$OUT"
echo "$OUT" | grep -q "adsb = true" && echo "$OUT" | grep -q "adsb_off true after: centre 96100000" \
  && echo "$OUT" | grep -q "audio before ADS-B flowing" && echo "$OUT" | grep -q "audio during ADS-B 0 bytes" \
  && echo "$OUT" | grep -q "audio after ADS-B flowing" \
  && [ "$(awk '/\[ADS-B\] mode ON/{on=1} /\[ADS-B\] mode OFF/{on=0} on' "$T/server.log" | grep -cE "gain step|gain transient" || true)" = 0 ] \
  && grep -q "tune ignored — ADS-B owns the receiver" "$T/server.log" \
  && [ "$(echo "$OUT" | sed -n 's/.*aircraft \([0-9]*\) with pos.*/\1/p' | tail -1)" -gt 50 ] \
  && { echo "test-adsb-mode: ok"; rm -rf "$T"; exit 0; }
echo "test-adsb-mode: FAIL (logs in $T)"; exit 1
