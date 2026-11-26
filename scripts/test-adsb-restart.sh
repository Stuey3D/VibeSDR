#!/usr/bin/env bash
# test-adsb-restart.sh — kept-running ADS-B at 2.56 MS/s comes back after every IN-PROCESS restart (b96, the Sony 11-26).
# Builds a 2.56 MS/s 1090 MHz clip (test-adsb --write-clip), replays it with fake-rtl-tcp, and runs test-adsb-restart
# (the real shim in one process: start → kept ADS-B → stop → start …). SILENT: nothing is played. ~5 s a start.
#   usage: BUILD=vibeserver/build [STARTS=10] scripts/test-adsb-restart.sh
set -u
cd "$(dirname "$0")/.."
B=${BUILD:-vibeserver/build}
[ -x "$B/test-adsb-restart" ] || { echo "test-adsb-restart: NOT RUN (no $B/test-adsb-restart)"; exit 3; }
T=$(mktemp -d /tmp/vs-adsbrestart-sh-XXXXXX)
# ★ the clip writer: test-adsb, one file (as scripts/test-server-adsb-range.mjs compiles it)
nice -n 10 c++ -std=c++17 -O2 -I vibeserver -I android/app/src/main/cpp -o "$T/test-adsb" vibeserver/test-adsb.cpp || exit 1
PORT=$(( 47600 + RANDOM % 300 ))
"$T/test-adsb" --write-clip "$T/adsb256.u8" 2 2560000 > /dev/null || { echo "test-adsb-restart: could not write the clip"; exit 1; }
nice -n 10 node --max-old-space-size=256 vibeserver/fake-rtl-tcp.mjs --port "$PORT" --rate 2400000 --adsb "$T/adsb256.u8" > "$T/fake.log" 2>&1 &
RTL=$!
trap 'kill $RTL 2>/dev/null; rm -rf "$T"' EXIT
sleep 0.5
nice -n 10 "$B/test-adsb-restart" "$PORT" "$T/fake.log" "${STARTS:-10}" 2> "$T/server.log"
rc=$?
[ $rc -ne 0 ] && { echo "--- server log (ADS-B / rate lines, last 40) ---"; grep -E 'ADS-B|sample rate|stop:|rate ' "$T/server.log" | tail -40; }
exit $rc
