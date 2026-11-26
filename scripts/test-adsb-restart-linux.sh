#!/usr/bin/env bash
# test-adsb-restart-linux.sh — the Linux daemon's side of test-adsb-restart (b96, Stuart 11-26 asked whether the Pi 500's
# V4L had the Sony's fault). Every Linux restart is a NEW PROCESS (execv / systemd), so the in-process stale-mode fault
# cannot happen there; this pins that a fresh daemon with adsb256 + landingAdsb + adsbKeepRunning (base 2.4) enters
# ADS-B at 2.56 MS/s on 1090 with aircraft on EVERY start, SIGTERM'd (a clean stop) between them.
#   usage: VIBESERVER_BIN=vibeserver/build/vibeserver [STARTS=10] scripts/test-adsb-restart-linux.sh
set -u
cd "$(dirname "$0")/.."
W=$(pwd)
BIN=${VIBESERVER_BIN:-}; N=${STARTS:-10}
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "test-adsb-restart-linux: NOT RUN (set VIBESERVER_BIN)"; exit 3; }
T=$(mktemp -d /tmp/vs-lxloop-XXXXXX)
nice -n 10 c++ -std=c++17 -O2 -I $W/vibeserver -I $W/android/app/src/main/cpp -o $T/ta $W/vibeserver/test-adsb.cpp && $T/ta --write-clip $T/c.u8 2 2560000 >/dev/null
RP=$((47900 + RANDOM % 50)); SP=$((48100 + RANDOM % 50)); FP=$((48200 + RANDOM % 50))
nice -n 10 node --max-old-space-size=256 $W/vibeserver/fake-rtl-tcp.mjs --port $RP --rate 2400000 --adsb $T/c.u8 > $T/fake.log 2>&1 & RTL=$!
mkdir -p $T/data $T/run
cat > $T/config.json <<J
{"configured":true,"fullMode":true,"sharing":"public","port":$FP,"radios":[{"serial":"fake","driver":"rtlsdr","enabled":true,"configured":true,"freq":96100000,"rate":2400000,"demodMode":"wfm","landingFreq":96100000,"users":10,"dabRateBoost":true,"adsb256":true,"landingAdsb":true,"adsbKeepRunning":true}]}
J
good=0
for i in $(seq 1 $N); do
  m=$(wc -c < $T/fake.log)
  VIBESERVER_CONFIG=$T/config.json VIBESERVER_DATA_DIR=$T/data VIBESERVER_RUNTIME_DIR=$T/run nice -n 10 $BIN --tcp 127.0.0.1:$RP --port $SP --rate 2400000 --idle-grace 0 > $T/srv$i.log 2>&1 & S=$!
  r=""; for k in $(seq 1 24); do sleep 0.5; c=$(curl -s --max-time 2 "http://127.0.0.1:$SP/vibeserver/aircraft.json?since=3" | sed -n 's/.*"count":\([0-9]*\).*/\1/p'); t=$(tail -c +$((m+1)) $T/fake.log); if [[ "$t" == *"tune 1090."* && "$t" == *"sample rate 2560000"* && "${c:-0}" -gt 0 ]]; then r=ok; break; fi; done
  [ "$r" = ok ] && good=$((good+1)); echo "start $i: ${r:-FAIL} (aircraft ${c:-?})"
  kill -TERM $S; wait $S 2>/dev/null
done
echo "$good of $N fresh-process starts in ADS-B at 2.56 with aircraft"
kill $RTL 2>/dev/null; rm -rf $T; [ $good -eq $N ]
