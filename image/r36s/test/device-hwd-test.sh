#!/bin/sh
# device-hwd-test.sh — on the R36S (ROCKNIX's python3 is enough): run vibe-hwd in DRY-RUN mode against the real
# backlight / ALSA / power supply, and drive it with fake-keys.py. Prints the event log and the hardware after each step.
# ★ ROCKNIX's own input.service / powerstate.service must be stopped first (they also own the volume keys / governor).
T=/storage/vibe-r36s-test
cd $T || exit 1
[ -f hwd.pid ] && kill "$(cat hwd.pid)" 2>/dev/null; [ -f listen.pid ] && kill "$(cat listen.pid)" 2>/dev/null
sleep 1
VIBE_HWD_DRYRUN=1 VIBE_STATE_DIR=$T/state VIBE_HWD_SOCK=/tmp/vibe-hwd.sock python3 -I bin/vibe-hwd > hwd.log 2>&1 &
echo $! > hwd.pid
sleep 2
python3 bin/hwd-client.py /tmp/vibe-hwd.sock listen 90 > events.log 2>&1 &
echo $! > listen.pid
sleep 1
vol() { amixer -c rk817ext cget name="Master Playback Volume" | sed -n 's/.*: values=//p'; }
mux() { amixer -c rk817ext cget name="Playback Mux" | sed -n 's/.*: values=//p'; }
bl() { echo "bl_power=$(cat /sys/class/backlight/backlight/bl_power) brightness=$(cat /sys/class/backlight/backlight/brightness)"; }
echo "start:            volume raw $(vol), mux $(mux) (0=HP 1=SPK), $(bl)"
python3 bin/fake-keys.py volume up 3;   echo "after vol+ x3:    volume raw $(vol)"
python3 bin/fake-keys.py volume down 2; echo "after vol- x2:    volume raw $(vol)"
python3 bin/fake-keys.py jack in;       echo "after jack in:    mux $(mux)"
python3 bin/fake-keys.py jack out;      echo "after jack out:   mux $(mux)"
python3 bin/fake-keys.py power short;   echo "after short pwr:  $(bl)"
python3 bin/fake-keys.py pad-press;     echo "after pad A:      $(bl)"
python3 bin/fake-keys.py power long;    sleep 6; echo "after long pwr (dry run)"
echo "--- events"; cat events.log
echo "--- hwd log"; cat hwd.log
echo "--- saved"; cat state/hwd.json; echo
kill "$(cat listen.pid)" "$(cat hwd.pid)" 2>/dev/null
