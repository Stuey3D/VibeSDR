#!/usr/bin/env bash
# test-sattrack-restart.sh — the satellite Doppler tracker ("vibe-sat-track") stops with the server across IN-PROCESS restarts
# (beta96; the XCover SIGABRT "mutex_lock called on a destroyed mutex" / Sony SIGSEGV of 11-25/26, both in that thread).
# fake-rtl-tcp at 2.4 MS/s + the ORBCOMM sats.json fixture; test-sattrack-restart runs the real shim in one process:
# start → ORBCOMM pick with Doppler (the tracker runs) → stop → start …, on the shared dial and with a listener's own lock.
# SILENT: no audio socket is opened.
#   usage: BUILD=vibeserver/build [STARTS=12] scripts/test-sattrack-restart.sh
#   ★ under a sanitiser (its own tree, the whole shim instrumented — slow to build, several minutes):
#     SAN=address scripts/test-sattrack-restart.sh   (or SAN=thread) — builds vibeserver/build-<san>-sattrack via the build lock
set -u
cd "$(dirname "$0")/.."
B=${BUILD:-vibeserver/build}
if [ -n "${SAN:-}" ]; then
  B="vibeserver/build-${SAN}-sattrack"
  F="-fsanitize=${SAN} -fno-omit-frame-pointer -g -O1"
  LOCK=/Users/stuey3d/VibeSDR/.claude/build-lock.sh; [ -x "$LOCK" ] || LOCK=
  $LOCK nice -n 10 cmake -S vibeserver -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$F" -DCMAKE_C_FLAGS="$F" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${SAN}" > /dev/null || exit 1
  $LOCK nice -n 10 cmake --build "$B" -j6 --target test-sattrack-restart > "$B.build.log" 2>&1 || { tail -30 "$B.build.log"; exit 1; }
  export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=1:print_stacktrace=1"
  export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:report_signal_unsafe=0"
fi
[ -x "$B/test-sattrack-restart" ] || { echo "test-sattrack-restart: NOT RUN (no $B/test-sattrack-restart)"; exit 3; }
T=$(mktemp -d /tmp/vs-sattrack-sh-XXXXXX)
PORT=$(( 47900 + RANDOM % 300 ))
nice -n 10 node --max-old-space-size=256 vibeserver/fake-rtl-tcp.mjs --port "$PORT" --rate 2400000 > "$T/fake.log" 2>&1 &
RTL=$!
trap 'kill $RTL 2>/dev/null; rm -rf "$T"' EXIT
sleep 0.5
nice -n 10 "$B/test-sattrack-restart" "$PORT" vibeserver/testdata-sat/sats-orbcomm-SYNTHETIC-2026-11-22.json "${STARTS:-12}" 2> "$T/server.log"
rc=$?
if [ -n "${SAN:-}" ]; then
  n=$(grep -c "ERROR: AddressSanitizer\|WARNING: ThreadSanitizer" "$T/server.log")
  echo "sanitiser ($SAN) reports: $n"
  grep -E "ERROR: AddressSanitizer|WARNING: ThreadSanitizer|SUMMARY:" "$T/server.log" | sort | uniq -c | head -20
  [ -n "${SAN_LOG:-}" ] && cp "$T/server.log" "$SAN_LOG"
  [ "$SAN" = address ] && [ "$n" -gt 0 ] && rc=1
fi
[ $rc -ne 0 ] && { echo "--- server log ([sat] / stop lines, last 40) ---"; grep -E '\[sat\]|stop:|tracker|Sanitizer' "$T/server.log" | tail -40; }
exit $rc
