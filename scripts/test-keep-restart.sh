#!/usr/bin/env bash
# test-keep-restart.sh — kept VDL2 and kept ISM 868 come back after every IN-PROCESS restart (b96-keeprun). The real shim in
# one process against fake-rtl-tcp (synthetic IQ: the test asks only where each start TUNED the radio). SILENT. ~3 s a start.
#   usage: BUILD=vibeserver/build [STARTS=6] scripts/test-keep-restart.sh
set -u
cd "$(dirname "$0")/.."
B=${BUILD:-vibeserver/build}
[ -x "$B/test-keep-restart" ] || { echo "test-keep-restart: NOT RUN (no $B/test-keep-restart)"; exit 3; }
T=$(mktemp -d /tmp/vs-keeprestart-sh-XXXXXX)
PORT=$(( 47900 + RANDOM % 300 ))
nice -n 10 node --max-old-space-size=256 vibeserver/fake-rtl-tcp.mjs --port "$PORT" --rate 2400000 > "$T/fake.log" 2>&1 &
RTL=$!
trap 'kill $RTL 2>/dev/null; rm -rf "$T"' EXIT
sleep 0.5
nice -n 10 "$B/test-keep-restart" "$PORT" "$T/fake.log" "${STARTS:-6}" 2> "$T/server.log"
rc=$?
[ $rc -ne 0 ] && { echo "--- server log (MAP / AVDATA / ISM / stop lines, last 40) ---"; grep -E 'MAP|AVDATA|ISM|stop:' "$T/server.log" | tail -40; }
exit $rc
