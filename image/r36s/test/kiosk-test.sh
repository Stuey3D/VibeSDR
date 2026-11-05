#!/bin/sh
# kiosk-test.sh URL [SECONDS] — INSIDE the image root on the R36S (enter-root.sh): the boot's display sequence for real.
# vibe-splash (boot mode) → progress messages → hand-off → Cog as vibe-kiosk with the kiosk unit's exact flags → then
# measure: hand-off time, CPU of each process, memory, whether ALSA is RUNNING, DRM mode.
# ★ Needs Panfrost loaded and nothing else on the display (ROCKNIX: systemctl stop essway sway; rmmod mali_kbase;
#   modprobe panfrost). Quiet: Master volume is set low first.
URL=${1:?url}; SECS=${2:-40}
L=/run/kiosk-test; mkdir -p $L     # ★ /run is the host's (enter-root.sh gives each entry a fresh /tmp)
amixer -q -c rk817ext cset name='Playback Mux' SPK; amixer -q -c rk817ext cset name='Master Playback Volume' 150,150
/usr/lib/vibesdr-r36s/vibe-splash > $L/splash.log 2>&1 &
SP=$!
sleep 1.5
for p in 12 22 35 50; do /usr/lib/vibesdr-r36s/vibe-splash-ctl progress $p; sleep 0.4; done
/usr/lib/vibesdr-r36s/vibe-splash-ctl message "Radio settings reset"; sleep 1
/usr/lib/vibesdr-r36s/vibe-splash-ctl message ""
VIBE_ENGINE_URL=$URL /usr/lib/vibesdr-r36s/vibe-wait-engine
T0=$(date +%s.%N)
mkdir -p /run/vibe-kiosk/cache /var/lib/vibe-kiosk; chown -R vibe-kiosk:vibe-kiosk /run/vibe-kiosk /var/lib/vibe-kiosk; chmod 0700 /run/vibe-kiosk
# ★ under ROCKNIX the device nodes carry ROCKNIX's gids (video 39, input 104, render 105, audio 63), not Debian's: add both
G=$(id -G vibe-kiosk | tr " " ","),39,104,105,63
setpriv --reuid vibe-kiosk --regid vibe-kiosk --groups "$G" env -i HOME=/var/lib/vibe-kiosk XDG_RUNTIME_DIR=/run/vibe-kiosk \
  XDG_CACHE_HOME=/run/vibe-kiosk/cache PATH=/usr/bin:/bin \
  cog -P drm --platform-params=renderer=gles --gamepad=manette \
  --enable-webgl=true --enable-2d-canvas-acceleration=true --enable-write-console-messages-to-stdout=true \
  --bg-color=#070907FF "$URL" > $L/cog.log 2>&1 &
CP=$!
wait $SP 2>/dev/null
echo "splash: $(cat $L/splash.log | tr '\n' ' ')"
sleep $SECS
echo "--- after ${SECS} s"
echo "cog alive: $(kill -0 $CP 2>/dev/null && echo yes || echo NO)"
ps -o pid,user,rss,args -e 2>/dev/null | grep -E '[c]og |WPE' | cut -c1-110
top -b -n 2 -d 5 | awk '/^top -/{n++} n==2' | grep -E 'WPE|cog|%Cpu' | head -8
free -m | head -2
for s in /proc/asound/card0/pcm0p/sub0/status; do echo "ALSA: $(head -1 $s)"; done
grep -l panfrost /proc/$(pgrep -x WPEWebProcess | head -1)/fdinfo/* 2>/dev/null | wc -l | sed 's/^/panfrost fds in WPEWebProcess: /'
echo "--- page console"; grep -v "^$" $L/cog.log | cut -c1-220 | tail -25
kill $CP 2>/dev/null; sleep 1
