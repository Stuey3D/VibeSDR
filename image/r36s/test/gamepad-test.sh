#!/bin/sh
# gamepad-test.sh [URL] — INSIDE the image root on the R36S: does the page see the pad, and does an A press start
# the audio? Opens gamepad.html (or URL) in the kiosk, then a uinput Xbox-360-identity pad presses A; samples ALSA.
# ★ For the REAL r36s_Gamepad to reach libmanette under ROCKNIX, the image's udev rule must be live on the host:
#   cp 70-vibe-r36s-gamepad.rules /run/udev/rules.d/ && udevadm control -R && udevadm trigger -s input  (volatile)
URL=${1:-http://127.0.0.1:8099/gamepad.html}
cd /opt/test || exit 1
python3 -m http.server 8099 --bind 127.0.0.1 >/dev/null 2>&1 &
H=$!
sleep 1
sh /opt/test/kiosk-test.sh "$URL" 30 > /run/kiosk-test/kiosk.out 2>&1 &
K=$!
sleep 14
python3 /opt/test/fake-keys.py pad-a 5 &
P=$!
R=""
for i in $(seq 1 40); do
  sleep 0.5
  case "$(head -1 /proc/asound/card0/pcm0p/sub0/status 2>/dev/null)" in *RUNNING*) R="ALSA RUNNING ~$((i / 2)) s after the fake pad appeared (A pressed at ~5 s)"; break ;; esac
done
echo "${R:-ALSA never RUNNING}"
wait $P; wait $K
grep -E "\[gamepad\]|\[audio\]|gamepad|Gamepad" /run/kiosk-test/cog.log | sed 's/.*CONSOLE[^]]*\] //' | cut -c1-200 | head -30
kill $H 2>/dev/null
