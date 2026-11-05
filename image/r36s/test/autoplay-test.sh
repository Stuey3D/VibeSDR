#!/bin/sh
# autoplay-test.sh — INSIDE the image root on the R36S: serve autoplay.html locally, open it in the kiosk exactly as
# vibe-kiosk does, sample ALSA while the tone should play, print the page's own report.
cd /opt/test || exit 1
python3 -m http.server 8099 --bind 127.0.0.1 >/dev/null 2>&1 &
H=$!
sleep 1
sh /opt/test/kiosk-test.sh http://127.0.0.1:8099/autoplay.html 0 >/dev/null 2>&1 &
K=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  sleep 0.5
  s=$(head -1 /proc/asound/card0/pcm0p/sub0/status 2>/dev/null)
  case "$s" in *RUNNING*) echo "ALSA RUNNING at sample $i: $(grep -E 'rate|format|channels' /proc/asound/card0/pcm0p/sub0/hw_params | tr '\n' ' ')"; break ;; esac
done
sleep 4
grep -E "\[autoplay\]" /run/kiosk-test/cog.log | sed 's/.*\[autoplay\]/  page:/'
wait $K 2>/dev/null; kill $H 2>/dev/null
